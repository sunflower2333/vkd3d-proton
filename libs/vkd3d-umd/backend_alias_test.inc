/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_ENABLE_TEST_DEVICE
static void check_alias_lifetime(vkdu_device *device)
{
    unsigned round;
    for (round = 0; round < 3; ++round) {
        vkdu_object *heap, *a, *b, *queue, *allocator, *command, *gate, *done;
        uint32_t destroyed_a = 0, destroyed_b = 0;
        uint64_t completed;
        struct vkdu_resource_barrier barrier;
        CHECK(vkdu_test_memory_heap_create(device, 65536, 0, &heap));
        CHECK(vkdu_buffer_place(device, heap, 0, 4096, D3D12_RESOURCE_STATE_COPY_DEST, &a));
        CHECK(vkdu_buffer_place(device, heap, 0, 4096, D3D12_RESOURCE_STATE_COPY_SOURCE, &b));
        CHECK(vkdu_test_destroy_counter(a, &destroyed_a)); CHECK(vkdu_test_destroy_counter(b, &destroyed_b));
        CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
        CHECK(vkdu_command_create(device, allocator, 0, &command));
        CHECK(vkdu_fence_create(device, 0, &gate)); CHECK(vkdu_fence_create(device, 0, &done));
        barrier = (struct vkdu_resource_barrier){VKDU_BARRIER_ALIASING, a, 0, 0, b};
        CHECK(vkdu_command_barriers(command, 1, &barrier));
        CHECK(vkdu_command_barriers(command, 1, &barrier)); CHECK(vkdu_command_close(command));
        vkdu_object_destroy(a); vkdu_object_destroy(b); vkdu_object_destroy(heap);
        if (vkdu_test_counter_load(&destroyed_a) || vkdu_test_counter_load(&destroyed_b)) {
            fputs("FAIL alias barrier released an endpoint before recording retirement\n", stderr); exit(1);
        }
        if (round == 1) CHECK(vkdu_allocator_reset(allocator));
        if (round == 2) {
            CHECK(vkdu_queue_wait(queue, gate, 1)); CHECK(vkdu_queue_execute(queue, 1, &command));
            CHECK(vkdu_queue_signal(queue, done, 1)); CHECK(vkdu_queue_drain_enqueue(queue));
            CHECK(vkdu_fence_completed(done, &completed));
            if (completed) { fputs("FAIL alias pending queue gate\n", stderr); exit(1); }
        }
        vkdu_object_destroy(command); vkdu_object_destroy(allocator);
        if (round == 2) {
            if (vkdu_test_counter_load(&destroyed_a) || vkdu_test_counter_load(&destroyed_b)) {
                fputs("FAIL alias endpoint released while GPU pending\n", stderr); exit(1);
            }
            CHECK(vkdu_fence_signal_cpu(gate, 1)); CHECK(vkdu_fence_wait(done, 1, 10000));
        }
        vkdu_object_destroy(queue); vkdu_object_destroy(gate); vkdu_object_destroy(done);
        if (vkdu_test_counter_load(&destroyed_a) != 1 || vkdu_test_counter_load(&destroyed_b) != 1) {
            fputs("FAIL alias endpoint not destroyed exactly once\n", stderr); exit(1);
        }
    }
    puts("PASS actual buffer alias barrier owners: both endpoints, duplicate recording, cancelled list, allocator reset and pending GPU completion");
}

static void check_alias_switch(vkdu_device *device, vkdu_device *foreign)
{
    vkdu_object *heap, *a, *b, *upload, *readback, *queue, *allocator, *command, *fence, *wrong, *texture;
    uint32_t *mapped, i, round;
    struct vkdu_resource_barrier alias, invalid[2];
    CHECK(vkdu_test_memory_heap_create(device, 65536, 0, &heap));
    CHECK(vkdu_buffer_place(device, heap, 0, 4096, D3D12_RESOURCE_STATE_COPY_DEST, &a));
    CHECK(vkdu_buffer_place(device, heap, 0, 4096, D3D12_RESOURCE_STATE_COPY_SOURCE, &b));
    if (vkdu_buffer_address(a) != vkdu_buffer_address(b)) { fputs("FAIL overlapping alias addresses\n", stderr); exit(1); }
    CHECK(vkdu_buffer_create(device, 8192, D3D12_HEAP_TYPE_UPLOAD, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
    CHECK(vkdu_buffer_create(device, 8192, D3D12_HEAP_TYPE_READBACK, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_buffer_create(foreign, 4096, D3D12_HEAP_TYPE_DEFAULT, 0, D3D12_RESOURCE_STATE_COMMON, &wrong));
    CHECK(vkdu_render_target_create(device, 4, 4, &texture));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
    alias = (struct vkdu_resource_barrier){VKDU_BARRIER_ALIASING, a, 0, 0, b};
    invalid[0] = alias; invalid[1] = alias; invalid[1].alias_after = wrong;
    REJECT(vkdu_command_barriers(command, 2, invalid));
    invalid[1].alias_after = texture; REJECT(vkdu_command_barriers(command, 2, invalid));
    invalid[1] = alias; invalid[1].before = 1; REJECT(vkdu_command_barriers(command, 2, invalid));
    invalid[1] = alias; invalid[1].after = 1; REJECT(vkdu_command_barriers(command, 2, invalid));
    invalid[1] = alias; invalid[1].resource = fence; REJECT(vkdu_command_barriers(command, 2, invalid));
    for (round = 0; round < 4; ++round) {
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, a, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
            CHECK(vkdu_command_transition(command, b, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
        }
        CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
        for (i = 0; i < 2048; ++i) mapped[i] = (0x835a0000 + round * 61) ^ (i * 47);
        CHECK(vkdu_buffer_unmap(upload, 0, 8192));
        CHECK(vkdu_command_copy(command, a, 0, upload, 0, 4096));
        alias = (struct vkdu_resource_barrier){VKDU_BARRIER_ALIASING, round & 1 ? NULL : a, 0, 0, round & 2 ? NULL : b};
        CHECK(vkdu_command_barriers(command, 1, &alias));
        CHECK(vkdu_command_copy(command, readback, 0, b, 0, 4096));
        /* Aliasing does not change either resource's independent transition
         * state or discard defined buffer contents. Partial writes through B
         * must be visible when A is activated again. */
        CHECK(vkdu_command_transition(command, b, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
        CHECK(vkdu_command_copy(command, b, 256, upload, 4096, 2048));
        alias = (struct vkdu_resource_barrier){VKDU_BARRIER_ALIASING, b, 0, 0, a};
        CHECK(vkdu_command_barriers(command, 1, &alias));
        CHECK(vkdu_command_transition(command, a, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 4096, a, 0, 4096));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, round + 1)); CHECK(vkdu_fence_wait(fence, round + 1, 10000));
        CHECK(vkdu_buffer_map(readback, 0, 8192, (void **)&mapped));
        for (i = 0; i < 2048; ++i) {
            const uint32_t word = i % 1024;
            const uint32_t source = i >= 1024 && word >= 64 && word < 576 ? word - 64 + 1024 : word;
            const uint32_t expected = (0x835a0000 + round * 61) ^ (source * 47);
            if (mapped[i] != expected) {
                fprintf(stderr, "FAIL overlapping alias activation round%u word%u=%08x expected%08x\n", round, i, mapped[i], expected); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        printf("PASS actual overlapping buffer alias switch round%u:2048 readback words, both/one/no endpoint, independent states and partial-write content preservation\n", round);
    }
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(a); vkdu_object_destroy(b); vkdu_object_destroy(heap); vkdu_object_destroy(texture);
    vkdu_object_destroy(upload); vkdu_object_destroy(readback); vkdu_object_destroy(wrong);
}
#endif
