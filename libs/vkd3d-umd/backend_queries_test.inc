/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_ENABLE_TEST_DEVICE
static void check_query_lifetime(vkdu_device *device, vkdu_device *foreign)
{
    uint32_t round;
    for (round = 0; round < 3; round++) {
        vkdu_object *queue, *allocator, *command, *heap, *destination, *other, *fence, *gate;
        uint32_t destroyed_heap = 0, destroyed_destination = 0;
        uint64_t completed;
        CHECK(vkdu_queue_create(device, 0, &queue));
        CHECK(vkdu_allocator_create(device, 0, &allocator));
        CHECK(vkdu_command_create(device, allocator, 0, &command));
        CHECK(vkdu_query_heap_create(device, D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1, &heap));
        CHECK(vkdu_query_heap_create(foreign, D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1, &other));
        CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &destination));
        CHECK(vkdu_fence_create(device, 0, &fence)); CHECK(vkdu_fence_create(device, 0, &gate));
        CHECK(vkdu_test_destroy_counter(heap, &destroyed_heap));
        CHECK(vkdu_test_destroy_counter(destination, &destroyed_destination));
        REJECT(vkdu_command_query(command, other, D3D12_QUERY_TYPE_TIMESTAMP, 0, 0));
        REJECT(vkdu_command_query(command, heap, D3D12_QUERY_TYPE_TIMESTAMP, 1, 0));
        REJECT(vkdu_command_query_resolve(command, heap, D3D12_QUERY_TYPE_TIMESTAMP, UINT32_MAX, UINT32_MAX, destination, 0));
        REJECT(vkdu_command_query_resolve(command, heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, destination, UINT64_MAX - 7));
        CHECK(vkdu_command_query(command, heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 0));
        CHECK(vkdu_command_query_resolve(command, heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, destination, 64));
        CHECK(vkdu_command_close(command));
        vkdu_object_destroy(heap); vkdu_object_destroy(destination); vkdu_object_destroy(other);
        if (vkdu_test_counter_load(&destroyed_heap) || vkdu_test_counter_load(&destroyed_destination)) {
            fputs("FAIL query owners released before recording retirement\n", stderr); exit(1);
        }
        if (round == 1) {
            CHECK(vkdu_allocator_reset(allocator));
            if (vkdu_test_counter_load(&destroyed_heap) != 1 || vkdu_test_counter_load(&destroyed_destination) != 1) {
                fputs("FAIL query owners not released at legal allocator reset\n", stderr); exit(1);
            }
        }
        if (round == 2) {
            CHECK(vkdu_queue_wait(queue, gate, 1));
            CHECK(vkdu_queue_execute(queue, 1, &command));
            CHECK(vkdu_queue_signal(queue, fence, 1));
            CHECK(vkdu_queue_drain_enqueue(queue));
            CHECK(vkdu_fence_completed(fence, &completed));
            if (completed) { fputs("FAIL query lifetime gate did not hold GPU completion\n", stderr); exit(1); }
        }
        vkdu_object_destroy(command); vkdu_object_destroy(allocator);
        if (round == 2) {
            if (vkdu_test_counter_load(&destroyed_heap) || vkdu_test_counter_load(&destroyed_destination)) {
                fputs("FAIL query owners released while GPU submission remains pending\n", stderr); exit(1);
            }
            CHECK(vkdu_fence_signal_cpu(gate, 1)); CHECK(vkdu_fence_wait(fence, 1, 10000));
        }
        vkdu_object_destroy(queue); vkdu_object_destroy(fence); vkdu_object_destroy(gate);
        if (vkdu_test_counter_load(&destroyed_heap) != 1 || vkdu_test_counter_load(&destroyed_destination) != 1) {
            fputs("FAIL query owners did not retire exactly once\n", stderr); exit(1);
        }
    }
    puts("PASS actual query owner retention: cancelled recording, legal reset, pending GPU completion and exact destruction");
}
#endif
