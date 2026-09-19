/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../tests/shaders/command/headers/unaligned_vertex_stride_vs.h"
#include "../../tests/shaders/command/headers/unaligned_vertex_stride_ps.h"

static void check_vertex_inputs(vkdu_device *device, vkdu_device *foreign)
{
    vkdu_object *root, *pipeline, *target, *rtv, *queue, *allocator, *command, *fence;
    vkdu_object *vertices, *colors, *indices, *readback, *other;
    const struct vkdu_signature inputs[] = {{0, 0, 15, 3}, {0, 1, 15, 3}};
    const struct vkdu_signature outputs[] = {{1, 0, 15, 3}, {0, 1, 15, 3}};
    const struct vkdu_signature pixel_output = {0, 0, 15, 3};
    struct vkdu_input_element elements[] = {{5, 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, 0},
        {7, UINT32_MAX, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 2, 1}};
    struct vkdu_graphics_desc state = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT32_MAX, 3, 1, 0, 1, 15, 2, elements};
    const uint32_t *vt = graphics_tokens(unaligned_vertex_stride_vs_code_dxbc);
    const uint32_t *pt = graphics_tokens(unaligned_vertex_stride_ps_code_dxbc);
    void *vs, *ps, *mapped; size_t vs_size, ps_size;
    uint32_t round, i;
    CHECK(vkdu_shader_dxbc(1, vt, vt[1], inputs, 2, outputs, 2, &vs, &vs_size));
    CHECK(vkdu_shader_dxbc(0, pt, pt[1], outputs, 2, &pixel_output, 1, &ps, &ps_size));
    CHECK(vkdu_root_create(device, NULL, 0, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, &root));
    CHECK(vkdu_graphics_pipeline_create(device, root, vs, vs_size, ps, ps_size, &state, &pipeline));
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps);
    {
        struct vkdu_input_element invalid[2];
        memcpy(invalid, elements, sizeof(invalid));
        invalid[1].reg = 0; REJECT(vkdu_input_layout_validate(2, invalid)); invalid[1].reg = 1;
        invalid[1].slot = 5; REJECT(vkdu_input_layout_validate(2, invalid)); invalid[1].slot = 7;
        invalid[0].offset = 2044; REJECT(vkdu_input_layout_validate(2, invalid)); invalid[0].offset = 4;
        invalid[0].offset = 1; REJECT(vkdu_input_layout_validate(2, invalid)); invalid[0].offset = 4;
        invalid[0].step_rate = 1; REJECT(vkdu_input_layout_validate(2, invalid)); invalid[0].step_rate = 0;
        invalid[0].format = DXGI_FORMAT_BC1_UNORM; REJECT(vkdu_input_layout_validate(2, invalid));
        REJECT(vkdu_input_layout_validate(33, elements)); REJECT(vkdu_input_layout_validate(1, NULL));
        CHECK(vkdu_input_layout_validate(0, NULL));
    }
    CHECK(vkdu_render_target_create(device, 16, 12, &target));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, 0, &rtv));
    CHECK(vkdu_texture2d_rtv(rtv, 0, target));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &vertices));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &colors));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &indices));
    CHECK(vkdu_buffer_create(foreign, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &other));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_buffer_map(vertices, 0, 0, &mapped));
    memset(mapped, 0, 4096);
    {
        const float triangle[3][4] = {{-1, -1, 0, 1}, {-1, 3, 0, 1}, {3, -1, 0, 1}};
        for (i = 0; i < 3; i++) memcpy((char *)mapped + 64 + (i + 1) * 20 + 4, triangle[i], 16);
    }
    CHECK(vkdu_buffer_unmap(vertices, 0, 160));
    CHECK(vkdu_buffer_map(colors, 0, 0, &mapped));
    memset(mapped, 0, 4096);
    {
        const float rgba[3][4] = {{1, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
        memcpy((char *)mapped + 128, rgba, sizeof(rgba));
    }
    CHECK(vkdu_buffer_unmap(colors, 0, 192));
    CHECK(vkdu_buffer_map(indices, 0, 0, &mapped));
    memset(mapped, 0, 4096);
    ((uint16_t *)mapped)[6] = 0; ((uint16_t *)mapped)[7] = 1; ((uint16_t *)mapped)[8] = 2;
    CHECK(vkdu_buffer_unmap(indices, 0, 18));
    for (round = 0; round < 5; round++) {
        const uint32_t sentinel = 0x81230000 ^ round;
        const uint32_t colors_expected[] = {0xffff0000, 0xff00ff00, 0xffff0000, 0xffff00ff, 0};
        const struct vkdu_viewport viewport = {0, 0, 16, 12, 0, 1};
        const struct vkdu_rect scissor = {2, 1, 13, 10};
        const float clear[4] = {1, 0, 0, 1};
        struct vkdu_vertex_buffer views[3] = {{vertices, 64, 80, 20}, {0}, {colors, 128, 48, round == 3 ? 0 : 16}};
        struct vkdu_vertex_buffer invalid = {other, 0, 16, 16};
        struct vkdu_texture_copy_location src = {target, 0, 0, 0, 0, 0, 0, 0};
        struct vkdu_texture_copy_location dst = {readback, 512, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 16, 12, 1, 256};
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
        }
        CHECK(vkdu_buffer_map(readback, 0, 0, &mapped));
        for (i = 0; i < 1024; i++) ((uint32_t *)mapped)[i] = sentinel;
        CHECK(vkdu_buffer_unmap(readback, 0, 4096));
        CHECK(vkdu_command_clear_rtv(command, rtv, 0, clear, 0, NULL));
        CHECK(vkdu_command_graphics_root(command, root)); CHECK(vkdu_command_pipeline(command, pipeline));
        CHECK(vkdu_command_render_target(command, rtv, 0)); CHECK(vkdu_command_topology(command, 4));
        CHECK(vkdu_command_viewports(command, 1, &viewport)); CHECK(vkdu_command_scissors(command, 1, &scissor));
        CHECK(vkdu_command_vertex_buffers(command, 5, 3, views));
        REJECT(vkdu_command_vertex_buffers(command, 5, 1, &invalid));
        invalid.buffer = vertices; invalid.offset = UINT64_MAX;
        REJECT(vkdu_command_vertex_buffers(command, 5, 1, &invalid));
        invalid.offset = 4090; REJECT(vkdu_command_vertex_buffers(command, 5, 1, &invalid));
        invalid.offset = 0; invalid.stride = 2049; REJECT(vkdu_command_vertex_buffers(command, 5, 1, &invalid));
        REJECT(vkdu_command_vertex_buffers(command, 31, 3, views));
        CHECK(vkdu_command_vertex_buffers(command, 32, 0, NULL));
        if (round == 4) CHECK(vkdu_command_vertex_buffers(command, 7, 1, NULL));
        if (round == 1) {
            CHECK(vkdu_command_index_buffer(command, indices, 8, 10, DXGI_FORMAT_R16_UINT));
            CHECK(vkdu_command_draw_indexed(command, 3, 1, 2, 1, 1));
        } else {
            CHECK(vkdu_command_draw(command, 3, round == 2 ? 3 : 1, 1, round == 0 ? 2 : round == 3 ? 8 : 1));
        }
        CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command, &dst, 0, 0, 0, &src, NULL)); CHECK(vkdu_command_close(command));
        REJECT(vkdu_command_vertex_buffers(command, 5, 3, views));
        CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, fence, round + 1));
        CHECK(vkdu_fence_wait(fence, round + 1, 10000));
        CHECK(vkdu_buffer_map(readback, 0, 4096, &mapped));
        for (i = 0; i < 1024; i++) {
            uint32_t expected = sentinel;
            if (i >= 128 && i < 128 + 64 * 12 && (i - 128) % 64 < 16) {
                uint32_t x = (i - 128) % 64, y = (i - 128) / 64;
                expected = x >= 2 && x < 13 && y >= 1 && y < 10 ? colors_expected[round] : 0xff0000ff;
            }
            if (((uint32_t *)mapped)[i] != expected) {
                fprintf(stderr, "FAIL vertex fetch round%u word%u=%08x expected%08x\n", round, i,
                        ((uint32_t *)mapped)[i], expected); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        printf("PASS actual vertex fetch round%u: offset, register, sparse slots, start/base, divisor, stride/null and1024 pixel/sentinel words\n", round);
    }
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(vertices); vkdu_object_destroy(colors); vkdu_object_destroy(indices);
    vkdu_object_destroy(readback); vkdu_object_destroy(other); vkdu_object_destroy(rtv);
    vkdu_object_destroy(target); vkdu_object_destroy(pipeline); vkdu_object_destroy(root);
}

#ifdef VKDU_ENABLE_TEST_DEVICE
static void check_vertex_lifetime(vkdu_device *device)
{
    uint32_t round;
    for (round = 0; round < 3; round++) {
        vkdu_object *queue, *allocator, *command, *vertices, *indices, *fence, *gate;
        uint32_t destroyed_vertices = 0, destroyed_indices = 0;
        struct vkdu_vertex_buffer views[2];
        uint64_t completed;
        CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
        CHECK(vkdu_command_create(device, allocator, 0, &command));
        CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &vertices));
        CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &indices));
        CHECK(vkdu_fence_create(device, 0, &fence)); CHECK(vkdu_fence_create(device, 0, &gate));
        CHECK(vkdu_test_destroy_counter(vertices, &destroyed_vertices));
        CHECK(vkdu_test_destroy_counter(indices, &destroyed_indices));
        views[0] = (struct vkdu_vertex_buffer){vertices, 0, 1024, 16}; views[1] = views[0];
        CHECK(vkdu_command_vertex_buffers(command, 3, 2, views));
        CHECK(vkdu_command_vertex_buffers(command, 3, 2, views));
        CHECK(vkdu_command_vertex_buffers(command, 3, 2, NULL));
        CHECK(vkdu_command_index_buffer(command, indices, 0, 32, DXGI_FORMAT_R16_UINT));
        CHECK(vkdu_command_index_buffer(command, indices, 0, 32, DXGI_FORMAT_R16_UINT));
        CHECK(vkdu_command_index_buffer(command, NULL, 0, 0, 0)); CHECK(vkdu_command_close(command));
        vkdu_object_destroy(vertices); vkdu_object_destroy(indices);
        if (vkdu_test_counter_load(&destroyed_vertices) || vkdu_test_counter_load(&destroyed_indices)) {
            fputs("FAIL vertex/index owners released before recording retirement\n", stderr); exit(1);
        }
        if (round == 1) {
            CHECK(vkdu_allocator_reset(allocator));
            if (vkdu_test_counter_load(&destroyed_vertices) != 1 || vkdu_test_counter_load(&destroyed_indices) != 1) {
                fputs("FAIL vertex/index owners not released at legal reset\n", stderr); exit(1);
            }
        }
        if (round == 2) {
            CHECK(vkdu_queue_wait(queue, gate, 1)); CHECK(vkdu_queue_execute(queue, 1, &command));
            CHECK(vkdu_queue_signal(queue, fence, 1)); CHECK(vkdu_queue_drain_enqueue(queue));
            CHECK(vkdu_fence_completed(fence, &completed));
            if (completed) { fputs("FAIL vertex lifetime gate did not hold GPU\n", stderr); exit(1); }
        }
        vkdu_object_destroy(command); vkdu_object_destroy(allocator);
        if (round == 2) {
            if (vkdu_test_counter_load(&destroyed_vertices) || vkdu_test_counter_load(&destroyed_indices)) {
                fputs("FAIL vertex/index owners released while GPU pending\n", stderr); exit(1);
            }
            CHECK(vkdu_fence_signal_cpu(gate, 1)); CHECK(vkdu_fence_wait(fence, 1, 10000));
        }
        vkdu_object_destroy(queue); vkdu_object_destroy(fence); vkdu_object_destroy(gate);
        if (vkdu_test_counter_load(&destroyed_vertices) != 1 || vkdu_test_counter_load(&destroyed_indices) != 1) {
            fputs("FAIL vertex/index owners not destroyed exactly once\n", stderr); exit(1);
        }
    }
    puts("PASS actual vertex/index owners: duplicate binds, unbind, cancelled recording, reset and pending GPU completion");
}
#endif
