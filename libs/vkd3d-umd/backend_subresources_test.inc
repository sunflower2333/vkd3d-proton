/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../tests/shaders/descriptors/headers/tex2d_array_reinterpretation.h"
void vkd3d_compute_dxbc_checksum(const void *dxbc, size_t size, uint32_t checksum[4]);
static uint32_t subresource_test_width(uint32_t mip) { return (9u >> mip) ? 9u >> mip : 1; }
static uint32_t subresource_test_height(uint32_t mip) { return (7u >> mip) ? 7u >> mip : 1; }
static uint32_t subresource_test_word(uint32_t format, uint32_t sub, uint32_t x, uint32_t y)
{
    uint32_t red = 16 + sub * 12 + (y * subresource_test_width(sub % 4) + x) % 11;
    if (format == DXGI_FORMAT_R32_FLOAT) { float value = red / 255.0f; memcpy(&red, &value, 4); return red; }
    if (format == DXGI_FORMAT_B8G8R8A8_TYPELESS) return 0xff000000u | y | (x << 8) | (red << 16);
    return 0xff000000u | red | (x << 8) | (y << 16);
}
static void subresource_test_transition(vkdu_object *command, vkdu_object *image, uint32_t sub, uint32_t before, uint32_t after)
{
    struct vkdu_resource_barrier barrier = {VKDU_BARRIER_TRANSITION, image, before, after, NULL, sub + 1};
    CHECK(vkdu_command_barriers(command, 1, &barrier));
}
static void check_texture_subresources(vkdu_device *device, int uniform_query_control)
{
    const uint32_t formats[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_TYPELESS};
    unsigned f;
    for (f = 0; f < 3; ++f) {
        struct vkdu_texture2d_shape shape = {9, 7, formats[f], 4, 3, 0, 1, 0};
        vkdu_object *src, *dst, *memory, *upload, *readback, *output, *heap, *root, *pipeline, *allocator, *command, *queue, *fence, *bad = NULL;
        struct vkdu_descriptor_range ranges[2] = {{0, 1, 0, 0, 0}, {0, 1, 0, 1, 0}};
        struct vkdu_root_parameter roots[3] = {{4, 0, 0, 0, 0}, {0, 0, 0, 0, 0, ranges, 1}, {0, 0, 0, 0, 0, ranges + 1, 1}};
        uint32_t *mapped, sub, x, y, i, expected[8192], destroyed_src = 0, destroyed_dst = 0;
        uint64_t bytes, align, large_bytes, large_align;
        uint32_t typed = f == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM : f == 2 ? DXGI_FORMAT_B8G8R8A8_UNORM : formats[f];
        CHECK(vkdu_texture2d_shape_allocation(device, &shape, &bytes, &align));
        shape.layers = 4; CHECK(vkdu_texture2d_shape_allocation(device, &shape, &large_bytes, &large_align));
        if (!bytes || align != 65536 || large_align != align || large_bytes < bytes) exit(1);
        shape.layers = 3; shape.mips = 5; REJECT(vkdu_texture2d_shape_create(device, &shape, &bad));
        shape.mips = 0; REJECT(vkdu_texture2d_shape_create(device, &shape, &bad)); shape.mips = 4;
        shape.layers = 2049; REJECT(vkdu_texture2d_shape_create(device, &shape, &bad)); shape.layers = 3;
        CHECK(vkdu_texture2d_shape_create(device, &shape, &src));
        CHECK(vkdu_test_texture_heap_create(device, bytes * 2, &memory));
        REJECT(vkdu_texture2d_shape_place(device, memory, 1, &shape, D3D12_RESOURCE_STATE_COPY_DEST, &bad));
        REJECT(vkdu_texture2d_shape_place(device, memory, bytes * 2, &shape, D3D12_RESOURCE_STATE_COPY_DEST, &bad));
        CHECK(vkdu_texture2d_shape_place(device, memory, bytes, &shape, D3D12_RESOURCE_STATE_COPY_DEST, &dst));
        vkdu_object_destroy(memory);
        CHECK(vkdu_test_destroy_counter(src, &destroyed_src)); CHECK(vkdu_test_destroy_counter(dst, &destroyed_dst));
        CHECK(vkdu_buffer_create(device, 32768, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
        CHECK(vkdu_buffer_create(device, 32768, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
        CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, &output));
        CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
        for (i = 0; i < 8192; ++i) mapped[i] = expected[i] = 0xacbdcedf;
        for (sub = 0; sub < 12; ++sub) for (y = 0; y < subresource_test_height(sub % 4); ++y)
            for (x = 0; x < subresource_test_width(sub % 4); ++x)
                mapped[128 + sub * 512 + y * 64 + x] = subresource_test_word(formats[f], sub, x, y);
        CHECK(vkdu_buffer_unmap(upload, 0, 32768));
        CHECK(vkdu_buffer_map(readback, 0, 0, (void **)&mapped));
        for (i = 0; i < 8192; ++i) mapped[i] = expected[i];
        CHECK(vkdu_buffer_unmap(readback, 0, 32768));
        CHECK(vkdu_allocator_create(device, 0, &allocator)); CHECK(vkdu_command_create(device, allocator, 0, &command));
        CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_fence_create(device, 0, &fence));
        CHECK(vkdu_heap_create(device, 0, 2, 1, &heap));
        CHECK(vkdu_texture2d_srv_range(heap, 0, dst, typed, 1, 2, 0, 1));
        CHECK(vkdu_texture2d_srv_range(heap, 1, dst, typed, 1, UINT32_MAX, 1, 2));
        REJECT(vkdu_texture2d_srv_range(heap, 1, dst, typed, 4, 1, 0, 1));
        REJECT(vkdu_texture2d_srv_range(heap, 1, dst, typed, 1, 4, 0, 1));
        REJECT(vkdu_texture2d_srv_range(heap, 1, dst, typed, 1, 1, 3, 1));
        REJECT(vkdu_texture2d_srv_range(heap, 1, dst, typed, 1, 1, 1, UINT32_MAX));
        CHECK(vkdu_root_create(device, roots, 3, 0, &root));
        if (uniform_query_control) {
            /* Independent control for lavapipe 25.2.8's documented first-lane
             * resinfo limitation. Only the two resinfo LOD operands change
             * from GroupIndex to GroupID.x (zero for this one-group dispatch).
             * All Load coordinates remain per-thread. The original legal
             * divergent query remains available through --subresources-only. */
            uint32_t code[sizeof(tex2d_array_reinterpretation_code_dxbc) / 4];
            uint32_t at, end, found = 0;
            memcpy(code, tex2d_array_reinterpretation_code_dxbc, sizeof(code));
            at = code[10] / 4 + 2; end = at + code[at + 1]; at += 2;
            while (at < end) {
                uint32_t words = (code[at] >> 24) & 127;
                if (!words || words > end - at) exit(1);
                if ((code[at] & 0x7ff) == 61) {
                    if (words != 8 || code[at + 3] != 0x0002400a) exit(1);
                    code[at + 3] = 0x0002100a; ++found;
                }
                at += words;
            }
            if (found != 2) exit(1);
            vkd3d_compute_dxbc_checksum(code, sizeof(code), code + 1);
            CHECK(vkdu_pipeline_create(device, root, code, sizeof(code), &pipeline));
        } else CHECK(vkdu_pipeline_create(device, root, tex2d_array_reinterpretation_code_dxbc,
                sizeof(tex2d_array_reinterpretation_code_dxbc), &pipeline));
        CHECK(vkdu_command_copy(command, output, 0, upload, 28672, 4096));
        for (sub = 0; sub < 12; ++sub) {
            struct vkdu_texture_copy_location from = {upload, 512 + sub * 2048, 1, formats[f],
                subresource_test_width(sub % 4), subresource_test_height(sub % 4), 1, 256}, to = {src, sub};
            CHECK(vkdu_command_texture_copy(command, &to, 0, 0, 0, &from, NULL));
            subresource_test_transition(command, src, sub, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        {
            struct vkdu_resource_barrier invalid = {VKDU_BARRIER_TRANSITION, src, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, 13};
            REJECT(vkdu_command_barriers(command, 1, &invalid));
        }
        CHECK(vkdu_command_resource_copy(command, dst, src));
        CHECK(vkdu_command_transition(command, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
        {
            struct vkdu_texture_copy_location from = {dst, 11}, to = {dst, 7}, invalid = {dst, UINT64_MAX};
            subresource_test_transition(command, dst, 7, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            REJECT(vkdu_command_texture_copy(command, &to, 0, 0, 0, &invalid, NULL));
            CHECK(vkdu_command_texture_copy(command, &to, 0, 0, 0, &from, NULL));
            subresource_test_transition(command, dst, 7, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        for (sub = 0; sub < 12; ++sub) {
            struct vkdu_texture_copy_location from = {dst, sub}, to = {readback, 512 + sub * 2048, 1, formats[f],
                subresource_test_width(sub % 4), subresource_test_height(sub % 4), 1, 256};
            CHECK(vkdu_command_texture_copy(command, &to, 0, 0, 0, &from, NULL));
            for (y = 0; y < to.height; ++y) for (x = 0; x < to.width; ++x)
                expected[128 + sub * 512 + y * 64 + x] = subresource_test_word(formats[f], sub == 7 ? 11 : sub, x, y);
        }
        CHECK(vkdu_command_transition(command, dst, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        CHECK(vkdu_command_heaps(command, 1, &heap)); CHECK(vkdu_command_root(command, root));
        CHECK(vkdu_command_pipeline(command, pipeline)); CHECK(vkdu_command_uav(command, 0, output, 0));
        CHECK(vkdu_command_table(command, 1, heap, 0)); CHECK(vkdu_command_table(command, 2, heap, 1));
        CHECK(vkdu_command_dispatch(command, 1, 1, 1));
        CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 28672, output, 0, 4096));
        CHECK(vkdu_command_close(command));
        vkdu_object_destroy(src); vkdu_object_destroy(dst);
        if (vkdu_test_counter_load(&destroyed_src) || vkdu_test_counter_load(&destroyed_dst)) exit(1);
        CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, fence, 1));
        CHECK(vkdu_fence_wait(fence, 1, 10000));
        CHECK(vkdu_buffer_map(readback, 0, 32768, (void **)&mapped));
        for (i = 0; i < 8192; ++i) {
            /* GetDimensions outside the view's mip range is unspecified; all
             * valid dimensions plus every Load and every untouched byte count. */
            if (i >= 7168 && i < 7168 + 16 * 9) {
                uint32_t thread = (i - 7168) / 9, member = (i - 7168) % 9, value;
                float sample;
                if (!uniform_query_control && member < 7 && thread >= 2) continue;
                if (member < 4) value = (member & 1) ? subresource_test_height(uniform_query_control ? 1 : thread + 1) :
                        subresource_test_width(uniform_query_control ? 1 : thread + 1);
                else if (member < 7) value = member == 4 ? 2 : member == 5 ? 2 : 3;
                else if (member == 7) { sample = thread < 2 ? (float)(16 + (thread + 1) * 12) : 0.0f; memcpy(&value, &sample, 4); }
                else {
                    uint32_t layer = thread % 4, mip = thread / 4;
                    uint32_t which = (layer + 1) * 4 + mip + 1;
                    if (which == 7) which = 11;
                    sample = layer < 2 && mip < 3 ? (float)(16 + which * 12) : 0.0f;
                    memcpy(&value, &sample, 4);
                }
                expected[i] = value;
            }
            if (mapped[i] != expected[i]) {
                fprintf(stderr, "FAIL subresources format%u word%u=%08x expected%08x\n", formats[f], i, mapped[i], expected[i]);
                for (unsigned t = 0; t < 16; ++t) {
                    fprintf(stderr, "thread%u", t);
                    for (unsigned m = 0; m < 9; ++m) fprintf(stderr, " %08x", mapped[7168 + t * 9 + m]);
                    fputc('\n', stderr);
                }
                exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        vkdu_object_destroy(command); vkdu_object_destroy(allocator);
        CHECK(vkdu_queue_signal(queue, fence, 2)); CHECK(vkdu_fence_wait(fence, 2, 10000));
        if (vkdu_test_counter_load(&destroyed_src) != 1 || vkdu_test_counter_load(&destroyed_dst) != 1) exit(1);
        vkdu_object_destroy(heap); vkdu_object_destroy(root); vkdu_object_destroy(pipeline);
        vkdu_object_destroy(upload); vkdu_object_destroy(readback); vkdu_object_destroy(output);
        vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    }
    printf("PASS actual non-RT texture mip/layers: 3 formats x12 subresources, committed and nonzero-offset placed textures with retired heap, subresource/all-resource/in-place copies, per-subresource barriers, first-mip/first-layer SRV dimensions and 32 loads, padding and early resource destruction; %s query; upstream SM5.1 DXBC fixture, not native SM5.1 admission\n",
            uniform_query_control ? "uniform LOD control (does not pass the original divergent case)" : "original divergent LOD");
}
