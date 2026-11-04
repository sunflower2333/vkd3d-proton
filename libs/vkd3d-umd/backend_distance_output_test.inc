/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_distance_output(vkdu_device *device)
{
    vkdu_object *root,*pipeline,*queue,*allocator,*command,*fence,*upload,*output,*readback,*target,*rtv,*pixels;
    const uint32_t ps_tokens[]={0x50,15,0x0100086a,0x03000065,0x001020f2,0,
        0x08000036,0x001020f2,0,0x00004002,0x3f800000,0,0,0x3f800000,0x0100003e};
    const struct vkdu_signature pout={0,0,15,3};
    const struct vkdu_viewport viewport={0,0,8,4,0,1};
    const struct vkdu_rect scissor={0,0,8,4};
    const float clear[]={0,0,0,1};
    struct vkdu_graphics_desc state={DXGI_FORMAT_R8G8B8A8_UNORM,UINT32_MAX,3,1,0,1,15,0,NULL};
    void *ps,*mapped; size_t ps_size; uint32_t round;
    CHECK(vkdu_shader_dxbc(0,ps_tokens,15,NULL,0,&pout,1,&ps,&ps_size));
    CHECK(vkdu_root_create(device,NULL,0,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT,&root));
    CHECK(vkdu_queue_create(device,0,&queue)); CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command)); CHECK(vkdu_fence_create(device,0,&fence));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&upload));
    CHECK(vkdu_buffer_create(device,4096,1,0,D3D12_RESOURCE_STATE_COPY_DEST,&output));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&pixels));
    CHECK(vkdu_render_target_create(device,8,4,&target));
    CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,0,&rtv)); CHECK(vkdu_texture2d_rtv(rtv,0,target));
    state.topology_type=D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    for (round=0;round<24;++round) {
        const uint32_t gs=round/12, shape=(round/4)%3, system=2+(round/2)%2, clipped=round&1;
        const uint32_t systems[]={system,shape==2 ? 5-system : system}, masks[]={shape==1 ? 14 : 15,shape==1 ? 12 : 15};
        uint32_t values[2][4]={{0x3fa00000,0x40200000,0x40700000,0x40900000},
            {0x40a80000,0x40d00000,0x40f80000,0x41080000}};
        struct vkdu_signature signature[]={{1,0,15,3},{systems[1],2,masks[1],3},{systems[0],1,masks[0],3}};
        struct vkdu_stream_output_desc so={0,2,0,{{0}},{8,4}};
        struct vkdu_stream_output_buffer views[2];
        struct vkdu_texture_copy_location src={target},dst={pixels,512,1,DXGI_FORMAT_R8G8B8A8_UNORM,8,4,1,256};
        uint32_t tokens[96], vn, gn, g_tokens[96], expected[1024], i,j,c,v, counts[2]={0}, offsets[2]={128,1024};
        const uint32_t sentinel=0x7e650000+round;
        void *vs,*geometry=NULL; size_t vs_size,gs_size=0;
        if (clipped) values[1][3]|=0x80000000u;
        vn=stage_distance_tokens(1,systems,masks,values,tokens);
        CHECK(vkdu_shader_dxbc(1,tokens,vn,NULL,0,signature,3,&vs,&vs_size));
        if (gs) {
            gn=stage_distance_tokens(2,systems,masks,values,g_tokens);
            CHECK(vkdu_shader_dxbc(2,g_tokens,gn,NULL,0,signature,3,&geometry,&gs_size));
        }
        if (clipped && shape!=2) {
            /* Legal API signatures can number semantics in the opposite
             * order from registers. Exercise complete-signature mapping. */
            uint32_t *words=gs ? geometry : vs, *sig=words+words[9]/4+2;
            const uint32_t entry_words=gs ? 7 : 6, index_word=gs ? 2 : 1;
            for (i=0;i<3;++i) {
                uint32_t *e=sig+2+i*entry_words;
                if (e[index_word+1]==system) e[index_word]=1-e[index_word];
            }
            vkd3d_compute_dxbc_checksum(words,gs ? gs_size : vs_size,words+1);
        }
        /* Capture all distance values in reverse component order across two
         * buffers, with a gap and trailing padding. 8 values are real payload. */
        for (j=2;j;--j) {
            if (j==1) so.entries[so.entry_count++]=(struct vkdu_stream_output_entry){0,0,UINT32_MAX,1};
            for (c=4;c;--c) if (masks[j-1]&(1u<<(c-1))) {
                so.entries[so.entry_count++]=(struct vkdu_stream_output_entry){0,j-1,j,1u<<(c-1)};
                so.strides[j-1]+=4; ++counts[j-1];
            }
        }
        for (j=0;j<2;++j) views[j]=(struct vkdu_stream_output_buffer){output,output,offsets[j],2*so.strides[j],3072+j*4};
        state.stream_output=&so; state.gs=geometry; state.gs_size=gs_size;
        CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,ps,ps_size,&state,&pipeline));
        /* Compiler/container lifetimes must not depend on caller storage. */
        memset(vs,0xcc,vs_size); vkdu_shader_dxbc_free(vs);
        if (geometry) { memset(geometry,0xcc,gs_size); vkdu_shader_dxbc_free(geometry); }
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command,allocator));
            CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST));
            CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));
        }
        for (i=0;i<1024;++i) expected[i]=sentinel;
        expected[768]=expected[769]=0;
        CHECK(vkdu_buffer_map(upload,0,0,&mapped)); memcpy(mapped,expected,sizeof(expected)); CHECK(vkdu_buffer_unmap(upload,0,sizeof(expected)));
        CHECK(vkdu_command_copy(command,output,0,upload,0,sizeof(expected))); CHECK(vkdu_command_copy(command,pixels,0,upload,0,sizeof(expected)));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_STREAM_OUT));
        CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_pipeline(command,pipeline));
        CHECK(vkdu_command_targets(command,rtv,0,NULL,0)); CHECK(vkdu_command_clear_rtv(command,rtv,0,clear,0,NULL));
        CHECK(vkdu_command_viewports(command,1,&viewport)); CHECK(vkdu_command_scissors(command,1,&scissor)); CHECK(vkdu_command_topology(command,1));
        for (i=0;i<3;++i) { CHECK(vkdu_command_stream_output(command,0,2,views)); CHECK(vkdu_command_draw(command,1,1,0,0)); }
        CHECK(vkdu_command_stream_output(command,0,2,NULL));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command,readback,0,output,0,sizeof(expected))); CHECK(vkdu_command_texture_copy(command,&dst,0,0,0,&src,NULL));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue,1,&command));
        CHECK(vkdu_queue_signal(queue,fence,round+1)); CHECK(vkdu_fence_wait(fence,round+1,10000));
        for (j=0;j<2;++j) {
            expected[768+j]=2*so.strides[j];
            for (v=0;v<2;++v) {
                uint32_t word=offsets[j]/4+v*so.strides[j]/4+(j==0);
                for (c=4;c;--c) if (masks[j]&(1u<<(c-1))) expected[word++]=values[j][c-1];
            }
        }
        CHECK(vkdu_buffer_map(readback,0,sizeof(expected),&mapped));
        for (i=0;i<1024;++i) if (((uint32_t *)mapped)[i]!=expected[i]) {
            fprintf(stderr,"FAIL distance round%u GS%u shape%u SO word%u=%08x expected%08x\n",round,gs,shape,i,((uint32_t *)mapped)[i],expected[i]); exit(1);
        }
        CHECK(vkdu_buffer_unmap(readback,0,0)); CHECK(vkdu_buffer_map(pixels,0,sizeof(expected),&mapped));
        for (i=0;i<1024;++i) {
            uint32_t e=i<128 || i>=384 || (i-128)%64>=8 ? (i==768 || i==769 ? 0 : sentinel) :
                (!clipped && i==128+64+3 ? 0xff0000ff : 0xff000000);
            if (((uint32_t *)mapped)[i]!=e) {
                fprintf(stderr,"FAIL distance round%u GS%u clipped%u pixel%u=%08x expected%08x\n",round,gs,clipped,i,((uint32_t *)mapped)[i],e); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(pixels,0,0)); vkdu_object_destroy(pipeline);
        printf("PASS actual distance round%u GS%u shape%u systems%u/%u clipped%u:2048 exact SO/pixel words, %u payload components, reversed semantics/masks, append/overflow, copied code\n",
            round,gs,shape,systems[0],systems[1],clipped,counts[0]+counts[1]);
    }
    vkdu_shader_dxbc_free(ps); vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(root); vkdu_object_destroy(upload); vkdu_object_destroy(output); vkdu_object_destroy(readback);
    vkdu_object_destroy(target); vkdu_object_destroy(rtv); vkdu_object_destroy(pixels);
}
