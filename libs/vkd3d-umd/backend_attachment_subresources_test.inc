/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_ENABLE_TEST_DEVICE
static void check_attachment_subresources(vkdu_device *device)
{
    const uint32_t formats[] = {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_TYPELESS,
        DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM};
    const uint64_t predicates[] = {0, UINT64_C(1) << 32};
    const struct vkdu_rect patches[] = {{1, 1, 4, 3}, {3, 2, 9, 7}, {2, 2, 2, 3}};
    const struct vkdu_signature vin = {6, 0, 1, 1}, vout = {1, 0, 15, 3}, pout = {0, 0, 15, 3};
    const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc), *p = graphics_tokens(ps_flat_color_code_dxbc);
    struct d3d12_device *native_device = NULL;
    void *vs = NULL, *ps = NULL; size_t vs_size = 0, ps_size = 0;
    uint32_t f, round;
    CHECK(vkdu_shader_dxbc(1, v, v[1], &vin, 1, &vout, 1, &vs, &vs_size));
    CHECK(vkdu_shader_dxbc(0, p, p[1], NULL, 0, &pout, 1, &ps, &ps_size));
    attachment_begin_count = attachment_clear_count = 0;
    for (f = 0; f < 4; ++f) for (round = 0; round < 4; ++round) {
        const uint32_t depth = f >= 2, format = formats[f], typed = f == 0 ? 28 : f == 1 ? 87 : format;
        const uint32_t bpp = format == DXGI_FORMAT_D16_UNORM ? 2 : 4;
        const uint32_t state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
        const uint32_t op = round / 2, executes = op ? predicates[round % 2] == 0 : predicates[round % 2] != 0;
        struct vkdu_texture2d_shape shape = {9, 7, format, 4, 3, depth ? 2 : 1, 1, 0};
        struct vkdu_graphics_desc desc = {depth ? 0 : typed, UINT32_MAX, 3, 1, 0, 1, 15};
        const struct vkdu_viewport viewport = {0, 0, 2, 1, 0, 1};
        const struct vkdu_rect draw_rect = {0, 0, 2, 1};
        vkdu_object *image, *memory = NULL, *heap, *root, *pipeline, *allocator, *command, *queue, *fence, *readback, *predicate, *bad = NULL;
        uint32_t expected[8192], *mapped, sub, i, x, y, destroyed = 0;
        uint64_t bytes, align;
        CHECK(vkdu_texture2d_shape_allocation(device, &shape, &bytes, &align));
        if (!bytes || align != 65536) exit(1);
        if (round & 1) {
            CHECK(vkdu_test_texture_heap_create(device, bytes * 2, &memory));
            REJECT(vkdu_texture2d_shape_place(device, memory, bytes, &shape, state, &bad));
            vkdu_object_destroy(memory);
            CHECK(vkdu_test_attachment_heap_create(device, bytes * 2, &memory));
            REJECT(vkdu_texture2d_shape_place(device, memory, 1, &shape, state, &bad));
            REJECT(vkdu_texture2d_shape_place(device, memory, bytes * 2, &shape, state, &bad));
            CHECK(vkdu_texture2d_shape_place(device, memory, bytes, &shape, state, &image));
            vkdu_object_destroy(memory);
        } else CHECK(vkdu_texture2d_shape_create(device, &shape, &image));
        CHECK(vkdu_test_destroy_counter(image, &destroyed));
        CHECK(vkdu_heap_create(device, depth ? 3 : 2, 13, 0, &heap));
        for (sub = 0; sub < 12; ++sub) {
            if (depth) CHECK(vkdu_texture2d_dsv_range(heap, sub, image, 0, sub % 4, sub / 4, 1));
            else CHECK(vkdu_texture2d_rtv_range(heap, sub, image, typed, sub % 4, sub / 4, 1));
        }
        if (depth) {
            REJECT(vkdu_texture2d_dsv_range(heap, 12, image, 0, 4, 1, 2));
            REJECT(vkdu_texture2d_dsv_range(heap, 12, image, 0, 1, 2, 2));
            REJECT(vkdu_texture2d_dsv_range(heap, 12, image, 0, 1, 1, 0));
            CHECK(vkdu_texture2d_dsv_range(heap, 12, image, 0, 1, 1, 2));
        } else {
            REJECT(vkdu_texture2d_rtv_range(heap, 12, image, typed, 4, 1, 2));
            REJECT(vkdu_texture2d_rtv_range(heap, 12, image, typed, 1, 2, 2));
            REJECT(vkdu_texture2d_rtv_range(heap, 12, image, typed, 1, 1, 0));
            CHECK(vkdu_texture2d_rtv_range(heap, 12, image, typed, 1, 1, 2));
        }
        desc.depth_format = depth ? format : 0;
        desc.depth_enable = desc.depth_write = depth;
        desc.depth_func = D3D12_COMPARISON_FUNC_LESS;
        CHECK(vkdu_root_create(device, NULL, 0, 0, &root));
        CHECK(vkdu_graphics_pipeline_create(device, root, vs, vs_size, depth ? NULL : ps,
            depth ? 0 : ps_size, &desc, &pipeline));
        CHECK(vkdu_allocator_create(device, 0, &allocator)); CHECK(vkdu_command_create(device, allocator, 0, &command));
        CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_fence_create(device, 0, &fence));
        if (!native_device) {
            native_device = ((struct d3d12_command_list *)vkdu_test_command_iface(command))->device;
            if (!native_device->device_info.conditional_rendering_features.conditionalRendering ||
                    !native_device->vk_procs.vkCmdBeginConditionalRenderingEXT) exit(1);
            attachment_begin_original = native_device->vk_procs.vkCmdBeginConditionalRenderingEXT;
            attachment_clear_original = native_device->vk_procs.vkCmdClearAttachments;
            native_device->vk_procs.vkCmdBeginConditionalRenderingEXT = attachment_begin_counted;
            native_device->vk_procs.vkCmdClearAttachments = attachment_clear_counted;
        }
        CHECK(vkdu_buffer_create(device, 32768, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
        CHECK(vkdu_buffer_map(readback, 0, 0, (void **)&mapped));
        for (i = 0; i < 8192; ++i) mapped[i] = expected[i] = 0xcba98765;
        CHECK(vkdu_buffer_unmap(readback, 0, 32768));
        CHECK(vkdu_buffer_create(device, 16, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &predicate));
        CHECK(vkdu_buffer_map(predicate, 0, 0, (void **)&mapped)); memcpy(mapped, predicates, sizeof(predicates));
        CHECK(vkdu_buffer_unmap(predicate, 0, sizeof(predicates)));
        for (sub = 0; sub < 12; ++sub) {
            float value = (sub + 1) / 16.0f, color[4] = {value, 0, 1.0f - value, 1};
            if (depth) CHECK(vkdu_command_clear_dsv(command, heap, sub, 1, value, 0, 0, NULL));
            else CHECK(vkdu_command_clear_rtv(command, heap, sub, color, 0, NULL));
        }
        /* Draw only mip2/layer0 (two pixels); the later two-layer clear uses
         * mip1/layers1..2. Every other mip/layer remains a distinct sentinel. */
        CHECK(vkdu_command_targets(command, depth ? NULL : heap, depth ? 0 : 2, depth ? heap : NULL, depth ? 2 : 0));
        CHECK(vkdu_command_graphics_root(command, root)); CHECK(vkdu_command_pipeline(command, pipeline));
        CHECK(vkdu_command_topology(command, 4)); CHECK(vkdu_command_viewports(command, 1, &viewport));
        CHECK(vkdu_command_scissors(command, 1, &draw_rect)); CHECK(vkdu_command_draw(command, 3, 1, 0, 0));
        CHECK(vkdu_command_predication(command, predicate, 8 * (round % 2), op));
        if (depth) CHECK(vkdu_command_clear_dsv(command, heap, 12, 1, 0.875f, 0, 3, patches));
        else {
            const float color[4] = {0.875f, 0, 0.125f, 1};
            CHECK(vkdu_command_clear_rtv(command, heap, 12, color, 3, patches));
        }
        CHECK(vkdu_command_predication(command, NULL, 0, 0));
        for (sub = 0; sub < 12; ++sub) {
            struct vkdu_texture_copy_location from = {image, sub}, to = {readback, 512 + sub * 2048, 1,
                typed, subresource_test_width(sub % 4), subresource_test_height(sub % 4), 1, 256};
            subresource_test_transition(command, image, sub, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            CHECK(vkdu_command_texture_copy(command, &to, 0, 0, 0, &from, NULL));
            for (y = 0; y < to.height; ++y) for (x = 0; x < to.width; ++x) {
                float value = sub == 2 ? 0 : executes && (sub == 5 || sub == 9) && x >= 1 && y >= 1 ? 0.875f : (sub + 1) / 16.0f;
                uint8_t *at = (uint8_t *)expected + to.offset + y * 256 + x * bpp;
                if (bpp == 2) { uint16_t word = (uint16_t)floorf(value * 65535 + 0.5f); memcpy(at, &word, 2); }
                else if (depth) memcpy(at, &value, 4);
                else { uint32_t word = sub == 2 ? UINT32_MAX : attachment_color_word(typed, value); memcpy(at, &word, 4); }
            }
        }
        CHECK(vkdu_command_close(command));
        vkdu_object_destroy(image); vkdu_object_destroy(heap);
        if (vkdu_test_counter_load(&destroyed)) { fputs("FAIL attachment retired before queued usage\n", stderr); exit(1); }
        CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, fence, 1));
        CHECK(vkdu_fence_wait(fence, 1, 10000)); CHECK(vkdu_buffer_map(readback, 0, 32768, (void **)&mapped));
        for (i = 0; i < 8192; ++i) if (mapped[i] != expected[i]) {
            fprintf(stderr, "FAIL attachment-subresources format%u round%u word%u=%08x expected%08x\n", format, round, i, mapped[i], expected[i]); exit(1);
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        vkdu_object_destroy(command); vkdu_object_destroy(allocator);
        CHECK(vkdu_queue_signal(queue, fence, 2)); CHECK(vkdu_fence_wait(fence, 2, 10000));
        if (vkdu_test_counter_load(&destroyed) != 1) { fputs("FAIL attachment owner survived final command release\n", stderr); exit(1); }
        vkdu_object_destroy(root); vkdu_object_destroy(pipeline); vkdu_object_destroy(predicate); vkdu_object_destroy(readback);
        vkdu_object_destroy(queue); vkdu_object_destroy(fence);
        printf("PASS actual attachment mip/layer format%u round%u:12 subresources/8192 words, mip2-layer0 Draw, mip1-layers1..2 clipped predicate, %s resource and early owner retirement\n",
            format, round, round & 1 ? "nonzero-offset placed" : "committed");
    }
    native_device->vk_procs.vkCmdBeginConditionalRenderingEXT = attachment_begin_original;
    native_device->vk_procs.vkCmdClearAttachments = attachment_clear_original;
    if (attachment_begin_count < 16 || attachment_clear_count < 16) exit(1);
    printf("PASS actual attachment subresources:16 rounds, explicit EXT begins=%u attachment clears=%u; RGBA/BGRA typed views,D32,D16 only\n",
        attachment_begin_count, attachment_clear_count);
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps);
}
#endif
