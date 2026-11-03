/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_ENABLE_TEST_DEVICE
static PFN_vkCmdBeginConditionalRenderingEXT attachment_begin_original;
static PFN_vkCmdClearAttachments attachment_clear_original;
static unsigned attachment_begin_count, attachment_clear_count;
static void VKAPI_CALL attachment_begin_counted(VkCommandBuffer command, const VkConditionalRenderingBeginInfoEXT *info)
{
    ++attachment_begin_count;
#ifdef VKDU_TEST_ATTACHMENT_INVERT
    VkConditionalRenderingBeginInfoEXT broken = *info;
    broken.flags ^= VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT;
    info = &broken;
#endif
    attachment_begin_original(command, info);
}
static void VKAPI_CALL attachment_clear_counted(VkCommandBuffer command, uint32_t count,
        const VkClearAttachment *attachments, uint32_t rect_count, const VkClearRect *rects)
{
    ++attachment_clear_count;
    attachment_clear_original(command, count, attachments, rect_count, rects);
}
static uint32_t attachment_color_word(uint32_t format, float value)
{
    float channels[4] = {value, 0.0f, 1.0f - value, 1.0f};
    uint32_t c[4], i;
    for (i = 0; i < 4; ++i) {
        float v = channels[i];
        if (i < 3 && (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
            v = test_linear_to_srgb(v);
        c[i] = (uint32_t)floorf(v * 255.0f + 0.5f);
    }
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        i = c[0]; c[0] = c[2]; c[2] = i;
    }
    return c[0] | (c[1] << 8) | (c[2] << 16) | (c[3] << 24);
}
static void check_predicated_attachments(vkdu_device *device)
{
    const uint32_t formats[] = {28, 29, 87, 91, 40, 55};
    const uint64_t predicates[] = {0, 1, UINT64_C(1) << 32, UINT64_MAX};
    const struct vkdu_rect patch[] = {{1, 1, 4, 3}, {4, 3, 9, 9}, {2, 2, 2, 4}};
    vkdu_object *allocator, *command, *queue, *fence, *upload, *readback, *query;
    struct d3d12_command_list *native;
    uint32_t f, round, i, *mapped; uint64_t serial = 0;
    CHECK(vkdu_allocator_create(device, 0, &allocator)); CHECK(vkdu_command_create(device, allocator, 0, &command));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_buffer_create(device, 32, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
    CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped)); memcpy(mapped, predicates, sizeof(predicates));
    CHECK(vkdu_buffer_unmap(upload, 0, sizeof(predicates)));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_query_heap_create(device, D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1, &query));
    native = vkdu_test_command_iface(command);
    if (!native->device->device_info.conditional_rendering_features.conditionalRendering ||
            !native->device->vk_procs.vkCmdBeginConditionalRenderingEXT) {
        fputs("FAIL attachment fixture requires real enabled EXT conditional rendering\n", stderr); exit(1);
    }
    attachment_begin_original = native->device->vk_procs.vkCmdBeginConditionalRenderingEXT;
    attachment_clear_original = native->device->vk_procs.vkCmdClearAttachments;
    attachment_begin_count = attachment_clear_count = 0;
    native->device->vk_procs.vkCmdBeginConditionalRenderingEXT = attachment_begin_counted;
    native->device->vk_procs.vkCmdClearAttachments = attachment_clear_counted;
    for (f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f) for (round = 0; round < 8; ++round) {
        const uint32_t format = formats[f], depth = format == 40 || format == 55;
        const uint32_t bpp = format == 55 ? 2 : 4;
        const uint32_t state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
        const uint32_t operation = round / 4;
        const int executes = operation ? predicates[round % 4] == 0 : predicates[round % 4] != 0;
        const float initial[4] = {0.25f, 0, 0.75f, 1}, full[4] = {0.5f, 0, 0.5f, 1}, partial[4] = {0.75f, 0, 0.25f, 1};
        vkdu_object *texture, *heap;
        uint32_t expected[1024], x, y;
        if (serial) { CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator)); }
        if (depth) CHECK(vkdu_depth_create(device, 5, 4, format, &texture));
        else CHECK(vkdu_render_target_create_format(device, 5, 4, format, &texture));
        CHECK(vkdu_heap_create(device, depth ? 3 : 2, 1, 0, &heap));
        if (depth) CHECK(vkdu_texture2d_dsv(heap, 0, texture, 0));
        else CHECK(vkdu_texture2d_rtv(heap, 0, texture));
        CHECK(vkdu_buffer_map(readback, 0, 0, (void **)&mapped));
        for (i = 0; i < 1024; ++i) mapped[i] = expected[i] = 0x91a2b3c4;
        CHECK(vkdu_buffer_unmap(readback, 0, 4096));
        if (depth) CHECK(vkdu_command_clear_dsv(command, heap, 0, 1, 0.25f, 0, 0, NULL));
        else CHECK(vkdu_command_clear_rtv(command, heap, 0, initial, 0, NULL));
        CHECK(vkdu_command_predication(command, upload, 8 * (round % 4), operation));
        CHECK(vkdu_command_query(command, query, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 1));
        if (depth) {
            CHECK(vkdu_command_clear_dsv(command, heap, 0, 1, 0.5f, 0, 0, NULL));
            CHECK(vkdu_command_clear_dsv(command, heap, 0, 1, 0.75f, 0, 3, patch));
        } else {
            CHECK(vkdu_command_clear_rtv(command, heap, 0, full, 0, NULL));
            CHECK(vkdu_command_clear_rtv(command, heap, 0, partial, 3, patch));
        }
        CHECK(vkdu_command_query(command, query, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 0));
#ifdef VKDU_TEST_ATTACHMENT_QUERY_PREDICATED
        /* Reproduce the missing internal conditional suspension. The real
         * Vulkan predicate remains active while the state helper sees false. */
        native->predication.current_enabled = false;
#endif
        CHECK(vkdu_command_query_resolve(command, query, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 1, readback, 2048));
#ifdef VKDU_TEST_ATTACHMENT_QUERY_PREDICATED
        native->predication.current_enabled = true;
#endif
        CHECK(vkdu_command_predication(command, NULL, 0, 0));
        CHECK(vkdu_command_transition(command, texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE));
        const struct vkdu_texture_copy_location src = {texture}, dst = {readback, 512, 1, format, 5, 4, 1, 256};
        CHECK(vkdu_command_texture_copy(command, &dst, 0, 0, 0, &src, NULL));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, ++serial)); CHECK(vkdu_fence_wait(fence, serial, 10000));
        expected[512] = expected[513] = 0; /* Clears are not occluding geometry. */
        for (y = 0; y < 4; ++y) for (x = 0; x < 5; ++x) {
            const int in_patch = (x >= 1 && x < 4 && y >= 1 && y < 3) || (x == 4 && y == 3);
            float value = !executes ? 0.25f : in_patch ? 0.75f : 0.5f;
            uint8_t *at = (uint8_t *)expected + 512 + y * 256 + x * bpp;
            if (format == 55) { uint16_t v = (uint16_t)floorf(value * 65535 + 0.5f); memcpy(at, &v, 2); }
            else if (depth) memcpy(at, &value, 4);
            else { uint32_t v = attachment_color_word(format, value); memcpy(at, &v, 4); }
        }
        CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 1024; ++i) if (mapped[i] != expected[i]) {
            fprintf(stderr, "FAIL predicated attachment format%u round%u word%u=%08x expected%08x\n", format, round, i, mapped[i], expected[i]); exit(1);
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        vkdu_object_destroy(heap); vkdu_object_destroy(texture);
    }
    native->device->vk_procs.vkCmdBeginConditionalRenderingEXT = attachment_begin_original;
    native->device->vk_procs.vkCmdClearAttachments = attachment_clear_original;
    if (attachment_begin_count < 48 || attachment_clear_count < 96) {
        fputs("FAIL attachment readback bypassed real EXT/clear commands\n", stderr); exit(1);
    }
    printf("PASS actual predicated RTV/DSV:48x1024 words, RGBA/BGRA linear/sRGB,D32/D16,64-bit both predicates, full/clipped/empty rectangles, preserved prior clear and zero occlusion; EXT begins=%u attachment clears=%u\n", attachment_begin_count, attachment_clear_count);
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(upload); vkdu_object_destroy(readback); vkdu_object_destroy(query);
}

static void check_attachment_lifetime(vkdu_device *device)
{
    unsigned depth, mode;
    for (depth = 0; depth < 2; ++depth) for (mode = 0; mode < 3; ++mode) {
        vkdu_object *texture, *replacement, *heap, *allocator, *command, *queue, *gate, *done, *predicate;
        uint32_t destroyed = 0; uint64_t *mapped, completed;
        const float color[4] = {0.25f, 0, 0.75f, 1};
        CHECK(vkdu_allocator_create(device, 0, &allocator)); CHECK(vkdu_command_create(device, allocator, 0, &command));
        CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_fence_create(device, 0, &gate)); CHECK(vkdu_fence_create(device, 0, &done));
        CHECK(vkdu_buffer_create(device, 8, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &predicate));
        CHECK(vkdu_buffer_map(predicate, 0, 0, (void **)&mapped)); *mapped = UINT64_C(1) << 32;
        CHECK(vkdu_buffer_unmap(predicate, 0, 8));
        if (depth) { CHECK(vkdu_depth_create(device, 5, 4, 40, &texture)); CHECK(vkdu_depth_create(device, 5, 4, 40, &replacement)); }
        else { CHECK(vkdu_render_target_create(device, 5, 4, &texture)); CHECK(vkdu_render_target_create(device, 5, 4, &replacement)); }
        CHECK(vkdu_test_destroy_counter(texture, &destroyed));
        CHECK(vkdu_heap_create(device, depth ? 3 : 2, 1, 0, &heap));
        if (depth) CHECK(vkdu_texture2d_dsv(heap, 0, texture, 0)); else CHECK(vkdu_texture2d_rtv(heap, 0, texture));
        if (mode) CHECK(vkdu_command_predication(command, predicate, 0, 0));
        if (depth) CHECK(vkdu_command_clear_dsv(command, heap, 0, 1, 0.25f, 0, 0, NULL));
        else CHECK(vkdu_command_clear_rtv(command, heap, 0, color, 0, NULL));
        if (depth) CHECK(vkdu_texture2d_dsv(heap, 0, replacement, 0)); else CHECK(vkdu_texture2d_rtv(heap, 0, replacement));
        vkdu_object_destroy(heap); vkdu_object_destroy(texture); vkdu_object_destroy(replacement);
        if (vkdu_test_counter_load(&destroyed)) { fputs("FAIL clear-only attachment retired before recording\n", stderr); exit(1); }
        if (!mode) { vkdu_object_destroy(command); vkdu_object_destroy(allocator); }
        else if (mode == 1) {
            CHECK(vkdu_command_close(command)); CHECK(vkdu_allocator_reset(allocator));
            if (vkdu_test_counter_load(&destroyed) != 1) { fputs("FAIL attachment not released at legal reset\n", stderr); exit(1); }
            vkdu_object_destroy(command); vkdu_object_destroy(allocator);
        } else {
            CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_wait(queue, gate, 1));
            CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, done, 1));
            vkdu_object_destroy(command); vkdu_object_destroy(allocator);
            CHECK(vkdu_fence_completed(done, &completed));
            if (completed || vkdu_test_counter_load(&destroyed)) { fputs("FAIL attachment retired during gated GPU execution\n", stderr); exit(1); }
            CHECK(vkdu_fence_signal_cpu(gate, 1)); CHECK(vkdu_fence_wait(done, 1, 10000));
            CHECK(vkdu_queue_signal(queue, done, 2)); CHECK(vkdu_fence_wait(done, 2, 10000));
        }
        if (vkdu_test_counter_load(&destroyed) != 1) { fputs("FAIL clear attachment exact retirement\n", stderr); exit(1); }
        vkdu_object_destroy(queue); vkdu_object_destroy(gate); vkdu_object_destroy(done); vkdu_object_destroy(predicate);
    }
    puts("PASS actual clear-only RTV/DSV ownership: descriptor replacement/destruction, canceled recording, legal reset and gated GPU completion");
}
#endif
