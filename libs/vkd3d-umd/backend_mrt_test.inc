/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_ENABLE_TEST_DEVICE
/* SM5 assembly fixture: dcl_output o0..o7; mov oN, l(color[N]); ret.
 * Equivalent HLSL has eight float4 fields with SV_Target0..SV_Target7.
 * The oracle below uses these literal colors, independently of PSO/OM state. */
static const float mrt_colors[8][4] = {
    {0.125f,0.25f,0.375f,0.5f}, {0.25f,0.375f,0.5f,0.625f},
    {0.375f,0.5f,0.625f,0.75f}, {0.5f,0.625f,0.75f,0.875f},
    {0.625f,0.75f,0.875f,0.125f}, {0.75f,0.875f,0.125f,0.25f},
    {0.875f,0.125f,0.25f,0.375f}, {0.1875f,0.3125f,0.4375f,0.5625f}
};
static void mrt_shader(void **code, size_t *size)
{
    uint32_t words[92] = {0x50,92,0x0100086a}, pos = 3, i;
    struct vkdu_signature outputs[8];
    for (i = 0; i < 8; ++i) {
        words[pos++] = 0x03000065; words[pos++] = 0x001020f2; words[pos++] = i;
        outputs[i] = (struct vkdu_signature){0,i,15,3};
    }
    for (i = 0; i < 8; ++i) {
        words[pos++] = 0x08000036; words[pos++] = 0x001020f2; words[pos++] = i; words[pos++] = 0x00004002;
        memcpy(words + pos, mrt_colors[i], 16); pos += 4;
    }
    words[pos++] = 0x0100003e;
    if (pos != 92) abort();
    CHECK(vkdu_shader_dxbc(0, words, pos, NULL, 0, outputs, 8, code, size));
}
static void check_mrt(vkdu_device *device, vkdu_device *other)
{
    const uint32_t formats[8] = {28,87,29,91,28,87,29,91};
    const uint32_t masks[8] = {1,2,4,8,3,5,10,15}, permutation[8] = {7,5,3,1,6,4,2,0};
    const float clear[4] = {0.0625f,0.1875f,0.3125f,0.4375f}, factor[4] = {0.25f,0.5f,0.75f,0.125f};
    const struct vkdu_viewport viewport = {0,0,4,4,0,1};
    const struct vkdu_rect scissor = {1,1,3,3};
    const struct vkdu_signature vin = {6,0,1,1}, vout = {1,0,15,3};
    const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc);
    vkdu_object *root, *targets[8], *heaps[2], *pipeline = NULL, *queue, *allocator, *command, *fence, *readback;
    struct vkdu_render_target_desc states[8];
    struct vkdu_render_target_binding bindings[8];
    struct vkdu_graphics_desc desc = {28,UINT32_MAX,3,1,0,1,15};
    void *vs = NULL, *ps = NULL; size_t vs_size = 0, ps_size = 0;
    uint32_t i, round, *mapped, compared = 0;
    CHECK(vkdu_shader_dxbc(1,v,v[1],&vin,1,&vout,1,&vs,&vs_size)); mrt_shader(&ps,&ps_size);
    CHECK(vkdu_root_create(device,NULL,0,0,&root));
    CHECK(vkdu_heap_create(device,2,16,0,&heaps[0])); CHECK(vkdu_heap_create(device,2,16,0,&heaps[1]));
    for (i = 0; i < 8; ++i) {
        struct vkdu_texture2d_shape shape = {i & 1 ? 8 : 4,i & 1 ? 8 : 4,formats[i],i & 1 ? 2 : 1,i & 1 ? 2 : 1,1, 1, 0};
        if (i & 1) {
            vkdu_object *memory; uint64_t bytes, align;
            CHECK(vkdu_texture2d_shape_allocation(device,&shape,&bytes,&align));
            CHECK(vkdu_test_attachment_heap_create(device,bytes*2,&memory));
            CHECK(vkdu_texture2d_shape_place(device,memory,bytes,&shape,D3D12_RESOURCE_STATE_RENDER_TARGET,&targets[i]));
            vkdu_object_destroy(memory);
        } else CHECK(vkdu_texture2d_shape_create(device,&shape,&targets[i]));
        CHECK(vkdu_texture2d_rtv_range(heaps[0],i+4,targets[i],formats[i],i&1,i&1,1));
        bindings[i] = (struct vkdu_render_target_binding){heaps[0],i+4};
    }
    CHECK(vkdu_queue_create(device,0,&queue)); CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command)); CHECK(vkdu_fence_create(device,0,&fence));
    CHECK(vkdu_buffer_create(device,16384,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    for (round = 0; round < 4; ++round) {
        uint32_t inverse[8];
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command,allocator));
            for (i=0;i<8;++i) subresource_test_transition(command,targets[i],i&1?3:0,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
            vkdu_object_destroy(pipeline); pipeline = NULL;
        }
        for (i=0;i<8;++i) {
            uint32_t physical = round == 3 ? permutation[i] : i;
            inverse[physical] = i;
            states[i] = (struct vkdu_render_target_desc){formats[physical],round == 1 ? masks[i] : round == 2 ? 7 : 15,
                round == 1 ? i % 4 != 0 : round == 2, D3D12_BLEND_BLEND_FACTOR,D3D12_BLEND_INV_BLEND_FACTOR,
                D3D12_BLEND_OP_ADD,D3D12_BLEND_BLEND_FACTOR,D3D12_BLEND_INV_BLEND_FACTOR,D3D12_BLEND_OP_ADD};
            if (round == 1 && i % 4 == 2) {
                states[i].src_blend = states[i].dst_blend = D3D12_BLEND_ONE;
                states[i].blend_op = D3D12_BLEND_OP_SUBTRACT;
                states[i].src_blend_alpha = D3D12_BLEND_ZERO; states[i].dst_blend_alpha = D3D12_BLEND_ONE;
            } else if (round == 1 && i % 4 == 3) {
                states[i].blend_op = D3D12_BLEND_OP_MIN; states[i].blend_op_alpha = D3D12_BLEND_OP_MAX;
            }
            bindings[i] = (struct vkdu_render_target_binding){heaps[0],physical+4};
            if (round == 2) {
                CHECK(vkdu_descriptor_copy(heaps[1],i*2,heaps[0],physical+4,1));
                bindings[i] = (struct vkdu_render_target_binding){heaps[1],i*2};
            }
        }
        if (round == 3) {
            struct vkdu_descriptor_span from[8], to[8];
            for (i=0;i<8;++i) {
                from[i] = (struct vkdu_descriptor_span){heaps[0],permutation[i]+4,1};
                to[i] = (struct vkdu_descriptor_span){heaps[1],i*2,1};
                bindings[i] = (struct vkdu_render_target_binding){heaps[1],i*2};
            }
            CHECK(vkdu_descriptor_copy_ranges(device,2,8,to,8,from));
        }
#ifndef VKDU_TEST_MRT_BASELINE_ADAPTER
        desc.render_targets = states; desc.render_target_count = 8;
#else
        /* Explicit legacy adapter control: old production binds only slot 0. */
        desc.format = states[0].format;
#endif
        CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,ps,ps_size,&desc,&pipeline));
        CHECK(vkdu_buffer_map(readback,0,0,(void **)&mapped));
        for (i=0;i<4096;++i) mapped[i] = 0xabcddcba;
        CHECK(vkdu_buffer_unmap(readback,0,16384));
        for (i=0;i<8;++i) CHECK(vkdu_command_clear_rtv(command,heaps[0],i+4,clear,0,NULL));
        CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_pipeline(command,pipeline));
        CHECK(vkdu_command_topology(command,4)); CHECK(vkdu_command_viewports(command,1,&viewport));
        CHECK(vkdu_command_scissors(command,1,&scissor)); CHECK(vkdu_command_blend_factor(command,factor));
#ifndef VKDU_TEST_MRT_BASELINE_ADAPTER
        CHECK(vkdu_command_render_targets(command,8,bindings,NULL,0));
        {
            struct vkdu_render_target_binding invalid[8]; memcpy(invalid,bindings,sizeof(invalid));
            invalid[7].index = UINT32_MAX;
            REJECT(vkdu_command_render_targets(command,8,invalid,NULL,0));
            REJECT(vkdu_command_render_targets(command,9,bindings,NULL,0));
            REJECT(vkdu_command_render_targets(command,UINT32_MAX,bindings,NULL,0));
            REJECT(vkdu_command_render_targets(command,1,NULL,NULL,0));
        }
#else
        CHECK(vkdu_command_render_target(command,bindings[0].heap,bindings[0].index));
#endif
        CHECK(vkdu_command_draw(command,3,1,0,0));
        for (i=0;i<8;++i) {
            struct vkdu_texture_copy_location from = {targets[i],i & 1 ? 3 : 0};
            struct vkdu_texture_copy_location to = {readback,512+i*2048,1,formats[i],4,4,1,256};
            subresource_test_transition(command,targets[i],i&1?3:0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            CHECK(vkdu_command_texture_copy(command,&to,0,0,0,&from,NULL));
        }
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue,1,&command));
        CHECK(vkdu_queue_signal(queue,fence,round+1)); CHECK(vkdu_fence_wait(fence,round+1,10000));
        CHECK(vkdu_buffer_map(readback,0,16384,(void **)&mapped));
        for (i=0;i<4096;++i) {
            uint32_t physical = i / 512, within = i % 512;
            if (physical < 8 && within >= 128 && within < 384 && (within-128)%64 < 4) {
                uint32_t x = (within-128)%64, y = (within-128)/64, output = inverse[physical], c;
                uint32_t mask = round == 1 ? masks[output] : round == 2 ? 7 : 15;
                for (c=0;c<4;++c) {
                    float want = clear[c], source = mrt_colors[output][c];
                    uint32_t byte = (formats[physical] == 87 || formats[physical] == 91) && c < 3 ? 2-c : c;
                    int got = (mapped[i] >> (byte*8)) & 255, expected;
                    if (x>=1 && x<3 && y>=1 && y<3 && (mask & (1u<<c))) {
                        want = source;
                        if (round == 2 || (round == 1 && output%4 == 1)) want = source*factor[c]+clear[c]*(1-factor[c]);
                        else if (round == 1 && output%4 == 2) want = c<3 ? fmaxf(0,source-clear[c]) : clear[c];
                        else if (round == 1 && output%4 == 3) want = c<3 ? fminf(source,clear[c]) : fmaxf(source,clear[c]);
                    }
                    if ((formats[physical] == 29 || formats[physical] == 91) && c<3) want=test_linear_to_srgb(want);
                    expected=(int)floorf(want*255+0.5f);
                    if (abs(got-expected)>1) {
                        fprintf(stderr,"FAIL MRT round%u physical%u output%u pixel%u,%u channel%u=%u expected%u\n",round,physical,output,x,y,c,got,expected);exit(1);
                    }
                    ++compared;
                }
            } else if (mapped[i] != 0xabcddcba) { fprintf(stderr,"FAIL MRT padding round%u word%u\n",round,i);exit(1); }
        }
        CHECK(vkdu_buffer_unmap(readback,0,0));
        printf("PASS actual MRT round%u:8 distinct outputs,4 typed formats,128 pixels,3968 padding words,clipped draw,mixed effective mip/layer views%s\n",
            round,round==1?",independent blend/write masks":round==2?",broadcast blend/simple copies":round==3?",permuted range copies":"");
    }
    if (compared != 2048) abort();
    CHECK(vkdu_device_status(device));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(readback); vkdu_object_destroy(pipeline); vkdu_object_destroy(root);
    for (i=0;i<8;++i) vkdu_object_destroy(targets[i]);
    vkdu_object_destroy(heaps[0]); vkdu_object_destroy(heaps[1]);
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps);
    (void)other;
}
#endif
