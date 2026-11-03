/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_depth_format(vkdu_device *device, vkdu_device *other, uint32_t format)
{
    vkdu_object *root = NULL, *pipeline[4] = {0}, *depth = NULL, *color = NULL, *dsv = NULL, *rtv = NULL, *foreign = NULL;
    vkdu_object *queue = NULL, *allocator = NULL, *command = NULL, *fence = NULL, *readback = NULL;
    vkdu_object *bounded = NULL, *short_buffer = NULL;
    const uint32_t width = format == DXGI_FORMAT_D16_UNORM ? 5 : 4;
    const uint32_t pixel_bytes = format == DXGI_FORMAT_D16_UNORM ? 2 : 4;
    struct vkdu_graphics_desc desc = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT32_MAX, 3, 1, 0, 1, 15};
    const struct vkdu_signature vin = {6, 0, 1, 1}, vout = {1, 0, 15, 3}, pout = {0, 0, 15, 3};
    const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc), *p = graphics_tokens(ps_flat_color_code_dxbc);
    const struct vkdu_viewport viewport = {0, 0, width, 4, 0, 1};
    const struct vkdu_rect scissor = {1, 1, 3, 3}, patch = {0, 0, 1, 1};
    void *vs = NULL, *ps = NULL; size_t vs_size = 0, ps_size = 0;
    uint32_t *mapped, i, round; uint64_t bytes, alignment;
    CHECK(vkdu_shader_dxbc(1, v, v[1], &vin, 1, &vout, 1, &vs, &vs_size));
    CHECK(vkdu_shader_dxbc(0, p, p[1], NULL, 0, &pout, 1, &ps, &ps_size));
    CHECK(vkdu_root_create(device, NULL, 0, 0, &root));
    for (round = 0; round < 4; ++round) {
        desc.depth_format = format; desc.depth_enable = round != 3;
        desc.depth_write = round < 2; desc.depth_func = round == 1 ? D3D12_COMPARISON_FUNC_GREATER : D3D12_COMPARISON_FUNC_LESS;
        CHECK(vkdu_graphics_pipeline_create(device, root, vs, vs_size, ps, ps_size, &desc, &pipeline[round]));
    }
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps);
    CHECK(vkdu_depth_allocation(device, width, 4, format, &bytes, &alignment));
    if (!bytes || alignment != 65536) exit(1);
    CHECK(vkdu_depth_create(device, width, 4, format, &depth)); CHECK(vkdu_render_target_create(device, width, 4, &color));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 4, 0, &dsv));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, 0, &rtv));
    CHECK(vkdu_heap_create(other, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 4, 0, &foreign));
    CHECK(vkdu_texture2d_dsv(dsv, 2, depth, 0)); CHECK(vkdu_texture2d_rtv(rtv, 1, color));
    REJECT(vkdu_texture2d_dsv(foreign, 2, depth, 0)); REJECT(vkdu_texture2d_dsv(dsv, 2, color, 0));
    REJECT(vkdu_texture2d_dsv(dsv, 2, depth, D3D12_DSV_FLAG_READ_ONLY_STENCIL));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_buffer_create(device, 512 + 3 * 256 + width * pixel_bytes, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &bounded));
    CHECK(vkdu_buffer_create(device, 512 + 3 * 256 + width * pixel_bytes - 1, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &short_buffer));
    for (round = 0; round < 4; ++round) {
        const float red[] = {1, 0, 0, 1};
        struct vkdu_texture_copy_location src_depth = {depth, 0, 0, 0, 0, 0, 0, 0};
        struct vkdu_texture_copy_location dst_depth = {readback, 512, 1, format, width, 4, 1, 256};
        struct vkdu_texture_copy_location src_color = {color, 0, 0, 0, 0, 0, 0, 0};
        struct vkdu_texture_copy_location dst_color = {readback, 2048, 1, DXGI_FORMAT_R8G8B8A8_UNORM, width, 4, 1, 256};
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE));
            CHECK(vkdu_command_transition(command, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
        }
        CHECK(vkdu_buffer_map(readback, 0, 0, (void **)&mapped));
        for (i = 0; i < 1024; ++i) mapped[i] = 0x76543210;
        CHECK(vkdu_buffer_unmap(readback, 0, 4096));
        CHECK(vkdu_command_clear_dsv(command, dsv, 2, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, NULL));
        CHECK(vkdu_command_clear_dsv(command, dsv, 2, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 1, &patch));
        REJECT(vkdu_command_clear_dsv(command, dsv, 2, D3D12_CLEAR_FLAG_STENCIL, 0.2f, 0, 0, NULL));
        REJECT(vkdu_command_clear_dsv(command, dsv, 2, D3D12_CLEAR_FLAG_DEPTH, 1.1f, 0, 0, NULL));
        CHECK(vkdu_command_clear_rtv(command, rtv, 1, red, 0, NULL));
        CHECK(vkdu_command_targets(command, rtv, 1, dsv, 2));
        REJECT(vkdu_command_targets(command, rtv, 1, foreign, 2));
        CHECK(vkdu_command_graphics_root(command, root)); CHECK(vkdu_command_pipeline(command, pipeline[round]));
        CHECK(vkdu_command_topology(command, 4)); CHECK(vkdu_command_viewports(command, 1, &viewport));
        CHECK(vkdu_command_scissors(command, 1, &scissor)); CHECK(vkdu_command_draw(command, 3, 1, 0, 0));
        CHECK(vkdu_command_transition(command, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_transition(command, color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
        {
            struct vkdu_box partial = {0, 0, 0, 1, 1, 1};
            REJECT(vkdu_command_texture_copy(command, &dst_depth, 0, 0, 0, &src_depth, &partial));
            REJECT(vkdu_command_texture_copy(command, &dst_depth, 1, 0, 0, &src_depth, NULL));
        }
        CHECK(vkdu_command_texture_copy(command, &dst_depth, 0, 0, 0, &src_depth, NULL));
        dst_depth.resource = short_buffer;
        REJECT(vkdu_command_texture_copy(command, &dst_depth, 0, 0, 0, &src_depth, NULL));
        dst_depth.resource = bounded;
        CHECK(vkdu_command_texture_copy(command, &dst_depth, 0, 0, 0, &src_depth, NULL));
        CHECK(vkdu_command_texture_copy(command, &dst_color, 0, 0, 0, &src_color, NULL));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, round + 1)); CHECK(vkdu_fence_wait(fence, round + 1, 10000));
        CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        {
            uint32_t expected[1024], x, y;
            for (i = 0; i < 1024; ++i) expected[i] = 0x76543210;
            for (y = 0; y < 4; ++y) for (x = 0; x < width; ++x) {
                float z = x == 0 && y == 0 ? 0.75f : !round && x >= 1 && x < 3 && y >= 1 && y < 3 ? 0 : 0.5f;
                uint8_t *destination = (uint8_t *)expected + 512 + y * 256 + x * pixel_bytes;
                if (pixel_bytes == 2) {
                    uint16_t value = (uint16_t)(z * 65535 + 0.5f); memcpy(destination, &value, 2);
                } else memcpy(destination, &z, 4);
                expected[512 + y * 64 + x] = round != 1 && x >= 1 && x < 3 && y >= 1 && y < 3 ? UINT32_MAX : 0xff0000ff;
            }
            for (i = 0; i < 1024; ++i) if (mapped[i] != expected[i]) {
                fprintf(stderr, "depth format%u round%u word%u=%08x expected%08x\n", format, round, i, mapped[i], expected[i]); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        printf("PASS actual depth format%u round%u clear/rect, LESS/GREATER, write-mask/disabled, exact footprint bounds and color/depth readback sentinels\n", format, round);
    }
    CHECK(vkdu_device_status(device));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(readback); vkdu_object_destroy(dsv); vkdu_object_destroy(rtv); vkdu_object_destroy(foreign);
    vkdu_object_destroy(depth); vkdu_object_destroy(color);
    vkdu_object_destroy(bounded); vkdu_object_destroy(short_buffer);
    for (i = 0; i < 4; ++i) vkdu_object_destroy(pipeline[i]);
    vkdu_object_destroy(root);
}
static void check_depth(vkdu_device *device, vkdu_device *other)
{
    check_depth_format(device, other, DXGI_FORMAT_D32_FLOAT);
    check_depth_format(device, other, DXGI_FORMAT_D16_UNORM);
}
