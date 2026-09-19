/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../tests/shaders/render_target/headers/vs_flat_color.h"
#include "../../tests/shaders/render_target/headers/ps_flat_color.h"
#include "../../tests/shaders/render_target/headers/vs_gradient.h"
#include "../../tests/shaders/render_target/headers/ps_gradient.h"
static const uint32_t *graphics_tokens(const uint32_t *container)
{
    uint32_t i;
    for (i = 0; i < container[7]; ++i) {
        const uint32_t *chunk = container + container[8 + i] / 4;
        if (chunk[0] == 0x58454853 || chunk[0] == 0x52444853) return chunk + 2;
    }
    fprintf(stderr, "missing test shader tokens\n"); exit(1);
}
static void check_graphics(vkdu_device *device, vkdu_device *other)
{
    vkdu_object *root = NULL, *pipeline = NULL, *gradient = NULL, *target = NULL, *heap = NULL, *wrong_heap = NULL;
    vkdu_object *queue = NULL, *allocator = NULL, *command = NULL, *fence = NULL, *readback = NULL, *indices = NULL;
    /* Runtime signatures may be a superset of actual shader declarations. */
    const struct vkdu_signature vertex_input[] = {{6, 0, 1, 1}, {0, 3, 15, 3}};
    const struct vkdu_signature vertex_output[] = {{1, 0, 15, 3}, {0, 5, 15, 3}};
    const struct vkdu_signature pixel_output = {0, 0, 15, 3};
    const uint32_t *vs_tokens = graphics_tokens(vs_flat_color_code_dxbc), *ps_tokens = graphics_tokens(ps_flat_color_code_dxbc);
    struct vkdu_graphics_desc desc = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT32_MAX, 3, 1, 0, 1, 15};
    void *vs = NULL, *ps = NULL, *mapped; size_t vs_size = 0, ps_size = 0;
    uint32_t i, round;
    CHECK(vkdu_shader_dxbc(1, vs_tokens, vs_tokens[1], vertex_input, 2, vertex_output, 2, &vs, &vs_size));
    CHECK(vkdu_shader_dxbc(0, ps_tokens, ps_tokens[1], NULL, 0, &pixel_output, 1, &ps, &ps_size));
    {
        void *rejected = NULL; size_t rejected_size = 0;
        struct vkdu_signature wrong = vertex_input[0]; wrong.system_value = 8;
        REJECT(vkdu_shader_dxbc(1, vs_tokens, vs_tokens[1], &wrong, 1, vertex_output, 2, &rejected, &rejected_size));
        REJECT(vkdu_shader_dxbc(1, vs_tokens, vs_tokens[1] - 1, vertex_input, 2, vertex_output, 2, &rejected, &rejected_size));
        REJECT(vkdu_shader_dxbc(1, vs_tokens, vs_tokens[1], NULL, 0, vertex_output, 2, &rejected, &rejected_size));
        if (rejected || rejected_size) { fprintf(stderr, "rejected shader published storage\n"); exit(1); }
    }
    CHECK(vkdu_root_create(device, NULL, 0, 0, &root));
    CHECK(vkdu_graphics_pipeline_create(device, root, vs, vs_size, ps, ps_size, &desc, &pipeline));
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps);
    {
        const struct vkdu_signature varying[] = {{1, 0, 15, 3}, {0, 1, 3, 3}, {0, 5, 15, 3}};
        const uint32_t *vertex = graphics_tokens(vs_gradient_code_dxbc), *pixel = graphics_tokens(ps_gradient_code_dxbc);
        CHECK(vkdu_shader_dxbc(1, vertex, vertex[1], vertex_input, 2, varying, 3, &vs, &vs_size));
        CHECK(vkdu_shader_dxbc(0, pixel, pixel[1], varying, 3, &pixel_output, 1, &ps, &ps_size));
        CHECK(vkdu_graphics_pipeline_create(device, root, vs, vs_size, ps, ps_size, &desc, &gradient));
        vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps);
    }
    CHECK(vkdu_render_target_create(device, 16, 12, &target));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, 0, &heap));
    CHECK(vkdu_heap_create(other, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, 0, &wrong_heap));
    CHECK(vkdu_texture2d_rtv(heap, 2, target));
    REJECT(vkdu_texture2d_rtv(heap, 4, target));
    REJECT(vkdu_texture2d_rtv(wrong_heap, 2, target));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &indices));
    CHECK(vkdu_buffer_map(indices, 0, 0, &mapped));
    ((uint16_t *)mapped)[0] = ((uint16_t *)mapped)[1] = 99;
    ((uint16_t *)mapped)[2] = 0; ((uint16_t *)mapped)[3] = 1; ((uint16_t *)mapped)[4] = 2;
    CHECK(vkdu_buffer_unmap(indices, 0, 10));
    for (round = 0; round < 5; ++round) {
        const uint32_t sentinel = 0x71710000u ^ round;
        const float clear[4] = {1, 0, 0, 1};
        const struct vkdu_viewport viewport = {0, 0, 16, 12, 0, 1};
        const struct vkdu_rect scissor = {2 + round, 1 + round, 12, 9};
        struct vkdu_texture_copy_location src = {target, 0, 0, 0, 0, 0, 0, 0};
        struct vkdu_texture_copy_location dst = {readback, 512, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 16, 12, 1, 256};
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
        }
        CHECK(vkdu_buffer_map(readback, 0, 0, &mapped));
        for (i = 0; i < 1024; ++i) ((uint32_t *)mapped)[i] = sentinel;
        CHECK(vkdu_buffer_unmap(readback, 0, 4096));
        REJECT(vkdu_command_draw(command, 3, 1, 0, 0)); /* Reset invalidates prior graphics state. */
        CHECK(vkdu_command_clear_rtv(command, heap, 2, clear, 0, NULL));
        CHECK(vkdu_command_graphics_root(command, root)); CHECK(vkdu_command_pipeline(command, round == 4 ? gradient : pipeline));
        CHECK(vkdu_command_render_target(command, heap, 2));
        CHECK(vkdu_command_topology(command, 4)); CHECK(vkdu_command_viewports(command, 1, &viewport));
        CHECK(vkdu_command_scissors(command, 1, &scissor));
        REJECT(vkdu_command_render_target(command, wrong_heap, 2));
        REJECT(vkdu_command_topology(command, 99)); /* Rejected state must preserve valid state. */
        if (round & 1) {
            CHECK(vkdu_command_index_buffer(command, indices, 0, 10, DXGI_FORMAT_R16_UINT));
            REJECT(vkdu_command_draw_indexed(command, 4, 1, 2, 13, 0));
            /* SV_VertexID excludes base vertex; no vertex attributes are bound
             * in this shader. A nonzero base must not shift its generated shape. */
            CHECK(vkdu_command_draw_indexed(command, round == 3 ? 0 : 3, 1, 2, 13, 7));
        } else CHECK(vkdu_command_draw(command, 3, round == 2 ? 0 : 1, 0, 5));
        CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command, &dst, 0, 0, 0, &src, NULL));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, round + 1)); CHECK(vkdu_fence_wait(fence, round + 1, 10000));
        CHECK(vkdu_buffer_map(readback, 0, 4096, &mapped));
        for (i = 0; i < 1024; ++i) {
            uint32_t expected = sentinel;
            if (i >= 128 && i < 128 + 64 * 12 && (i - 128) % 64 < 16) {
                const uint32_t x = (i - 128) % 64, y = (i - 128) / 64;
                expected = round < 2 && x >= (uint32_t)scissor.left && x < (uint32_t)scissor.right &&
                    y >= (uint32_t)scissor.top && y < (uint32_t)scissor.bottom ? 0xffffffff : 0xff0000ff;
                if (round == 4 && x >= (uint32_t)scissor.left && x < (uint32_t)scissor.right &&
                        y >= (uint32_t)scissor.top && y < (uint32_t)scissor.bottom) {
                    const float u = (x + 0.5f) / 16, v = 1 - (y + 0.5f) / 12;
                    const float values[4] = {u, v, u + v, u * v};
                    uint32_t channel;
                    /* Account only for the permitted UNORM rounding variance;
                     * coordinates, channel order and alpha are independently checked. */
                    for (channel = 0; channel < 4; ++channel) {
                        const int reference = (int)((values[channel] < 1 ? values[channel] : 1) * 255 + 0.5f);
                        const int got = (((uint32_t *)mapped)[i] >> (channel * 8)) & 255;
                        if (abs(got - reference) > 1) {
                            fprintf(stderr, "graphics varying word%u channel%u=%u expected%u\n", i, channel, got, reference); exit(1);
                        }
                    }
                    continue;
                }
            }
            if (((uint32_t *)mapped)[i] != expected) {
                fprintf(stderr, "graphics round%u word%u=%08x expected%08x\n", round, i, ((uint32_t *)mapped)[i], expected); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        printf("PASS actual graphics round%u rebuilt VS/PS, draw/index/zero count/scissor/varyings:1024 checked words\n", round);
    }
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(indices); vkdu_object_destroy(readback); vkdu_object_destroy(wrong_heap);
    vkdu_object_destroy(heap); vkdu_object_destroy(target); vkdu_object_destroy(gradient); vkdu_object_destroy(pipeline); vkdu_object_destroy(root);
}
