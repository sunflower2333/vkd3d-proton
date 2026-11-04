/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_blend_format(vkdu_device *device, uint32_t format)
{
    vkdu_object *root = NULL, *pipelines[3] = {0}, *target = NULL, *heap = NULL;
    vkdu_object *queue = NULL, *allocator = NULL, *command = NULL, *fence = NULL, *readback = NULL;
    struct vkdu_graphics_desc desc = {format, UINT32_MAX, 3, 1, 0, 1, 15};
    const struct vkdu_signature vin = {6, 0, 1, 1}, vout = {1, 0, 15, 3}, pout = {0, 0, 15, 3};
    const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc), *p = graphics_tokens(ps_flat_color_code_dxbc);
    const struct vkdu_viewport viewport = {0, 0, 4, 4, 0, 1};
    const struct vkdu_rect scissor = {1, 1, 3, 3};
    const float clear[] = {0.2f, 0.4f, 0.6f, 0.8f};
    void *vs = NULL, *ps = NULL; size_t vs_size = 0, ps_size = 0;
    uint32_t i, round, *mapped;
    CHECK(vkdu_shader_dxbc(1, v, v[1], &vin, 1, &vout, 1, &vs, &vs_size));
    CHECK(vkdu_shader_dxbc(0, p, p[1], NULL, 0, &pout, 1, &ps, &ps_size));
    CHECK(vkdu_root_create(device, NULL, 0, 0, &root));
    desc.blend_enable = 1; desc.src_blend = desc.src_blend_alpha = D3D12_BLEND_BLEND_FACTOR;
    desc.dst_blend = desc.dst_blend_alpha = D3D12_BLEND_INV_BLEND_FACTOR;
    desc.blend_op = desc.blend_op_alpha = D3D12_BLEND_OP_ADD;
    CHECK(vkdu_graphics_pipeline_create(device, root, vs, vs_size, ps, ps_size, &desc, &pipelines[0]));
    desc.src_blend = desc.dst_blend = D3D12_BLEND_ONE; desc.blend_op = D3D12_BLEND_OP_SUBTRACT;
    desc.src_blend_alpha = D3D12_BLEND_ZERO; desc.dst_blend_alpha = D3D12_BLEND_ONE;
    CHECK(vkdu_graphics_pipeline_create(device, root, vs, vs_size, ps, ps_size, &desc, &pipelines[1]));
    desc.blend_op = D3D12_BLEND_OP_MIN; desc.blend_op_alpha = D3D12_BLEND_OP_MAX; desc.write_mask = 5;
    CHECK(vkdu_graphics_pipeline_create(device, root, vs, vs_size, ps, ps_size, &desc, &pipelines[2]));
    {
        vkdu_object *invalid = NULL;
        desc.src_blend_alpha = D3D12_BLEND_SRC_COLOR;
        REJECT(vkdu_graphics_pipeline_create(device, root, vs, vs_size, ps, ps_size, &desc, &invalid));
        desc.src_blend_alpha = D3D12_BLEND_ONE; desc.blend_op = 0;
        REJECT(vkdu_graphics_pipeline_create(device, root, vs, vs_size, ps, ps_size, &desc, &invalid));
    }
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps);
    CHECK(vkdu_render_target_create_format(device, 4, 4, format, &target));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, 0, &heap));
    CHECK(vkdu_texture2d_rtv(heap, 0, target));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    for (round = 0; round < 4; ++round) {
        float factor[] = {0.25f, 0.5f, 0.75f, 0.125f}, expected[4];
        struct vkdu_texture_copy_location src = {target, 0, 0, 0, 0, 0, 0, 0};
        struct vkdu_texture_copy_location dst = {readback, 512, 1, format, 4, 4, 1, 256};
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
        }
        CHECK(vkdu_buffer_map(readback, 0, 0, (void **)&mapped));
        for (i = 0; i < 1024; ++i) mapped[i] = 0x1234abcd;
        CHECK(vkdu_buffer_unmap(readback, 0, 4096));
        CHECK(vkdu_command_clear_rtv(command, heap, 0, clear, 0, NULL));
        CHECK(vkdu_command_graphics_root(command, root)); CHECK(vkdu_command_pipeline(command, pipelines[round % 3]));
        CHECK(vkdu_command_render_target(command, heap, 0)); CHECK(vkdu_command_topology(command, 4));
        CHECK(vkdu_command_viewports(command, 1, &viewport)); CHECK(vkdu_command_scissors(command, 1, &scissor));
        CHECK(vkdu_command_blend_factor(command, factor));
        if (round == 3) {
            factor[0] = 0.75f; factor[1] = 0.25f; factor[2] = 0.5f; factor[3] = 0.25f;
            CHECK(vkdu_command_blend_factor(command, factor));
        }
        for (i = 0; i < 4; ++i) expected[i] = round % 3 == 0 ? factor[i] + (1 - factor[i]) * clear[i] :
            round == 1 && i < 3 ? 1 - clear[i] : clear[i];
        memset(factor, 0, sizeof(factor));
        REJECT(vkdu_command_blend_factor(command, NULL));
        CHECK(vkdu_command_draw(command, 3, 1, 0, 0));
        CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command, &dst, 0, 0, 0, &src, NULL));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, round + 1)); CHECK(vkdu_fence_wait(fence, round + 1, 10000));
        CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 1024; ++i) {
            if (i >= 128 && i < 128 + 4 * 64 && (i - 128) % 64 < 4) {
                uint32_t channel, x = (i - 128) % 64, y = (i - 128) / 64;
                for (channel = 0; channel < 4; ++channel) {
                    float value = x >= 1 && x < 3 && y >= 1 && y < 3 ? expected[channel] : clear[channel];
                    int want, got;
                    uint32_t byte = (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) &&
                        channel < 3 ? 2 - channel : channel;
                    if ((format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) && channel < 3)
                        value = test_linear_to_srgb(value);
                    want = (int)(value * 255 + 0.5f); got = (mapped[i] >> (byte * 8)) & 255;
                    if (abs(got - want) > 1) {
                        fprintf(stderr, "blend format%u round%u word%u channel%u=%u expected%u\n", format, round, i, channel, got, want); exit(1);
                    }
                }
            } else if (mapped[i] != 0x1234abcd) { fprintf(stderr, "blend padding word%u overwritten\n", i); exit(1); }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        printf("PASS actual blend format%u round%u dynamic factor/caller lifetime, separate alpha, subtract/min/write masks, channel order/sRGB encoding and padded readback\n", format, round);
    }
    CHECK(vkdu_device_status(device));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(readback); vkdu_object_destroy(heap); vkdu_object_destroy(target);
    for (i = 0; i < 3; ++i) vkdu_object_destroy(pipelines[i]);
    vkdu_object_destroy(root);
}
static void check_blend(vkdu_device *device)
{
    const uint32_t formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    uint32_t i;
    for (i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) check_blend_format(device, formats[i]);
}
