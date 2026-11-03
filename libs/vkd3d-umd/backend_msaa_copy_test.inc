/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Review regressions: legacy non-RT resolve destinations, full same-count
 * copies, and two distinct resource wrappers aliasing one heap range. */
static void check_msaa_alias_copy(vkdu_device *device)
{
    const struct vkdu_texture2d_shape shape = {17,9,28,1,3,1,4,0};
    vkdu_object *heap = NULL, *src = NULL, *dst = NULL, *allocator = NULL, *command = NULL;
    uint64_t bytes, alignment; uint32_t failures = 0;
    CHECK(vkdu_texture2d_shape_allocation(device,&shape,&bytes,&alignment));
    CHECK(vkdu_test_attachment_heap_create_aligned(device,bytes*2,alignment,&heap));
    CHECK(vkdu_texture2d_shape_place(device,heap,bytes,&shape,D3D12_RESOURCE_STATE_COPY_SOURCE,&src));
    CHECK(vkdu_texture2d_shape_place(device,heap,bytes,&shape,D3D12_RESOURCE_STATE_COPY_DEST,&dst));
    CHECK(vkdu_allocator_create(device,0,&allocator)); CHECK(vkdu_command_create(device,allocator,0,&command));
    if (SUCCEEDED(vkdu_command_resource_copy(command,dst,src))) {
        fprintf(stderr,"FAIL MSAA alias CopyResource accepted overlapping heap ranges\n"); ++failures;
    }
    {
        const struct vkdu_texture_copy_location a = {dst,1}, b = {src,1};
        if (SUCCEEDED(vkdu_command_texture_copy(command,&a,0,0,0,&b,NULL))) {
            fprintf(stderr,"FAIL MSAA alias CopyTextureRegion accepted overlapping heap ranges\n"); ++failures;
        }
    }
    /* Invalid copies are never submitted, even on the intentionally RED build. */
    vkdu_object_destroy(command); vkdu_object_destroy(allocator);
    vkdu_object_destroy(src); vkdu_object_destroy(dst); vkdu_object_destroy(heap);
    if (failures) exit(1);
    puts("PASS MSAA alias CopyResource and CopyTextureRegion rejection");
}
static void check_msaa_copy_nonrt(vkdu_device *device)
{
    const struct vkdu_texture2d_shape shape = {17,9,28,1,3,1,4,0};
    vkdu_object *nonrt_heap = NULL, *rt_heap = NULL, *src = NULL, *whole = NULL, *region = NULL;
    vkdu_object *dst[6] = {0}, *rtv = NULL, *allocator = NULL, *command = NULL, *queue = NULL, *fence = NULL, *readback = NULL;
    uint64_t bytes, alignment, nonrt_bytes; uint8_t *mapped, expected[65536];
    uint32_t i, layer, x, y, c, mismatches = 0;
    CHECK(vkdu_texture2d_allocation(device,17,9,28,&nonrt_bytes,&alignment));
    CHECK(vkdu_test_texture_heap_create(device,nonrt_bytes*6,&nonrt_heap));
    for (i=0;i<6;++i) {
        HRESULT hr = vkdu_texture2d_place(device,nonrt_heap,nonrt_bytes*i,17,9,28,D3D12_RESOURCE_STATE_RESOLVE_DEST,&dst[i]);
        if (FAILED(hr)) { fprintf(stderr,"FAIL MSAA non-RT RESOLVE_DEST placement returned %08x\n",(unsigned)hr); exit(1); }
    }
    CHECK(vkdu_texture2d_shape_allocation(device,&shape,&bytes,&alignment));
    CHECK(vkdu_test_attachment_heap_create_aligned(device,bytes*3,alignment,&rt_heap));
    CHECK(vkdu_texture2d_shape_place(device,rt_heap,0,&shape,D3D12_RESOURCE_STATE_RENDER_TARGET,&src));
    CHECK(vkdu_texture2d_shape_place(device,rt_heap,bytes,&shape,D3D12_RESOURCE_STATE_COPY_DEST,&whole));
    CHECK(vkdu_texture2d_shape_place(device,rt_heap,bytes*2,&shape,D3D12_RESOURCE_STATE_RENDER_TARGET,&region));
    CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,6,0,&rtv));
    for (layer=0;layer<3;++layer) {
        CHECK(vkdu_texture2d_rtv_range(rtv,layer,src,28,0,layer,1));
        CHECK(vkdu_texture2d_rtv_range(rtv,3+layer,region,28,0,layer,1));
    }
    CHECK(vkdu_allocator_create(device,0,&allocator)); CHECK(vkdu_command_create(device,allocator,0,&command));
    CHECK(vkdu_queue_create(device,0,&queue)); CHECK(vkdu_fence_create(device,0,&fence));
    CHECK(vkdu_buffer_create(device,65536,D3D12_HEAP_TYPE_READBACK,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    CHECK(vkdu_buffer_map(readback,0,0,(void **)&mapped)); memset(mapped,0xa5,65536); CHECK(vkdu_buffer_unmap(readback,0,65536));
    memset(expected,0xa5,sizeof(expected));
    for (layer=0;layer<3;++layer) {
        float color[4]; msaa_float_color(msaa_clear_colors[layer],color);
        CHECK(vkdu_command_clear_rtv(command,rtv,layer,color,0,NULL));
        msaa_float_color(msaa_destination_clear,color);
        CHECK(vkdu_command_clear_rtv(command,rtv,3+layer,color,0,NULL));
    }
    CHECK(vkdu_command_transition(command,src,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
    CHECK(vkdu_command_resource_copy(command,whole,src));
    CHECK(vkdu_command_transition(command,whole,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE));
    CHECK(vkdu_command_transition(command,region,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST));
    {
        const struct vkdu_texture_copy_location a = {region,2}, b = {whole,1};
        CHECK(vkdu_command_texture_copy(command,&a,0,0,0,&b,NULL));
    }
    CHECK(vkdu_command_transition(command,whole,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RESOLVE_SOURCE));
    CHECK(vkdu_command_transition(command,region,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RESOLVE_SOURCE));
    for (i=0;i<6;++i) {
        const uint8_t *color = i<3 ? msaa_clear_colors[i] : i==5 ? msaa_clear_colors[1] : msaa_destination_clear;
        const struct vkdu_texture_copy_location a = {readback,512+i*8192,1,28,17,9,1,256}, b = {dst[i],0};
        CHECK(vkdu_command_resolve(command,dst[i],0,i<3 ? whole : region,i%3,28));
        CHECK(vkdu_command_transition(command,dst[i],D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command,&a,0,0,0,&b,NULL));
        for (y=0;y<9;++y) for (x=0;x<17;++x) for (c=0;c<4;++c) expected[512+i*8192+y*256+x*4+c]=color[c];
    }
    CHECK(vkdu_command_close(command));
    for (i=0;i<6;++i) vkdu_object_destroy(dst[i]);
    vkdu_object_destroy(src); vkdu_object_destroy(whole); vkdu_object_destroy(region);
    vkdu_object_destroy(nonrt_heap); vkdu_object_destroy(rt_heap); vkdu_object_destroy(rtv);
    CHECK(vkdu_queue_execute(queue,1,&command)); CHECK(vkdu_queue_signal(queue,fence,1)); CHECK(vkdu_fence_wait(fence,1,10000));
    CHECK(vkdu_buffer_map(readback,0,65536,(void **)&mapped));
    for (i=0;i<65536;++i) if (mapped[i]!=expected[i]) {
        if (mismatches++<4) fprintf(stderr,"FAIL MSAA copy/non-RT byte%u got%u expected%u\n",i,mapped[i],expected[i]);
    }
    CHECK(vkdu_buffer_unmap(readback,0,0));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence); vkdu_object_destroy(readback);
    if (mismatches) exit(1);
    puts("PASS MSAA count4 full CopyResource and CopyTextureRegion, non-RT RESOLVE_DEST, 6 subresources, guard65536, retained-before-execute");
}
