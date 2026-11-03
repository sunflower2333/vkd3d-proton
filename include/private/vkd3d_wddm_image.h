/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VKD3D_WDDM_IMAGE_PRIVATE_H
#define VKD3D_WDDM_IMAGE_PRIVATE_H
#include <vulkan/vulkan.h>
#include "mesa_wddm_runtime.h"

/* No CPU fallback: the layout describes the same linear native storage which
 * the matched ICD imports through the provider token. */
struct vkd3d_wddm_image_desc
{
    struct mwd_allocation allocation;
    uint32_t width, height, stride;
    VkFormat format;
    VkImageUsageFlags usage;
};
struct vkd3d_wddm_image_device
{
    VkPhysicalDevice physical;
    VkDevice device;
    void *owner;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 query_format;
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkGetImageMemoryRequirements requirements;
    PFN_vkGetImageSubresourceLayout layout;
    PFN_vkAllocateMemory allocate;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindImageMemory2 bind;
};
struct vkd3d_wddm_image
{
    VkImage image;
    VkDeviceMemory memory;
};
#endif
