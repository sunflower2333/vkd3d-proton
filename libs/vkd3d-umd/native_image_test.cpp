/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Runs production image construction with controlled Vulkan callbacks.
 * No KMD, AHB allocation, pixel rendering or system admission is claimed. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "vkd3d_wddm_image.h"
#define CHECK(c, message) do { if (!(c)) { std::fprintf(stderr, "FAIL native image: %s\n", message); std::exit(1); } } while (0)
// PRODUCTION_FUNCTIONS

static unsigned images, memories, creates, imports;
static unsigned bad_layout, failure;
static bool no_device_only, poison_input;
static vkd3d_wddm_image_desc request, expected;
static vkd3d_wddm_image_device dispatch;
static VkImage image_handle = (VkImage)(uintptr_t)0x4567;
static VkDeviceMemory memory_handle = (VkDeviceMemory)(uintptr_t)0x5678;
static VkResult VKAPI_CALL query(VkPhysicalDevice physical, const VkPhysicalDeviceImageFormatInfo2 *in,
        VkImageFormatProperties2 *out)
{
    CHECK(physical == (VkPhysicalDevice)(uintptr_t)0x1111 && in->type == VK_IMAGE_TYPE_2D &&
        in->format == expected.format && in->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT &&
        in->usage == expected.usage && in->flags == VK_IMAGE_CREATE_ALIAS_BIT, "exact native format query");
    const auto *modifier = (const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *)in->pNext;
    CHECK(modifier && modifier->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT &&
        !modifier->pNext && !modifier->drmFormatModifier && modifier->sharingMode == VK_SHARING_MODE_EXCLUSIVE &&
        !modifier->queueFamilyIndexCount, "linear modifier query");
    if (poison_input) { std::memset(&request, 0xff, sizeof(request)); std::memset(&dispatch, 0xff, sizeof(dispatch)); }
    out->imageFormatProperties.maxExtent = {16384, 16384, 1};
    out->imageFormatProperties.maxMipLevels = out->imageFormatProperties.maxArrayLayers = 1;
    out->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    if (bad_layout == 9) out->imageFormatProperties.maxExtent.width = expected.width - 1;
    if (bad_layout == 10) out->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_4_BIT;
    if (bad_layout == 11) out->imageFormatProperties.maxMipLevels = 0;
    return failure == 1 ? VK_ERROR_FORMAT_NOT_SUPPORTED : VK_SUCCESS;
}
static VkResult VKAPI_CALL create(VkDevice device, const VkImageCreateInfo *in, const VkAllocationCallbacks *, VkImage *out)
{
    CHECK(device == (VkDevice)(uintptr_t)0x2222 && in->imageType == VK_IMAGE_TYPE_2D &&
        in->format == expected.format && in->extent.width == expected.width && in->extent.height == expected.height &&
        in->extent.depth == 1 && in->mipLevels == 1 && in->arrayLayers == 1 && in->samples == VK_SAMPLE_COUNT_1_BIT &&
        in->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED && in->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT &&
        in->flags == VK_IMAGE_CREATE_ALIAS_BIT && in->usage == expected.usage, "exact native image create");
    const auto *modifier = (const VkImageDrmFormatModifierExplicitCreateInfoEXT *)in->pNext;
    CHECK(modifier && modifier->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT &&
        !modifier->pNext && !modifier->drmFormatModifier && modifier->drmFormatModifierPlaneCount == 1 &&
        modifier->pPlaneLayouts->rowPitch == expected.stride && !modifier->pPlaneLayouts->offset &&
        !modifier->pPlaneLayouts->size && !modifier->pPlaneLayouts->arrayPitch &&
        !modifier->pPlaneLayouts->depthPitch, "exact native plane");
    ++creates;
    *out = image_handle;
    if (failure == 2) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    ++images;
    return VK_SUCCESS;
}
static void VKAPI_CALL destroy(VkDevice, VkImage image, const VkAllocationCallbacks *)
{ CHECK(image == image_handle && images == 1, "image destroyed once"); --images; }
static void VKAPI_CALL requirements(VkDevice, VkImage image, VkMemoryRequirements *out)
{
    CHECK(image == image_handle && images == 1, "live image requirement query");
    *out = {65536, 65536, 7};
    if (bad_layout == 5) out->size = 65537;
    if (bad_layout == 6) out->size = 0;
    if (bad_layout == 7) out->alignment = 0;
    if (bad_layout == 8) out->alignment = 262144;
}
static void VKAPI_CALL layout(VkDevice, VkImage image, const VkImageSubresource *in, VkSubresourceLayout *out)
{
    CHECK(image == image_handle && in->aspectMask == VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT &&
        !in->mipLevel && !in->arrayLayer, "native memory plane queried");
    *out = {}; out->rowPitch = expected.stride; out->size = uint64_t(expected.stride) * expected.height;
    if (bad_layout == 1) out->rowPitch += 4;
    if (bad_layout == 2) out->offset = 4096;
    if (bad_layout == 3) out->size = 0;
    if (bad_layout == 4) out->size = 65537;
}
static void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *out)
{
    out->memoryTypeCount = 3;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    out->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    out->memoryTypes[2].propertyFlags = no_device_only ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
static VkResult VKAPI_CALL allocate(VkDevice, const VkMemoryAllocateInfo *in, const VkAllocationCallbacks *, VkDeviceMemory *out)
{
    CHECK(in->allocationSize == 65536 && in->memoryTypeIndex == 2, "device-only exact backing import");
    const auto *imported = (const mwd_import_memory_info *)in->pNext;
    CHECK(imported && imported->sType == MWD_STYPE_IMPORT && !imported->pNext &&
        imported->owner == (void *)(uintptr_t)0x3333 && imported->token == expected.allocation.token,
        "native owner and token preserved");
    ++imports; *out = memory_handle;
    if (failure == 3) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    ++memories;
    return VK_SUCCESS;
}
static void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks *)
{
    CHECK(memory == memory_handle && memories == 1 && !images, "image destroyed before imported memory release");
    --memories;
}
static VkResult VKAPI_CALL bind(VkDevice, uint32_t count, const VkBindImageMemoryInfo *in)
{
    CHECK(count == 1 && !in->pNext && in->image == image_handle && in->memory == memory_handle && !in->memoryOffset &&
        images == 1 && memories == 1, "zero-offset native image binding");
    return failure == 4 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static void reset()
{
    CHECK(!images && !memories, "memory remains retained");
    creates = imports = failure = bad_layout = 0;
    no_device_only = poison_input = false;
    request = {};
    request.allocation = {(void *)(uintptr_t)0x9999, 0x20000, 65536, 7, 41, 4u | MWD_ALLOCATION_NATIVE_SURFACE};
    request.width = 17; request.height = 7; request.stride = 128;
    request.format = VK_FORMAT_R8G8B8A8_UNORM;
    request.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    expected = request;
    dispatch = {(VkPhysicalDevice)(uintptr_t)0x1111, (VkDevice)(uintptr_t)0x2222, (void *)(uintptr_t)0x3333,
        query, properties, create, destroy, requirements, layout, allocate, free_memory, bind};
}
int main()
{
    vkd3d_wddm_image output{};
    for (auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB})
    {
        reset(); request.format = expected.format = format;
        CHECK(vkd3d_wddm_image_create(&dispatch, &request, &output) == VK_SUCCESS && images == 1 && memories == 1,
            "native constructor accepts supported color format");
        vkd3d_wddm_image_destroy(&dispatch, &output);
        CHECK(!output.image && !output.memory && !images && !memories, "successful import releases exactly once");
    }
    reset(); const auto stable = dispatch; poison_input = true;
    CHECK(vkd3d_wddm_image_create(&dispatch, &request, &output) == VK_SUCCESS, "caller poisoning does not change request");
    vkd3d_wddm_image_destroy(&stable, &output);
    for (unsigned invalid = 1; invalid <= 11; ++invalid)
    {
        reset(); bad_layout = invalid;
        CHECK(vkd3d_wddm_image_create(&dispatch, &request, &output) != VK_SUCCESS && !output.image && !output.memory,
            "rejected layout mismatch");
        CHECK(!images && !memories && !imports, "invalid layout never imports native backing");
    }
    for (unsigned stage = 1; stage <= 4; ++stage)
    {
        reset(); failure = stage;
        CHECK(vkd3d_wddm_image_create(&dispatch, &request, &output) != VK_SUCCESS && !output.image && !output.memory,
            "Vulkan failure propagates");
        CHECK(!images && !memories, "Vulkan failure releases exact owners");
    }
    reset(); no_device_only = true;
    CHECK(vkd3d_wddm_image_create(&dispatch, &request, &output) == VK_ERROR_FEATURE_NOT_PRESENT && !imports && !images,
        "host-visible or lazy memory cannot back native image");
    for (unsigned invalid = 0; invalid < 18; ++invalid)
    {
        reset();
        switch (invalid)
        {
            case 0: request.width = 0; break;
            case 1: request.height = 0; break;
            case 2: request.width = 16385; break;
            case 3: request.stride = 64; break;
            case 4: request.stride = 129; break;
            case 5: request.stride = UINT32_MAX - 3; break;
            case 6: request.allocation.flags = 6; break;
            case 7: request.allocation.token = nullptr; break;
            case 8: request.allocation.handle = 0; break;
            case 9: request.allocation.address = 0; break;
            case 10: ++request.allocation.address; break;
            case 11: request.allocation.size = 0; break;
            case 12: --request.allocation.size; break;
            case 13: request.allocation.size = UINT64_MAX & ~uint64_t(4095); break;
            case 14: request.allocation.generation = 0; break;
            case 15: request.usage |= VK_IMAGE_USAGE_STORAGE_BIT; break;
            case 16: request.usage = 0; break;
            case 17: request.format = VK_FORMAT_D32_SFLOAT; break;
        }
        CHECK(vkd3d_wddm_image_create(&dispatch, &request, &output) != VK_SUCCESS && !creates && !imports &&
            !images && !memories && !output.image && !output.memory, "invalid descriptor rejected before Vulkan allocation");
    }
    reset();
    std::puts("PASS production native image: 4 formats, copied requests, exact linear layout/device-only import, 34 rejection/failure cases, ordered destruction");
    return 0;
}
