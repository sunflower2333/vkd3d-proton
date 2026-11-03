/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <math.h>
/* D3D11.3 spec sections 11.7.2, 11.7.6 and 11.7.7: integer factor2 has a central degenerate
 * inner triangle and six outer vertices, producing six triangles. */
#include "../../tests/shaders/tessellation/headers/quad_tess_vs.h"
#include "../../tests/shaders/tessellation/headers/nop_hs.h"
#include "../../tests/shaders/tessellation/headers/nop_cb.h"
#include "../../tests/shaders/tessellation/headers/nop_ds.h"
#include "../../tests/shaders/tessellation/headers/control_point_phase_hs.h"
#include "../../tests/shaders/tessellation/headers/quad_tess_hs_cw.h"
#include "../../tests/shaders/tessellation/headers/quad_tess_ds.h"

static void check_tessellation(vkdu_device *device)
{
    vkdu_object *root, *pipeline, *target, *rtv, *queue, *allocator, *command, *fence;
    vkdu_object *vertices, *upload, *output, *readback, *pixels, *statistics;
    const struct vkdu_signature vin = {0,0,15,3}, position = {1,0,15,3}, pout = {0,0,15,3};
    const struct vkdu_signature patch[] = {{17,0,1,3},{18,1,1,3},{19,2,1,3},{20,3,1,3}};
    const struct vkdu_signature qpatch[]={{11,0,1,3},{12,1,1,3},{13,2,1,3},{14,3,1,3},{15,4,1,3},{16,5,1,3}};
    const struct vkdu_root_parameter parameter = {1,2,0,0,6};
    const struct vkdu_input_element element = {0,0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,0};
    struct vkdu_graphics_desc state = {DXGI_FORMAT_R8G8B8A8_UNORM,UINT32_MAX,3,1,0,1,15,1,&element};
    const struct vkdu_stream_output_desc so = {1,1,0,{{0,0,0,15}},{16}};
    const uint32_t *vt = graphics_tokens(quad_tess_vs_code_dxbc), *pt = graphics_tokens(ps_flat_color_code_dxbc);
    const uint32_t *dt = graphics_tokens(nop_ds_code_dxbc);
    void *vs, *ps, *ds, *qds, *mapped; size_t vs_size, ps_size, ds_size, qds_size;
    uint32_t round, i;
    CHECK(vkdu_shader_dxbc(1,vt,vt[1],&vin,1,&position,1,&vs,&vs_size));
    CHECK(vkdu_shader_dxbc(0,pt,pt[1],NULL,0,&pout,1,&ps,&ps_size));
    CHECK(vkdu_shader_tess_dxbc(4,dt,dt[1],&position,1,&position,1,patch,4,&ds,&ds_size));
    dt=graphics_tokens(quad_tess_ds_code_dxbc);
    CHECK(vkdu_shader_tess_dxbc(4,dt,dt[1],&position,1,&position,1,qpatch,6,&qds,&qds_size));
    CHECK(vkdu_root_create(device,&parameter,1,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT,&root));
    CHECK(vkdu_render_target_create(device,16,12,&target)); CHECK(vkdu_heap_create(device,2,1,0,&rtv));
    CHECK(vkdu_texture2d_rtv(rtv,0,target)); CHECK(vkdu_queue_create(device,0,&queue));
    CHECK(vkdu_allocator_create(device,0,&allocator)); CHECK(vkdu_command_create(device,allocator,0,&command));
    CHECK(vkdu_fence_create(device,0,&fence)); CHECK(vkdu_query_heap_create(device,2,1,&statistics));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&vertices));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&upload));
    CHECK(vkdu_buffer_create(device,4096,1,0,D3D12_RESOURCE_STATE_COPY_DEST,&output));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&pixels));
    for (round=0;round<8;++round) {
        const uint32_t sentinel=0x6e710000u+round, count=round==4 ? 0 : round==3 ? 18 : round>=6 ? (round==7 ? 24 : 6) : 3;
        const float quad[4][4]={{-1,-1,0,1},{1,-1,0,1},{-1,1,0,1},{1,1,0,1}};
        const float triangle[3][4]={{-1,-1,0,1},{-1,3,0,1},{3,-1,0,1}};
        const float control_triangle[3][4]={{-1,1,0,1},{3,1,0,1},{-1,-3,0,1}};
        const float midpoints[3][4]={{-1,1,0,1},{1,-1,0,1},{1,1,0,1}};
        const uint32_t *ht=graphics_tokens(round>=6 ? quad_tess_hs_cw_code_dxbc : round==0 ? nop_hs_code_dxbc : round==1 ? control_point_phase_hs_code_dxbc : nop_cb_code_dxbc);
        const struct vkdu_viewport viewport={0,0,16,12,0,1}; const struct vkdu_rect scissor={2,1,14,11};
        const struct vkdu_vertex_buffer vb={vertices,64,64,16};
        const struct vkdu_stream_output_buffer view={output,output,128,1024,2048};
        const struct vkdu_texture_copy_location src={target}, dst={pixels,512,1,DXGI_FORMAT_R8G8B8A8_UNORM,16,12,1,256};
        const float clear[4]={1,0,0,1}; float factors[6]={1,1,1,1,1,1}; uint32_t factor_bits[6], hits[7]={0};
        void *hs; size_t hs_size;
        CHECK(vkdu_shader_tess_dxbc(3,ht,ht[1],round==1 ? NULL : &position,round==1 ? 0 : 1,
            &position,1,round>=6 ? qpatch : patch,round>=6 ? 6 : 4,&hs,&hs_size));
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command,allocator));
            CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST));
            CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));
        }
        state.topology_type=4; state.hs=hs; state.hs_size=hs_size; state.ds=round>=6 ? qds : ds; state.ds_size=round>=6 ? qds_size : ds_size; state.stream_output=&so;
        CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,ps,ps_size,&state,&pipeline));
        vkdu_shader_dxbc_free(hs);
        CHECK(vkdu_buffer_map(vertices,0,0,&mapped)); memcpy((char *)mapped+64,round>=6 ? (const void *)quad : triangle,round>=6 ? sizeof(quad) : sizeof(triangle)); CHECK(vkdu_buffer_unmap(vertices,64,128));
        CHECK(vkdu_buffer_map(upload,0,0,&mapped)); for(i=0;i<1024;++i)((uint32_t *)mapped)[i]=sentinel;
        ((uint32_t *)mapped)[512]=0; CHECK(vkdu_buffer_unmap(upload,0,4096));
        CHECK(vkdu_buffer_map(pixels,0,0,&mapped)); for(i=0;i<1024;++i)((uint32_t *)mapped)[i]=sentinel;
        CHECK(vkdu_buffer_unmap(pixels,0,4096));
        CHECK(vkdu_command_copy(command,output,0,upload,0,4096));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_STREAM_OUT));
        CHECK(vkdu_command_clear_rtv(command,rtv,0,clear,0,NULL));
        CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_pipeline(command,pipeline));
        CHECK(vkdu_command_vertex_buffers(command,0,1,&vb)); CHECK(vkdu_command_topology(command,round==1 ? 33 : round>=6 ? 36 : 35));
        REJECT(vkdu_command_topology(command,32)); REJECT(vkdu_command_topology(command,65));
        CHECK(vkdu_command_viewports(command,1,&viewport)); CHECK(vkdu_command_scissors(command,1,&scissor));
        CHECK(vkdu_command_render_target(command,rtv,0)); CHECK(vkdu_command_stream_output(command,0,1,&view));
        if(round==3 || round==7) for(i=0;i<6;++i)factors[i]=2;
        if(round==4)factors[0]=0;
        memcpy(factor_bits,factors,sizeof(factors)); CHECK(vkdu_command_graphics_constants(command,0,0,6,factor_bits));
        /* The sentinel is a nonzero64-bit predicate. A skipped HS/DS Draw
         * must not append vertices; later counters stay below capacity, so
         * an extra Draw cannot be hidden by overflow. Restoring predication
         * must also preserve the bound hull-stage root constants. */
        CHECK(vkdu_command_predication(command,upload,0,D3D12_PREDICATION_OP_NOT_EQUAL_ZERO));
        CHECK(vkdu_command_draw(command,round==1 ? 1 : round>=6 ? 4 : 3,1,0,0));
        CHECK(vkdu_command_predication(command,NULL,0,0));
        CHECK(vkdu_command_query(command,statistics,D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0,1));
        CHECK(vkdu_command_draw(command,round==1 ? 1 : round>=6 ? 4 : 3,1,0,0));
        CHECK(vkdu_command_query(command,statistics,D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0,0));
        CHECK(vkdu_command_stream_output(command,0,1,NULL));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command,readback,0,output,0,4096));
        CHECK(vkdu_command_query_resolve(command,statistics,D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0,1,readback,3072));
        CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command,&dst,0,0,0,&src,NULL)); CHECK(vkdu_command_close(command));
        CHECK(vkdu_queue_execute(queue,1,&command)); CHECK(vkdu_queue_signal(queue,fence,round+1)); CHECK(vkdu_fence_wait(fence,round+1,10000));
        CHECK(vkdu_buffer_map(readback,0,4096,&mapped));
        for(i=0;i<1024;++i) {
            uint32_t expected=sentinel;
            if(i>=768 && i<768+sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)/4)continue;
            if(i>=32 && i<32+count*4)continue;
            if(i==512)expected=count*16;
            if(((uint32_t *)mapped)[i]!=expected){fprintf(stderr,"FAIL tess round%u word%u=%08x expected%08x\n",round,i,((uint32_t *)mapped)[i],expected);exit(1);}
        }
        if(round>=6) {
            uint32_t cell_hits[4]={0}, triangle_masks[8]={0}, divisions=round==7 ? 2 : 1;
            const float *v=(const float *)((const char *)mapped+128); const float step=2.0f/divisions;
            for(i=0;i<count;i+=3) {
                uint32_t j,k,minx=2,miny=2,maxx=0,maxy=0,mask=0;
                float area=fabsf((v[(i+1)*4]-v[i*4])*(v[(i+2)*4+1]-v[i*4+1])-
                    (v[(i+2)*4]-v[i*4])*(v[(i+1)*4+1]-v[i*4+1]));
                if(area!=step*step){fprintf(stderr,"FAIL quad triangle area round%u triangle%u\n",round,i/3);exit(1);}
                for(j=0;j<3;++j) {
                    const float *p=v+(i+j)*4; int x=(int)((p[0]+1)/step), y=(int)((p[1]+1)/step);
                    if(x<0 || y<0 || x>(int)divisions || y>(int)divisions || p[0]!=x*step-1 || p[1]!=y*step-1 || p[2]!=0 || p[3]!=1) {
                        fprintf(stderr,"FAIL quad domain position round%u vertex%u\n",round,i+j);exit(1);
                    }
                    if((uint32_t)x<minx)minx=x; if((uint32_t)x>maxx)maxx=x;
                    if((uint32_t)y<miny)miny=y; if((uint32_t)y>maxy)maxy=y;
                    mask|=1u<<(y*(divisions+1)+x);
                }
                if(maxx-minx!=1 || maxy-miny!=1){fprintf(stderr,"FAIL quad triangle cell\n");exit(1);}
                for(k=0;k<i/3;++k)if(triangle_masks[k]==mask){fprintf(stderr,"FAIL duplicate quad triangle\n");exit(1);}
                triangle_masks[i/3]=mask; ++cell_hits[miny*divisions+minx];
            }
            for(i=0;i<divisions*divisions;++i)if(cell_hits[i]!=2){fprintf(stderr,"FAIL missing quad cell\n");exit(1);}
        } else {
        for(i=0;i<count;++i) {
            uint32_t j; const void *vertex=(char *)mapped+128+i*16;
            for(j=0;j<(round==3 ? 7u : 3u);++j) {
                const float center[4]={1.0f/3.0f,1.0f/3.0f,0,1};
                const void *expected=round==1 ? control_triangle[j] : j<3 ? triangle[j] : j<6 ? midpoints[j-3] : center;
                /* The fixed-point tessellator converts thirds back to float;
                 * allow 2^-14 coordinate error only at the central vertex. */
                const float *v=vertex;
                if(j==6 ? (fabsf(v[0]-center[0])<0x1p-14f && fabsf(v[1]-center[1])<0x1p-14f && v[2]==0 && v[3]==1) : !memcmp(vertex,expected,16)) {
                    ++hits[j];break;
                }
            }
            if(j==(round==3 ? 7u : 3u)){fprintf(stderr,"FAIL tess round%u domain position%u\n",round,i);exit(1);}
        }
        for(i=0;i<7;++i)if(hits[i]!=(round==4 ? 0u : round==3 ? (i==6 ? 6u : 2u) : i<3 ? 1u : 0u)){
            fprintf(stderr,"FAIL tess round%u vertex multiplicity%u=%u\n",round,i,hits[i]);exit(1);
        }
        }
        {
            const D3D12_QUERY_DATA_PIPELINE_STATISTICS *stats=(const void *)((char *)mapped+3072);
            if(stats->HSInvocations!=1 || stats->DSInvocations!=(round==4 ? 0u : round==3 ? 7u : round>=6 ? (round==7 ? 9u : 4u) : 3u) || stats->GSInvocations || stats->CSInvocations){
                fprintf(stderr,"FAIL tess round%u HS=%llu DS=%llu GS=%llu CS=%llu\n",round,
                    (unsigned long long)stats->HSInvocations,(unsigned long long)stats->DSInvocations,
                    (unsigned long long)stats->GSInvocations,(unsigned long long)stats->CSInvocations);exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback,0,0)); CHECK(vkdu_buffer_map(pixels,0,4096,&mapped));
        for(i=0;i<1024;++i) {
            uint32_t expected=sentinel;
            if(i>=128 && i<896 && (i-128)%64<16) {
                uint32_t x=(i-128)%64,y=(i-128)/64;
                expected=round!=4 && x>=2 && x<14 && y>=1 && y<11 ? 0xffffffff : 0xff0000ff;
            }
            if(((uint32_t *)mapped)[i]!=expected){fprintf(stderr,"FAIL tess raster round%u word%u=%08x expected%08x\n",round,i,((uint32_t *)mapped)[i],expected);exit(1);}
        }
        CHECK(vkdu_buffer_unmap(pixels,0,0)); vkdu_object_destroy(pipeline);
        printf("PASS actual HS/DS round%u: skipped predicate Draw, control/default/fork phases, patch constants, factor/cull, DS SO vertices and statistics,2048 checked words\n",round);
    }
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps); vkdu_shader_dxbc_free(ds); vkdu_shader_dxbc_free(qds);
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(vertices); vkdu_object_destroy(upload); vkdu_object_destroy(output); vkdu_object_destroy(readback);
    vkdu_object_destroy(pixels); vkdu_object_destroy(statistics); vkdu_object_destroy(rtv); vkdu_object_destroy(target); vkdu_object_destroy(root);
}
