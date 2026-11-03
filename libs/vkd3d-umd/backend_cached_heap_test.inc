/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_ENABLE_TEST_DEVICE
static void check_buffer_alias_copy(vkdu_device *device)
{
    vkdu_object *heap, *parent, *child, *sibling, *readback, *queue, *allocator, *command, *fence;
    uint32_t *mapped, *input, round, i;
    CHECK(vkdu_test_memory_heap_create(device, 262144, 1, &heap));
    CHECK(vkdu_buffer_place(device, heap, 0, 262144, D3D12_RESOURCE_STATE_GENERIC_READ, &parent));
    CHECK(vkdu_buffer_place(device, heap, 65536, 65536, D3D12_RESOURCE_STATE_GENERIC_READ, &child));
    CHECK(vkdu_buffer_place(device, heap, 131072, 65536, D3D12_RESOURCE_STATE_COPY_DEST, &sibling));
    if (vkdu_buffer_address(child) != vkdu_buffer_address(parent) + 65536 ||
            vkdu_buffer_address(sibling) != vkdu_buffer_address(parent) + 131072) {
        fprintf(stderr, "FAIL actual placed buffer alias GPUVA offset\n"); exit(1);
    }
    CHECK(vkdu_buffer_map(parent, 0, 0, (void **)&input));
    for (i = 0; i < 65536; ++i) input[i] = 0x83170000 ^ (i * 37);
    CHECK(vkdu_buffer_unmap(parent, 0, 262144));
    /* Actual resources keep the shared VkDeviceMemory after both public owners
     * disappear. No native KMT import is involved in this CPU Vulkan factory. */
    vkdu_object_destroy(parent); vkdu_object_destroy(heap);
    CHECK(vkdu_buffer_create(device, 65536, D3D12_HEAP_TYPE_READBACK, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
    for (round = 0; round < 4; ++round) {
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, sibling, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
        }
        CHECK(vkdu_buffer_map(child, 0, 0, (void **)&input));
        for (i = 0; i < 16384; ++i) input[i] = (0x34560000 + round * 71) ^ (i * 43);
        CHECK(vkdu_buffer_unmap(child, 0, 65536));
        CHECK(vkdu_buffer_map(sibling, 0, 0, (void **)&mapped));
        for (i = 0; i < 16384; ++i) mapped[i] = 0xac190000 + round;
        CHECK(vkdu_buffer_unmap(sibling, 0, 65536));
        CHECK(vkdu_command_copy(command, sibling, 4096, child, 2048, 32768));
        CHECK(vkdu_command_transition(command, sibling, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 0, sibling, 0, 65536));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, round + 1)); CHECK(vkdu_fence_wait(fence, round + 1, 10000));
        CHECK(vkdu_buffer_map(readback, 0, 65536, (void **)&mapped));
        for (i = 0; i < 16384; ++i) {
            const uint32_t expected = i >= 1024 && i < 9216 ?
                (0x34560000 + round * 71) ^ ((i - 512) * 43) : 0xac190000 + round;
            if (mapped[i] != expected) {
                fprintf(stderr, "FAIL alias copy round%u word%u=%08x expected%08x\n", round, i, mapped[i], expected); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        printf("PASS actual overlapping parent/child buffer copy round%u: GPUVA equality, retired parent/heap, independent sibling state and16384 readback/sentinel words\n", round);
    }
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(child); vkdu_object_destroy(sibling); vkdu_object_destroy(readback);
}

static void check_cached_heaps(vkdu_device *device, vkdu_device *foreign)
{
    vkdu_object *heap, *upload, *target, *readback, *bad = NULL;
    vkdu_object *queue, *allocator, *command, *fence;
    uint32_t *input, *mapped, i, round;
    REJECT(vkdu_test_memory_heap_create(device, 65535, 1, &bad));
    CHECK(vkdu_test_memory_heap_create(device, 131072, 1, &heap));
    REJECT(vkdu_buffer_place(foreign, heap, 0, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, &bad));
    REJECT(vkdu_buffer_place(device, heap, 1, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, &bad));
    REJECT(vkdu_buffer_place(device, heap, 131072, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, &bad));
    REJECT(vkdu_buffer_place(device, heap, 0, 4096, D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_COPY_SOURCE, &bad));
    REJECT(vkdu_buffer_place(device, heap, 0, 4096, D3D12_RESOURCE_STATE_RENDER_TARGET, &bad));
    REJECT(vkdu_buffer_place(device, heap, 0, 4096, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &bad));
    CHECK(vkdu_buffer_place(device, heap, 0, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
    CHECK(vkdu_buffer_place(device, heap, 65536, 4096, D3D12_RESOURCE_STATE_COPY_DEST, &target));
    CHECK(vkdu_buffer_create(device, 4096, D3D12_HEAP_TYPE_READBACK, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
    /* Keep the cached upload persistently mapped across ordered submissions. */
    CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&input));
    for (round = 0; round < 4; ++round) {
        const uint32_t sentinel = 0xa5190000 ^ round;
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
        }
        for (i = 0; i < 1024; ++i) input[i] = (i * 37) ^ (0x13570000 + round * 113);
        CHECK(vkdu_buffer_map(target, 0, 0, (void **)&mapped));
        for (i = 0; i < 1024; ++i) mapped[i] = sentinel;
        CHECK(vkdu_buffer_unmap(target, 0, 4096));
        CHECK(vkdu_command_copy(command, target, 256, upload, 128, 2048));
        CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 0, target, 0, 4096));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, round + 1)); CHECK(vkdu_fence_wait(fence, round + 1, 10000));
        CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 1024; ++i) {
            const uint32_t expected = i >= 64 && i < 576 ? input[i - 32] : sentinel;
            if (mapped[i] != expected) {
                fprintf(stderr, "FAIL cached heap copy round%u word%u=%08x expected%08x\n", round, i, mapped[i], expected);
                exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        printf("PASS actual cached heap upload/copy/readback round%u: two placed offsets, persistent map and1024 data/sentinel words\n", round);
    }
    CHECK(vkdu_buffer_unmap(upload, 0, 4096));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(upload); vkdu_object_destroy(target); vkdu_object_destroy(readback); vkdu_object_destroy(heap);
}
#endif
