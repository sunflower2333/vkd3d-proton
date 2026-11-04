/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../tests/shaders/resource/headers/ps_store_buffer.h"
#include "../../tests/shaders/descriptors/headers/null_srv_buffer.h"
static void check_graphics_roots(vkdu_device *device, vkdu_device *other)
{
    vkdu_object *roots[4] = {0}, *pipelines[4] = {0}, *empty = NULL;
    vkdu_object *target = NULL, *rtv = NULL, *descriptors = NULL, *wrong = NULL;
    vkdu_object *upload = NULL, *buffer = NULL, *readback = NULL, *input = NULL;
    vkdu_object *queue = NULL, *allocator = NULL, *command = NULL, *fence = NULL;
    const struct vkdu_signature vin = {6, 0, 1, 1}, vout = {1, 0, 15, 3}, pout = {0, 0, 15, 3};
    const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc), *p = graphics_tokens(ps_store_buffer_code_dxbc);
    const struct vkdu_descriptor_range ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 1},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3}};
    struct vkdu_root_parameter params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 0, 0, 0, 2},
        {D3D12_ROOT_PARAMETER_TYPE_UAV, 0, 0, 0, 0}};
    const struct vkdu_graphics_desc desc = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT32_MAX, 3, 1, 0, 1, 15};
    const struct vkdu_viewport viewport = {0, 0, 4, 4, 0, 1};
    const struct vkdu_rect scissor = {1, 2, 2, 3};
    void *vs = NULL, *ps = NULL; size_t vs_size = 0, ps_size = 0;
    uint32_t *mapped, round, i;
    CHECK(vkdu_shader_dxbc(1, v, v[1], &vin, 1, &vout, 1, &vs, &vs_size));
    CHECK(vkdu_shader_dxbc(0, p, p[1], NULL, 0, NULL, 0, &ps, &ps_size));
    CHECK(vkdu_root_create(device, NULL, 0, 0, &empty));
    for (round = 0; round < 3; ++round) {
        if (round == 1) { params[0].type = D3D12_ROOT_PARAMETER_TYPE_CBV; params[0].constant_count = 0; }
        if (round == 2) { params[0].type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[0].ranges = ranges; params[0].range_count = 2; }
        CHECK(vkdu_root_create(device, params, round == 2 ? 1 : 2, 0, &roots[round]));
        CHECK(vkdu_graphics_pipeline_create(device, roots[round], vs, vs_size, ps, ps_size, &desc, &pipelines[round]));
    }
    vkdu_shader_dxbc_free(ps);
    p = graphics_tokens(null_srv_buffer_code_dxbc);
    CHECK(vkdu_shader_dxbc(0, p, p[1], &vout, 1, &pout, 1, &ps, &ps_size));
    memset(params, 0, sizeof(params));
    params[0].type = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[1].constant_count = 1;
    CHECK(vkdu_root_create(device, params, 2, 0, &roots[3]));
    CHECK(vkdu_graphics_pipeline_create(device, roots[3], vs, vs_size, ps, ps_size, &desc, &pipelines[3]));
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps);
    CHECK(vkdu_render_target_create(device, 4, 4, &target));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, 0, &rtv));
    CHECK(vkdu_texture2d_rtv(rtv, 0, target));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, 1, &descriptors));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &input));
    CHECK(vkdu_buffer_create(other, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &wrong));
    CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_DEST, &buffer));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
    for (i = 0; i < 1024; ++i) mapped[i] = 0x13579bdf;
    CHECK(vkdu_buffer_unmap(upload, 0, 4096));
    for (round = 0; round < 4; ++round) {
        const uint32_t expected = 0x9abc1234u + round;
        uint32_t values[2] = {20 + round * 4, expected};
        const float red[] = {1, 0, 0, 1};
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
        }
        REJECT(vkdu_command_graphics_constants(command, 0, 0, 2, values));
        CHECK(vkdu_command_copy(command, buffer, 0, upload, 0, 4096));
        CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        CHECK(vkdu_command_graphics_root(command, roots[round]));
        // An unrelated compute root must not replace graphics root metadata.
        CHECK(vkdu_command_root(command, empty));
        REJECT(vkdu_command_constants(command, 0, 0, 1, values));
        CHECK(vkdu_command_pipeline(command, pipelines[round]));
        CHECK(vkdu_command_render_target(command, rtv, 0));
        CHECK(vkdu_command_topology(command, 4)); CHECK(vkdu_command_viewports(command, 1, &viewport));
        CHECK(vkdu_command_scissors(command, 1, &scissor));
        CHECK(vkdu_buffer_map(input, 0, 0, (void **)&mapped));
        memset(mapped, 0, 4096); mapped[64] = values[0]; mapped[65] = expected; mapped[16 + 7] = 1;
        CHECK(vkdu_buffer_unmap(input, 0, 4096));
        if (!round) {
            CHECK(vkdu_command_graphics_constants(command, 0, 0, 2, values));
            values[1] = 0;
            CHECK(vkdu_command_graphics_root(command, roots[round])); // Same root preserves values.
            CHECK(vkdu_command_graphics_constants(command, 0, 2, 0, NULL));
            REJECT(vkdu_command_graphics_constants(command, 0, 1, 2, values));
            CHECK(vkdu_command_graphics_uav(command, 1, buffer, 64));
        } else if (round == 1) {
            CHECK(vkdu_command_graphics_cbv(command, 0, input, 256));
            CHECK(vkdu_command_graphics_uav(command, 1, buffer, 64));
            REJECT(vkdu_command_graphics_cbv(command, 0, input, 1));
            REJECT(vkdu_command_graphics_cbv(command, 0, wrong, 0));
            REJECT(vkdu_command_graphics_srv(command, 0, input, 0));
            REJECT(vkdu_command_graphics_uav(command, 1, input, 0));
        } else if (round == 2) {
            CHECK(vkdu_buffer_cbv(descriptors, 3, input, 256, 256));
            CHECK(vkdu_buffer_uav(descriptors, 5, buffer, DXGI_FORMAT_R32_TYPELESS, 16, 100, 0, 1, NULL, 0));
            REJECT(vkdu_command_graphics_table(command, 0, descriptors, 2)); // Heap must be bound.
            CHECK(vkdu_command_heaps(command, 1, &descriptors));
            CHECK(vkdu_command_graphics_table(command, 0, descriptors, 2));
            REJECT(vkdu_command_graphics_table(command, 0, descriptors, 6));
        } else {
            uint32_t location = 28;
            CHECK(vkdu_command_clear_rtv(command, rtv, 0, red, 0, NULL));
            CHECK(vkdu_command_graphics_srv(command, 0, input, 64));
            CHECK(vkdu_command_graphics_constants(command, 1, 0, 1, &location));
            location = 0;
            REJECT(vkdu_command_graphics_srv(command, 0, input, 1));
            REJECT(vkdu_command_graphics_srv(command, 0, wrong, 0));
        }
        CHECK(vkdu_command_draw(command, 3, 1, 0, 0));
        CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        if (round < 3) CHECK(vkdu_command_copy(command, readback, 0, buffer, 0, 4096));
        else {
            struct vkdu_texture_copy_location src = {target, 0, 0, 0, 0, 0, 0, 0};
            struct vkdu_texture_copy_location dst = {readback, 512, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256};
            CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
            CHECK(vkdu_command_texture_copy(command, &dst, 0, 0, 0, &src, NULL));
        }
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, round + 1)); CHECK(vkdu_fence_wait(fence, round + 1, 10000));
        CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 1024; ++i) {
            uint32_t want = i == 16 + 5 + round ? expected : 0x13579bdf;
            if (round == 3) {
                if (i < 128 || i >= 128 + 64 * 4 || (i - 128) % 64 >= 4) continue;
                want = i == 128 + 64 * 2 + 1 ? UINT32_MAX : 0xff0000ff;
            }
            if (mapped[i] != want) { fprintf(stderr, "graphics roots round%u word%u=%08x expected%08x\n", round, i, mapped[i], want); exit(1); }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        printf("PASS actual graphics roots round%u constants/CBV/UAV/table/SRV and independent compute metadata\n", round);
    }
    CHECK(vkdu_device_status(device));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    for (i = 0; i < 4; ++i) { vkdu_object_destroy(pipelines[i]); vkdu_object_destroy(roots[i]); }
    vkdu_object_destroy(empty); vkdu_object_destroy(readback); vkdu_object_destroy(buffer); vkdu_object_destroy(input);
    vkdu_object_destroy(upload); vkdu_object_destroy(wrong); vkdu_object_destroy(descriptors);
    vkdu_object_destroy(rtv); vkdu_object_destroy(target);
}
