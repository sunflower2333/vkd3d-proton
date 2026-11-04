/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* CPU Vulkan oracle for the native probe's workload, not KMT/GPU evidence. */
#ifdef VKDU_ENABLE_TEST_DEVICE
static void check_native_compute_layout(vkdu_device *device)
{
    vkdu_object *memory[3], *buffers[3], *queues[2], *allocator, *command, *fence;
    vkdu_object *cpu_heap, *gpu_heap, *root, *pipeline;
    const struct vkdu_descriptor_range range = {1, 1, 0, 0, 1};
    const struct vkdu_root_parameter parameter = {0, 0, 0, 0, 0, &range, 1};
    uint32_t *mapped, round, i;
    for (i = 0; i < 3; ++i) {
        CHECK(vkdu_test_memory_heap_create(device, 65536, i != 1, &memory[i]));
        CHECK(vkdu_buffer_place(device, memory[i], 0, 65536,
                i ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ, &buffers[i]));
    }
    CHECK(vkdu_queue_create(device, 0, &queues[0])); CHECK(vkdu_queue_create(device, 0, &queues[1]));
    CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command));
    CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_heap_create(device, 0, 8, 0, &cpu_heap));
    CHECK(vkdu_heap_create(device, 0, 8, 1, &gpu_heap));
    CHECK(vkdu_root_create(device, &parameter, 1, 0, &root));
    CHECK(vkdu_pipeline_create_tokens(device, root, execute_indirect_cs_code_dxbc + 21,
            execute_indirect_cs_code_dxbc[22], &pipeline));
    for (round = 0; round < 8; ++round) {
        const uint32_t first = 64 + round * 32;
        const uint32_t sentinel = 0xfedc0000u ^ (round * 0x01010101u);
        const uint32_t poison = 0x13579bdfu ^ (round * 0x11111111u);
        CHECK(vkdu_buffer_map(buffers[0], 0, 0, (void **)&mapped));
        for (i = 0; i < 16384; ++i) mapped[i] = sentinel;
        CHECK(vkdu_buffer_unmap(buffers[0], 0, 65536));
        CHECK(vkdu_buffer_map(buffers[2], 0, 0, (void **)&mapped));
        for (i = 0; i < 16384; ++i) mapped[i] = poison;
        CHECK(vkdu_buffer_unmap(buffers[2], 0, 65536));
        CHECK(vkdu_buffer_uav(cpu_heap, 2, buffers[1], DXGI_FORMAT_R32_TYPELESS, first, 1024, 0, 1, NULL, 0));
        CHECK(vkdu_descriptor_copy(gpu_heap, 4, cpu_heap, 2, 1));
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, buffers[1], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
        }
        CHECK(vkdu_command_copy(command, buffers[1], 0, buffers[0], 0, 65536));
        CHECK(vkdu_command_transition(command, buffers[1], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        CHECK(vkdu_command_root(command, root)); CHECK(vkdu_command_pipeline(command, pipeline));
        CHECK(vkdu_command_heaps(command, 1, &gpu_heap)); CHECK(vkdu_command_table(command, 0, gpu_heap, 3));
#ifndef VKDU_TEST_NATIVE_COMPUTE_DROP_DISPATCH
        CHECK(vkdu_command_dispatch(command, 1024, 1, 1));
#endif
        CHECK(vkdu_command_transition(command, buffers[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, buffers[2], 0, buffers[1], 0, 65536));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queues[round & 1], 1, &command));
        CHECK(vkdu_queue_signal(queues[round & 1], fence, round + 1)); CHECK(vkdu_fence_wait(fence, round + 1, 10000));
        CHECK(vkdu_buffer_map(buffers[2], 0, 65536, (void **)&mapped));
        for (i = 0; i < 16384; ++i) {
            const uint32_t expected = i >= first && i < first + 1024 ? i - first : sentinel;
            if (mapped[i] != expected) {
                fprintf(stderr, "FAIL CPU COMPUTE round=%u word=%u actual=%08x expected=%08x\n", round, i, mapped[i], expected);
                exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(buffers[2], 0, 0));
        printf("PASS CPU COMPUTE round=%u queue=%u first=%u outputs=1024 padding=15360\n", round, round & 1, first);
    }
    vkdu_object_destroy(command); vkdu_object_destroy(allocator);
    vkdu_object_destroy(queues[0]); vkdu_object_destroy(queues[1]); vkdu_object_destroy(fence);
    vkdu_object_destroy(pipeline); vkdu_object_destroy(root);
    vkdu_object_destroy(cpu_heap); vkdu_object_destroy(gpu_heap);
    for (i = 0; i < 3; ++i) { vkdu_object_destroy(buffers[i]); vkdu_object_destroy(memory[i]); }
    puts("PASS CPU COMPUTE actual SM5/default UAV/WRITE_BACK/full readback; no KMT or native hardware proof");
}
#endif
