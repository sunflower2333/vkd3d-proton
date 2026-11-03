/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real UMD readback fixture; direct-engine mode retains the separate RED. */
#ifdef VKDU_ENABLE_TEST_DEVICE
#include "backend_topology_observer_test.h"
static int32_t topology_test_draw(vkdu_object *command, int indexed)
{ return indexed ? vkdu_command_draw_indexed(command,3,1,0,0,0) : vkdu_command_draw(command,3,1,0,0); }
static int check_topology_clear(vkdu_device *device, int engine_only, int indexed)
{
    vkdu_object *root=NULL,*pipeline=NULL,*target=NULL,*heap=NULL,*queue=NULL;
    vkdu_object *allocator=NULL,*command=NULL,*fence=NULL,*readback=NULL,*indices=NULL;
    const struct vkdu_signature vertex_input={6,0,1,1},vertex_output={1,0,15,3},pixel_output={0,0,15,3};
    const uint32_t *vertex=graphics_tokens(vs_flat_color_code_dxbc),*pixel=graphics_tokens(ps_flat_color_code_dxbc);
    struct vkdu_graphics_desc desc={DXGI_FORMAT_R8G8B8A8_UNORM,UINT32_MAX,3,1,0,1,15};
    const struct vkdu_viewport viewport={0,0,16,4,0,1};
    const struct vkdu_rect warm={0,0,4,4},unbound={4,0,8,4},rebound={8,0,12,4};
    const float clear[4]={1,0,0,1};
    void *vs=NULL,*ps=NULL,*mapped=NULL;size_t vs_size=0,ps_size=0;
    unsigned round,i,failures=0,total_pixels=0,total_padding=0;
    CHECK(vkdu_shader_dxbc(1,vertex,vertex[1],&vertex_input,1,&vertex_output,1,&vs,&vs_size));
    CHECK(vkdu_shader_dxbc(0,pixel,pixel[1],NULL,0,&pixel_output,1,&ps,&ps_size));
    CHECK(vkdu_root_create(device,NULL,0,0,&root));
    CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_size,ps,ps_size,&desc,&pipeline));
    vkdu_shader_dxbc_free(vs);vkdu_shader_dxbc_free(ps);
    CHECK(vkdu_render_target_create(device,16,4,&target));
    CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,0,&heap));
    CHECK(vkdu_texture2d_rtv(heap,0,target));
    CHECK(vkdu_queue_create(device,0,&queue));CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command));CHECK(vkdu_fence_create(device,0,&fence));
    CHECK(vkdu_buffer_create(device,4096,D3D12_HEAP_TYPE_READBACK,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    if (indexed) {
        CHECK(vkdu_buffer_create(device,4096,D3D12_HEAP_TYPE_UPLOAD,0,D3D12_RESOURCE_STATE_GENERIC_READ,&indices));
        CHECK(vkdu_buffer_map(indices,0,0,&mapped));
        ((uint16_t *)mapped)[0]=0;((uint16_t *)mapped)[1]=1;((uint16_t *)mapped)[2]=2;
        CHECK(vkdu_buffer_unmap(indices,0,6));
    }
    for (round=0;round<(engine_only?1u:4u);++round) {
        const int bound=engine_only||round==1||round==3;
        const int invalid=!engine_only&&round==3;
        const char *name=engine_only?"engine-clear":round==0?"fresh":round==1?"bound-clear":round==2?"reset":"invalid-enum";
        const uint32_t sentinel=0x6b5a0000u^round;
        int32_t set_result=0,draw_result=0;
        struct vkdu_topology_observation before,cleared,drawn,rebound_set,after;
        unsigned pixel_mismatches=0,padding_mismatches=0,first_mismatch=UINT32_MAX,first_actual=0,first_expected=0;
        unsigned middle_white=0,middle_red=0,rebound_white=0;
        struct vkdu_texture_copy_location src={target,0,0,0,0,0,0,0};
        struct vkdu_texture_copy_location dst={readback,512,1,DXGI_FORMAT_R8G8B8A8_UNORM,16,4,1,256};
        ID3D12GraphicsCommandList *native=vkdu_test_command_iface(command);
        if (!native) abort();
        if (round) {
            CHECK(vkdu_allocator_reset(allocator));CHECK(vkdu_command_reset(command,allocator));
            CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));
        }
        CHECK(vkdu_buffer_map(readback,0,0,&mapped));
        for(i=0;i<1024;++i)((uint32_t *)mapped)[i]=sentinel;
        CHECK(vkdu_buffer_unmap(readback,0,4096));
        CHECK(vkdu_command_clear_rtv(command,heap,0,clear,0,NULL));
        CHECK(vkdu_command_graphics_root(command,root));CHECK(vkdu_command_pipeline(command,pipeline));
        CHECK(vkdu_command_render_target(command,heap,0));CHECK(vkdu_command_viewports(command,1,&viewport));
        if(indexed)CHECK(vkdu_command_index_buffer(command,indices,0,6,DXGI_FORMAT_R16_UINT));
        if (bound) {
            CHECK(vkdu_command_topology(command,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST));
            CHECK(vkdu_command_scissors(command,1,&warm));CHECK(topology_test_draw(command,indexed));
        }
        before=vkdu_test_topology_observation();
        if (engine_only) {
            /* Deliberately isolate the actual embedded COM setter from the
             * UMD validity gate. This is an invalid-Draw diagnostic, not a
             * claim that applications may draw with undefined topology. */
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(native,D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
        } else set_result=vkdu_command_topology(command,invalid?99:D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
        cleared=vkdu_test_topology_observation();
        CHECK(vkdu_command_scissors(command,1,&unbound));
        if (engine_only) ID3D12GraphicsCommandList_DrawInstanced(native,3,1,0,0);
        else draw_result=topology_test_draw(command,indexed);
        drawn=vkdu_test_topology_observation();
        /* Rebind exactly the old value, without reinitializing PSO/root/RT. */
        CHECK(vkdu_command_topology(command,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST));
        rebound_set=vkdu_test_topology_observation();
        CHECK(vkdu_command_scissors(command,1,&rebound));CHECK(topology_test_draw(command,indexed));
        after=vkdu_test_topology_observation();
        if (!engine_only) {
            const uint64_t draw_count=drawn.draw_before-cleared.draw_before;
            const uint64_t indexed_count=drawn.indexed_before-cleared.indexed_before;
            const uint64_t wanted=invalid?1:0;
            printf("CALLS topology indexed=%d case=%s clear_setter=%llu/%llu unbound_draw=%llu/%llu unbound_indexed=%llu/%llu rebind_setter=%llu/%llu recovery_draw=%llu/%llu recovery_indexed=%llu/%llu\n",
                indexed,name,(unsigned long long)(cleared.setter_before-before.setter_before),(unsigned long long)(cleared.setter_after-before.setter_after),
                (unsigned long long)draw_count,(unsigned long long)(drawn.draw_after-cleared.draw_after),
                (unsigned long long)indexed_count,(unsigned long long)(drawn.indexed_after-cleared.indexed_after),
                (unsigned long long)(rebound_set.setter_before-drawn.setter_before),(unsigned long long)(rebound_set.setter_after-drawn.setter_after),
                (unsigned long long)(after.draw_before-rebound_set.draw_before),(unsigned long long)(after.draw_after-rebound_set.draw_after),
                (unsigned long long)(after.indexed_before-rebound_set.indexed_before),(unsigned long long)(after.indexed_after-rebound_set.indexed_after));
            if (cleared.setter_before!=before.setter_before || cleared.setter_after!=before.setter_after ||
                    draw_count!=(indexed?0:wanted) || drawn.draw_after-cleared.draw_after!=draw_count ||
                    indexed_count!=(indexed?wanted:0) || drawn.indexed_after-cleared.indexed_after!=indexed_count ||
                    rebound_set.setter_before-drawn.setter_before!=1 || rebound_set.setter_after-drawn.setter_after!=1 ||
                    after.draw_before-rebound_set.draw_before!=(indexed?0:1) || after.draw_after-rebound_set.draw_after!=(indexed?0:1) ||
                    after.indexed_before-rebound_set.indexed_before!=(indexed?1:0) || after.indexed_after-rebound_set.indexed_after!=(indexed?1:0)) {
                ++failures;printf("FAIL topology %s actual-callee-counts\n",name);
            }
        }
        CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command,&dst,0,0,0,&src,NULL));
        CHECK(vkdu_command_close(command));CHECK(vkdu_queue_execute(queue,1,&command));
        CHECK(vkdu_queue_signal(queue,fence,round+1));CHECK(vkdu_fence_wait(fence,round+1,10000));
        CHECK(vkdu_buffer_map(readback,0,4096,&mapped));
        for(i=0;i<1024;++i) {
            uint32_t want=sentinel,got=((const uint32_t *)mapped)[i];
            const int is_pixel=i>=128&&i<384&&(i-128)%64<16;
            if(is_pixel) {
                const unsigned x=(i-128)%64;
                want=((bound&&x<4)||(invalid&&x>=4&&x<8)||(x>=8&&x<12))?0xffffffff:0xff0000ff;
                ++total_pixels;
                if(x>=4&&x<8){middle_white+=got==0xffffffff;middle_red+=got==0xff0000ff;}
                if(x>=8&&x<12)rebound_white+=got==0xffffffff;
                pixel_mismatches+=got!=want;
            } else {++total_padding;padding_mismatches+=got!=want;}
            if(got!=want&&first_mismatch==UINT32_MAX){first_mismatch=i;first_actual=got;first_expected=want;}
        }
        CHECK(vkdu_buffer_unmap(readback,0,0));
        printf("OBS topology mode=%s case=%s set=%08x draw=%08x middle_white=%u middle_red=%u rebound_white=%u pixel_mismatches=%u padding_mismatches=%u first_word=%u actual=%08x expected=%08x\n",
            engine_only?"engine":"wrapper",name,(unsigned)set_result,(unsigned)draw_result,middle_white,middle_red,rebound_white,
            pixel_mismatches,padding_mismatches,first_mismatch,first_actual,first_expected);
        if(!engine_only) {
            if((invalid?set_result>=0:set_result<0)){++failures;printf("FAIL topology %s setter-contract\n",name);}
            if((invalid?draw_result<0:draw_result>=0)){++failures;printf("FAIL topology %s draw-gate\n",name);}
        }
        if(pixel_mismatches||padding_mismatches){++failures;printf("FAIL topology %s independent-readback\n",name);}
        if(rebound_white!=16){++failures;printf("FAIL topology %s same-value-rebind\n",name);}
    }
    CHECK(vkdu_device_status(device));
    vkdu_object_destroy(command);vkdu_object_destroy(allocator);vkdu_object_destroy(queue);vkdu_object_destroy(fence);
    vkdu_object_destroy(indices);vkdu_object_destroy(readback);vkdu_object_destroy(target);vkdu_object_destroy(heap);
    vkdu_object_destroy(pipeline);vkdu_object_destroy(root);
    printf("%s topology-clear mode=%s indexed=%d cases=%u pixels=%u padding=%u failures=%u real_completion=1 cleanup=1\n",
        failures?"RED":"PASS",engine_only?"engine":"wrapper",indexed,engine_only?1:4,total_pixels,total_padding,failures);
    return failures?1:0;
}
#endif
