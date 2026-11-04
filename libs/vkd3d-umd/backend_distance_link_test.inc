/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Build a real consumer stage: it reads the producer's second distance
 * register and forwards those components to its output. Constants initialize
 * only the other components, so a wrong input mapping changes the final pixel. */
static uint32_t distance_link_consumer_tokens(uint32_t stage,uint32_t system,uint32_t mode,
        const uint32_t values[2][4],uint32_t words[256])
{
    uint32_t base[96],systems[]={system,system},masks[]={15,15},n=0,i,j,bn,mask=mode==2 ? 12 : 15;
    uint32_t initialized[2][4];memcpy(initialized,values,sizeof(initialized));
    for (i=0;i<4;++i)initialized[1][i]=0x3e000000; /* A missing input read cannot look correct. */
    bn=stage_distance_tokens(stage==2 ? 2 : 1,systems,masks,initialized,base);
    if (stage==4) {
        words[n++]=0x00040050;words[n++]=0;words[n++]=0x01001893;words[n++]=0x01001095;words[n++]=0x0100086a;
    } else {memcpy(words,base,12);n=3;}
    for (i=0;i<3;++i) {
        if (i==1 && mode)continue;
        words[n++]=0x05000061;words[n++]=(stage==2 ? 0x00201002 : 0x00219002)|((i==2 ? mask : 15)<<4);
        words[n++]=stage==2 ? 1 : 3;words[n++]=i;words[n++]=i ? system : 1;
    }
    for (i=3;i<bn;) {
        uint32_t length=(base[i]>>24)&127,op=base[i]&2047;
        if (op==117 || op==62)break;
        memcpy(words+n,base+i,length*4);n+=length;i+=length;
    }
    words[n++]=0x06000036;words[n++]=0x00102002|(mask<<4);words[n++]=2;
    words[n++]=stage==2 ? 0x00201e46 : 0x00219e46;words[n++]=0;words[n++]=2;
    for (j=i;j<bn;++j)words[n++]=base[j];
    words[1]=n;return n;
}
static uint32_t distance_link_hs_tokens(uint32_t system,uint32_t mode,int explicit_phase,
        const uint32_t values[2][4],uint32_t words[256])
{
    const uint32_t *source=graphics_tokens(stage_nop_hs_code_dxbc);
    uint32_t i=2,n=2,mask=mode==2 ? 12 : 15,reg;
    words[0]=source[0];words[1]=0;
    while (i<source[1] && (source[i]&2047)!=115) {
        uint32_t length=(source[i]>>24)&127;
        memcpy(words+n,source+i,length*4);
        if ((source[i]&2047)==151)words[n]=(source[i]&~(7u<<11))|(1u<<11); /* Point tessellator output. */
        n+=length;i+=length;
    }
    if (explicit_phase) {
        words[n++]=0x01000072;
        for (reg=0;reg<3;++reg) {
            if (reg==1 && mode)continue;
            words[n++]=0x05000061;words[n++]=0x00219002|((reg==2 ? mask : 15)<<4);
            words[n++]=3;words[n++]=reg;words[n++]=reg ? system : 1;
        }
        for (reg=0;reg<3;++reg) {
            words[n++]=0x04000067;words[n++]=0x001020f2;words[n++]=reg;words[n++]=reg ? system : 1;
        }
        for (reg=0;reg<3;++reg) {
            const uint32_t position[]={0xbe000000,0x3e800000,0,0x3f800000};
            words[n++]=0x08000036;words[n++]=0x001020f2;words[n++]=reg;words[n++]=0x00004002;
            memcpy(words+n,reg ? values[reg-1] : position,16);
            if (reg==2) for (uint32_t c=0;c<4;++c)words[n+c]=0x3e000000;
            n+=4;
        }
        words[n++]=0x06000036;words[n++]=0x00102002|(mask<<4);words[n++]=2;
        words[n++]=0x00219e46;words[n++]=0;words[n++]=2;words[n++]=0x0100003e;
    }
    memcpy(words+n,source+i,(source[1]-i)*4);n+=source[1]-i;words[1]=n;return n;
}
static void check_distance_link(vkdu_device *device)
{
    vkdu_object *root,*pipeline,*queue,*allocator,*command,*fence,*target,*rtv,*pixels;
    const struct vkdu_viewport viewport={0,0,8,4,0,1};
    const struct vkdu_rect scissor={0,0,8,4};
    const float clear[]={0,0,0,1};
    const struct vkdu_signature pout={0,0,15,3};
    struct vkdu_graphics_desc state={DXGI_FORMAT_R8G8B8A8_UNORM,UINT32_MAX,3,1,0,1,15,0,NULL};
    uint32_t round;void *mapped;
    const char *path_env=getenv("VKDU_TEST_DISTANCE_PATH");
    uint32_t selected_path=UINT32_MAX;
    if (path_env) {
        char *end;unsigned long value=strtoul(path_env,&end,10);
        if (!*path_env || *end || value>=6) {fprintf(stderr,"FAIL invalid distance path\n");exit(1);}
        selected_path=(uint32_t)value;
    }
    CHECK(vkdu_root_create(device,NULL,0,0,&root)); CHECK(vkdu_queue_create(device,0,&queue));
    CHECK(vkdu_allocator_create(device,0,&allocator)); CHECK(vkdu_command_create(device,allocator,0,&command));
    CHECK(vkdu_fence_create(device,0,&fence)); CHECK(vkdu_render_target_create(device,8,4,&target));
    CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,0,&rtv)); CHECK(vkdu_texture2d_rtv(rtv,0,target));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&pixels));
    state.topology_type=D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    for (round=0;round<48;++round) {
        const uint32_t path=round/8,system=2+(round%8)/4,mode=round%4,subset=mode!=0,pmask=mode==2 ? 12 : 15,systems[]={system,system},masks[]={15,15};
        const uint32_t values[2][4]={{0x3f800000,0x3e800000,0x3f000000,0x3f800000},
            {0x3e800000,0x3f000000,0x3f400000,0x3f800000}};
        struct vkdu_signature signatures[]={{1,0,15,3},{system,1,15,3},{system,2,15,3}};
        struct vkdu_signature inputs[3];
        struct vkdu_texture_copy_location src={target},dst={pixels,0,1,DXGI_FORMAT_R8G8B8A8_UNORM,8,4,1,256};
        uint32_t vs_tokens[96],ps_tokens[40],stage_tokens[256],vn,pn=0,i;void *vs,*ps,*gs=NULL,*hs=NULL,*ds=NULL;
        size_t vs_size,ps_size,gs_size=0,hs_size=0,ds_size=0;int32_t hr;
        if (selected_path!=UINT32_MAX && path!=selected_path)continue;
        vn=stage_distance_tokens(1,systems,masks,values,vs_tokens);
        CHECK(vkdu_shader_dxbc(1,vs_tokens,vn,NULL,0,signatures,3,&vs,&vs_size));
        ps_tokens[pn++]=0x50;ps_tokens[pn++]=0;ps_tokens[pn++]=0x0100086a;
        for (i=0;i<3;++i) {
            if (i==1 && subset) continue;
            ps_tokens[pn++]=i ? 0x04001064 : 0x04002064;ps_tokens[pn++]=0x00101002|((i==2 ? pmask : 15)<<4);
            ps_tokens[pn++]=i;ps_tokens[pn++]=i ? system : 1;
        }
        ps_tokens[pn++]=0x03000065;ps_tokens[pn++]=0x001020f2;ps_tokens[pn++]=0;
        if (mode==2) {
            ps_tokens[pn++]=0x08000036;ps_tokens[pn++]=0x001020f2;ps_tokens[pn++]=0;ps_tokens[pn++]=0x00004002;
            ps_tokens[pn++]=0;ps_tokens[pn++]=0;ps_tokens[pn++]=0;ps_tokens[pn++]=0x3f800000;
        }
        ps_tokens[pn++]=0x05000036;ps_tokens[pn++]=mode==2 ? 0x00102032 : 0x001020f2;ps_tokens[pn++]=0;
        ps_tokens[pn++]=mode==2 ? 0x00101ee6 : 0x00101e46;ps_tokens[pn++]=2;
        ps_tokens[pn++]=0x0100003e;ps_tokens[1]=pn;
        /* Full runtime input metadata is deliberately retained even when the
         * consumer token declarations omit r1: the production filter sees it. */
        memcpy(inputs,signatures,sizeof(inputs));inputs[2].mask=pmask;
        if (mode==3)inputs[1]=inputs[2];
        CHECK(vkdu_shader_dxbc(0,ps_tokens,pn,inputs,mode==3 ? 2 : 3,&pout,1,&ps,&ps_size));
        if (path==1 || path==2 || path==5) {
            vn=path==1 ? stage_distance_tokens(2,systems,masks,values,stage_tokens) :
                distance_link_consumer_tokens(2,system,mode,values,stage_tokens);
            CHECK(vkdu_shader_dxbc(2,stage_tokens,vn,path==1 ? NULL : inputs,path==1 ? 0 : mode==3 ? 2 : 3,
                signatures,3,&gs,&gs_size));
        }
        if (path>=3) {
            const struct vkdu_signature patch[]={{17,0,1,3},{18,1,1,3},{19,2,1,3},{20,3,1,3}};
            vn=distance_link_hs_tokens(system,mode,path>=4,values,stage_tokens);
            CHECK(vkdu_shader_tess_dxbc(3,stage_tokens,vn,path==3 ? signatures : inputs,
                path>=4 && mode==3 ? 2 : 3,signatures,3,patch,4,&hs,&hs_size));
            vn=distance_link_consumer_tokens(4,system,mode,values,stage_tokens);
            CHECK(vkdu_shader_tess_dxbc(4,stage_tokens,vn,signatures,3,signatures,3,patch,4,&ds,&ds_size));
        }
        state.gs=gs;state.gs_size=gs_size;state.hs=hs;state.hs_size=hs_size;state.ds=ds;state.ds_size=ds_size;
        state.topology_type=path>=3 ? 4 : 1;
        pipeline=NULL;hr=vkdu_graphics_pipeline_create(device,root,vs,vs_size,ps,ps_size,&state,&pipeline);
        if (hr<0 || !pipeline) {
            fprintf(stderr,"FAIL distance-link round%u system%u consumer-second-only%u PSO=%08x\n",round,system,subset,(uint32_t)hr);exit(1);
        }
        memset(vs,0xcc,vs_size);memset(ps,0xcc,ps_size);vkdu_shader_dxbc_free(vs);vkdu_shader_dxbc_free(ps);
        if(gs){memset(gs,0xcc,gs_size);vkdu_shader_dxbc_free(gs);}
        if(hs){memset(hs,0xcc,hs_size);vkdu_shader_dxbc_free(hs);}
        if(ds){memset(ds,0xcc,ds_size);vkdu_shader_dxbc_free(ds);}
        if (round && (selected_path==UINT32_MAX || round%8)) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command,allocator));
            CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));
        }
        CHECK(vkdu_command_graphics_root(command,root));CHECK(vkdu_command_pipeline(command,pipeline));
        CHECK(vkdu_command_targets(command,rtv,0,NULL,0));CHECK(vkdu_command_clear_rtv(command,rtv,0,clear,0,NULL));
        CHECK(vkdu_command_viewports(command,1,&viewport));CHECK(vkdu_command_scissors(command,1,&scissor));
        CHECK(vkdu_command_topology(command,path>=3 ? 35 : 1));CHECK(vkdu_command_draw(command,path>=3 ? 3 : 1,1,0,0));
        CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command,&dst,0,0,0,&src,NULL));CHECK(vkdu_command_close(command));
        CHECK(vkdu_queue_execute(queue,1,&command));CHECK(vkdu_queue_signal(queue,fence,round+1));CHECK(vkdu_fence_wait(fence,round+1,10000));
        CHECK(vkdu_buffer_map(pixels,0,1024,&mapped));
        for (i=0;i<4*64;++i) if (i%64<8) {
            uint32_t expected=i==64+3 ? (mode==2 ? 0xff00ffbf : 0xffbf8040) : 0xff000000;
            if (((uint32_t *)mapped)[i]!=expected) {
                fprintf(stderr,"FAIL distance-link round%u pixel%u=%08x expected%08x\n",round,i,((uint32_t *)mapped)[i],expected);exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(pixels,0,0));vkdu_object_destroy(pipeline);
        printf("PASS actual distance-link round%u path%u system%u consumer-second-only%u: actual r2 reads through stages into32 checked pixels\n",round,path,system,subset);
    }
    vkdu_object_destroy(command);vkdu_object_destroy(allocator);vkdu_object_destroy(queue);vkdu_object_destroy(fence);
    vkdu_object_destroy(root);vkdu_object_destroy(target);vkdu_object_destroy(rtv);vkdu_object_destroy(pixels);
}
