/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Included by resource.c and the contract fixture. These are the production
 * Vulkan calls, with their exact data, failure unwinding and ownership order. */
#include "vkd3d_wddm_image.h"

static void vkd3d_wddm_image_destroy(const struct vkd3d_wddm_image_device *device,
        struct vkd3d_wddm_image *image)
{
    if (image->image) device->destroy_image(device->device, image->image, NULL);
    if (image->memory) device->free_memory(device->device, image->memory, NULL);
    memset(image, 0, sizeof(*image));
}

static VkResult vkd3d_wddm_image_create(const struct vkd3d_wddm_image_device *device,
        const struct vkd3d_wddm_image_desc *desc, struct vkd3d_wddm_image *out)
{
    const VkImageUsageFlags allowed = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    struct vkd3d_wddm_image_desc request;
    struct vkd3d_wddm_image_device dispatch;
    struct vkd3d_wddm_image created = {0};
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT query_modifier = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
    VkPhysicalDeviceImageFormatInfo2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    VkImageFormatProperties2 supported = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    VkImageCreateInfo create = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkImageSubresource subresource = {VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
    VkSubresourceLayout plane = {0}, layout = {0};
    VkMemoryRequirements requirements = {0};
    VkPhysicalDeviceMemoryProperties properties = {0};
    struct mwd_import_memory_info imported = {MWD_STYPE_IMPORT};
    VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkBindImageMemoryInfo bind = {VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
    uint32_t memory_type = UINT32_MAX, i;
    VkResult vr;

    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out, 0, sizeof(*out));
    if (!device || !device->owner || !device->physical || !device->device || !desc ||
            !device->query_format || !device->memory_properties || !device->create_image ||
            !device->destroy_image || !device->requirements || !device->layout || !device->allocate ||
            !device->free_memory || !device->bind) return VK_ERROR_INITIALIZATION_FAILED;
    request = *desc;
    desc = &request;
    dispatch = *device;
    device = &dispatch;
    if (!desc->allocation.token || !desc->allocation.handle || !desc->allocation.generation ||
            !desc->allocation.address || desc->allocation.address % 4096 || !desc->allocation.size ||
            desc->allocation.size % 4096 || desc->allocation.size > (UINT32_MAX & ~UINT64_C(4095)) ||
            desc->allocation.flags != (4u | MWD_ALLOCATION_NATIVE_SURFACE) ||
            !desc->width || !desc->height || desc->width > 16384 || desc->height > 16384 ||
            desc->stride < (uint64_t)desc->width * 4 || desc->stride % 4 ||
            (uint64_t)desc->stride * desc->height > desc->allocation.size ||
            !desc->usage || (desc->usage & ~allowed)) return VK_ERROR_INITIALIZATION_FAILED;
    if (desc->format != VK_FORMAT_R8G8B8A8_UNORM && desc->format != VK_FORMAT_R8G8B8A8_SRGB &&
            desc->format != VK_FORMAT_B8G8R8A8_UNORM && desc->format != VK_FORMAT_B8G8R8A8_SRGB)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    query_modifier.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    query.pNext = &query_modifier; query.format = desc->format; query.type = VK_IMAGE_TYPE_2D;
    query.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT; query.usage = desc->usage;
    query.flags = VK_IMAGE_CREATE_ALIAS_BIT;
    if ((vr = device->query_format(device->physical, &query, &supported)) != VK_SUCCESS) return vr;
    if (supported.imageFormatProperties.maxExtent.width < desc->width ||
            supported.imageFormatProperties.maxExtent.height < desc->height ||
            !supported.imageFormatProperties.maxExtent.depth || !supported.imageFormatProperties.maxMipLevels ||
            !supported.imageFormatProperties.maxArrayLayers ||
            !(supported.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    plane.rowPitch = desc->stride;
    modifier.drmFormatModifierPlaneCount = 1; modifier.pPlaneLayouts = &plane;
    create.pNext = &modifier; create.flags = query.flags; create.imageType = VK_IMAGE_TYPE_2D;
    create.format = desc->format; create.extent.width = desc->width; create.extent.height = desc->height;
    create.extent.depth = 1; create.mipLevels = create.arrayLayers = 1; create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = query.tiling; create.usage = desc->usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if ((vr = device->create_image(device->device, &create, NULL, &created.image)) != VK_SUCCESS)
    { created.image = VK_NULL_HANDLE; goto fail; }
    if (!created.image) { vr = VK_ERROR_INITIALIZATION_FAILED; goto fail; }
    device->requirements(device->device, created.image, &requirements);
    device->layout(device->device, created.image, &subresource, &layout);
    if (layout.offset || layout.rowPitch != desc->stride || !layout.size || layout.size > desc->allocation.size ||
            !requirements.size || requirements.size > desc->allocation.size || !requirements.alignment ||
            desc->allocation.address % requirements.alignment)
    { vr = VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT; goto fail; }
    device->memory_properties(device->physical, &properties);
    if (properties.memoryTypeCount > VK_MAX_MEMORY_TYPES) { vr = VK_ERROR_INITIALIZATION_FAILED; goto fail; }
    for (i = 0; i < properties.memoryTypeCount; ++i)
    {
        const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
        if ((requirements.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                !(flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)))
        { memory_type = i; break; }
    }
    if (memory_type == UINT32_MAX) { vr = VK_ERROR_FEATURE_NOT_PRESENT; goto fail; }
    imported.owner = device->owner; imported.token = desc->allocation.token;
    allocate.pNext = &imported; allocate.allocationSize = desc->allocation.size; allocate.memoryTypeIndex = memory_type;
    if ((vr = device->allocate(device->device, &allocate, NULL, &created.memory)) != VK_SUCCESS)
    { created.memory = VK_NULL_HANDLE; goto fail; }
    if (!created.memory) { vr = VK_ERROR_INITIALIZATION_FAILED; goto fail; }
    bind.image = created.image; bind.memory = created.memory;
    if ((vr = device->bind(device->device, 1, &bind)) != VK_SUCCESS) goto fail;
    *out = created;
    return VK_SUCCESS;
fail:
    vkd3d_wddm_image_destroy(device, &created);
    return vr;
}
