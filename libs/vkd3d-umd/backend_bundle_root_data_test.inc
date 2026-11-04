/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual shader data, not an inspection of engine root-binding arrays. */
static void bundle_root_data_barrier(vkdu_object *command, vkdu_object *output)
{
    const struct vkdu_resource_barrier barrier = {VKDU_BARRIER_UAV,output,0,0};
    CHECK(vkdu_command_barriers(command,1,&barrier));
}
static void check_bundle_root_data(vkdu_device *device)
{
    vkdu_object *constants_root,*table_root,*graphics,*table_graphics,*compute,*table_compute;
    vkdu_object *target,*rtv,*heap,*input,*upload,*output,*readback,*queue,*allocator,*command,*fence;
    vkdu_object *bundle_allocator,*clear_bundle,*table_bundle;
    const struct vkdu_descriptor_range range={D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0,0,1};
    struct vkdu_root_parameter parameters[2]={{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,0,0,0,4},
        {D3D12_ROOT_PARAMETER_TYPE_UAV,0,0,0,0}};
    const struct vkdu_graphics_desc desc={DXGI_FORMAT_R8G8B8A8_UNORM,UINT32_MAX,3,1,0,1,15};
    const struct vkdu_viewport viewport={0,0,4,4,0,1};
    const struct vkdu_rect scissor={1,2,2,3};
    const struct vkdu_signature vin={6,0,1,1},vout={1,0,15,3};
    const uint32_t *v=graphics_tokens(vs_flat_color_code_dxbc),*p=graphics_tokens(ps_store_buffer_code_dxbc);
    uint32_t *mapped,round,i,values[4];
    const uint32_t sentinel=0x13579bdf;
    void *vs,*ps;size_t vs_bytes,ps_bytes;
    CHECK(vkdu_root_create(device,parameters,2,0,&constants_root));
    CHECK(vkdu_pipeline_create(device,constants_root,execute_indirect_multi_dispatch_root_constants_code_dxbc,
        sizeof(execute_indirect_multi_dispatch_root_constants_code_dxbc),&compute));
    parameters[0]=(struct vkdu_root_parameter){D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,0,0,0,0,&range,1};
    CHECK(vkdu_root_create(device,parameters,2,0,&table_root));
    CHECK(vkdu_pipeline_create(device,table_root,update_root_descriptors_code_dxbc,sizeof(update_root_descriptors_code_dxbc),&table_compute));
    CHECK(vkdu_shader_dxbc(1,v,v[1],&vin,1,&vout,1,&vs,&vs_bytes));
    CHECK(vkdu_shader_dxbc(0,p,p[1],NULL,0,NULL,0,&ps,&ps_bytes));
    CHECK(vkdu_graphics_pipeline_create(device,constants_root,vs,vs_bytes,ps,ps_bytes,&desc,&graphics));
    CHECK(vkdu_graphics_pipeline_create(device,table_root,vs,vs_bytes,ps,ps_bytes,&desc,&table_graphics));
    vkdu_shader_dxbc_free(vs);vkdu_shader_dxbc_free(ps);
    CHECK(vkdu_render_target_create(device,4,4,&target));
    CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,0,&rtv));CHECK(vkdu_texture2d_rtv(rtv,0,target));
    CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,8,1,&heap));
    CHECK(vkdu_buffer_create(device,1024,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&input));
    CHECK(vkdu_buffer_map(input,0,0,(void **)&mapped));memset(mapped,0,1024);
    mapped[0]=44;mapped[1]=0x0badcafe; /* Valid alternative for the table-offset RED. */
    mapped[64]=28;mapped[65]=0x2468ace0;mapped[128]=41;mapped[129]=0x89abcdef;
    CHECK(vkdu_buffer_unmap(input,0,1024));
    CHECK(vkdu_buffer_cbv(heap,1,input,0,256));CHECK(vkdu_buffer_cbv(heap,4,input,256,256));
    CHECK(vkdu_buffer_cbv(heap,6,input,512,256));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&upload));
    CHECK(vkdu_buffer_map(upload,0,0,(void **)&mapped));for(i=0;i<1024;++i)mapped[i]=sentinel;
    CHECK(vkdu_buffer_unmap(upload,0,4096));
    CHECK(vkdu_buffer_create(device,4096,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST,&output));
    CHECK(vkdu_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    CHECK(vkdu_queue_create(device,0,&queue));CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command));CHECK(vkdu_fence_create(device,0,&fence));
    CHECK(vkdu_allocator_create(device,1,&bundle_allocator));CHECK(vkdu_command_create(device,bundle_allocator,1,&clear_bundle));
    CHECK(vkdu_command_close(clear_bundle));
    CHECK(vkdu_command_create(device,bundle_allocator,1,&table_bundle));CHECK(vkdu_command_close(table_bundle));
    for(round=0;round<3;++round){
        const uint32_t seed_graphics=0xa1b2c300+round,patch=0xdecaf000+round;
        const uint32_t seed_compute=0x04020107+round;
        if(round){
            CHECK(vkdu_allocator_reset(allocator));CHECK(vkdu_command_reset(command,allocator));
            CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST));
        }
        CHECK(vkdu_command_copy(command,output,0,upload,0,4096));
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        CHECK(vkdu_command_graphics_root(command,constants_root));CHECK(vkdu_command_root(command,constants_root));
        CHECK(vkdu_command_render_target(command,rtv,0));CHECK(vkdu_command_viewports(command,1,&viewport));
        CHECK(vkdu_command_scissors(command,1,&scissor));CHECK(vkdu_command_topology(command,4));
        values[0]=20;values[1]=seed_graphics;values[2]=0x20000000;values[3]=0x40000000;
        CHECK(vkdu_command_graphics_constants(command,0,0,4,values));
        values[0]=7+round;values[1]=0x100;values[2]=0x20000;values[3]=0x4000000;
        CHECK(vkdu_command_constants(command,0,0,4,values));
        CHECK(vkdu_command_graphics_uav(command,1,output,0));CHECK(vkdu_command_uav(command,1,output,128));
        /* ClearRootArguments is recorded in a bundle. It must not eagerly
         * mutate this primary; seeded shader reads precede ExecuteBundle. */
        CHECK(vkdu_command_reset(clear_bundle,bundle_allocator));
        CHECK(vkdu_command_clear_root_arguments(clear_bundle));CHECK(vkdu_command_close(clear_bundle));
        CHECK(vkdu_command_pipeline(command,graphics));CHECK(vkdu_command_draw(command,3,1,0,0));
        bundle_root_data_barrier(command,output);
        CHECK(vkdu_command_pipeline(command,compute));CHECK(vkdu_command_dispatch(command,1,1,1));
        bundle_root_data_barrier(command,output);
        CHECK(vkdu_command_pipeline(command,graphics));CHECK(vkdu_command_execute_bundle(command,clear_bundle));
        /* Root layouts, PSO, topology, viewport and scissor stay live. Rebind
         * raw UAVs before shader access; the value constant remains zero. */
        values[0]=64;CHECK(vkdu_command_graphics_constants(command,0,0,1,values));
        CHECK(vkdu_command_graphics_uav(command,1,output,0));CHECK(vkdu_command_draw(command,3,1,0,0));
        bundle_root_data_barrier(command,output);
        CHECK(vkdu_command_graphics_constants(command,0,1,1,&patch));
        CHECK(vkdu_command_graphics_uav(command,1,output,16));CHECK(vkdu_command_draw(command,3,1,0,0));
        bundle_root_data_barrier(command,output);
        CHECK(vkdu_command_pipeline(command,compute));CHECK(vkdu_command_uav(command,1,output,256));
        CHECK(vkdu_command_dispatch(command,1,1,1));bundle_root_data_barrier(command,output);
        values[0]=0x00080000;CHECK(vkdu_command_constants(command,0,2,1,values));
        CHECK(vkdu_command_uav(command,1,output,320));CHECK(vkdu_command_dispatch(command,1,1,1));
        bundle_root_data_barrier(command,output);
        /* Replay the same closed stream after partial writes. It must clear
         * the new values as well, without re-recording the bundle. */
        CHECK(vkdu_command_pipeline(command,graphics));CHECK(vkdu_command_execute_bundle(command,clear_bundle));
        values[0]=88;CHECK(vkdu_command_graphics_constants(command,0,0,1,values));
        CHECK(vkdu_command_graphics_uav(command,1,output,0));CHECK(vkdu_command_draw(command,3,1,0,0));
        bundle_root_data_barrier(command,output);
        CHECK(vkdu_command_pipeline(command,compute));CHECK(vkdu_command_uav(command,1,output,384));
        CHECK(vkdu_command_dispatch(command,1,1,1));bundle_root_data_barrier(command,output);
        /* A separate bundle clears both stages and supplies real nonzero CBV
         * table bases. Round0 inherits root layouts; later rounds set their own.
         * Reading only rebound descriptors avoids dereferencing cleared tables. */
        CHECK(vkdu_command_reset(table_bundle,bundle_allocator));CHECK(vkdu_command_clear_root_arguments(table_bundle));
        if(round){CHECK(vkdu_command_graphics_root(table_bundle,table_root));CHECK(vkdu_command_root(table_bundle,table_root));}
        CHECK(vkdu_command_graphics_table(table_bundle,0,heap,3));CHECK(vkdu_command_table(table_bundle,0,heap,5));
        CHECK(vkdu_command_close(table_bundle));
        CHECK(vkdu_command_graphics_root(command,table_root));CHECK(vkdu_command_root(command,table_root));
        REJECT(vkdu_command_execute_bundle(command,table_bundle)); /* Heap mismatch, before any shader work. */
        CHECK(vkdu_command_heaps(command,1,&heap));CHECK(vkdu_command_execute_bundle(command,table_bundle));
        CHECK(vkdu_command_pipeline(command,table_graphics));CHECK(vkdu_command_graphics_uav(command,1,output,0));
        CHECK(vkdu_command_draw(command,3,1,0,0));bundle_root_data_barrier(command,output);
        CHECK(vkdu_command_pipeline(command,table_compute));CHECK(vkdu_command_uav(command,1,output,0));
        CHECK(vkdu_command_dispatch(command,1,1,1));bundle_root_data_barrier(command,output);
        CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command,readback,0,output,0,4096));CHECK(vkdu_command_close(command));
        CHECK(vkdu_queue_execute(queue,1,&command));CHECK(vkdu_queue_signal(queue,fence,round+1));
        CHECK(vkdu_fence_wait(fence,round+1,10000));CHECK(vkdu_buffer_map(readback,0,4096,(void **)&mapped));
        for(i=0;i<1024;++i){
            uint32_t expected=sentinel;
            if(i==5)expected=seed_graphics;if(i==16 || i==22)expected=0;if(i==20)expected=patch;
            if(i==32)expected+=seed_compute;if(i==80)expected+=0x00080000;
            if(i==7)expected=0x2468ace0;if(i==41)expected=0x89abcdef;
            if(mapped[i]!=expected){fprintf(stderr,"FAIL bundle root data round%u word%u=%08x expected%08x\n",round,i,mapped[i],expected);exit(1);}
        }
        CHECK(vkdu_buffer_unmap(readback,0,0));
        printf("PASS bundle root data round%u:1024 words; seeded-before-execute,dual-stage-clear,partial-rebind,CBV-table actual shader reads\n",round);
    }
    CHECK(vkdu_device_status(device));
    vkdu_object_destroy(command);vkdu_object_destroy(allocator);vkdu_object_destroy(clear_bundle);vkdu_object_destroy(table_bundle);
    vkdu_object_destroy(bundle_allocator);vkdu_object_destroy(queue);vkdu_object_destroy(fence);
    vkdu_object_destroy(graphics);vkdu_object_destroy(table_graphics);vkdu_object_destroy(compute);vkdu_object_destroy(table_compute);
    vkdu_object_destroy(constants_root);vkdu_object_destroy(table_root);vkdu_object_destroy(target);vkdu_object_destroy(rtv);
    vkdu_object_destroy(heap);vkdu_object_destroy(input);vkdu_object_destroy(upload);vkdu_object_destroy(output);vkdu_object_destroy(readback);
    puts("PASS bundle root data actual CPU Vulkan:3x1024 full readbacks; real clear replay,graphics/compute partial constants,inherited/own CBV tables,heap refusal,reset/replay");
}
