/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native images stay GENERAL. The matched ICD separately opts into retaining
 * these image barriers as KMD native access references. KMD/host synchronization
 * remains responsible for foreign AHB access; no invented Vulkan queue family. */
static VkResult vkd3d_wddm_present_barriers(PFN_vkCmdPipelineBarrier2 record,
        VkCommandBuffer command, uint32_t count, const VkImage *images)
{
    VkImageMemoryBarrier2 barriers[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    uint32_t i;
    if (!record || !command || !images || !count || count > 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (i = 0; i < count; ++i)
    {
        if (!images[i] || (i && images[i] == images[0])) return VK_ERROR_INITIALIZATION_FAILED;
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[i].srcStageMask = barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[i].srcAccessMask = barriers[i].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barriers[i].oldLayout = barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = images[i];
        barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[i].subresourceRange.levelCount = barriers[i].subresourceRange.layerCount = 1;
    }
    dependency.imageMemoryBarrierCount = count;
    dependency.pImageMemoryBarriers = barriers;
    record(command, &dependency);
    return VK_SUCCESS;
}
