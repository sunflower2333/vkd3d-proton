/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_multistream_raster(vkdu_device *device)
{
    vkdu_object *root, *pipeline, *queue, *allocator, *command, *fence, *vertices, *upload, *output, *readback;
    vkdu_object *target, *rtv, *pixels;
    const struct vkdu_signature vin[] = {{0,0,15,3},{0,1,15,3}}, link[] = {{1,0,15,3},{0,1,15,3}}, pout = {0,0,15,3};
    const uint32_t colors[4] = {0xff0000ff,0xff00ff00,0xffff0000,0xff00ffff};
    const float color[4][4] = {{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,0,1}}, clear[4] = {0,0,0,1};
    const struct vkdu_viewport viewport = {0,0,8,4,0,1};
    const struct vkdu_rect scissor = {0,0,8,4};
    struct vkdu_input_element inputs[] = {{0,0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,0},
        {0,16,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,1}};
    struct vkdu_graphics_desc state = {DXGI_FORMAT_R8G8B8A8_UNORM,UINT32_MAX,3,1,0,1,15,2,inputs};
    const uint32_t *vt = graphics_tokens(unaligned_vertex_stride_vs_code_dxbc), *pt = graphics_tokens(unaligned_vertex_stride_ps_code_dxbc);
    uint32_t gt[512], gn = stage_multistream_raster_tokens(gt), round, i, s;
    void *vs, *ps, *gs, *mapped; size_t vs_size, ps_size, gs_size;
    CHECK(vkdu_shader_dxbc(1,vt,vt[1],vin,2,link,2,&vs,&vs_size));
    CHECK(vkdu_shader_dxbc(0,pt,pt[1],link,2,&pout,1,&ps,&ps_size));
    CHECK(vkdu_shader_dxbc(2,gt,gn,NULL,0,link,2,&gs,&gs_size));
    CHECK(vkdu_root_create(device,NULL,0,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT,&root));
    CHECK(vkdu_queue_create(device,0,&queue)); CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command)); CHECK(vkdu_fence_create(device,0,&fence));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&vertices));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&upload));
    CHECK(vkdu_buffer_create(device,4096,1,0,D3D12_RESOURCE_STATE_COPY_DEST,&output));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&pixels));
    CHECK(vkdu_render_target_create(device,8,4,&target));
    CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,0,&rtv)); CHECK(vkdu_texture2d_rtv(rtv,0,target));
    CHECK(vkdu_buffer_map(vertices,0,0,&mapped)); memset(mapped,0,4096); CHECK(vkdu_buffer_unmap(vertices,0,4096));
    state.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; state.gs = gs; state.gs_size = gs_size;
    for (round = 0; round < 10; ++round) {
        const uint32_t selected=round<5 ? (round<4 ? round : UINT32_MAX) : round==5 ? 0 : round==9 ? UINT32_MAX : round-5;
        struct vkdu_stream_output_desc so = {0,4,selected,{{0}},{20,20,20,20}};
        struct vkdu_stream_output_buffer views[4];
        struct vkdu_vertex_buffer vb = {vertices,64,32,32};
        struct vkdu_texture_copy_location src = {target}, dst = {pixels,512,1,DXGI_FORMAT_R8G8B8A8_UNORM,8,4,1,256};
        const uint32_t sentinel = 0x7e620000 + round;
        uint32_t expected[1024], v, c;
        for (i = 0; i < 1024; ++i) expected[i] = sentinel;
        for (s = 0; s < 4; ++s) {
            so.entries[so.entry_count++] = (struct vkdu_stream_output_entry){s,s,0,8};
            so.entries[so.entry_count++] = (struct vkdu_stream_output_entry){s,s,1,15};
            views[s] = (struct vkdu_stream_output_buffer){output,output,128+s*256,40,2048+s*4}; expected[512+s] = 0;
        }
        if (round>=5)so.entry_count=so.stride_count=0;
        state.stream_output = round==5 ? NULL : &so;
        CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,selected < 4 ? ps : NULL,selected < 4 ? ps_size : 0,&state,&pipeline));
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command,allocator));
            CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST));
            CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));
        }
        CHECK(vkdu_buffer_map(upload,0,0,&mapped)); memcpy(mapped,expected,sizeof(expected)); CHECK(vkdu_buffer_unmap(upload,0,sizeof(expected)));
        CHECK(vkdu_command_copy(command,output,0,upload,0,sizeof(expected))); CHECK(vkdu_command_copy(command,pixels,0,upload,0,sizeof(expected)));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_STREAM_OUT));
        CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_pipeline(command,pipeline));
        CHECK(vkdu_command_targets(command,rtv,0,NULL,0)); CHECK(vkdu_command_clear_rtv(command,rtv,0,clear,0,NULL));
        CHECK(vkdu_command_viewports(command,1,&viewport)); CHECK(vkdu_command_scissors(command,1,&scissor));
        CHECK(vkdu_command_vertex_buffers(command,0,1,&vb)); CHECK(vkdu_command_topology(command,1));
        for (i = 0; i < 3; ++i) { CHECK(vkdu_command_stream_output(command,0,4,views)); CHECK(vkdu_command_draw(command,1,1,0,0)); }
        CHECK(vkdu_command_stream_output(command,0,4,NULL));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command,readback,0,output,0,sizeof(expected))); CHECK(vkdu_command_texture_copy(command,&dst,0,0,0,&src,NULL));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue,1,&command));
        CHECK(vkdu_queue_signal(queue,fence,round+1)); CHECK(vkdu_fence_wait(fence,round+1,10000));
        for (s = 0; round<5 && s < 4; ++s) {
            expected[512+s] = 40;
            for (v = 0; v < 2; ++v) {
                expected[32+s*64+v*5] = 0x3f800000;
                for (c = 0; c < 4; ++c) memcpy(&expected[33+s*64+v*5+c],&color[s][c],4);
            }
        }
        CHECK(vkdu_buffer_map(readback,0,sizeof(expected),&mapped));
        for (i = 0; i < 1024; ++i) if (((uint32_t *)mapped)[i] != expected[i]) {
            fprintf(stderr,"FAIL stream raster%u SO word%u=%08x expected%08x\n",round,i,((uint32_t *)mapped)[i],expected[i]); exit(1);
        }
        CHECK(vkdu_buffer_unmap(readback,0,0)); CHECK(vkdu_buffer_map(pixels,0,sizeof(expected),&mapped));
        for (i = 0; i < 1024; ++i) {
            uint32_t e = i < 128 || i >= 384 || (i-128)%64 >= 8 ? (i >= 512 && i < 516 ? 0 : sentinel) :
                (selected < 4 && i == 128+64+1+2*selected ? colors[selected] : 0xff000000);
            if (((uint32_t *)mapped)[i] != e) {
                fprintf(stderr,"FAIL stream raster%u pixel word%u=%08x expected%08x\n",round,i,((uint32_t *)mapped)[i],e); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(pixels,0,0)); vkdu_object_destroy(pipeline);
        printf("PASS actual raster round%u selected%u SO%d:2048 exact SO/pixel words, repeated semantic/Position,partial mirrors,SO overflow independent raster\n",round,selected,round<5);
    }
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps); vkdu_shader_dxbc_free(gs);
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(root); vkdu_object_destroy(vertices); vkdu_object_destroy(upload); vkdu_object_destroy(output); vkdu_object_destroy(readback);
    vkdu_object_destroy(target); vkdu_object_destroy(rtv); vkdu_object_destroy(pixels);
}
