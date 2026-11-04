/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_stream_output_limits(vkdu_device *device)
{
    vkdu_object *root, *pipeline, *queue, *allocator, *command, *fence, *vertices, *upload, *output, *readback;
    const struct vkdu_signature vin[] = {{0,0,15,3},{0,1,15,3}}, link[] = {{1,0,15,3},{0,1,15,3}};
    struct vkdu_input_element inputs[] = {{0,0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,0},
        {0,16,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,1}};
    struct vkdu_graphics_desc state = {0,UINT32_MAX,3,1,0,1,15,2,inputs};
    struct vkdu_stream_output_desc so = {128,1,UINT32_MAX,{{0}},{2048}};
    const uint32_t *tokens = graphics_tokens(unaligned_vertex_stride_vs_code_dxbc);
    const float vertex[] = {.125f,.25f,.5f,1, .25f,.5f,.75f,1};
    struct vkdu_stream_output_buffer view;
    struct vkdu_vertex_buffer vb;
    void *code, *mapped; size_t size; uint32_t i;
    CHECK(vkdu_shader_dxbc(1,tokens,tokens[1],vin,2,link,2,&code,&size));
    for (i = 0; i < 127; ++i) so.entries[i] = (struct vkdu_stream_output_entry){0,0,UINT32_MAX,1};
    so.entries[127] = (struct vkdu_stream_output_entry){0,0,0,8};
    CHECK(vkdu_stream_output_validate(&so,code,size));
    so.entries[128] = (struct vkdu_stream_output_entry){0,0,UINT32_MAX,1}; so.entry_count = 129;
    REJECT(vkdu_stream_output_validate(&so,code,size)); so.entry_count = 128;
    so.strides[0] = 2052; REJECT(vkdu_stream_output_validate(&so,code,size));
    so.strides[0] = 508; REJECT(vkdu_stream_output_validate(&so,code,size)); so.strides[0] = 2048;
    so.entries[126].mask = 2; REJECT(vkdu_stream_output_validate(&so,code,size)); so.entries[126].mask = 1;
    CHECK(vkdu_root_create(device,NULL,0,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT,&root));
    state.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; state.stream_output = &so;
#ifdef VKDU_ENABLE_TEST_DEVICE
    /* This controlled CPU fixture selects lavapipe, whose advertised Vulkan
     * maximum is512. 2048 is D3D-valid but must not emit invalid Vulkan. */
    pipeline=(vkdu_object *)(uintptr_t)1;
    if (vkdu_graphics_pipeline_create(device,root,code,size,NULL,0,&state,&pipeline)!=(int32_t)0x80004001 || pipeline) {
        fputs("FAIL unsupported physical SO stride was not rejected before Vulkan\n",stderr);exit(1);
    }
#endif
    so.strides[0]=512;
    CHECK(vkdu_graphics_pipeline_create(device,root,code,size,NULL,0,&state,&pipeline));
    CHECK(vkdu_queue_create(device,0,&queue)); CHECK(vkdu_allocator_create(device,0,&allocator));
    CHECK(vkdu_command_create(device,allocator,0,&command)); CHECK(vkdu_fence_create(device,0,&fence));
    CHECK(vkdu_buffer_create(device,4096,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&vertices));
    CHECK(vkdu_buffer_create(device,8192,2,0,D3D12_RESOURCE_STATE_GENERIC_READ,&upload));
    CHECK(vkdu_buffer_create(device,8192,1,0,D3D12_RESOURCE_STATE_COPY_DEST,&output));
    CHECK(vkdu_buffer_create(device,8192,3,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
    CHECK(vkdu_buffer_map(vertices,0,0,&mapped)); memcpy((char *)mapped+64,vertex,sizeof(vertex));
    CHECK(vkdu_buffer_unmap(vertices,64,96));
    CHECK(vkdu_buffer_map(upload,0,0,&mapped));
    for (i = 0; i < 2048; ++i) ((uint32_t *)mapped)[i] = 0x7f7a7b7c;
    ((uint32_t *)mapped)[1920] = 0; CHECK(vkdu_buffer_unmap(upload,0,8192));
    CHECK(vkdu_command_copy(command,output,0,upload,0,8192));
    CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_STREAM_OUT));
    CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_pipeline(command,pipeline));
    vb = (struct vkdu_vertex_buffer){vertices,64,32,32}; CHECK(vkdu_command_vertex_buffers(command,0,1,&vb));
    CHECK(vkdu_command_topology(command,1));
    view = (struct vkdu_stream_output_buffer){output,output,256,1024,7680};
    CHECK(vkdu_command_stream_output(command,0,1,&view)); CHECK(vkdu_command_draw(command,1,1,0,0));
    CHECK(vkdu_command_stream_output(command,0,1,&view)); CHECK(vkdu_command_draw(command,1,1,0,0));
    CHECK(vkdu_command_draw(command,1,1,0,0));
    CHECK(vkdu_command_stream_output(command,0,1,NULL));
    CHECK(vkdu_command_transition(command,output,D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE));
    CHECK(vkdu_command_copy(command,readback,0,output,0,8192)); CHECK(vkdu_command_close(command));
    CHECK(vkdu_queue_execute(queue,1,&command)); CHECK(vkdu_queue_signal(queue,fence,1)); CHECK(vkdu_fence_wait(fence,1,10000));
    CHECK(vkdu_buffer_map(readback,0,8192,&mapped));
    for (i = 0; i < 2048; ++i) {
        uint32_t expected = i == 1920 ? 1024 : (i == 191 || i == 319) ? 0x3f800000 : 0x7f7a7b7c;
        if (((uint32_t *)mapped)[i] != expected) {
            fprintf(stderr,"FAIL SO limit word%u=%08x expected%08x\n",i,((uint32_t *)mapped)[i],expected); exit(1);
        }
    }
    CHECK(vkdu_buffer_unmap(readback,0,0));
    vkdu_shader_dxbc_free(code); vkdu_object_destroy(command); vkdu_object_destroy(allocator);
    vkdu_object_destroy(queue); vkdu_object_destroy(fence); vkdu_object_destroy(pipeline); vkdu_object_destroy(root);
    vkdu_object_destroy(vertices); vkdu_object_destroy(upload); vkdu_object_destroy(output); vkdu_object_destroy(readback);
    puts("PASS actual SO128 entries/components,512-byte output window/stride,physical stride rejection,append/overflow:2048 exact readback words");
}
