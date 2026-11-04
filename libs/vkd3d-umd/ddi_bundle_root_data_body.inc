/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual shader data, not an inspection of engine root-binding arrays. */
static void ddi_bundle_root_data_barrier(DdiItem *command, DdiItem *output)
{
    const struct DdiBarrier barrier = {VKDU_BARRIER_UAV,output,0,0};
    CHECK(ddii_command_barriers(command,1,&barrier));
}
static void check_ddi_bundle_root_data(DdiFixture *device)
{
    DdiItem *constants_root,*table_root,*graphics,*table_graphics,*compute,*table_compute;
    DdiItem *target,*rtv,*heap,*input,*upload,*output,*readback,*queue,*allocator,*command,*fence;
    DdiItem *bundle_allocator,*clear_bundle,*table_bundle;
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
    CHECK(ddii_root_create(device,parameters,2,0,&constants_root));
    CHECK(ddii_pipeline_create(device,constants_root,execute_indirect_multi_dispatch_root_constants_code_dxbc,
        sizeof(execute_indirect_multi_dispatch_root_constants_code_dxbc),&compute));
    parameters[0]=(struct vkdu_root_parameter){D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,0,0,0,0,&range,1};
    CHECK(ddii_root_create(device,parameters,2,0,&table_root));
    CHECK(ddii_pipeline_create(device,table_root,update_root_descriptors_code_dxbc,sizeof(update_root_descriptors_code_dxbc),&table_compute));
    CHECK(ddii_shader_dxbc(1,v,v[1],&vin,1,&vout,1,&vs,&vs_bytes));
    CHECK(ddii_shader_dxbc(0,p,p[1],NULL,0,NULL,0,&ps,&ps_bytes));
    CHECK(ddii_graphics_pipeline_create(device,constants_root,vs,vs_bytes,ps,ps_bytes,&desc,&graphics));
    CHECK(ddii_graphics_pipeline_create(device,table_root,vs,vs_bytes,ps,ps_bytes,&desc,&table_graphics));
    ddii_shader_dxbc_free(vs);ddii_shader_dxbc_free(ps);
    CHECK(ddii_render_target_create(device,4,4,&target));
    CHECK(ddii_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,0,&rtv));CHECK(ddii_texture2d_rtv(rtv,0,target));
    CHECK(ddii_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,8,1,&heap));
    CHECK(ddii_buffer_create(device,1024,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&input));
    CHECK(ddii_buffer_map(input,0,0,(void **)&mapped));memset(mapped,0,1024);
    mapped[0]=44;mapped[1]=0x0badcafe; /* Valid alternative for the table-offset RED. */
    mapped[64]=28;mapped[65]=0x2468ace0;mapped[128]=41;mapped[129]=0x89abcdef;
    CHECK(ddii_buffer_unmap(input,0,1024));
    CHECK(ddii_buffer_cbv(heap,1,input,0,256));CHECK(ddii_buffer_cbv(heap,4,input,256,256));
    CHECK(ddii_buffer_cbv(heap,6,input,512,256));
    CHECK(ddii_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&upload));
    CHECK(ddii_buffer_map(upload,0,0,(void **)&mapped));for(i=0;i<1024;++i)mapped[i]=sentinel;
    CHECK(ddii_buffer_unmap(upload,0,4096));
    CHECK(ddii_buffer_create(device,4096,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST,&output));
    CHECK(ddii_buffer_create(device,4096,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    CHECK(ddii_queue_create(device,0,&queue));CHECK(ddii_allocator_create(device,0,&allocator));
    CHECK(ddii_command_create(device,allocator,0,&command));CHECK(ddii_fence_create(device,0,&fence));
    CHECK(ddii_allocator_create(device,1,&bundle_allocator));CHECK(ddii_command_create(device,bundle_allocator,1,&clear_bundle));
    CHECK(ddii_command_close(clear_bundle));
    CHECK(ddii_command_create(device,bundle_allocator,1,&table_bundle));CHECK(ddii_command_close(table_bundle));
    for(round=0;round<3;++round){
        const uint32_t seed_graphics=0xa1b2c300+round,patch=0xdecaf000+round;
        const uint32_t seed_compute=0x04020107+round;
        if(round){
            CHECK(ddii_allocator_reset(allocator));CHECK(ddii_command_reset(command,allocator));
            CHECK(ddii_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST));
        }
        CHECK(ddii_command_copy(command,output,0,upload,0,4096));
        CHECK(ddii_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        CHECK(ddii_command_graphics_root(command,constants_root));CHECK(ddii_command_root(command,constants_root));
        CHECK(ddii_command_render_target(command,rtv,0));CHECK(ddii_command_viewports(command,1,&viewport));
        CHECK(ddii_command_scissors(command,1,&scissor));CHECK(ddii_command_topology(command,4));
        values[0]=20;values[1]=seed_graphics;values[2]=0x20000000;values[3]=0x40000000;
        CHECK(ddii_command_graphics_constants(command,0,0,4,values));
        values[0]=7+round;values[1]=0x100;values[2]=0x20000;values[3]=0x4000000;
        CHECK(ddii_command_constants(command,0,0,4,values));
        CHECK(ddii_command_graphics_uav(command,1,output,0));CHECK(ddii_command_uav(command,1,output,128));
        /* ClearRootArguments is recorded in a bundle. It must not eagerly
         * mutate this primary; seeded shader reads precede ExecuteBundle. */
        CHECK(ddii_command_reset(clear_bundle,bundle_allocator));
        CHECK(ddii_command_clear_root_arguments(clear_bundle));CHECK(ddii_command_close(clear_bundle));
        CHECK(ddii_command_pipeline(command,graphics));CHECK(ddii_command_draw(command,3,1,0,0));
        ddi_bundle_root_data_barrier(command,output);
        CHECK(ddii_command_pipeline(command,compute));CHECK(ddii_command_dispatch(command,1,1,1));
        ddi_bundle_root_data_barrier(command,output);
        CHECK(ddii_command_pipeline(command,graphics));CHECK(ddii_command_execute_bundle(command,clear_bundle));
        /* Root layouts, PSO, topology, viewport and scissor stay live. Rebind
         * raw UAVs before shader access; the value constant remains zero. */
        values[0]=64;CHECK(ddii_command_graphics_constants(command,0,0,1,values));
        CHECK(ddii_command_graphics_uav(command,1,output,0));CHECK(ddii_command_draw(command,3,1,0,0));
        ddi_bundle_root_data_barrier(command,output);
        CHECK(ddii_command_graphics_constants(command,0,1,1,&patch));
        CHECK(ddii_command_graphics_uav(command,1,output,16));CHECK(ddii_command_draw(command,3,1,0,0));
        ddi_bundle_root_data_barrier(command,output);
        CHECK(ddii_command_pipeline(command,compute));CHECK(ddii_command_uav(command,1,output,256));
        CHECK(ddii_command_dispatch(command,1,1,1));ddi_bundle_root_data_barrier(command,output);
        values[0]=0x00080000;CHECK(ddii_command_constants(command,0,2,1,values));
        CHECK(ddii_command_uav(command,1,output,320));CHECK(ddii_command_dispatch(command,1,1,1));
        ddi_bundle_root_data_barrier(command,output);
        /* Replay the same closed stream after partial writes. It must clear
         * the new values as well, without re-recording the bundle. */
        CHECK(ddii_command_pipeline(command,graphics));CHECK(ddii_command_execute_bundle(command,clear_bundle));
        values[0]=88;CHECK(ddii_command_graphics_constants(command,0,0,1,values));
        CHECK(ddii_command_graphics_uav(command,1,output,0));CHECK(ddii_command_draw(command,3,1,0,0));
        ddi_bundle_root_data_barrier(command,output);
        CHECK(ddii_command_pipeline(command,compute));CHECK(ddii_command_uav(command,1,output,384));
        CHECK(ddii_command_dispatch(command,1,1,1));ddi_bundle_root_data_barrier(command,output);
        /* A separate bundle clears both stages and supplies real nonzero CBV
         * table bases. Round0 inherits root layouts; later rounds set their own.
         * Reading only rebound descriptors avoids dereferencing cleared tables. */
        CHECK(ddii_command_reset(table_bundle,bundle_allocator));CHECK(ddii_command_clear_root_arguments(table_bundle));
        if(round){CHECK(ddii_command_graphics_root(table_bundle,table_root));CHECK(ddii_command_root(table_bundle,table_root));}
        CHECK(ddii_command_graphics_table(table_bundle,0,heap,3));CHECK(ddii_command_table(table_bundle,0,heap,5));
        CHECK(ddii_command_close(table_bundle));
        CHECK(ddii_command_graphics_root(command,table_root));CHECK(ddii_command_root(command,table_root));
        REJECT(ddii_command_execute_bundle(command,table_bundle)); /* Heap mismatch, before any shader work. */
        CHECK(ddii_command_heaps(command,1,&heap));CHECK(ddii_command_execute_bundle(command,table_bundle));
        CHECK(ddii_command_pipeline(command,table_graphics));CHECK(ddii_command_graphics_uav(command,1,output,0));
        CHECK(ddii_command_draw(command,3,1,0,0));ddi_bundle_root_data_barrier(command,output);
        CHECK(ddii_command_pipeline(command,table_compute));CHECK(ddii_command_uav(command,1,output,0));
        CHECK(ddii_command_dispatch(command,1,1,1));ddi_bundle_root_data_barrier(command,output);
        CHECK(ddii_command_transition(command,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(ddii_command_copy(command,readback,0,output,0,4096));CHECK(ddii_command_close(command));
        CHECK(ddii_queue_execute(queue,1,&command));CHECK(ddii_queue_signal(queue,fence,round+1));
        CHECK(ddii_fence_wait(fence,round+1,10000));CHECK(ddii_buffer_map(readback,0,4096,(void **)&mapped));
        for(i=0;i<1024;++i){
            uint32_t expected=sentinel;
            if(i==5)expected=seed_graphics;if(i==16 || i==22)expected=0;if(i==20)expected=patch;
            if(i==32)expected+=seed_compute;if(i==80)expected+=0x00080000;
            if(i==7)expected=0x2468ace0;if(i==41)expected=0x89abcdef;
            if(mapped[i]!=expected){fprintf(stderr,"FAIL integrated DDI root data round%u word%u=%08x expected%08x\n",round,i,mapped[i],expected);exit(1);}
        }
        CHECK(ddii_buffer_unmap(readback,0,0));
        printf("PASS integrated DDI root data round%u:1024 words; seeded-before-execute,dual-stage-clear,partial-rebind,CBV-table actual shader reads\n",round);
    }
    CHECK(ddii_device_status(device));
    ddii_object_destroy(command);ddii_object_destroy(allocator);ddii_object_destroy(clear_bundle);ddii_object_destroy(table_bundle);
    ddii_object_destroy(bundle_allocator);ddii_object_destroy(queue);ddii_object_destroy(fence);
    ddii_object_destroy(graphics);ddii_object_destroy(table_graphics);ddii_object_destroy(compute);ddii_object_destroy(table_compute);
    ddii_object_destroy(constants_root);ddii_object_destroy(table_root);ddii_object_destroy(target);ddii_object_destroy(rtv);
    ddii_object_destroy(heap);ddii_object_destroy(input);ddii_object_destroy(upload);ddii_object_destroy(output);ddii_object_destroy(readback);
    puts("PASS integrated DDI root data actual CPU Vulkan:3x1024 full readbacks; real clear replay,graphics/compute partial constants,inherited/own CBV tables,heap refusal,reset/replay");
}
