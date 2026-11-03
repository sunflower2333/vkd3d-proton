/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_typeless_color(vkdu_device *device)
{
    unsigned family;
    for (family = 0; family < 2; ++family) {
        const uint32_t storage = family ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
        const uint32_t formats[] = {family ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM,
            family ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
        const uint32_t incompatible = family ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        vkdu_object *target, *plain, *copied, *wrong, *rtv, *srv, *gfx_root, *gfx_pso[2], *root, *compute;
        vkdu_object *init, *output, *readback, *queue, *allocator, *command, *fence;
        const struct vkdu_signature vin = {6, 0, 1, 1}, vout = {1, 0, 15, 3}, pout = {0, 0, 15, 3};
        const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc), *p = graphics_tokens(ps_flat_color_code_dxbc);
        const struct vkdu_descriptor_range range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        const struct vkdu_root_parameter params[] = {{1, 0, 0, 0, 3}, {4, 0, 0, 0, 0}, {0, 0, 0, 0, 0, &range, 1}};
        struct vkdu_static_sampler sampler = {{0, 3, 3, 3, 0, 1, 8, {0}, 0, 1000}, 0, 0, 0, 0};
        const struct vkdu_viewport viewport = {0, 0, 4, 4, 0, 1};
        const struct vkdu_rect scissor = {1, 1, 3, 3};
        const float clear[4] = {0.2f, 0.4f, 0.6f, 0.8f}, uv[3] = {0.125f, 0.125f, 0};
        void *vs, *ps; size_t vs_bytes, ps_bytes;
        uint32_t *mapped, i, round, view, constants[3], flags, levels;
        uint64_t bytes, alignment;
        memcpy(constants, uv, sizeof(constants));
        CHECK(vkdu_format_support(device, storage, &flags));
        if (flags) { fputs("FAIL typeless storage leaked typed format support\n", stderr); exit(1); }
        CHECK(vkdu_multisample_quality(device, storage, 1, 0, &levels));
        if (levels != 1) { fputs("FAIL typeless single sample support\n", stderr); exit(1); }
        CHECK(vkdu_render_target_allocation_format(device, 4, 4, storage, &bytes, &alignment));
        if (!bytes || alignment != 65536) exit(1);
        CHECK(vkdu_texture2d_allocation(device, 4, 4, storage, &bytes, &alignment));
        CHECK(vkdu_render_target_create_format(device, 4, 4, storage, &target));
        CHECK(vkdu_texture2d_create(device, 4, 4, storage, &plain));
        CHECK(vkdu_texture2d_create(device, 4, 4, formats[0], &copied));
        CHECK(vkdu_texture2d_create(device, 4, 4, incompatible, &wrong));
        CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, 0, &rtv));
        CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, 1, &srv));
        for (view = 0; view < 2; ++view) {
            CHECK(vkdu_texture2d_rtv_format(rtv, view, target, formats[view]));
            CHECK(vkdu_texture2d_srv_format(srv, view, target, formats[view]));
            REJECT(vkdu_texture2d_rtv_format(rtv, view, target, storage));
            REJECT(vkdu_texture2d_srv_format(srv, view, target, storage));
            REJECT(vkdu_texture2d_rtv_format(rtv, view, target, incompatible));
            REJECT(vkdu_texture2d_srv_format(srv, view, target, incompatible));
        }
        REJECT(vkdu_texture2d_rtv(rtv, 0, target)); REJECT(vkdu_texture2d_srv(srv, 0, target));
        REJECT(vkdu_texture2d_rtv_format(rtv, 0, copied, formats[0]));
        REJECT(vkdu_texture2d_srv_format(srv, 0, copied, formats[1]));
        CHECK(vkdu_root_create(device, NULL, 0, 0, &gfx_root));
        CHECK(vkdu_shader_dxbc(1, v, v[1], &vin, 1, &vout, 1, &vs, &vs_bytes));
        CHECK(vkdu_shader_dxbc(0, p, p[1], NULL, 0, &pout, 1, &ps, &ps_bytes));
        for (view = 0; view < 2; ++view) {
            struct vkdu_graphics_desc desc = {formats[view], UINT32_MAX, 3, 1, 0, 1, 15};
            CHECK(vkdu_graphics_pipeline_create(device, gfx_root, vs, vs_bytes, ps, ps_bytes, &desc, &gfx_pso[view]));
        }
        vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps);
        CHECK(vkdu_root_create_samplers(device, params, 3, 0, &sampler, 1, &root));
        CHECK(vkdu_pipeline_create(device, root, sampler_rounding_code_dxbc, sizeof(sampler_rounding_code_dxbc), &compute));
        CHECK(vkdu_buffer_create(device, 32, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &init));
        CHECK(vkdu_buffer_map(init, 0, 0, (void **)&mapped));
        for (i = 0; i < 8; ++i) mapped[i] = 0x1234abcd;
        CHECK(vkdu_buffer_unmap(init, 0, 32));
        CHECK(vkdu_buffer_create(device, 32, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, &output));
        CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
        CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
        CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
        for (round = 0; round < 2; ++round) {
            struct vkdu_texture_copy_location image = {copied, 0, 0, 0, 0, 0, 0, 0};
            struct vkdu_texture_copy_location result = {readback, 512, 1, storage, 4, 4, 1, 256};
            if (round) {
                CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
                CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
                CHECK(vkdu_command_transition(command, copied, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
                CHECK(vkdu_command_transition(command, plain, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
                CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
            }
            CHECK(vkdu_buffer_map(readback, 0, 0, (void **)&mapped));
            for (i = 0; i < 1024; ++i) mapped[i] = 0x1234abcd;
            CHECK(vkdu_buffer_unmap(readback, 0, 4096));
            CHECK(vkdu_command_clear_rtv(command, rtv, round, clear, 0, NULL));
            CHECK(vkdu_command_graphics_root(command, gfx_root)); CHECK(vkdu_command_pipeline(command, gfx_pso[round]));
            CHECK(vkdu_command_render_target(command, rtv, round)); CHECK(vkdu_command_topology(command, 4));
            CHECK(vkdu_command_viewports(command, 1, &viewport)); CHECK(vkdu_command_scissors(command, 1, &scissor));
            CHECK(vkdu_command_draw(command, 3, 1, 0, 0));
            CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
            CHECK(vkdu_command_resource_copy(command, plain, target));
            CHECK(vkdu_command_transition(command, plain, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
            CHECK(vkdu_command_resource_copy(command, copied, plain));
            REJECT(vkdu_command_resource_copy(command, copied, wrong));
            CHECK(vkdu_command_transition(command, copied, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
            CHECK(vkdu_command_texture_copy(command, &result, 0, 0, 0, &image, NULL));
            result.format = incompatible;
            REJECT(vkdu_command_texture_copy(command, &result, 0, 0, 0, &image, NULL));
            CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            CHECK(vkdu_command_copy(command, output, 0, init, 0, 32));
            CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            CHECK(vkdu_command_root(command, root)); CHECK(vkdu_command_pipeline(command, compute));
            CHECK(vkdu_command_constants(command, 0, 0, 3, constants)); CHECK(vkdu_command_heaps(command, 1, &srv));
            for (view = 0; view < 2; ++view) {
                CHECK(vkdu_command_uav(command, 1, output, view * 16));
                CHECK(vkdu_command_table(command, 2, srv, view)); CHECK(vkdu_command_dispatch(command, 1, 1, 1));
            }
            CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
            CHECK(vkdu_command_copy(command, readback, 3072, output, 0, 32));
            CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
            CHECK(vkdu_queue_signal(queue, fence, round + 1)); CHECK(vkdu_fence_wait(fence, round + 1, 10000));
            CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
            for (i = 0; i < 1024; ++i) {
                if (i >= 128 && i < 384 && (i - 128) % 64 < 4) {
                    const unsigned x = (i - 128) % 64, y = (i - 128) / 64;
                    unsigned channel;
                    for (channel = 0; channel < 4; ++channel) {
                        float value = x >= 1 && x < 3 && y >= 1 && y < 3 ? 1.0f : clear[channel];
                        const unsigned byte = family && channel < 3 ? 2 - channel : channel;
                        int actual = (mapped[i] >> (byte * 8)) & 255;
                        if (round && channel < 3) value = test_linear_to_srgb(value);
                        if (abs(actual - (int)(value * 255 + 0.5f)) > 1) {
                            fprintf(stderr, "FAIL typeless%u RTV%u pixel%u channel%u=%d linear%g\n", storage, formats[round], i, channel, actual, value); exit(1);
                        }
                    }
                } else if (i == 768 || i == 772) {
                    float actual, code = round ? test_linear_to_srgb(clear[0]) : clear[0];
                    memcpy(&actual, &mapped[i], sizeof(actual));
                    code = (float)(int)(code * 255 + 0.5f);
                    if (!isfinite(actual) || actual < 0 || actual > 1 ||
                            fabsf((i == 772 ? test_linear_to_srgb(actual) : actual) * 255 - code) > 0.5f) {
                        fprintf(stderr, "FAIL typeless%u RTV%u SRV%u sample=%g expected code%g\n", storage, formats[round], formats[i == 772], actual, code); exit(1);
                    }
                } else if (mapped[i] != 0x1234abcd) { fputs("FAIL typeless copy/sample padding\n", stderr); exit(1); }
            }
            CHECK(vkdu_buffer_unmap(readback, 0, 0));
            printf("PASS typeless%u RTV%u clear/draw/raw family copy, both UNORM and sRGB views of same storage:1024 words including sample/padding\n", storage, formats[round]);
        }
        CHECK(vkdu_device_status(device));
        vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
        vkdu_object_destroy(root); vkdu_object_destroy(compute); vkdu_object_destroy(gfx_root);
        vkdu_object_destroy(gfx_pso[0]); vkdu_object_destroy(gfx_pso[1]); vkdu_object_destroy(rtv); vkdu_object_destroy(srv);
        vkdu_object_destroy(target); vkdu_object_destroy(plain); vkdu_object_destroy(copied); vkdu_object_destroy(wrong);
        vkdu_object_destroy(init); vkdu_object_destroy(output); vkdu_object_destroy(readback);
    }
}
