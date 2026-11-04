/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "wddm_native_image.inc"

HRESULT vkd3d_create_native_image_wddm(ID3D12Device *iface, const D3D12_RESOURCE_DESC *desc,
        void *owner, void *token, uint32_t stride, int preserve_contents, ID3D12Resource **out)
{
    struct d3d12_device *device;
    struct d3d12_resource *resource;
    struct vkd3d_wddm_image_device native;
    struct vkd3d_wddm_image_desc image_desc = {0};
    struct vkd3d_wddm_image image = {0};
    struct mwd_context_info context = {0};
    D3D12_RESOURCE_DESC1 copied = {0};
    D3D12_HEAP_PROPERTIES properties = {0};
    const struct vkd3d_format *format;
    HRESULT hr, release_hr;
    VkResult vr;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!iface || !desc || !owner || !token || (preserve_contents != 0 && preserve_contents != 1)) return E_INVALIDARG;
    device = impl_from_ID3D12Device((d3d12_device_iface *)iface);
    if (!device->wddm_native_surfaces) return DXGI_ERROR_UNSUPPORTED;
    if (device->wddm_runtime_owner != owner || desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc->Width > 16384 || desc->Height > 16384 || !desc->Width || !desc->Height ||
            desc->DepthOrArraySize != 1 || desc->MipLevels != 1 || desc->SampleDesc.Count != 1 ||
            desc->SampleDesc.Quality || desc->Alignment || desc->Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN ||
            (desc->Flags & ~(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)))
        return E_INVALIDARG;
    if (desc->Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc->Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
            desc->Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc->Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        return DXGI_ERROR_UNSUPPORTED;
    /* Copy caller storage before entering any provider/Vulkan callback. */
    copied.Dimension = desc->Dimension; copied.Width = desc->Width; copied.Height = desc->Height;
    copied.DepthOrArraySize = copied.MipLevels = 1; copied.Format = desc->Format;
    copied.SampleDesc = desc->SampleDesc; copied.Layout = desc->Layout; copied.Flags = desc->Flags;
    if ((hr = device->wddm_runtime_callbacks.retain(owner, token, &image_desc.allocation)) != S_OK)
        return FAILED(hr) ? hr : E_FAIL;
    hr = device->wddm_runtime_callbacks.context(owner, &context);
    if (hr != S_OK) { hr = FAILED(hr) ? hr : E_FAIL; goto release_token; }
    if (image_desc.allocation.token != token || image_desc.allocation.generation != context.generation ||
            !context.context_id || !context.queue_id || !context.va_size ||
            context.va_start > UINT64_MAX - context.va_size || image_desc.allocation.address < context.va_start ||
            image_desc.allocation.size > context.va_size ||
            image_desc.allocation.address > context.va_start + context.va_size - image_desc.allocation.size)
    { hr = E_INVALIDARG; goto release_token; }
    format = vkd3d_format_from_d3d12_resource_desc(device, &copied, 0);
    if (!format) { hr = DXGI_ERROR_UNSUPPORTED; goto release_token; }
    image_desc.width = copied.Width; image_desc.height = copied.Height; image_desc.stride = stride;
    image_desc.format = format->vk_format;
    image_desc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (copied.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) image_desc.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!(copied.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) image_desc.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    memset(&native, 0, sizeof(native));
    native.physical = device->vk_physical_device; native.device = device->vk_device; native.owner = owner;
    native.query_format = device->vkd3d_instance->vk_procs.vkGetPhysicalDeviceImageFormatProperties2;
    native.memory_properties = device->vkd3d_instance->vk_procs.vkGetPhysicalDeviceMemoryProperties;
    native.create_image = device->vk_procs.vkCreateImage; native.destroy_image = device->vk_procs.vkDestroyImage;
    native.requirements = device->vk_procs.vkGetImageMemoryRequirements;
    native.layout = device->vk_procs.vkGetImageSubresourceLayout;
    native.allocate = device->vk_procs.vkAllocateMemory; native.free_memory = device->vk_procs.vkFreeMemory;
    native.bind = device->vk_procs.vkBindImageMemory2;
    vr = vkd3d_wddm_image_create(&native, &image_desc, &image);
    hr = hresult_from_vk_result(vr);
    if (SUCCEEDED(hr))
    {
        hr = device->wddm_runtime_callbacks.status(owner);
        if (hr != S_OK) hr = FAILED(hr) ? hr : E_FAIL;
    }
    if (SUCCEEDED(hr))
    {
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        hr = d3d12_resource_create(device, VKD3D_RESOURCE_PLACED | VKD3D_RESOURCE_GENERAL_LAYOUT,
                &copied, &properties, D3D12_HEAP_FLAG_SHARED, D3D12_RESOURCE_STATE_COMMON, NULL, 0, NULL, &resource);
        if (SUCCEEDED(hr))
        {
            resource->res.vk_image = image.image;
            resource->wddm_native_memory = image.memory;
            resource->common_layout = VK_IMAGE_LAYOUT_GENERAL;
            resource->initial_layout_transition = !preserve_contents;
            *out = (ID3D12Resource *)&resource->ID3D12Resource_iface;
        }
    }
    if (FAILED(hr)) vkd3d_wddm_image_destroy(&native, &image);
release_token:
    release_hr = device->wddm_runtime_callbacks.release(owner, token);
    if (release_hr != S_OK && SUCCEEDED(hr))
    {
        if (*out) ID3D12Resource_Release(*out);
        *out = NULL;
        hr = FAILED(release_hr) ? release_hr : E_FAIL;
    }
    return hr;
}
