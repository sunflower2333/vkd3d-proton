/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "sm51_test_shaders.h"
void vkd3d_compute_dxbc_checksum(const void *dxbc, size_t size, uint32_t checksum[4]);
static void check_sm51_converter(uint32_t index)
{
    const struct sm51_fixture *f = &sm51_fixtures[index];
    uint32_t words;
    const uint32_t *tokens = sm51_chunk(f->container, f->size, 0x58454853, &words);
    void *code = (void *)(uintptr_t)1; size_t size = 1;
    int32_t hr = vkdu_shader_dxbc(f->stage, tokens, words, f->inputs, f->input_count,
        f->outputs, f->output_count, &code, &size);
    if (hr != S_OK) {
        fprintf(stderr, "FAIL SM51 converter %s returned %08x; output_cleared=%u\n",
            f->name, (unsigned)hr, code == NULL && size == 0); exit(1);
    }
    if (!sm51_preserved(f, code, size)) {
        fprintf(stderr, "FAIL SM51 converter %s SHEX/signature preservation\n", f->name); exit(1);
    }
    {
        uint32_t checksum[4];
        vkd3d_compute_dxbc_checksum(code,size,checksum);
        if (memcmp(checksum,(const uint32_t *)code+1,sizeof(checksum))) {
            fprintf(stderr,"FAIL SM51 converter %s checksum\n",f->name);exit(1);
        }
    }
    vkdu_shader_dxbc_free(code);
    /* R0 signatures may be a strict superset. Unused entries must disappear. */
    {
        struct vkdu_signature inputs[4], outputs[2];
        memcpy(inputs,f->inputs,sizeof(f->inputs));memcpy(outputs,f->outputs,sizeof(f->outputs));
        inputs[f->input_count]=(struct vkdu_signature){0,31,15,3};
        outputs[f->output_count]=(struct vkdu_signature){0,31,15,3};
        CHECK(vkdu_shader_dxbc(f->stage,tokens,words,inputs,f->input_count+1,outputs,f->output_count+1,&code,&size));
        if (!sm51_preserved(f,code,size)) {fprintf(stderr,"FAIL SM51 %s superset filtering\n",f->name);exit(1);}
        vkdu_shader_dxbc_free(code);
    }
    printf("PASS SM51 converter %s full original SHEX %u words and signatures\n", f->name, words);
}
static void check_sm51_rejections(void)
{
    uint32_t fixture, which;
    for (fixture=2;fixture<=4;fixture+=2) {
        const struct sm51_fixture *f=&sm51_fixtures[fixture];
        uint32_t words;const uint32_t *original=sm51_chunk(f->container,f->size,0x58454853,&words);
        for (which=0;which<7;++which) {
            uint32_t tokens[256],count=words,stage=f->stage;
            void *code=(void *)(uintptr_t)1;size_t size=1;int32_t hr;
            memcpy(tokens,original,words*4);
            if (which==0) tokens[0]=(tokens[0]&~255u)|0x52;
            if (which==1) tokens[0]=(tokens[0]&~255u)|0x41;
            if (which==2) stage^=1;
            if (which==3) --count;
            if (which==4) tokens[1]=1;
            if (which==5) tokens[2]&=0x00ffffff;
            if (which==6) tokens[2]=(tokens[2]&0x00ffffff)|0x7f000000;
            hr=vkdu_shader_dxbc(stage,tokens,count,f->inputs,f->input_count,f->outputs,f->output_count,&code,&size);
            if (hr!=E_INVALIDARG || code || size) {
                fprintf(stderr,"FAIL SM51 rejection fixture%u case%u hr%08x output_cleared=%u\n",
                    fixture,which,(unsigned)hr,code==NULL && !size);exit(1);
            }
        }
    }
    puts("PASS SM51 converter 14 unsupported version/stage/length/declaration rejection controls");
}

static void check_sm51_vs_pixels(vkdu_device *device)
{
    /* Six explicit rectangles. The shader consumes original CBV element0/11,
     * b0 in distinct spaces, SRV offset64 and SV_InstanceID. No shader math is
     * reused to calculate these independently tabulated 8x8 pixel masks. */
    static const float vertices[12]={-1,-1, 1,-1, -1,1, -1,1, 1,-1, 1,1};
    static const struct {float x,w,z;uint32_t first_instance;uint8_t rows[8];} cases[]={
        { 1,1,.25f,0,{0xf0,0xf0,0xf0,0xf0,0xf0,0xf0,0xf0,0xf0}},
        {-1,1,.25f,0,{0x0f,0x0f,0x0f,0x0f,0x0f,0x0f,0x0f,0x0f}},
        { 0,2,.25f,0,{0,0,0x3c,0x3c,0x3c,0x3c,0,0}},
        { 0,1,2.0f,0,{0,0,0,0,0,0,0,0}},
        { 0,1,.25f,0,{0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
        /* StartInstanceLocation offsets IA data, not shader-visible InstanceID. */
        { 0,1,.25f,1,{0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
    };
    const struct vkdu_input_element elements[]={{0,0,DXGI_FORMAT_R32_FLOAT,0,0,0},{0,4,DXGI_FORMAT_R32_FLOAT,0,0,1}};
    const struct vkdu_signature pout={0,0,15,3};
    const uint32_t *p=graphics_tokens(ps_flat_color_code_dxbc);
    struct vkdu_graphics_desc desc={DXGI_FORMAT_R8G8B8A8_UNORM,UINT32_MAX,3,1,0,1,15};
    const struct vkdu_viewport viewport={0,0,8,8,0,1};const struct vkdu_rect scissor={0,0,8,8};
    vkdu_object *target=NULL,*rtv=NULL,*input=NULL,*seed=NULL,*readback=NULL;
    vkdu_object *queue=NULL,*allocator=NULL,*command=NULL,*fence=NULL;
    uint32_t variant,round,i,serial=0,*mapped;void *ps=NULL;size_t ps_size=0;
    CHECK(vkdu_shader_dxbc(0,p,p[1],NULL,0,&pout,1,&ps,&ps_size));
    desc.input_count=2;desc.inputs=elements;
    CHECK(vkdu_render_target_create(device,8,8,&target));
    CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,0,&rtv));CHECK(vkdu_texture2d_rtv(rtv,0,target));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&input));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&seed));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    CHECK(vkdu_buffer_map(seed,0,0,(void **)&mapped));for(i=0;i<1024;++i)mapped[i]=0x1badc0de;
    CHECK(vkdu_buffer_unmap(seed,0,4096));
    CHECK(vkdu_queue_create(device,0,&queue));CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command));CHECK(vkdu_fence_create(device,0,&fence));
    for(variant=0;variant<2;++variant) {
        const struct sm51_fixture *f=&sm51_fixtures[4+variant];
        struct vkdu_root_parameter params[]={{2,0,0,0,0},{1,0,0,1,variant?48:4},{3,0,0,0,0}};
        vkdu_object *root=NULL,*pipeline=NULL;void *vs=NULL;size_t vs_size=0;uint32_t words;
        const uint32_t *v=sm51_chunk(f->container,f->size,0x58454853,&words);
        CHECK(vkdu_shader_dxbc(1,v,words,f->inputs,f->input_count,f->outputs,1,&vs,&vs_size));
        CHECK(vkdu_root_create(device,params,3,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,&root));
        CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,ps,ps_size,&desc,&pipeline));
        vkdu_shader_dxbc_free(vs);
        for(round=0;round<6;++round) {
            float constants[48];uint32_t values[48];const float red[]={1,0,0,1};
            struct vkdu_vertex_buffer vb={input,512,sizeof(vertices),8};
            struct vkdu_texture_copy_location src={target,0,0,0,0,0,0,0};
            struct vkdu_texture_copy_location dst={readback,512,1,DXGI_FORMAT_R8G8B8A8_UNORM,8,8,1,256};
            if(serial) {CHECK(vkdu_allocator_reset(allocator));CHECK(vkdu_command_reset(command,allocator));
                CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));}
            for(i=0;i<48;++i)constants[i]=100.0f;
            i=variant?44:0;constants[i]=cases[round].x;constants[i+1]=constants[i+2]=constants[i+3]=0;
            memcpy(values,constants,sizeof(values));
            CHECK(vkdu_buffer_map(input,0,0,(void **)&mapped));
            for(i=0;i<1024;++i)mapped[i]=0x7fc00000;
            memcpy(mapped+64,&cases[round].z,4);memcpy(mapped+16,&cases[round].w,4);memcpy(mapped+128,vertices,sizeof(vertices));
            CHECK(vkdu_buffer_unmap(input,0,4096));
            CHECK(vkdu_command_copy(command,readback,0,seed,0,4096));
            CHECK(vkdu_command_graphics_root(command,root));CHECK(vkdu_command_pipeline(command,pipeline));
            CHECK(vkdu_command_graphics_cbv(command,0,input,256));
            CHECK(vkdu_command_graphics_constants(command,1,0,params[1].constant_count,values));
            CHECK(vkdu_command_graphics_srv(command,2,input,64));
            CHECK(vkdu_command_vertex_buffers(command,0,1,&vb));
            CHECK(vkdu_command_render_target(command,rtv,0));CHECK(vkdu_command_topology(command,4));
            CHECK(vkdu_command_viewports(command,1,&viewport));CHECK(vkdu_command_scissors(command,1,&scissor));
            CHECK(vkdu_command_clear_rtv(command,rtv,0,red,0,NULL));
            CHECK(vkdu_command_draw(command,6,1,0,cases[round].first_instance));
            CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
            CHECK(vkdu_command_texture_copy(command,&dst,0,0,0,&src,NULL));
            CHECK(vkdu_command_close(command));CHECK(vkdu_queue_execute(queue,1,&command));
            CHECK(vkdu_queue_signal(queue,fence,++serial));CHECK(vkdu_fence_wait(fence,serial,10000));
            CHECK(vkdu_buffer_map(readback,0,4096,(void **)&mapped));
            for(i=0;i<1024;++i) {
                uint32_t want=0x1badc0de;
                if(i>=128 && i<128+8*64 && (i-128)%64<8) {
                    uint32_t x=(i-128)%64,y=(i-128)/64;
                    want=cases[round].rows[y]&(1u<<x)?0xffffffff:0xff0000ff;
                }
                if(mapped[i]!=want) {fprintf(stderr,"FAIL SM51 VS %s round%u word%u=%08x expected%08x\n",f->name,round,i,mapped[i],want);exit(1);}
            }
            CHECK(vkdu_buffer_unmap(readback,0,0));
            printf("PASS SM51 VS %s round%u independent RGBA mask and all padding:1024 words\n",f->name,round);
        }
        vkdu_object_destroy(pipeline);vkdu_object_destroy(root);
    }
    CHECK(vkdu_device_status(device));vkdu_shader_dxbc_free(ps);
    vkdu_object_destroy(command);vkdu_object_destroy(allocator);vkdu_object_destroy(queue);vkdu_object_destroy(fence);
    vkdu_object_destroy(readback);vkdu_object_destroy(seed);vkdu_object_destroy(input);vkdu_object_destroy(rtv);vkdu_object_destroy(target);
}

static void check_sm51_ps_spaces(vkdu_device *device, int swap_spaces)
{
    vkdu_object *root=NULL,*pipelines[2]={NULL,NULL},*target=NULL,*rtv=NULL,*upload=NULL;
    vkdu_object *buffers[4]={0},*readbacks[4]={0},*queue=NULL,*allocator=NULL,*command=NULL,*fence=NULL;
    const struct vkdu_signature vin={6,0,1,1},vout={1,0,15,3};
    const uint32_t *v=graphics_tokens(vs_flat_color_code_dxbc);
    struct vkdu_root_parameter params[4]={{4,0,0,0,0},{4,0,1,0,0},{4,0,0,1,0},{4,0,1,1,0}};
    const struct vkdu_graphics_desc desc={DXGI_FORMAT_R8G8B8A8_UNORM,UINT32_MAX,3,1,0,1,15};
    const struct vkdu_viewport viewport={0,0,4,4,0,1};const struct vkdu_rect scissor={1,2,2,3};
    void *vs=NULL;size_t vs_size=0;uint32_t b,i,round,draw,*mapped;
    CHECK(vkdu_shader_dxbc(1,v,v[1],&vin,1,&vout,1,&vs,&vs_size));
    CHECK(vkdu_root_create(device,params,4,0,&root));
    for(i=0;i<2;++i) {
        const struct sm51_fixture *f=&sm51_fixtures[2+i];uint32_t words;
        const uint32_t *p=sm51_chunk(f->container,f->size,0x58454853,&words);void *ps=NULL;size_t ps_size=0;
        CHECK(vkdu_shader_dxbc(0,p,words,NULL,0,NULL,0,&ps,&ps_size));
        CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,ps,ps_size,&desc,&pipelines[i]));vkdu_shader_dxbc_free(ps);
    }
    vkdu_shader_dxbc_free(vs);
    CHECK(vkdu_render_target_create(device,4,4,&target));CHECK(vkdu_heap_create(device,2,1,0,&rtv));CHECK(vkdu_texture2d_rtv(rtv,0,target));
    CHECK(vkdu_buffer_create(device,16384,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&upload));
    CHECK(vkdu_buffer_map(upload,0,0,(void **)&mapped));
    for(b=0;b<4;++b)for(i=0;i<1024;++i)mapped[b*1024+i]=0x40000000+b*0x1000+i;
    CHECK(vkdu_buffer_unmap(upload,0,16384));
    for(b=0;b<4;++b) {
        CHECK(vkdu_buffer_create(device,4096,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST,&buffers[b]));
        CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readbacks[b]));
    }
    CHECK(vkdu_queue_create(device,0,&queue));CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command));CHECK(vkdu_fence_create(device,0,&fence));
    for(round=0;round<4;++round) {
        const uint32_t space=round&1,draws=round*2+1;
        if(round) {CHECK(vkdu_allocator_reset(allocator));CHECK(vkdu_command_reset(command,allocator));}
        for(b=0;b<4;++b) {
            if(round)CHECK(vkdu_command_transition(command,buffers[b],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST));
            CHECK(vkdu_command_copy(command,buffers[b],0,upload,b*4096,4096));
            CHECK(vkdu_command_transition(command,buffers[b],D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        }
        CHECK(vkdu_command_graphics_root(command,root));CHECK(vkdu_command_pipeline(command,pipelines[space]));
        for(b=0;b<4;++b)CHECK(vkdu_command_graphics_uav(command,b,buffers[swap_spaces?(b^2):b],256+b*16));
        CHECK(vkdu_command_render_target(command,rtv,0));CHECK(vkdu_command_topology(command,4));
        CHECK(vkdu_command_viewports(command,1,&viewport));CHECK(vkdu_command_scissors(command,1,&scissor));
        for(draw=0;draw<draws;++draw)CHECK(vkdu_command_draw(command,3,1,0,0));
        for(b=0;b<4;++b) {
            CHECK(vkdu_command_transition(command,buffers[b],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE));
            CHECK(vkdu_command_copy(command,readbacks[b],0,buffers[b],0,4096));
        }
        CHECK(vkdu_command_close(command));CHECK(vkdu_queue_execute(queue,1,&command));
        CHECK(vkdu_queue_signal(queue,fence,round+1));CHECK(vkdu_fence_wait(fence,round+1,10000));
        for(b=0;b<4;++b) {
            CHECK(vkdu_buffer_map(readbacks[b],0,4096,(void **)&mapped));
            for(i=0;i<1024;++i) {
                uint32_t want=0x40000000+b*0x1000+i;
                if(b/2==space && i==64+b*4)want+=draws;
                if(mapped[i]!=want) {fprintf(stderr,"FAIL SM51 PS space%u round%u buffer%u word%u=%08x expected%08x\n",space,round,b,i,mapped[i],want);exit(1);}
            }
            CHECK(vkdu_buffer_unmap(readbacks[b],0,0));
            printf("PASS SM51 PS space%u round%u buffer%u selected/other-space/guards:1024 words\n",space,round,b);
        }
    }
    CHECK(vkdu_device_status(device));
    vkdu_object_destroy(command);vkdu_object_destroy(allocator);vkdu_object_destroy(queue);vkdu_object_destroy(fence);
    for(b=0;b<4;++b){vkdu_object_destroy(buffers[b]);vkdu_object_destroy(readbacks[b]);}
    vkdu_object_destroy(upload);vkdu_object_destroy(rtv);vkdu_object_destroy(target);
    vkdu_object_destroy(pipelines[0]);vkdu_object_destroy(pipelines[1]);vkdu_object_destroy(root);
}
static void check_sm50_converter_control(void)
{
    const struct vkdu_signature vin = {6,0,1,1}, vout = {1,0,15,3}, pout = {0,0,15,3};
    const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc), *p = graphics_tokens(ps_flat_color_code_dxbc);
    void *code = NULL; size_t size = 0;
    CHECK(vkdu_shader_dxbc(1,v,v[1],&vin,1,&vout,1,&code,&size)); vkdu_shader_dxbc_free(code);
    CHECK(vkdu_shader_dxbc(0,p,p[1],NULL,0,&pout,1,&code,&size)); vkdu_shader_dxbc_free(code);
    puts("PASS original SM50 VS/PS converter harness control");
}
