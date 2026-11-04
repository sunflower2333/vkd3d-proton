/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_predication(vkdu_device *device, vkdu_device *foreign)
{
    const uint64_t predicates[] = {0, 1, UINT64_C(1) << 32, UINT64_MAX};
    const uint32_t types[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY};
    unsigned t, failures = 0;
    for (t = 0; t < 3; ++t) {
        vkdu_object *queue, *allocator, *command, *fence, *upload, *predicate, *output, *readback;
        vkdu_object *cpu, *gpu, *root, *pipeline, *wrong, *fullsrc, *fulldst, *fullreadback;
        const struct vkdu_root_parameter param = {D3D12_ROOT_PARAMETER_TYPE_UAV};
        const uint32_t uints[4] = {0xa7125bdf, 2, 3, 4}, floats[4] = {0x3fa00000, 0, 0, 0};
        struct vkdu_resource_barrier uav = {VKDU_BARRIER_UAV};
        uint32_t *mapped, i, c;
        CHECK(vkdu_queue_create(device, types[t], &queue)); CHECK(vkdu_allocator_create(device, types[t], &allocator));
        CHECK(vkdu_command_create(device, allocator, types[t], &command)); CHECK(vkdu_fence_create(device, 0, &fence));
        CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
        CHECK(vkdu_buffer_create(device, 32, 1, 0, D3D12_RESOURCE_STATE_COPY_DEST, &predicate));
        CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, &output));
        CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
        CHECK(vkdu_buffer_create(foreign, 32, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &wrong));
        CHECK(vkdu_buffer_create(device, 256, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &fullsrc));
        CHECK(vkdu_buffer_create(device, 256, 1, 0, D3D12_RESOURCE_STATE_COPY_DEST, &fulldst));
        CHECK(vkdu_buffer_create(device, 2048, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &fullreadback));
        CHECK(vkdu_buffer_map(fullsrc, 0, 0, (void **)&mapped));
        for (i = 0; i < 64; ++i) mapped[i] = 0x27100000 + i;
        CHECK(vkdu_buffer_unmap(fullsrc, 0, 256));
        CHECK(vkdu_heap_create(device, 0, 16, 0, &cpu)); CHECK(vkdu_heap_create(device, 0, 16, 1, &gpu));
        CHECK(vkdu_root_create(device, &param, 1, 0, &root));
        CHECK(vkdu_pipeline_create(device, root, execute_indirect_cs_code_dxbc, sizeof(execute_indirect_cs_code_dxbc), &pipeline));
        CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
        for (i = 0; i < 1024; ++i) mapped[i] = 0xdeadbeef;
        memcpy(mapped, predicates, sizeof(predicates));
        for (i = 128; i < 136; ++i) mapped[i] = 0x12340000 + i;
        CHECK(vkdu_buffer_unmap(upload, 0, 4096));
        CHECK(vkdu_command_copy(command, output, 0, upload, 0, 4096));
        CHECK(vkdu_command_copy(command, predicate, 0, upload, 0, 32));
        CHECK(vkdu_command_transition(command, predicate, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION));
        REJECT(vkdu_command_predication(command, wrong, 0, 0));
        REJECT(vkdu_command_predication(command, predicate, 1, 0));
        REJECT(vkdu_command_predication(command, predicate, 32, 0));
        REJECT(vkdu_command_predication(command, predicate, UINT64_MAX - 7, 0));
        REJECT(vkdu_command_predication(command, predicate, 0, 2));
        REJECT(vkdu_command_predication(command, cpu, 0, 0));
        if (t != 2) {
            CHECK(vkdu_command_heaps(command, 1, &gpu)); CHECK(vkdu_command_root(command, root));
            CHECK(vkdu_command_pipeline(command, pipeline));
        }
        for (c = 0; c < 8; ++c) {
            const uint32_t base = c * 64;
            CHECK(vkdu_command_copy(command, fulldst, 0, upload, 2048, 256));
            CHECK(vkdu_command_predication(command, predicate, (c / 2) * 8, c & 1));
            CHECK(vkdu_command_copy(command, output, base * 4, upload, 512, 32));
            CHECK(vkdu_command_copy(command, output, base * 4 + 161, upload, 513, 27));
            CHECK(vkdu_command_resource_copy(command, fulldst, fullsrc));
            if (t != 2) {
                CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                CHECK(vkdu_buffer_uav(cpu, c * 2, output, DXGI_FORMAT_R32_UINT, base + 8, 8, 0, 0, NULL, 0));
                CHECK(vkdu_buffer_uav(cpu, c * 2 + 1, output, DXGI_FORMAT_R32_FLOAT, base + 16, 8, 0, 0, NULL, 0));
                CHECK(vkdu_descriptor_copy(gpu, c * 2, cpu, c * 2, 2));
                CHECK(vkdu_command_clear_uav(command, gpu, c * 2, cpu, c * 2, output, 0, uints, 0, NULL));
                uav.resource = output; CHECK(vkdu_command_barriers(command, 1, &uav));
                CHECK(vkdu_command_clear_uav(command, gpu, c * 2 + 1, cpu, c * 2 + 1, output, 1, floats, 0, NULL));
                CHECK(vkdu_command_barriers(command, 1, &uav));
                CHECK(vkdu_command_uav(command, 0, output, (base + 24) * 4));
                CHECK(vkdu_command_dispatch(command, 8, 1, 1));
                CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST));
            }
            CHECK(vkdu_command_predication(command, NULL, 0, 0));
            CHECK(vkdu_command_transition(command, fulldst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
            CHECK(vkdu_command_copy(command, fullreadback, c * 256, fulldst, 0, 256));
            CHECK(vkdu_command_transition(command, fulldst, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
        }
        CHECK(vkdu_command_predication(command, NULL, UINT64_MAX, 0));
        CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 0, output, 0, 4096)); CHECK(vkdu_command_close(command));
        REJECT(vkdu_command_predication(command, NULL, 0, 0));
        CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, fence, 1));
        CHECK(vkdu_fence_wait(fence, 1, 10000)); CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 1024; ++i) {
            uint32_t expected = 0xdeadbeef;
            c = i / 64;
            if (i < 8) memcpy(&expected, (const char *)predicates + i * 4, 4);
            if (i >= 128 && i < 136) expected = 0x12340000 + i;
            if (c < 8 && ((predicates[c / 2] != 0) != (c & 1))) {
                uint32_t lane = i % 64;
                if (lane < 8) expected = 0x12340080 + lane;
                for (unsigned b = 0; b < 4; ++b) {
                    const uint32_t byte = lane * 4 + b;
                    if (byte >= 161 && byte < 188) {
                        const uint32_t source_byte = 513 + byte - 161;
                        ((uint8_t *)&expected)[b] = (0x12340000 + source_byte / 4) >> (8 * (source_byte % 4));
                    }
                }
                if (t != 2) {
                    if (lane >= 8 && lane < 16) expected = uints[0];
                    if (lane >= 16 && lane < 24) expected = floats[0];
                    if (lane >= 24 && lane < 32) expected = lane - 24;
                }
            }
            if (mapped[i] != expected) {
                if (failures < 24) fprintf(stderr, "FAIL predication type%u word%u=%08x expected%08x\n", types[t], i, mapped[i], expected);
                ++failures;
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        CHECK(vkdu_buffer_map(fullreadback, 0, 2048, (void **)&mapped));
        for (i = 0; i < 512; ++i) {
            c = i / 64;
            const uint32_t expected = ((predicates[c / 2] != 0) != (c & 1)) ? 0x27100000 + i % 64 : 0xdeadbeef;
            if (mapped[i] != expected) {
                if (failures < 24) fprintf(stderr, "FAIL predicated CopyResource type%u word%u=%08x expected%08x\n", types[t], i, mapped[i], expected);
                ++failures;
            }
        }
        CHECK(vkdu_buffer_unmap(fullreadback, 0, 0));
        vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
        vkdu_object_destroy(upload); vkdu_object_destroy(predicate); vkdu_object_destroy(output); vkdu_object_destroy(readback);
        vkdu_object_destroy(cpu); vkdu_object_destroy(gpu); vkdu_object_destroy(root); vkdu_object_destroy(pipeline); vkdu_object_destroy(wrong);
        vkdu_object_destroy(fullsrc); vkdu_object_destroy(fulldst); vkdu_object_destroy(fullreadback);
    }
    if (failures) { fprintf(stderr, "FAIL predication total mismatched words=%u\n", failures); exit(1); }
    puts("PASS actual predication: Direct/Compute/Copy 3x1536 words, zero/one/high64/all64 and both operations, aligned/27-byte CopyBufferRegion, CopyResource, Uint/Float UAV clear, Dispatch, disable and invalid input");
}

#ifdef VKDU_ENABLE_TEST_DEVICE
static void check_geometry_output(vkdu_device *device, vkdu_device *other);
static void check_tessellation(vkdu_device *device);
static void check_tessellation_lines(vkdu_device *device);
static void check_predication_snapshot(vkdu_device *device)
{
    vkdu_object *upload, *output, *readback, *predicate, *query, *target, *rtv, *root, *graphics;
    vkdu_object *command, *allocator, *queue, *fence, *depth, *dsv, *other;
    const struct vkdu_root_parameter params[] = {{1, 0, 0, 0, 2}, {4, 0, 0, 0, 0}};
    const struct vkdu_graphics_desc desc = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT32_MAX, 3, 1, 0, 1, 15};
    const struct vkdu_viewport viewport = {0, 0, 4, 4, 0, 1};
    const struct vkdu_rect scissor = {1, 2, 2, 3};
    const struct vkdu_signature vin = {6, 0, 1, 1}, vout = {1, 0, 15, 3};
    const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc), *p = graphics_tokens(ps_store_buffer_code_dxbc);
    void *vs, *ps; size_t vs_bytes, ps_bytes;
    uint32_t *mapped, i, values[2] = {40, 0x9abcef01};
    CHECK(vkdu_root_create(device, params, 2, 0, &root));
    CHECK(vkdu_shader_dxbc(1, v, v[1], &vin, 1, &vout, 1, &vs, &vs_bytes));
    CHECK(vkdu_shader_dxbc(0, p, p[1], NULL, 0, NULL, 0, &ps, &ps_bytes));
    CHECK(vkdu_graphics_pipeline_create(device, root, vs, vs_bytes, ps, ps_bytes, &desc, &graphics));
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps);
    CHECK(vkdu_render_target_create(device, 4, 4, &target)); CHECK(vkdu_heap_create(device, 2, 1, 0, &rtv));
    CHECK(vkdu_texture2d_rtv(rtv, 0, target)); CHECK(vkdu_query_heap_create(device, 0, 1, &query));
    CHECK(vkdu_depth_create(device, 4, 4, DXGI_FORMAT_D32_FLOAT, &depth));
    CHECK(vkdu_heap_create(device, 3, 1, 0, &dsv)); CHECK(vkdu_texture2d_dsv(dsv, 0, depth, 0));
    CHECK(vkdu_render_target_create(device, 4, 4, &other));
    CHECK(vkdu_buffer_create(device, 8192, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
    CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, &output));
    CHECK(vkdu_buffer_create(device, 4352, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_buffer_create(device, 8, 1, 0, D3D12_RESOURCE_STATE_COPY_DEST, &predicate));
    CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
    for (i = 0; i < 2048; ++i) mapped[i] = 0xdeadbeef;
    mapped[1024] = mapped[1025] = mapped[1027] = 0; mapped[1026] = 1;
    CHECK(vkdu_buffer_unmap(upload, 0, 8192));
    CHECK(vkdu_allocator_create(device, 0, &allocator)); CHECK(vkdu_command_create(device, allocator, 0, &command));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_command_copy(command, output, 0, upload, 0, 4096));
    CHECK(vkdu_command_copy(command, readback, 4096, upload, 0, 8));
    CHECK(vkdu_command_copy(command, predicate, 0, upload, 4096, 8));
    CHECK(vkdu_command_transition(command, predicate, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION));
    CHECK(vkdu_command_predication(command, predicate, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO));
    {
        const float color[4] = {1, 0, 1, 1};
        const struct vkdu_texture_copy_location src = {target}, dst = {other};
        struct d3d12_command_list *native_clear = vkdu_test_command_iface(command);
        HRESULT clear_result = native_clear->device->device_info.conditional_rendering_features.conditionalRendering ? S_OK : E_NOTIMPL;
        if (vkdu_command_clear_rtv(command, rtv, 0, color, 0, NULL) != clear_result ||
                vkdu_command_clear_dsv(command, dsv, 0, 1, 1, 0, 0, NULL) != clear_result ||
                vkdu_command_resource_copy(command, other, target) != E_NOTIMPL ||
                vkdu_command_texture_copy(command, &dst, 0, 0, 0, &src, NULL) != E_NOTIMPL) {
            fputs("FAIL unfinished texture predication not closed\n", stderr); exit(1);
        }
    }
    CHECK(vkdu_command_transition(command, predicate, D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_COPY_DEST));
    CHECK(vkdu_command_copy(command, predicate, 0, upload, 4104, 8));
    CHECK(vkdu_command_transition(command, predicate, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION));
    CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    CHECK(vkdu_command_pipeline(command, graphics)); CHECK(vkdu_command_graphics_root(command, root));
    CHECK(vkdu_command_graphics_uav(command, 1, output, 0));
    CHECK(vkdu_command_render_target(command, rtv, 0)); CHECK(vkdu_command_topology(command, 4));
    CHECK(vkdu_command_viewports(command, 1, &viewport)); CHECK(vkdu_command_scissors(command, 1, &scissor));
    CHECK(vkdu_command_graphics_constants(command, 0, 0, 2, values));
    CHECK(vkdu_command_draw(command, 3, 1, 0, 0)); /* Snapshot remains zero despite GPU writing one. */
    CHECK(vkdu_command_predication(command, predicate, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO));
    values[0] = 44; CHECK(vkdu_command_graphics_constants(command, 0, 0, 2, values));
    CHECK(vkdu_command_query(command, query, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 1));
    CHECK(vkdu_command_draw(command, 3, 1, 0, 0));
    CHECK(vkdu_command_query(command, query, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 0));
    CHECK(vkdu_command_query_resolve(command, query, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 1, readback, 4096));
    CHECK(vkdu_command_predication(command, NULL, 0, 0));
    values[0] = 52; CHECK(vkdu_command_graphics_constants(command, 0, 0, 2, values)); CHECK(vkdu_command_draw(command, 3, 1, 0, 0));
    /* Close with false predication, then reset must begin disabled. */
    CHECK(vkdu_command_predication(command, predicate, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO));
    CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
    CHECK(vkdu_queue_signal(queue, fence, 1)); CHECK(vkdu_fence_wait(fence, 1, 10000));
    CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
    CHECK(vkdu_command_pipeline(command, graphics)); CHECK(vkdu_command_graphics_root(command, root));
    CHECK(vkdu_command_graphics_uav(command, 1, output, 0)); CHECK(vkdu_command_render_target(command, rtv, 0));
    CHECK(vkdu_command_topology(command, 4)); CHECK(vkdu_command_viewports(command, 1, &viewport));
    CHECK(vkdu_command_scissors(command, 1, &scissor));
    values[0] = 56; CHECK(vkdu_command_graphics_constants(command, 0, 0, 2, values)); CHECK(vkdu_command_draw(command, 3, 1, 0, 0));
    CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
    CHECK(vkdu_command_copy(command, readback, 0, output, 0, 4096)); CHECK(vkdu_command_close(command));
    CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, fence, 2)); CHECK(vkdu_fence_wait(fence, 2, 10000));
    CHECK(vkdu_buffer_map(readback, 0, 4104, (void **)&mapped));
    for (i = 0; i < 1026; ++i) {
        const uint32_t expected = i >= 1024 ? 0 : i == 10 || i == 13 || i == 14 ? 0x9abcef01 : 0xdeadbeef;
        if (mapped[i] != expected) { fprintf(stderr, "FAIL predicate snapshot/draw/query/reset word%u=%08x expected%08x\n", i, mapped[i], expected); exit(1); }
    }
    CHECK(vkdu_buffer_unmap(readback, 0, 0));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(upload); vkdu_object_destroy(output); vkdu_object_destroy(readback); vkdu_object_destroy(predicate);
    vkdu_object_destroy(query); vkdu_object_destroy(target); vkdu_object_destroy(rtv); vkdu_object_destroy(root); vkdu_object_destroy(graphics);
    vkdu_object_destroy(depth); vkdu_object_destroy(dsv); vkdu_object_destroy(other);
    puts("PASS predicate GPU snapshot/rebind, real Draw pixel UAV writes, unpredicated Begin/End/ResolveQueryData, NULL disable and Reset disabled: 1026 words");
}

static PFN_vkCmdBeginConditionalRenderingEXT stage_begin_original;
static PFN_vkCmdDrawIndirect stage_indirect_original;
static unsigned stage_begin_calls, stage_indirect_calls;
static void VKAPI_CALL stage_begin_counted(VkCommandBuffer command, const VkConditionalRenderingBeginInfoEXT *info)
{
    ++stage_begin_calls; stage_begin_original(command, info);
}
static void VKAPI_CALL stage_indirect_counted(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset,
        uint32_t count, uint32_t stride)
{
    ++stage_indirect_calls; stage_indirect_original(command, buffer, offset, count, stride);
}
static void stage_path_check(unsigned fallback, const char *stage)
{
    printf("ACTUAL predicate path stage=%s fallback=%u EXT begins=%u DrawIndirect=%u\n",
            stage, fallback, stage_begin_calls, stage_indirect_calls);
    if ((!fallback && !stage_begin_calls) || (fallback && (stage_begin_calls || !stage_indirect_calls))) {
        fputs("FAIL shader stage predicate execution route\n", stderr); exit(1);
    }
    stage_begin_calls = stage_indirect_calls = 0;
}
static void check_predication_fallback(vkdu_device *device, vkdu_device *foreign)
{
    vkdu_object *allocator, *command;
    struct d3d12_command_list *native;
    VkBool32 enabled;
    CHECK(vkdu_allocator_create(device, 0, &allocator)); CHECK(vkdu_command_create(device, allocator, 0, &command));
    native = vkdu_test_command_iface(command);
    enabled = native->device->device_info.conditional_rendering_features.conditionalRendering;
    if (!enabled || !native->device->vk_procs.vkCmdBeginConditionalRenderingEXT) {
        fputs("FAIL explicit EXT shader-stage fixture unavailable\n", stderr); exit(1);
    }
    stage_begin_original = native->device->vk_procs.vkCmdBeginConditionalRenderingEXT;
    stage_indirect_original = native->device->vk_procs.vkCmdDrawIndirect;
    native->device->vk_procs.vkCmdBeginConditionalRenderingEXT = stage_begin_counted;
    native->device->vk_procs.vkCmdDrawIndirect = stage_indirect_counted;
    for (unsigned fallback = 0; fallback < 2; ++fallback) {
        native->device->device_info.conditional_rendering_features.conditionalRendering = !fallback;
        stage_begin_calls = stage_indirect_calls = 0;
        check_predication(device, foreign); check_predication_snapshot(device);
        stage_path_check(fallback, "buffer/draw/query");
        check_geometry_output(device, foreign); stage_path_check(fallback, "GS/SO");
        check_tessellation(device); stage_path_check(fallback, "HS/DS tri/quad");
        check_tessellation_lines(device); stage_path_check(fallback, "HS/DS isoline/GS/SO");
    }
    native->device->vk_procs.vkCmdBeginConditionalRenderingEXT = stage_begin_original;
    native->device->vk_procs.vkCmdDrawIndirect = stage_indirect_original;
    native->device->device_info.conditional_rendering_features.conditionalRendering = enabled;
    CHECK(vkdu_command_close(command)); vkdu_object_destroy(command); vkdu_object_destroy(allocator);
    puts("PASS same predication GPU readbacks with counted actual EXT and counted forced-indirect routes");
}

static PFN_vkBeginCommandBuffer predication_begin_original;
static unsigned predication_begin_failures;
static VkResult VKAPI_CALL predication_begin_fail(VkCommandBuffer command, const VkCommandBufferBeginInfo *info)
{
    ++predication_begin_failures;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
static void check_predication_lifetime(vkdu_device *device)
{
    unsigned round;
    for (round = 0; round < 3; ++round) {
        vkdu_object *predicate, *allocator, *command, *queue, *gate, *done;
        uint32_t destroyed = 0; uint64_t *mapped, completed;
        struct d3d12_command_list *native;
        CHECK(vkdu_buffer_create(device, 32, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &predicate));
        CHECK(vkdu_buffer_map(predicate, 0, 0, (void **)&mapped)); mapped[0] = 1;
        CHECK(vkdu_buffer_unmap(predicate, 0, 8)); CHECK(vkdu_test_destroy_counter(predicate, &destroyed));
        CHECK(vkdu_allocator_create(device, 0, &allocator)); CHECK(vkdu_command_create(device, allocator, 0, &command));
        CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_fence_create(device, 0, &gate)); CHECK(vkdu_fence_create(device, 0, &done));
        native = vkdu_test_command_iface(command);
#if !defined(VKDU_TEST_PREDICATION_DROP)
        if (!round) {
            const VkQueueFlags flags = native->allocator->primary_pool.vk_queue_flags;
            native->allocator->primary_pool.vk_queue_flags = VK_QUEUE_TRANSFER_BIT;
            if (vkdu_command_predication(command, predicate, 0, 0) != E_NOTIMPL) { fputs("FAIL pure transfer admission\n", stderr); exit(1); }
            native->allocator->primary_pool.vk_queue_flags = flags;
            predication_begin_original = native->device->vk_procs.vkBeginCommandBuffer;
            native->device->vk_procs.vkBeginCommandBuffer = predication_begin_fail;
            const HRESULT hr = vkdu_command_predication(command, predicate, 0, 0);
            native->device->vk_procs.vkBeginCommandBuffer = predication_begin_original;
            if (hr != E_OUTOFMEMORY || predication_begin_failures != 1 || native->predication.va ||
                    native->cmd.vk_post_indirect_barrier_commands ||
                    native->cmd.iterations[native->cmd.iteration_count - 1].vk_post_indirect_barrier_commands) {
                fputs("FAIL predicate Begin failure rollback/error/handle reset\n", stderr); exit(1);
            }
        }
#endif
        CHECK(vkdu_command_predication(command, predicate, 0, 0));
        vkdu_object_destroy(predicate);
        if (vkdu_test_counter_load(&destroyed)) { fputs("FAIL predicate-only owner retired during recording\n", stderr); exit(1); }
        CHECK(vkdu_command_close(command));
        if (round == 1) CHECK(vkdu_allocator_reset(allocator));
        if (round == 2) {
            CHECK(vkdu_queue_wait(queue, gate, 1)); CHECK(vkdu_queue_execute(queue, 1, &command));
            CHECK(vkdu_queue_signal(queue, done, 1)); CHECK(vkdu_queue_drain_enqueue(queue));
            CHECK(vkdu_fence_completed(done, &completed));
            if (completed) { fputs("FAIL predicate pending gate\n", stderr); exit(1); }
        }
        vkdu_object_destroy(command); vkdu_object_destroy(allocator);
        if (round == 2) {
            if (vkdu_test_counter_load(&destroyed)) { fputs("FAIL predicate owner retired while GPU pending\n", stderr); exit(1); }
            CHECK(vkdu_fence_signal_cpu(gate, 1)); CHECK(vkdu_fence_wait(done, 1, 10000));
        }
        vkdu_object_destroy(queue); vkdu_object_destroy(gate); vkdu_object_destroy(done);
        if (vkdu_test_counter_load(&destroyed) != 1) { fputs("FAIL predicate owner not retired exactly once\n", stderr); exit(1); }
    }
    puts("PASS predicate-only owner: cancellation/reset/GPU-pending lifetimes; actual vkBeginCommandBuffer OOM rollback/retry and pure-transfer rejection");
}
#endif
