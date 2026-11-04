/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../tests/shaders/tessellation/headers/line_tessellation_vs.h"
#include "../../tests/shaders/tessellation/headers/line_tessellation_hs.h"
#include "../../tests/shaders/tessellation/headers/line_tessellation_ds.h"

static void check_tessellation_lines(vkdu_device *device)
{
    const struct vkdu_signature vin[]={{0,0,15,3},{0,1,7,3},{0,2,1,3},{0,3,1,3}};
    const struct vkdu_signature points[]={{1,0,15,3},{0,1,15,3},{0,2,1,3}};
    const struct vkdu_signature patch[]={{22,0,1,3},{0,0,14,3},{21,1,1,3},{0,1,2,1}};
    const struct vkdu_signature dout[]={{1,0,15,3},{0,1,15,3},{0,2,15,3}};
    const struct vkdu_input_element elements[]={{0,0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,0},
        {0,16,DXGI_FORMAT_R32G32B32_FLOAT,0,0,1},{0,28,DXGI_FORMAT_R32_FLOAT,0,0,2},{0,32,DXGI_FORMAT_R32_FLOAT,0,0,3}};
    const struct vkdu_stream_output_desc so={3,1,UINT32_MAX,{{0,0,0,15},{0,0,1,15},{0,0,2,15}},{48}};
    struct vkdu_graphics_desc state={0,UINT32_MAX,3,1,0,1,15,4,elements};
    const uint32_t *vt=graphics_tokens(line_tessellation_vs_code_dxbc), *ht=graphics_tokens(line_tessellation_hs_code_dxbc);
    const uint32_t *dt=graphics_tokens(line_tessellation_ds_code_dxbc);
    vkdu_object *root,*pipeline,*gs_pipeline,*partial_pipeline,*distance_pipeline[2],*vertices,*upload,*output,*readback,*queue,*allocator,*command,*fence,*statistics;
    void *vs,*hs,*ds,*mapped; size_t vs_size,hs_size,ds_size; uint32_t round,i;
    CHECK(vkdu_shader_dxbc(1,vt,vt[1],vin,4,points,3,&vs,&vs_size));
    CHECK(vkdu_shader_tess_dxbc(3,ht,ht[1],points,3,points,3,patch,4,&hs,&hs_size));
    CHECK(vkdu_shader_tess_dxbc(4,dt,dt[1],points,3,dout,3,patch,4,&ds,&ds_size));
    CHECK(vkdu_root_create(device,NULL,0,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT,&root));
    state.topology_type=4; state.hs=hs; state.hs_size=hs_size; state.ds=ds; state.ds_size=ds_size; state.stream_output=&so;
    CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,NULL,0,&state,&pipeline));
    for (i=0;i<2;++i) {
        uint32_t *distance_tokens=malloc((dt[1]+2)*4), in=2,out=2,edits=0;
        struct vkdu_signature distance_out[]={{1,0,15,3},{i+2,2,15,3},{i+2,1,15,3}};
        void *distance_ds; size_t distance_size;
        if (!distance_tokens)exit(1);
        distance_tokens[0]=dt[0];distance_tokens[1]=dt[1]+2;
        while (in<dt[1]) {
            uint32_t n=(dt[in]>>24)&127;
            if (!n || n>dt[1]-in)exit(1);
            if ((dt[in]&2047)==101 && n==3 && (dt[in+2]==1 || dt[in+2]==2)) {
                distance_tokens[out++]=0x04000067; distance_tokens[out++]=dt[in+1];
                distance_tokens[out++]=dt[in+2]; distance_tokens[out++]=i+2; ++edits;
            } else { memcpy(distance_tokens+out,dt+in,n*4);out+=n; }
            in+=n;
        }
        if (edits!=2 || out!=distance_tokens[1])exit(1);
        CHECK(vkdu_shader_tess_dxbc(4,distance_tokens,out,points,3,distance_out,3,patch,4,&distance_ds,&distance_size));
        free(distance_tokens); state.ds=distance_ds;state.ds_size=distance_size;
        CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,NULL,0,&state,&distance_pipeline[i]));
        memset(distance_ds,0xcc,distance_size);vkdu_shader_dxbc_free(distance_ds);
    }
    state.ds=ds;state.ds_size=ds_size;
    {
        const struct vkdu_stream_output_desc partial_so={4,1,UINT32_MAX,
            {{0,0,1,4},{0,0,UINT32_MAX,1},{0,0,0,8},{0,0,2,2}},{24}};
        state.stream_output=&partial_so;
        CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,NULL,0,&state,&partial_pipeline));
    }
    {
        /* Same upstream isoline HS, with the declared tessellator output set
         * to POINT (SM5 opcode151), lets the real GS consume DS point output. */
        uint32_t *point_tokens=malloc(ht[1]*4), at, found=0;
        const uint32_t *gt=graphics_tokens(stage_gs_code_dxbc);
        const struct vkdu_stream_output_desc gs_so={2,1,UINT32_MAX,{{0,0,0,15},{0,0,1,15}},{32}};
        void *point_hs,*gs; size_t point_size,gs_size;
        if(!point_tokens)exit(1); memcpy(point_tokens,ht,ht[1]*4);
        for(at=2;at<ht[1];at+=(ht[at]>>24)&127) if((ht[at]&0x7ff)==151) {
            point_tokens[at]=(point_tokens[at]&~(7u<<11))|(1u<<11); ++found;
        }
        if(found!=1)exit(1);
        CHECK(vkdu_shader_tess_dxbc(3,point_tokens,ht[1],points,3,points,3,patch,4,&point_hs,&point_size)); free(point_tokens);
        CHECK(vkdu_shader_dxbc(2,gt,gt[1],dout,2,dout,2,&gs,&gs_size));
        state.hs=point_hs; state.hs_size=point_size; state.gs=gs; state.gs_size=gs_size; state.stream_output=&gs_so;
        CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,NULL,0,&state,&gs_pipeline));
        vkdu_shader_dxbc_free(point_hs); vkdu_shader_dxbc_free(gs);
    }
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(hs); vkdu_shader_dxbc_free(ds);
    CHECK(vkdu_queue_create(device,0,&queue)); CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command)); CHECK(vkdu_fence_create(device,0,&fence));
    CHECK(vkdu_query_heap_create(device,2,1,&statistics));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&vertices));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&upload));
    CHECK(vkdu_buffer_create(device,4096,1,0,D3D12_RESOURCE_STATE_COPY_DEST,&output));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    for(round=0;round<8;++round) {
        const uint32_t first_count=round==4 ? 48 : round==1 ? 2 : round==2 ? 0 : 12, count=first_count+(round==4 ? 18 : 4), sentinel=0x75110000+round;
        float inputs[2][9]={{-.5f,.25f,0,1,.25f,.5f,.75f,2,3},{.25f,-.5f,0,1,.75f,.25f,.5f,1,2}};
        const struct vkdu_vertex_buffer vb={vertices,64,sizeof(inputs),36};
        const struct vkdu_stream_output_buffer view={output,output,128,2816,3072};
        const uint32_t stride=round==4 ? 8 : round==5 ? 6 : 12;
        if(round==1)inputs[0][7]=inputs[0][8]=1;
        if(round==2)inputs[0][7]=0;
        if(round) { CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command,allocator));
            CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST)); }
        CHECK(vkdu_buffer_map(vertices,0,0,&mapped)); memcpy((char *)mapped+64,inputs,sizeof(inputs));
        CHECK(vkdu_buffer_unmap(vertices,64,64+sizeof(inputs)));
        CHECK(vkdu_buffer_map(upload,0,0,&mapped)); for(i=0;i<1024;++i)((uint32_t *)mapped)[i]=sentinel;
        ((uint32_t *)mapped)[768]=0; CHECK(vkdu_buffer_unmap(upload,0,4096));
        CHECK(vkdu_command_copy(command,output,0,upload,0,4096));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_STREAM_OUT));
        CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_pipeline(command,round>=6 ? distance_pipeline[round-6] : round==4 ? gs_pipeline : round==5 ? partial_pipeline : pipeline));
        CHECK(vkdu_command_vertex_buffers(command,0,1,&vb)); CHECK(vkdu_command_topology(command,33));
        CHECK(vkdu_command_stream_output(command,0,1,&view));
        CHECK(vkdu_command_predication(command,upload,0,D3D12_PREDICATION_OP_NOT_EQUAL_ZERO));
        CHECK(vkdu_command_draw(command,2,1,0,0));
        CHECK(vkdu_command_predication(command,NULL,0,0));
        CHECK(vkdu_command_query(command,statistics,D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0,1));
        CHECK(vkdu_command_draw(command,2,1,0,0));
        CHECK(vkdu_command_query(command,statistics,D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0,0));
        CHECK(vkdu_command_stream_output(command,0,1,NULL));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command,readback,0,output,0,4096));
        CHECK(vkdu_command_query_resolve(command,statistics,D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0,1,readback,3584));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue,1,&command));
        CHECK(vkdu_queue_signal(queue,fence,round+1)); CHECK(vkdu_fence_wait(fence,round+1,10000));
        CHECK(vkdu_buffer_map(readback,0,4096,&mapped));
        for(i=0;i<1024;++i) {
            uint32_t expected=sentinel;
            if(i>=896 && i<896+sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)/4)continue;
            if(round==5 && i>=32 && i<32+count*stride) {
                uint32_t vertex=(i-32)/stride, component=(i-32)%stride, id=vertex<first_count ? 0 : 1;
                float f=component==0 ? inputs[id][6] : component==2 ? 1.f : (float)id;
                if(component==0 || component==2 || component==3)memcpy(&expected,&f,4);
            } else if(i>=32 && i<32+count*stride) {
                uint32_t vertex=(i-32)/stride, component=(i-32)%stride, id=vertex<first_count ? 0 : 1;
                float f=component<7 ? inputs[id][component] : component==7 ? 1 : (float)id;
                if(round==4 && component<2) {
                    const float delta[6][2]={{-.2f,-.2f},{-.2f,.2f},{.2f,-.2f},{-.2f,.2f},{.2f,.2f},{.2f,-.2f}};
                    f+=delta[vertex%6][component];
                }
                memcpy(&expected,&f,4);
            }
            if(i==768)expected=count*stride*4;
            if(((uint32_t *)mapped)[i]!=expected){fprintf(stderr,"FAIL isoline round%u word%u=%08x expected%08x\n",round,i,((uint32_t *)mapped)[i],expected);exit(1);}
        }
        {
            const D3D12_QUERY_DATA_PIPELINE_STATISTICS *q=(const void *)((char *)mapped+3584);
            const uint64_t ds_invocations=round==1 ? 5 : round==2 ? 3 : 11;
            if(q->HSInvocations!=2 || q->DSInvocations!=ds_invocations || q->GSInvocations!=(round==4 ? 11u : 0u) || q->PSInvocations || q->CSInvocations) {
                fprintf(stderr,"FAIL isoline round%u HS=%llu DS=%llu PS=%llu\n",round,(unsigned long long)q->HSInvocations,
                    (unsigned long long)q->DSInvocations,(unsigned long long)q->PSInvocations);exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback,0,0));
        printf("PASS actual isoline round%u: skipped predicate Draw, optional DS->GS chain, density/detail and cull/restore, packed float/uint patch constants, primitive IDs, DS SO and statistics,1024 checked words\n",round);
        if (round>=6)printf("PASS actual DS %s0/1:8 captured distance payload components, reversed signature order, distinct patch values, copied code\n",round==6 ? "ClipDistance" : "CullDistance");
    }
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(vertices); vkdu_object_destroy(upload); vkdu_object_destroy(output); vkdu_object_destroy(readback);
    vkdu_object_destroy(statistics); vkdu_object_destroy(pipeline); vkdu_object_destroy(gs_pipeline); vkdu_object_destroy(partial_pipeline); vkdu_object_destroy(root);
    vkdu_object_destroy(distance_pipeline[0]);vkdu_object_destroy(distance_pipeline[1]);
}
