/* SPDX-License-Identifier: LGPL-2.1-or-later */
static bool d3d12_command_list_predicated_buffer(struct d3d12_command_list *list,
        VkDeviceAddress dst, VkDeviceAddress src, VkDeviceSize bytes, uint32_t fill, bool is_fill)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    const struct vkd3d_predicate_ops *ops = &list->device->meta_ops.predicate;
    struct vkd3d_predicate_buffer_args args;
    VkMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    bool byte_copy = !is_fill && ((dst | src | bytes) & 3);
    uint32_t stride = byte_copy ? 1 : 4;
    uint64_t remaining = bytes / stride;
    VkPipeline pipeline = byte_copy ? ops->vk_buffer_bytes_pipeline : ops->vk_buffer_pipeline;
    if (!pipeline || !(list->allocator->primary_pool.vk_queue_flags & VK_QUEUE_COMPUTE_BIT))
    {
        d3d12_command_list_mark_as_invalid(list, "Predicated buffer operation requires compute/byte storage support.\n");
        return false;
    }
    d3d12_command_list_end_current_render_pass(list, true);
    d3d12_command_list_end_transfer_batch(list, false);
    d3d12_command_list_update_conditional_rendering_state(list, true);
    d3d12_command_list_invalidate_current_pipeline(list, true);
    /* This meta operation replaces a transfer (or ClearUAV) action with a
     * compute action. Bridge both directions explicitly, including implicit
     * transfer ordering, readback memory and the GPU predicate snapshot. */
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
    VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline));
    args.predicate_va = list->predication.va;
    args.src_va = src;
    args.dst_va = dst;
    args.fill = fill;
    args.is_fill = is_fill;
    while (remaining)
    {
        args.count = min(remaining, (uint64_t)128 * 65535);
        d3d12_command_list_meta_push_data(list, list->cmd.vk_command_buffer,
                ops->vk_buffer_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(args), &args);
        VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, (args.count + 127) / 128, 1, 1));
        remaining -= args.count;
        args.src_va += (uint64_t)args.count * stride;
        args.dst_va += (uint64_t)args.count * stride;
    }
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
    d3d12_command_list_reset_transfer_waw_tracking(list);
    d3d12_command_list_update_conditional_rendering_state(list, false);
    return true;
}
