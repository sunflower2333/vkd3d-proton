/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "stage_test_shaders.h"

/* Mutate exact fixture output declarations and matching MOV destinations only;
 * source input registers remain float4. The resulting ATTR1 starts at column y. */
static uint32_t *stage_packed_color_tokens(const uint32_t *tokens)
{
    uint32_t *copy = malloc(tokens[1] * 4), pos = 2, edits = 0;
    if (!copy) exit(1);
    memcpy(copy, tokens, tokens[1] * 4);
    while (pos < copy[1]) {
        const uint32_t length = (copy[pos] >> 24) & 127, op = copy[pos] & 2047;
        if (!length || length > copy[1] - pos) exit(1);
        if ((op == 101 || op == 54) && length >= 3 && copy[pos + 1] == 0x001020f2 && copy[pos + 2] == 1) {
            copy[pos + 1] = 0x001020e2; ++edits;
        }
        pos += length;
    }
    if (edits < 2) exit(1);
    return copy;
}

static uint32_t *stage_builtin_color_tokens(const uint32_t *tokens, uint32_t system)
{
    uint32_t *copy=malloc((tokens[1]+1)*4), in=2, out=2, edits=0;
    if(!copy)exit(1); copy[0]=tokens[0];copy[1]=tokens[1]+1;
    while(in<tokens[1]) {
        uint32_t n=(tokens[in]>>24)&127;
        if(!n || n>tokens[1]-in)exit(1);
        if((tokens[in]&2047)==101 && n==3 && tokens[in+1]==0x001020f2 && tokens[in+2]==1) {
            copy[out++]=0x04000067;copy[out++]=tokens[in+1];copy[out++]=1;copy[out++]=system;++edits;
        } else {memcpy(copy+out,tokens+in,n*4);out+=n;}
        in+=n;
    }
    if(edits!=1 || out!=copy[1])exit(1);
    return copy;
}

static void check_geometry_output(vkdu_device *device, vkdu_device *other)
{
    vkdu_object *root, *pipeline, *target, *rtv, *queue, *allocator, *command, *fence;
    vkdu_object *vertices, *upload, *output, *readback, *pixels, *foreign, *skip_count;
    const struct vkdu_signature vin[] = {{0, 0, 15, 3}, {0, 1, 15, 3}};
    const struct vkdu_signature link[] = {{1, 0, 15, 3}, {0, 1, 15, 3}};
    const struct vkdu_signature pout = {0, 0, 15, 3};
    struct vkdu_input_element inputs[] = {{0, 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, 0},
        {0, 16, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, 1}};
    struct vkdu_graphics_desc state = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT32_MAX, 3, 1, 0, 1, 15, 2, inputs};
    struct vkdu_stream_output_desc so = {3, 3, 0,
        {{0, 1, 0, 15}, {0, 1, UINT32_MAX, 1}, {0, 2, 1, 15}}, {0, 20, 16}};
    const uint32_t *vt = graphics_tokens(unaligned_vertex_stride_vs_code_dxbc);
    const uint32_t *pt = graphics_tokens(unaligned_vertex_stride_ps_code_dxbc);
    const uint32_t *gt = graphics_tokens(stage_gs_code_dxbc);
    void *vs, *ps, *gs, *packed_vs, *packed_gs, *integer_vs[2], *builtin_vs[2], *mapped; size_t vs_size, ps_size, gs_size, packed_vs_size, packed_gs_size, integer_vs_size[2], builtin_vs_size[2];
    struct vkdu_signature packed_link[] = {{1,0,15,3},{0,1,14,3}};
    uint32_t *packed_vt = stage_packed_color_tokens(vt), *packed_gt = stage_packed_color_tokens(gt);
    uint32_t round, i;
    CHECK(vkdu_shader_dxbc(1, vt, vt[1], vin, 2, link, 2, &vs, &vs_size));
    CHECK(vkdu_shader_dxbc(0, pt, pt[1], link, 2, &pout, 1, &ps, &ps_size));
    CHECK(vkdu_shader_dxbc(2, gt, gt[1], link, 2, link, 2, &gs, &gs_size));
    CHECK(vkdu_shader_dxbc(1, packed_vt, packed_vt[1], vin, 2, packed_link, 2, &packed_vs, &packed_vs_size));
    CHECK(vkdu_shader_dxbc(2, packed_gt, packed_gt[1], link, 2, packed_link, 2, &packed_gs, &packed_gs_size));
    free(packed_vt); free(packed_gt);
    for (i = 0; i < 2; ++i) {
        struct vkdu_signature ivin[] = {{0,0,15,3},{0,1,15,i+1}}, ilink[] = {{1,0,15,3},{0,1,15,i+1}};
        uint32_t *bt=stage_builtin_color_tokens(vt,i+2);
        struct vkdu_signature blink[]={{1,0,15,3},{i+2,1,15,3}};
        CHECK(vkdu_shader_dxbc(1,vt,vt[1],ivin,2,ilink,2,&integer_vs[i],&integer_vs_size[i]));
        CHECK(vkdu_shader_dxbc(1,bt,bt[1],vin,2,blink,2,&builtin_vs[i],&builtin_vs_size[i]));free(bt);
    }
    CHECK(vkdu_stream_output_validate(&so, gs, gs_size));
    {
        struct vkdu_stream_output_desc bad = so;
        bad.entries[0].reg = 3; REJECT(vkdu_stream_output_validate(&bad, gs, gs_size)); bad = so;
        bad.entries[0].stream = 1; REJECT(vkdu_stream_output_validate(&bad, gs, gs_size)); bad = so;
        bad.strides[1] = 16; REJECT(vkdu_stream_output_validate(&bad, gs, gs_size)); bad = so;
        bad.strides[1] = 2052; REJECT(vkdu_stream_output_validate(&bad, gs, gs_size)); bad = so;
        bad.entries[1].mask = 16; REJECT(vkdu_stream_output_validate(&bad, gs, gs_size)); bad = so;
        bad.entries[1].mask = 5; REJECT(vkdu_stream_output_validate(&bad, gs, gs_size)); bad = so;
        bad.entries[0].mask = 5; REJECT(vkdu_stream_output_validate(&bad, gs, gs_size)); bad = so;
        bad.entries[2].reg = 0; REJECT(vkdu_stream_output_validate(&bad, gs, gs_size)); bad = so;
        bad.stride_count = 2; REJECT(vkdu_stream_output_validate(&bad, gs, gs_size));
    }
    CHECK(vkdu_root_create(device, NULL, 0,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT, &root));
    CHECK(vkdu_render_target_create(device, 10, 10, &target));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, 0, &rtv));
    CHECK(vkdu_texture2d_rtv(rtv, 0, target));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &vertices));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
    CHECK(vkdu_buffer_create(device, 4096, 1, 0, D3D12_RESOURCE_STATE_COPY_DEST, &output));
    CHECK(vkdu_buffer_create(other, 4096, 1, 0, D3D12_RESOURCE_STATE_STREAM_OUT, &foreign));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &pixels));
    CHECK(vkdu_buffer_create(device, 8, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &skip_count));
    for (round = 0; round < 13; ++round) {
        const uint32_t sentinel = 0x7f710000u + round;
        const float colors[13][4] = {{0,1,0,1}, {0,0,1,1}, {1,0,1,1}, {1,1,0,1}, {0,1,1,1},
            {.25f,.5f,.75f,1}, {.5f,.75f,.25f,1}, {.25f,.5f,.75f,1}, {.5f,.75f,.25f,1}, {-1.25f,2.5f,-3.75f,4.5f}, {-4.5f,-3.75f,2.5f,-1.25f}, {.25f,.5f,.75f,1}, {.5f,.75f,.25f,1}};
        const int partial = round >= 5, has_so = round != 3, has_gs = round < 9 && round != 4 && round != 6 && round != 7,
            rasterize = round < 9 && round != 1 && round != 4 && round != 6 && round != 7 && round != 8;
        const uint32_t stride2 = partial ? 12 : 16;
        struct vkdu_stream_output_desc partial_so = {6,3,0,
            {{0,1,UINT32_MAX,1},{0,1,0,8},{0,1,0,2},{0,1,1,4},{0,2,1,1},{0,2,0,1}}, {0,20,12}};
        const uint32_t vertex_count = !has_so ? 0 : has_gs ? 12 : 3;
        const float position[4] = {0, 0, 0, 1}, clear[4] = {1,0,0,1};
        const struct vkdu_viewport viewport = {0,0,10,10,0,1};
        const struct vkdu_rect scissor = {0,0,10,10};
        const struct vkdu_vertex_buffer vb = {vertices, 64, 32, 32};
        struct vkdu_stream_output_buffer views[2] = {{output, output, 128, 240, 1024},
            {output, output, 512, 12 * stride2, 1028}};
        struct vkdu_texture_copy_location src = {target}, dst = {pixels, 512, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 10,10,1,256};
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
            CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
        }
        state.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        state.gs = has_gs ? (round == 8 ? packed_gs : gs) : NULL;
        state.gs_size = has_gs ? (round == 8 ? packed_gs_size : gs_size) : 0;
        if (round == 7 || round == 8) partial_so.entries[4].mask = 8;
        inputs[1].format = round < 9 || round >= 11 ? DXGI_FORMAT_R32G32B32A32_FLOAT :
            round == 9 ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_SINT;
        state.stream_output = has_so ? (partial ? &partial_so : &so) : NULL;
        so.rasterized_stream = partial_so.rasterized_stream = rasterize ? 0 : UINT32_MAX;
        CHECK(vkdu_graphics_pipeline_create(device, root, round >= 11 ? builtin_vs[round-11] : round >= 9 ? integer_vs[round-9] : round == 7 ? packed_vs : vs,
            round >= 11 ? builtin_vs_size[round-11] : round >= 9 ? integer_vs_size[round-9] : round == 7 ? packed_vs_size : vs_size,
            rasterize ? ps : NULL, rasterize ? ps_size : 0, &state, &pipeline));
        CHECK(vkdu_buffer_map(vertices, 0, 0, &mapped));
        memcpy((char *)mapped + 64, position, 16); memcpy((char *)mapped + 80, colors[round], 16);
        CHECK(vkdu_buffer_unmap(vertices, 64, 96));
        CHECK(vkdu_buffer_map(upload, 0, 0, &mapped));
        for (i = 0; i < 1024; ++i) ((uint32_t *)mapped)[i] = sentinel;
        ((uint32_t *)mapped)[256] = ((uint32_t *)mapped)[257] = 0;
        CHECK(vkdu_buffer_unmap(upload, 0, 4096));
        CHECK(vkdu_buffer_map(pixels, 0, 0, &mapped));
        for (i = 0; i < 1024; ++i) ((uint32_t *)mapped)[i] = sentinel;
        CHECK(vkdu_buffer_unmap(pixels, 0, 4096));
        CHECK(vkdu_command_copy(command, output, 0, upload, 0, 4096));
        CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT));
        CHECK(vkdu_command_clear_rtv(command, rtv, 0, clear, 0, NULL));
        CHECK(vkdu_command_graphics_root(command, root)); CHECK(vkdu_command_pipeline(command, pipeline));
        CHECK(vkdu_command_vertex_buffers(command, 0, 1, &vb)); CHECK(vkdu_command_topology(command, 1));
        CHECK(vkdu_command_viewports(command, 1, &viewport)); CHECK(vkdu_command_scissors(command, 1, &scissor));
        CHECK(vkdu_command_render_target(command, rasterize ? rtv : NULL, 0));
        CHECK(vkdu_command_stream_output(command, 1, 2, has_so ? views : NULL));
        REJECT(vkdu_command_stream_output(command, 4, 1, views));
        views[1].counter = foreign; REJECT(vkdu_command_stream_output(command, 1, 2, views)); views[1].counter = output;
        views[1].counter_offset = 4093; REJECT(vkdu_command_stream_output(command, 1, 2, views)); views[1].counter_offset = 1028;
        views[1].size = UINT64_MAX; REJECT(vkdu_command_stream_output(command, 1, 2, views)); views[1].size = 12 * stride2;
        if (has_so) {
            CHECK(vkdu_command_predication(command, upload, 2048, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO));
            CHECK(vkdu_command_draw(command, 1, 1, 0, 0));
            CHECK(vkdu_command_predication(command, NULL, 0, 0));
            CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE));
            CHECK(vkdu_command_copy(command, skip_count, 0, output, 1024, 8));
            CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT));
        }
        CHECK(vkdu_command_draw(command, 1, 1, 0, 0));
        /* End/restart transform feedback at a bind boundary, preserving the
         * real filled-size counters. One more draw exactly fills both views. */
        CHECK(vkdu_command_stream_output(command, 1, 2, has_so ? views : NULL));
        CHECK(vkdu_command_draw(command, 1, 1, 0, 0));
        CHECK(vkdu_command_draw(command, 1, 1, 0, 0)); /* bounded overflow */
        CHECK(vkdu_command_stream_output(command, 1, 2, NULL));
        CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 0, output, 0, 4096));
        CHECK(vkdu_command_transition(command, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command, &dst, 0, 0, 0, &src, NULL)); CHECK(vkdu_command_close(command));
        CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, fence, round + 1));
        CHECK(vkdu_fence_wait(fence, round + 1, 10000));
        if (has_so) {
            CHECK(vkdu_buffer_map(skip_count, 0, 8, &mapped));
            if (((uint32_t *)mapped)[0] || ((uint32_t *)mapped)[1]) {
                fprintf(stderr, "FAIL predicated GS/SO round%u captured %u/%u bytes while disabled\n", round,
                    ((uint32_t *)mapped)[0], ((uint32_t *)mapped)[1]); exit(1);
            }
            CHECK(vkdu_buffer_unmap(skip_count, 0, 0));
        }
        CHECK(vkdu_buffer_map(readback, 0, 4096, &mapped));
        for (i = 0; i < 1024; ++i) {
            uint32_t expected = sentinel;
            if (i == 256) expected = vertex_count * 20;
            else if (i == 257) expected = vertex_count * stride2;
            else if (partial && i >= 32 && i < 32 + vertex_count * 5) {
                const float ys[6] = {-.2f,.2f,-.2f,.2f,.2f,-.2f};
                const uint32_t col = (i - 32) % 5;
                float value = col == 1 ? 1.f : col == 2 ? (has_gs ? ys[((i - 32) / 5) % 6] : 0.f) : colors[round][2];
                if (col >= 1 && col <= 3) memcpy(&expected, &value, 4);
            } else if (partial && i >= 128 && i < 128 + vertex_count * 3) {
                const float xs[6] = {-.2f,-.2f,.2f,-.2f,.2f,.2f};
                const uint32_t col = (i - 128) % 3;
                float value = col == 0 ? colors[round][round == 7 || round == 8 ? 3 : 0] : has_gs ? xs[((i - 128) / 3) % 6] : 0.f;
                if (col < 2) memcpy(&expected, &value, 4);
            }
            else if (!partial && i >= 32 && i < 32 + vertex_count * 5 && (i - 32) % 5 < 4) {
                /* D3D strip order is ABC, BDC, preserving the first vertex
                 * of each primitive (Microsoft primitive-topology contract). */
                const float positions[6][4] = {{-.2f,-.2f,0,1},{-.2f,.2f,0,1},{.2f,-.2f,0,1},
                    {-.2f,.2f,0,1},{.2f,.2f,0,1},{.2f,-.2f,0,1}};
                memcpy(&expected, has_gs ? &positions[((i - 32) / 5) % 6][(i - 32) % 5] :
                    &position[(i - 32) % 5], 4);
            } else if (!partial && i >= 128 && i < 128 + vertex_count * 4) memcpy(&expected, &colors[round][(i - 128) % 4], 4);
            if (((uint32_t *)mapped)[i] != expected) {
                fprintf(stderr,"FAIL GS SO round%u word%u=%08x expected%08x\n",round,i,((uint32_t *)mapped)[i],expected); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0)); CHECK(vkdu_buffer_map(pixels, 0, 4096, &mapped));
        for (i = 0; i < 1024; ++i) {
            uint32_t expected = sentinel;
            if (i >= 128 && i < 768 && (i - 128) % 64 < 10) {
                const uint32_t x = (i - 128) % 64, y = (i - 128) / 64;
                expected = rasterize && x >= 4 && x < 6 && y >= 4 && y < 6 ?
                    (round == 0 ? 0xff00ff00 : round == 2 ? 0xffff00ff : round == 5 ? 0xffbf8040 : 0xff00ffff) : 0xff0000ff;
            }
            if (((uint32_t *)mapped)[i] != expected) {
                fprintf(stderr,"FAIL GS raster round%u word%u=%08x expected%08x\n",round,i,((uint32_t *)mapped)[i],expected); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(pixels, 0, 0)); vkdu_object_destroy(pipeline);
        printf("PASS actual shader stages round%u GS=%u SO=%u raster=%u partial=%u:2048 readback words, sparse targets/gaps/strides,append/bounds\n",
            round,has_gs,has_so,rasterize,partial);
    }
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps); vkdu_shader_dxbc_free(gs);
    vkdu_shader_dxbc_free(packed_vs); vkdu_shader_dxbc_free(packed_gs);
    for (i = 0; i < 2; ++i) { vkdu_shader_dxbc_free(integer_vs[i]); vkdu_shader_dxbc_free(builtin_vs[i]); }
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(vertices); vkdu_object_destroy(upload); vkdu_object_destroy(output); vkdu_object_destroy(foreign);
    vkdu_object_destroy(readback); vkdu_object_destroy(pixels); vkdu_object_destroy(rtv); vkdu_object_destroy(target); vkdu_object_destroy(root);
    vkdu_object_destroy(skip_count);
}

#ifdef VKDU_ENABLE_TEST_DEVICE
static void check_stream_output_lifetime(vkdu_device *device)
{
    uint32_t round;
    for (round = 0; round < 3; round++) {
        vkdu_object *queue, *allocator, *command, *output, *counter, *fence, *gate;
        uint32_t destroyed_output = 0, destroyed_counter = 0;
        struct vkdu_stream_output_buffer views[2];
        uint64_t completed;
        CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
        CHECK(vkdu_command_create(device, allocator, 0, &command));
        CHECK(vkdu_buffer_create(device, 4096, 1, 0, D3D12_RESOURCE_STATE_STREAM_OUT, &output));
        CHECK(vkdu_buffer_create(device, 4096, 1, 0, D3D12_RESOURCE_STATE_STREAM_OUT, &counter));
        CHECK(vkdu_fence_create(device, 0, &fence)); CHECK(vkdu_fence_create(device, 0, &gate));
        CHECK(vkdu_test_destroy_counter(output, &destroyed_output));
        CHECK(vkdu_test_destroy_counter(counter, &destroyed_counter));
        views[0] = (struct vkdu_stream_output_buffer){output, counter, 16, 1024, 4}; views[1] = views[0];
        CHECK(vkdu_command_stream_output(command, 2, 2, views));
        CHECK(vkdu_command_stream_output(command, 2, 2, views));
        CHECK(vkdu_command_stream_output(command, 2, 2, NULL));
        CHECK(vkdu_command_close(command));
        vkdu_object_destroy(output); vkdu_object_destroy(counter);
        if (vkdu_test_counter_load(&destroyed_output) || vkdu_test_counter_load(&destroyed_counter)) {
            fputs("FAIL stream output/counter owners released before recording retirement\n", stderr); exit(1);
        }
        if (round == 1) {
            CHECK(vkdu_allocator_reset(allocator));
            if (vkdu_test_counter_load(&destroyed_output) != 1 || vkdu_test_counter_load(&destroyed_counter) != 1) {
                fputs("FAIL stream output/counter owners not released at legal reset\n", stderr); exit(1);
            }
        }
        if (round == 2) {
            CHECK(vkdu_queue_wait(queue, gate, 1)); CHECK(vkdu_queue_execute(queue, 1, &command));
            CHECK(vkdu_queue_signal(queue, fence, 1)); CHECK(vkdu_queue_drain_enqueue(queue));
            CHECK(vkdu_fence_completed(fence, &completed));
            if (completed) { fputs("FAIL stream output lifetime gate did not hold GPU\n", stderr); exit(1); }
        }
        vkdu_object_destroy(command); vkdu_object_destroy(allocator);
        if (round == 2) {
            if (vkdu_test_counter_load(&destroyed_output) || vkdu_test_counter_load(&destroyed_counter)) {
                fputs("FAIL stream output/counter owners released while GPU pending\n", stderr); exit(1);
            }
            CHECK(vkdu_fence_signal_cpu(gate, 1)); CHECK(vkdu_fence_wait(fence, 1, 10000));
        }
        vkdu_object_destroy(queue); vkdu_object_destroy(fence); vkdu_object_destroy(gate);
        if (vkdu_test_counter_load(&destroyed_output) != 1 || vkdu_test_counter_load(&destroyed_counter) != 1) {
            fputs("FAIL stream output/counter owners not destroyed exactly once\n", stderr); exit(1);
        }
    }
    puts("PASS actual stream output/counter owners: duplicate binds, unbind, cancelled recording, reset and pending GPU completion");
}
#endif
