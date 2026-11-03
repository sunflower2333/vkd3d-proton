/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_uav_clears(vkdu_device *device, vkdu_device *foreign)
{
    const uint32_t formats[] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT,
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT};
    uint32_t round;
    for (round = 0; round < 7; ++round) {
        vkdu_object *queue, *allocator, *command, *fence, *buffer, *other, *upload, *readback;
        vkdu_object *cpu, *gpu, *staging, *wrong, *texture, *root, *pipeline;
        uint32_t *mapped, i, values[4] = {0xa6312709, 0x12345678, 0xabcdef12, 0x87654321};
        uint32_t patch[4] = {0x7392e5c1, 0x91234567, 0xbcdef123, 0x76543218};
        uint32_t type = round & 1 ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_DIRECT;
        int floating = round >= 4;
        struct vkdu_rect rects[19] = {{-8, -2, 7, 2}, {250, 0, 999, 1}, {20, 0, 22, 1},
            {0, 1, 256, 2}, {0, -2, 256, 0}, {-20, 0, -1, 1}, {300, 0, 400, 1}};
        struct vkdu_rect empty = {0, 0, 0, 1}, invalid[2] = {{0, 0, 4, 1}, {2, 0, 1, 1}};
        struct vkdu_resource_barrier uav = {VKDU_BARRIER_UAV};
        struct vkdu_root_parameter param = {D3D12_ROOT_PARAMETER_TYPE_UAV};
        struct vkdu_descriptor_span dst[2], src[2];
        if (round == 4) { values[0] = 0x3fa00000; patch[0] = 0xc0200000; }
        if (round == 5) { values[0] = 0x7f800000; patch[0] = 0xff800000; }
        if (round == 6) { values[0] = 0x7fc12345; patch[0] = 0xffc23456; }
        CHECK(vkdu_queue_create(device, type, &queue)); CHECK(vkdu_allocator_create(device, type, &allocator));
        CHECK(vkdu_command_create(device, allocator, type, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
        CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, &buffer));
        CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, &other));
        CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
        CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
        CHECK(vkdu_heap_create(device, 0, 8, 0, &cpu)); CHECK(vkdu_heap_create(device, 0, 8, 0, &staging));
        CHECK(vkdu_heap_create(device, 0, 8, 1, &gpu)); CHECK(vkdu_heap_create(foreign, 0, 8, 0, &wrong));
        CHECK(vkdu_texture2d_create(device, 4, 4, DXGI_FORMAT_R32_FLOAT, &texture));
        CHECK(vkdu_root_create(device, &param, 1, 0, &root));
        CHECK(vkdu_pipeline_create(device, root, execute_indirect_cs_code_dxbc, sizeof(execute_indirect_cs_code_dxbc), &pipeline));
        CHECK(vkdu_command_root(command, root)); CHECK(vkdu_command_pipeline(command, pipeline));
        CHECK(vkdu_command_uav(command, 0, buffer, 2048));
        CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
        for (i = 0; i < 1024; ++i) mapped[i] = (0x51d70000 + round * 13) ^ i;
        CHECK(vkdu_buffer_unmap(upload, 0, 4096));
        CHECK(vkdu_command_copy(command, buffer, 0, upload, 0, 4096));
        CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        CHECK(vkdu_buffer_uav(cpu, 1, buffer, formats[round], 128, 256, 0, round == 3, NULL, 0));
        CHECK(vkdu_descriptor_copy(gpu, 4, cpu, 1, 1));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, values, 0, NULL));
        CHECK(vkdu_command_heaps(command, 1, &gpu));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, other, floating, values, 0, NULL));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, wrong, 1, buffer, floating, values, 0, NULL));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, gpu, 4, buffer, floating, values, 0, NULL));
        REJECT(vkdu_command_clear_uav(command, cpu, 1, cpu, 1, buffer, floating, values, 0, NULL));
        REJECT(vkdu_command_clear_uav(command, gpu, 8, cpu, 1, buffer, floating, values, 0, NULL));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, NULL, 0, NULL));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, values, 1, NULL));
        if (round == 0 || round == 1 || round == 3)
            REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, 1, values, 0, NULL));
        /* Every type-0 writer and both copy forms must replace the clear
         * identity. Invalid late ranges must preserve the existing view. */
        CHECK(vkdu_buffer_cbv(cpu, 1, buffer, 0, 256));
        CHECK(vkdu_descriptor_copy(gpu, 4, cpu, 1, 1));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, values, 0, NULL));
        CHECK(vkdu_buffer_uav(cpu, 1, buffer, formats[round], 128, 256, 0, round == 3, NULL, 0));
        CHECK(vkdu_buffer_srv(cpu, 1, buffer, DXGI_FORMAT_R32_UINT, 0, 256, 0, 0, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING));
        CHECK(vkdu_descriptor_copy(gpu, 4, cpu, 1, 1));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, values, 0, NULL));
        CHECK(vkdu_buffer_uav(cpu, 1, buffer, formats[round], 128, 256, 0, round == 3, NULL, 0));
        CHECK(vkdu_texture2d_srv(cpu, 1, texture)); CHECK(vkdu_descriptor_copy(gpu, 4, cpu, 1, 1));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, values, 0, NULL));
        CHECK(vkdu_buffer_uav(cpu, 1, buffer, DXGI_FORMAT_UNKNOWN, 0, 256, 4, 0, NULL, 0));
        CHECK(vkdu_descriptor_copy(gpu, 4, cpu, 1, 1));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, values, 0, NULL));
        CHECK(vkdu_buffer_uav(cpu, 1, buffer, formats[round], 129, 256, 0, round == 3, NULL, 0));
        CHECK(vkdu_descriptor_copy(gpu, 4, cpu, 1, 1));
        CHECK(vkdu_buffer_uav(cpu, 1, buffer, formats[round], 128, 256, 0, round == 3, NULL, 0));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, values, 0, NULL));
        CHECK(vkdu_descriptor_copy(staging, 3, cpu, 1, 1));
        dst[0] = (struct vkdu_descriptor_span){gpu, 4, 1}; dst[1] = (struct vkdu_descriptor_span){gpu, 6, 1};
        src[0] = (struct vkdu_descriptor_span){staging, 3, 1}; src[1] = (struct vkdu_descriptor_span){cpu, 1, 1};
        CHECK(vkdu_descriptor_copy_ranges(device, 0, 2, dst, 2, src));
        src[1].first = 8; REJECT(vkdu_descriptor_copy_ranges(device, 0, 2, dst, 2, src));
        CHECK(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, values, 0, NULL));
        uav.resource = buffer; CHECK(vkdu_command_barriers(command, 1, &uav));
        CHECK(vkdu_command_clear_uav(command, gpu, 6, staging, 3, buffer, floating, patch, 19, rects));
        CHECK(vkdu_command_barriers(command, 1, &uav));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, values, 2, invalid));
        CHECK(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, values, 1, &empty));
        CHECK(vkdu_command_dispatch(command, 256, 1, 1));
        CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 0, buffer, 0, 4096)); CHECK(vkdu_command_close(command));
        REJECT(vkdu_command_clear_uav(command, gpu, 4, cpu, 1, buffer, floating, values, 0, NULL));
        CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, fence, 1));
        CHECK(vkdu_fence_wait(fence, 1, 10000)); CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 1024; ++i) {
            uint32_t expected = (0x51d70000 + round * 13) ^ i;
            if (i >= 128 && i < 384) expected = values[0];
            if ((i >= 128 && i < 135) || (i >= 378 && i < 384) || (i >= 148 && i < 150)) expected = patch[0];
            if (i >= 512 && i < 768) expected = i - 512;
            if (mapped[i] != expected) {
                fprintf(stderr, "FAIL UAV clear round%u word%u=%08x expected%08x\n", round, i, mapped[i], expected); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
        vkdu_object_destroy(buffer); vkdu_object_destroy(other); vkdu_object_destroy(upload); vkdu_object_destroy(readback);
        vkdu_object_destroy(cpu); vkdu_object_destroy(gpu); vkdu_object_destroy(staging); vkdu_object_destroy(wrong);
        vkdu_object_destroy(texture); vkdu_object_destroy(root); vkdu_object_destroy(pipeline);
    }
    puts("PASS actual UAV Uint/Float buffer clears: 7x1024 words, typed/raw and float finite/Inf/NaN bits, clipped 19-rect batch, untouched guards, compute state, descriptor copy/replacement and invalid batch rejection");
}

#ifdef VKDU_ENABLE_TEST_DEVICE
static void check_uav_clear_lifetime(vkdu_device *device)
{
    unsigned round;
    for (round = 0; round < 3; ++round) {
        vkdu_object *memory, *a, *b, *cpu, *gpu, *command, *allocator, *queue, *gate, *done;
        vkdu_object *copy_allocator, *copy_command;
        struct d3d12_command_list *native;
        struct d3d12_descriptor_heap *retained_heap;
        uint32_t destroyed_a = 0, destroyed_b = 0, destroyed_gpu = 0;
        const uint32_t values[4] = {0xa2469bcd, 1, 2, 3};
        uint64_t completed;
        CHECK(vkdu_test_memory_heap_create(device, 65536, 0, &memory));
        CHECK(vkdu_buffer_place(device, memory, 0, 4096, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &a));
        CHECK(vkdu_buffer_place(device, memory, 0, 4096, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &b));
        if (vkdu_buffer_address(a) != vkdu_buffer_address(b)) { fputs("FAIL UAV alias addresses\n", stderr); exit(1); }
        CHECK(vkdu_test_destroy_counter(a, &destroyed_a)); CHECK(vkdu_test_destroy_counter(b, &destroyed_b));
        CHECK(vkdu_heap_create(device, 0, 1, 0, &cpu)); CHECK(vkdu_heap_create(device, 0, 1, 1, &gpu));
        CHECK(vkdu_test_destroy_counter(gpu, &destroyed_gpu));
        CHECK(vkdu_buffer_uav(cpu, 0, a, DXGI_FORMAT_R32_UINT, 0, 1024, 0, 0, NULL, 0));
        CHECK(vkdu_descriptor_copy(gpu, 0, cpu, 0, 1));
        CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
        CHECK(vkdu_command_create(device, allocator, 0, &command));
        CHECK(vkdu_fence_create(device, 0, &gate)); CHECK(vkdu_fence_create(device, 0, &done));
        CHECK(vkdu_command_heaps(command, 1, &gpu));
        native = vkdu_test_command_iface(command);
        if (native->allocator->descriptor_heaps_count != 1) {
            fputs("FAIL bound UAV descriptor heap not retained by allocator\n", stderr); exit(1);
        }
        retained_heap = native->allocator->descriptor_heaps[0];
        REJECT(vkdu_command_clear_uav(command, gpu, 0, cpu, 0, b, 0, values, 0, NULL));
        CHECK(vkdu_allocator_create(device, 3, &copy_allocator));
        CHECK(vkdu_command_create(device, copy_allocator, 3, &copy_command));
        REJECT(vkdu_command_clear_uav(copy_command, gpu, 0, cpu, 0, a, 0, values, 0, NULL));
        CHECK(vkdu_command_close(copy_command)); vkdu_object_destroy(copy_command); vkdu_object_destroy(copy_allocator);
        if (round == 0) {
            // Descriptors do not keep A alive. Reuse its exact memory through B
            // and prove copied stale views are still rejected by object ID.
            vkdu_object_destroy(a);
            if (vkdu_test_counter_load(&destroyed_a) != 1) { fputs("FAIL UAV descriptor owns resource\n", stderr); exit(1); }
            REJECT(vkdu_command_clear_uav(command, gpu, 0, cpu, 0, b, 0, values, 0, NULL));
            CHECK(vkdu_buffer_uav(cpu, 0, b, DXGI_FORMAT_R32_UINT, 0, 1024, 0, 0, NULL, 0));
            CHECK(vkdu_descriptor_copy(gpu, 0, cpu, 0, 1));
            CHECK(vkdu_command_clear_uav(command, gpu, 0, cpu, 0, b, 0, values, 0, NULL));
        } else {
            CHECK(vkdu_command_clear_uav(command, gpu, 0, cpu, 0, a, 0, values, 0, NULL));
            vkdu_object_destroy(a);
        }
        vkdu_object_destroy(b); vkdu_object_destroy(memory); vkdu_object_destroy(cpu); vkdu_object_destroy(gpu);
        /* Heap destruction notification covers external COM references. The
         * allocator retains its internal backing separately, so that notifier
         * must already fire even while the descriptor storage remains live. */
        if ((round ? vkdu_test_counter_load(&destroyed_a) : vkdu_test_counter_load(&destroyed_b)) ||
                vkdu_test_counter_load(&destroyed_gpu) != 1 || retained_heap->internal_refcount < 1 ||
                native->allocator->descriptor_heaps[0] != retained_heap) {
            fputs("FAIL UAV clear-only resource or internal heap retired before recording completion\n", stderr); exit(1);
        }
        CHECK(vkdu_command_close(command));
        if (round == 1) CHECK(vkdu_allocator_reset(allocator));
        if (round == 2) {
            CHECK(vkdu_queue_wait(queue, gate, 1)); CHECK(vkdu_queue_execute(queue, 1, &command));
            CHECK(vkdu_queue_signal(queue, done, 1)); CHECK(vkdu_queue_drain_enqueue(queue));
            CHECK(vkdu_fence_completed(done, &completed));
            if (completed) { fputs("FAIL UAV pending queue gate\n", stderr); exit(1); }
        }
        vkdu_object_destroy(command); vkdu_object_destroy(allocator);
        if (round == 2) {
            if (vkdu_test_counter_load(&destroyed_a) || retained_heap->internal_refcount < 1) {
                fputs("FAIL UAV clear resource/heap retired with GPU pending\n", stderr); exit(1);
            }
            CHECK(vkdu_fence_signal_cpu(gate, 1)); CHECK(vkdu_fence_wait(done, 1, 10000));
        }
        vkdu_object_destroy(queue); vkdu_object_destroy(gate); vkdu_object_destroy(done);
        if (vkdu_test_counter_load(&destroyed_a) != 1 || vkdu_test_counter_load(&destroyed_b) != 1 ||
                vkdu_test_counter_load(&destroyed_gpu) != 1) {
            fputs("FAIL UAV clear-only owners not destroyed exactly once\n", stderr); exit(1);
        }
    }
    puts("PASS actual clear-only UAV resource/heap lifetime: same-VA aliases, stale nonowning descriptors, cancel/reset/pending completion and exact destruction");
}
#endif
