/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_bundles(vkdu_device *device, vkdu_device *other)
{
    vkdu_object *root, *root_constants, *pipeline, *target, *heap, *queue, *allocator, *command, *fence, *readback;
    vkdu_object *ba, *bundle, *foreign_allocator, *foreign, *indices, *wrong = NULL;
    const uint32_t *vt = graphics_tokens(vs_flat_color_code_dxbc), *pt = graphics_tokens(ps_flat_color_code_dxbc);
    const struct vkdu_signature input = {6,0,1,1}, position = {1,0,15,3}, color = {0,0,15,3};
    struct vkdu_graphics_desc desc = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT32_MAX,3,1,0,1,15};
    const struct vkdu_root_parameter parameter = {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, D3D12_SHADER_VISIBILITY_ALL,0,0,4,NULL,0};
    const struct vkdu_viewport view = {0,0,8,8,0,1};
    const struct vkdu_rect scissors[2] = {{0,0,4,8},{4,0,8,8}};
    const float clear[4] = {1,0,0,1};
    const uint32_t sentinel = 0x5a3cc3a5, values[4] = {1,2,3,4};
    void *vs, *ps, *mapped; size_t vbytes, pbytes;
    uint32_t i, round;
    CHECK(vkdu_root_create(device,NULL,0,0,&root));
    CHECK(vkdu_root_create(device,&parameter,1,0,&root_constants));
    CHECK(vkdu_shader_dxbc(1,vt,vt[1],&input,1,&position,1,&vs,&vbytes));
    CHECK(vkdu_shader_dxbc(0,pt,pt[1],NULL,0,&color,1,&ps,&pbytes));
    CHECK(vkdu_graphics_pipeline_create(device,root,vs,vbytes,ps,pbytes,&desc,&pipeline));
    vkdu_shader_dxbc_free(vs);vkdu_shader_dxbc_free(ps);
    CHECK(vkdu_render_target_create(device,8,8,&target));
    CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,0,&heap));
    CHECK(vkdu_texture2d_rtv(heap,0,target));
    CHECK(vkdu_queue_create(device,0,&queue));CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command));CHECK(vkdu_fence_create(device,0,&fence));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    CHECK(vkdu_buffer_create(device,256,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&indices));
    CHECK(vkdu_buffer_map(indices,0,0,&mapped)); ((uint16_t *)mapped)[0]=0;((uint16_t *)mapped)[1]=1;((uint16_t *)mapped)[2]=2;
    CHECK(vkdu_buffer_unmap(indices,0,6));
    CHECK(vkdu_allocator_create(device,1,&ba));CHECK(vkdu_command_create(device,ba,1,&bundle));
    CHECK(vkdu_allocator_create(other,1,&foreign_allocator));CHECK(vkdu_command_create(other,foreign_allocator,1,&foreign));
    /* Deferred root compatibility never relaxes the global 64-DWORD/index bounds.
     * Keep the first oversized payload valid in memory, so the unfixed control
     * demonstrates acceptance without relying on an out-of-bounds CPU read. */
    {
        vkdu_object *visible; uint32_t oversized[65] = {0};
        REJECT(vkdu_command_constants(bundle,0,0,65,oversized));
        REJECT(vkdu_command_graphics_constants(bundle,0,0,65,oversized));
        REJECT(vkdu_command_constants(bundle,0,0,65536,values));
        REJECT(vkdu_command_graphics_constants(bundle,0,0,UINT32_MAX,values));
        REJECT(vkdu_command_constants(bundle,0,64,1,values));
        REJECT(vkdu_command_graphics_constants(bundle,0,UINT32_MAX,1,values));
        REJECT(vkdu_command_constants(bundle,64,0,1,values));
        REJECT(vkdu_command_graphics_constants(bundle,64,0,1,values));
        REJECT(vkdu_command_srv(bundle,64,indices,0));
        REJECT(vkdu_command_graphics_srv(bundle,64,indices,0));
        CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,1,&visible));
        REJECT(vkdu_command_table(bundle,64,visible,0));
        REJECT(vkdu_command_graphics_table(bundle,64,visible,0));
        vkdu_object_destroy(visible);
    }
    REJECT(vkdu_queue_create(device,1,&wrong));REJECT(vkdu_command_create(device,allocator,1,&wrong));
    REJECT(vkdu_command_create(device,ba,0,&wrong));REJECT(vkdu_allocator_reset(ba));
    REJECT(vkdu_command_copy(bundle,readback,0,indices,0,4));
    REJECT(vkdu_command_transition(bundle,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
    REJECT(vkdu_command_render_target(bundle,heap,0));REJECT(vkdu_command_viewports(bundle,1,&view));
    REJECT(vkdu_command_scissors(bundle,1,&scissors[0]));REJECT(vkdu_command_dispatch(bundle,1,1,1));
    REJECT(vkdu_command_clear_rtv(bundle,heap,0,clear,0,NULL));
    {
        vkdu_object *upload;
        CHECK(vkdu_buffer_create(device,2048,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&upload));
        REJECT(vkdu_command_texture_upload(bundle,target,upload,0,256));
        vkdu_object_destroy(upload);
    }
    REJECT(vkdu_command_execute_bundle(command,bundle)); /* Recording bundle. */
    CHECK(vkdu_command_pipeline(bundle,pipeline));CHECK(vkdu_command_topology(bundle,4));
    CHECK(vkdu_command_draw_indexed(bundle,3,1,0,0,0)); /* Inherit root/IB/targets; PSO/topology set in bundle. */
    CHECK(vkdu_command_close(bundle));CHECK(vkdu_command_close(foreign));
    REJECT(vkdu_command_execute_bundle(bundle,bundle));REJECT(vkdu_command_execute_bundle(command,foreign));
    REJECT(vkdu_command_execute_bundle(command,command));REJECT(vkdu_queue_execute(queue,1,&bundle));
    REJECT(vkdu_command_execute_bundle(command,bundle)); /* No inherited state. */
    /* Rejected draws must not leave deferred inheritance requirements behind.
     * This stream supplies its own root after two invalid draws, while the
     * parent deliberately has no graphics root when ExecuteBundle is called. */
    {
        vkdu_object *probe_allocator,*probe;
        CHECK(vkdu_allocator_create(device,1,&probe_allocator));CHECK(vkdu_command_create(device,probe_allocator,1,&probe));
        CHECK(vkdu_command_pipeline(probe,pipeline));CHECK(vkdu_command_topology(probe,4));
        REJECT(vkdu_command_draw(probe,3,1,UINT32_MAX,0));
        REJECT(vkdu_command_draw_indexed(probe,3,1,UINT32_MAX,0,0));
        CHECK(vkdu_command_graphics_root(probe,root));CHECK(vkdu_command_draw(probe,3,1,0,0));
        CHECK(vkdu_command_close(probe));CHECK(vkdu_command_render_target(command,heap,0));
        CHECK(vkdu_command_viewports(command,1,&view));CHECK(vkdu_command_scissors(command,1,&scissors[0]));
        CHECK(vkdu_command_execute_bundle(command,probe));
        CHECK(vkdu_command_close(command));CHECK(vkdu_allocator_reset(allocator));CHECK(vkdu_command_reset(command,allocator));
        vkdu_object_destroy(probe);vkdu_object_destroy(probe_allocator);
    }
    /* Intermediate heap binds cannot be hidden by restoring a final pair. */
    {
        vkdu_object *probe_allocator,*probe,*first_heap,*second_heap;
        CHECK(vkdu_allocator_create(device,1,&probe_allocator));CHECK(vkdu_command_create(device,probe_allocator,1,&probe));
        CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,1,&first_heap));
        CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,1,&second_heap));
        CHECK(vkdu_command_heaps(probe,1,&first_heap));REJECT(vkdu_command_heaps(probe,1,&second_heap));
        CHECK(vkdu_command_heaps(probe,1,&first_heap));CHECK(vkdu_command_close(probe));
        CHECK(vkdu_command_heaps(command,1,&second_heap));REJECT(vkdu_command_execute_bundle(command,probe));
        CHECK(vkdu_command_heaps(command,1,&first_heap));CHECK(vkdu_command_execute_bundle(command,probe));
        CHECK(vkdu_command_close(command));CHECK(vkdu_allocator_reset(allocator));CHECK(vkdu_command_reset(command,allocator));
        vkdu_object_destroy(probe);vkdu_object_destroy(probe_allocator);vkdu_object_destroy(first_heap);vkdu_object_destroy(second_heap);
    }
    /* Closed stream switches allocator. Resetting its old allocator must not erase the new stream. */
    {
        vkdu_object *replacement;
        CHECK(vkdu_allocator_create(device,1,&replacement));CHECK(vkdu_command_reset(bundle,replacement));
        CHECK(vkdu_command_pipeline(bundle,pipeline));CHECK(vkdu_command_topology(bundle,4));
        CHECK(vkdu_command_draw_indexed(bundle,3,1,0,0,0));CHECK(vkdu_command_close(bundle));
        CHECK(vkdu_allocator_reset(ba));vkdu_object_destroy(ba);ba=replacement;
    }
    for (round=0;round<3;round++) {
        struct vkdu_texture_copy_location dst = {readback,512,1,DXGI_FORMAT_R8G8B8A8_UNORM,8,8,1,256};
        struct vkdu_texture_copy_location src = {target,0,0};
        if (round) {
            CHECK(vkdu_allocator_reset(allocator));CHECK(vkdu_command_reset(command,allocator));
            CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));
        }
        CHECK(vkdu_buffer_map(readback,0,0,&mapped));for(i=0;i<1024;i++)((uint32_t *)mapped)[i]=sentinel;
        CHECK(vkdu_buffer_unmap(readback,0,4096));
        CHECK(vkdu_command_clear_rtv(command,heap,0,clear,0,NULL));
        CHECK(vkdu_command_render_target(command,heap,0));CHECK(vkdu_command_viewports(command,1,&view));
        CHECK(vkdu_command_scissors(command,1,&scissors[0]));CHECK(vkdu_command_graphics_root(command,root));
        REJECT(vkdu_command_execute_bundle(command,bundle)); /* Missing inherited IB. */
        CHECK(vkdu_command_index_buffer(command,indices,0,6,DXGI_FORMAT_R16_UINT));
        CHECK(vkdu_command_execute_bundle(command,bundle));CHECK(vkdu_command_execute_bundle(command,bundle));
        REJECT(vkdu_allocator_reset(ba));REJECT(vkdu_command_reset(bundle,ba));
        CHECK(vkdu_command_scissors(command,1,&scissors[1]));
        CHECK(vkdu_command_draw_indexed(command,3,1,0,0,0)); /* Bundle PSO/topology persist in parent bookkeeping. */
        CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command,&dst,0,0,0,&src,NULL));
        CHECK(vkdu_command_close(command));REJECT(vkdu_command_execute_bundle(command,bundle));
        CHECK(vkdu_queue_execute(queue,1,&command));CHECK(vkdu_queue_signal(queue,fence,round+1));
        CHECK(vkdu_fence_wait(fence,round+1,10000));CHECK(vkdu_buffer_map(readback,0,4096,&mapped));
        for(i=0;i<1024;i++) {
            uint32_t expected=sentinel;
            if(i>=128 && i<640 && (i-128)%64<8)expected=0xffffffff;
            if(((uint32_t *)mapped)[i]!=expected) {fprintf(stderr,"FAIL bundle round%u word%u=%08x expected%08x\n",round,i,((uint32_t *)mapped)[i],expected);exit(1);}
        }
        CHECK(vkdu_buffer_unmap(readback,0,0));
    }
    /* Parent reset releases its bundle/allocator hold. Allocator reset invalidates old stream generation. */
    CHECK(vkdu_allocator_reset(allocator));CHECK(vkdu_command_reset(command,allocator));
    CHECK(vkdu_allocator_reset(ba));REJECT(vkdu_command_execute_bundle(command,bundle));CHECK(vkdu_command_reset(bundle,ba));
    CHECK(vkdu_command_graphics_constants(bundle,0,0,4,values)); /* Deferred inherited root shape. */
    CHECK(vkdu_command_close(bundle));REJECT(vkdu_command_execute_bundle(command,bundle));
    CHECK(vkdu_command_graphics_root(command,root));REJECT(vkdu_command_execute_bundle(command,bundle));
    CHECK(vkdu_command_graphics_root(command,root_constants));CHECK(vkdu_command_execute_bundle(command,bundle));
    CHECK(vkdu_command_close(command));CHECK(vkdu_queue_execute(queue,1,&command));
    CHECK(vkdu_queue_signal(queue,fence,4));CHECK(vkdu_fence_wait(fence,4,10000));
    /* Two closed bundles share an allocator. All public wrappers are dropped
     * before completion; the primary's COM owner ledger must retain replayed
     * objects independently of its deduplicated [B1,A,B2] release order. */
    CHECK(vkdu_allocator_reset(allocator));CHECK(vkdu_command_reset(command,allocator));
    CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));
    CHECK(vkdu_command_clear_rtv(command,heap,0,clear,0,NULL));
    CHECK(vkdu_command_render_target(command,heap,0));CHECK(vkdu_command_viewports(command,1,&view));
    CHECK(vkdu_command_scissors(command,1,&scissors[0]));CHECK(vkdu_command_graphics_root(command,root));
    CHECK(vkdu_command_index_buffer(command,indices,0,6,DXGI_FORMAT_R16_UINT));
    CHECK(vkdu_allocator_reset(ba));CHECK(vkdu_command_reset(bundle,ba));
    CHECK(vkdu_command_graphics_root(bundle,root));CHECK(vkdu_command_pipeline(bundle,pipeline));CHECK(vkdu_command_topology(bundle,4));
    CHECK(vkdu_command_index_buffer(bundle,indices,0,6,DXGI_FORMAT_R16_UINT));
    CHECK(vkdu_command_draw_indexed(bundle,3,1,0,0,0));CHECK(vkdu_command_close(bundle));
    {
        vkdu_object *second;struct vkdu_texture_copy_location dst={readback,512,1,DXGI_FORMAT_R8G8B8A8_UNORM,8,8,1,256},src={target,0,0};
        CHECK(vkdu_command_create(device,ba,1,&second));CHECK(vkdu_command_pipeline(second,pipeline));
        CHECK(vkdu_command_topology(second,4));CHECK(vkdu_command_draw_indexed(second,3,1,0,0,0));CHECK(vkdu_command_close(second));
        CHECK(vkdu_command_execute_bundle(command,bundle));CHECK(vkdu_command_execute_bundle(command,second));
        CHECK(vkdu_command_scissors(command,1,&scissors[1]));CHECK(vkdu_command_draw_indexed(command,3,1,0,0,0));
        vkdu_object_destroy(second);vkdu_object_destroy(bundle);bundle=NULL;vkdu_object_destroy(ba);ba=NULL;
        vkdu_object_destroy(root);root=NULL;vkdu_object_destroy(pipeline);pipeline=NULL;vkdu_object_destroy(indices);indices=NULL;
        CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command,&dst,0,0,0,&src,NULL));CHECK(vkdu_command_close(command));
        CHECK(vkdu_queue_execute(queue,1,&command));vkdu_object_destroy(command);command=NULL;
        CHECK(vkdu_queue_signal(queue,fence,5));CHECK(vkdu_fence_wait(fence,5,10000));
        CHECK(vkdu_buffer_map(readback,0,4096,&mapped));
        for(i=0;i<1024;i++){uint32_t expected=(i>=128 && i<640 && (i-128)%64<8)?0xffffffff:sentinel;
            if(((uint32_t *)mapped)[i]!=expected){fprintf(stderr,"FAIL bundle early-destroy word%u=%08x expected%08x\n",i,((uint32_t *)mapped)[i],expected);exit(1);}}
        CHECK(vkdu_buffer_unmap(readback,0,0));
    }
    vkdu_object_destroy(command);vkdu_object_destroy(bundle);vkdu_object_destroy(ba);
    vkdu_object_destroy(foreign);vkdu_object_destroy(foreign_allocator);vkdu_object_destroy(indices);
    vkdu_object_destroy(pipeline);vkdu_object_destroy(root);vkdu_object_destroy(root_constants);
    vkdu_object_destroy(readback);vkdu_object_destroy(heap);vkdu_object_destroy(target);
    vkdu_object_destroy(fence);vkdu_object_destroy(queue);vkdu_object_destroy(allocator);
    CHECK(vkdu_device_status(device));CHECK(vkdu_device_status(other));
    puts("PASS bundle actual CPU Vulkan: 4x1024 full readback words, inherited root/IB/targets, replay, resulting PSO/topology, cross-allocator reset, two shared-allocator bundles and early wrapper destruction; global64 bounds, rejected draw atomicity, upload/type/state negatives");
}
