/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* New successor integration oracle. Invalid sample combinations are recorded
 * only to exercise admission and are never submitted, including RED builds. */
static void check_msaa_bundle_case(vkdu_device *device, uint32_t format, uint32_t mode)
{
    static const uint8_t colors[4][4] = {{16,48,80,112},{80,112,144,176},{144,176,208,240},{208,240,16,48}};
    static const uint8_t average[4] = {112,144,112,144};
    const struct vkdu_texture2d_shape shape = {13,7,format,1,1,1,4,0};
    const struct vkdu_viewport view = {0,0,13,7,0,1};
    const struct vkdu_rect scissor = {0,0,13,7};
    const struct vkdu_signature vin = {6,0,1,1}, vout = {1,0,15,3}, pout = {0,0,15,3};
    const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc), *p = graphics_tokens(ps_flat_color_code_dxbc);
    vkdu_object *root, *pipelines[4] = {0}, *single, *target, *destination, *rtv;
    vkdu_object *so_root = NULL, *so_pipeline = NULL;
    vkdu_object *ba, *bundle, *allocator, *command, *queue, *fence, *readback;
    void *vs, *ps, *mapped; size_t vs_bytes, ps_bytes; uint32_t i, x, y, c, failures = 0;
    const float clear[4] = {0,0,0,0};
    CHECK(vkdu_private_color_samples(device,format,4,0));
    CHECK(vkdu_root_create(device,NULL,0,0,&root));
    CHECK(vkdu_shader_dxbc(1,v,v[1],&vin,1,&vout,1,&vs,&vs_bytes));
    if (p[1] != 15 || p[9] != 0x00004002) exit(1);
    for (i = 0; i < 4; ++i) {
        uint32_t tokens[15]; float rgba[4];
        struct vkdu_graphics_desc desc = {format,mode == 2 ? 1u << i : UINT32_MAX,3,1,0,1,15};
        memcpy(tokens,p,sizeof(tokens));
        for (c = 0; c < 4; ++c) rgba[c] = colors[i][c] / 255.0f;
        memcpy(tokens+10,rgba,sizeof(rgba));
        CHECK(vkdu_shader_dxbc(0,tokens,15,NULL,0,&pout,1,&ps,&ps_bytes));
        CHECK(vkdu_graphics_pipeline_create_sampled(device,root,vs,vs_bytes,ps,ps_bytes,&desc,4,0,&pipelines[i]));
        if (!i) CHECK(vkdu_graphics_pipeline_create(device,root,vs,vs_bytes,ps,ps_bytes,&desc,&single));
        vkdu_shader_dxbc_free(ps);
    }
    /* A stream-output-only draw has no raster attachment requirement. This
     * also covers a bundle that changes its root, so ExecuteBundle must not
     * infer OM state merely from the bundle's final PSO. */
    {
        struct vkdu_stream_output_desc so = {1,1,UINT32_MAX,{{0,0,0,15}},{16}};
        struct vkdu_graphics_desc so_desc = {0,UINT32_MAX,3,1,0,1,15,0,NULL};
        so_desc.topology_type = 1; so_desc.stream_output = &so;
        CHECK(vkdu_root_create(device,NULL,0,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT,&so_root));
        CHECK(vkdu_graphics_pipeline_create(device,so_root,vs,vs_bytes,NULL,0,&so_desc,&so_pipeline));
    }
    vkdu_shader_dxbc_free(vs);
    CHECK(vkdu_texture2d_shape_create(device,&shape,&target));
    CHECK(vkdu_render_target_create_format(device,13,7,format,&destination));
    CHECK(vkdu_heap_create(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,0,&rtv));
    CHECK(vkdu_texture2d_rtv(rtv,0,target)); CHECK(vkdu_texture2d_rtv(rtv,1,destination));
    CHECK(vkdu_allocator_create(device,0,&allocator)); CHECK(vkdu_command_create(device,allocator,0,&command));
    CHECK(vkdu_allocator_create(device,1,&ba)); CHECK(vkdu_command_create(device,ba,1,&bundle));
    CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_viewports(command,1,&view));
    CHECK(vkdu_command_scissors(command,1,&scissor)); CHECK(vkdu_command_topology(bundle,4));
    REJECT(vkdu_command_resolve(bundle,destination,0,target,0,format));
    if (mode == 1) {
        CHECK(vkdu_command_graphics_root(bundle,so_root)); CHECK(vkdu_command_pipeline(bundle,so_pipeline));
        CHECK(vkdu_command_draw(bundle,3,1,0,0)); CHECK(vkdu_command_close(bundle));
        /* No target bound on the primary; SO-only execution must still pass. */
        CHECK(vkdu_command_execute_bundle(command,bundle));
        /* Reset the primary first to drop its completion hold on the bundle;
         * allocator reset is deliberately forbidden while that hold exists. */
        CHECK(vkdu_command_close(command)); CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command,allocator));
        CHECK(vkdu_allocator_reset(ba)); CHECK(vkdu_command_reset(bundle,ba)); CHECK(vkdu_command_topology(bundle,4));
        CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_viewports(command,1,&view));
        CHECK(vkdu_command_scissors(command,1,&scissor));
    }
    if (!mode) {
        /* First draw's PSO matters even if a later PSO (without a draw) has a
         * different tuple. Exercise both final-PSO shapes independently. */
        for (i = 0; i < 2; ++i) {
            if (i) { CHECK(vkdu_command_close(command)); CHECK(vkdu_command_reset(command,allocator));
                CHECK(vkdu_command_reset(bundle,ba)); CHECK(vkdu_command_graphics_root(command,root));
                CHECK(vkdu_command_viewports(command,1,&view)); CHECK(vkdu_command_scissors(command,1,&scissor));
                CHECK(vkdu_command_topology(bundle,4)); }
            CHECK(vkdu_command_pipeline(bundle,pipelines[0])); CHECK(vkdu_command_draw(bundle,3,1,0,0));
            if (i) {
                CHECK(vkdu_command_pipeline(bundle,single));
                /* The rejected second raster draw must preserve the first
                 * draw's4x requirement despite leaving a final1x PSO. */
                REJECT(vkdu_command_draw(bundle,3,1,0,0));
            }
            CHECK(vkdu_command_close(bundle)); CHECK(vkdu_command_render_target(command,rtv,1));
            if (SUCCEEDED(vkdu_command_execute_bundle(command,bundle))) {
                fprintf(stderr,"FAIL MSAA bundle sample mismatch accepted earlier-draw4 final-pso%u target1; never submitted\n",i ? 1 : 4);
                ++failures;
            }
        }
    } else {
        CHECK(vkdu_command_clear_rtv(command,rtv,0,clear,0,NULL));
        CHECK(vkdu_command_render_target(command,rtv,0)); CHECK(vkdu_command_pipeline(command,single));
        if (mode == 2) for (i = 0; i < 4; ++i) {
            CHECK(vkdu_command_pipeline(bundle,pipelines[i])); CHECK(vkdu_command_draw(bundle,3,1,0,0));
        } else CHECK(vkdu_command_pipeline(bundle,pipelines[0]));
        CHECK(vkdu_command_close(bundle)); CHECK(vkdu_command_execute_bundle(command,bundle));
        /* A drawless bundle also changes the primary PSO. This valid draw must
         * see its sample tuple. Mode2 uses a zero-count draw to preserve the
         * four independent sample colors written by its preceding bundle. */
        CHECK(vkdu_command_draw(command,mode == 2 ? 0 : 3,1,0,0));
        CHECK(vkdu_command_transition(command,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE));
        CHECK(vkdu_command_transition(command,destination,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_DEST));
        CHECK(vkdu_command_resolve(command,destination,0,target,0,format));
        CHECK(vkdu_command_transition(command,destination,D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_buffer_create(device,4096,D3D12_HEAP_TYPE_READBACK,0,D3D12_RESOURCE_STATE_COPY_DEST,&readback));
        CHECK(vkdu_buffer_map(readback,0,0,&mapped)); memset(mapped,0xa7,4096); CHECK(vkdu_buffer_unmap(readback,0,4096));
        { const struct vkdu_texture_copy_location dst = {readback,512,1,format,13,7,1,256}, src = {destination,0};
          CHECK(vkdu_command_texture_copy(command,&dst,0,0,0,&src,NULL)); }
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_create(device,0,&queue)); CHECK(vkdu_fence_create(device,0,&fence));
        CHECK(vkdu_queue_execute(queue,1,&command)); CHECK(vkdu_queue_signal(queue,fence,1)); CHECK(vkdu_fence_wait(fence,1,10000));
        CHECK(vkdu_buffer_map(readback,0,4096,&mapped));
        for (i = 0; i < 4096; ++i) {
            int pixel = i >= 512 && (i-512)/256 < 7 && (i-512)%256 < 52;
            uint32_t expected = 0xa7;
            if (pixel) { c = (i-512)%4; if (format == 87 && c < 3) c = 2-c;
                expected = mode == 2 ? average[c] : colors[0][c]; }
            if (abs((int)((uint8_t *)mapped)[i]-(int)expected) > pixel) {
                if (failures++ < 4) fprintf(stderr,"FAIL MSAA bundle mode%u format%u byte%u got%u expected%u pixel%u\n",mode,format,i,((uint8_t *)mapped)[i],expected,pixel);
            }
        }
        CHECK(vkdu_buffer_unmap(readback,0,0));
        /* Parent reset drops its bundle hold. New recording must forget the
         * old4x draw requirement and accept an ordinary1x raster draw. */
        CHECK(vkdu_command_reset(command,allocator)); CHECK(vkdu_allocator_reset(ba)); CHECK(vkdu_command_reset(bundle,ba));
        CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_viewports(command,1,&view));
        CHECK(vkdu_command_scissors(command,1,&scissor)); CHECK(vkdu_command_render_target(command,rtv,1));
        CHECK(vkdu_command_pipeline(bundle,single)); CHECK(vkdu_command_topology(bundle,4));
        CHECK(vkdu_command_draw(bundle,3,1,0,0)); CHECK(vkdu_command_close(bundle)); CHECK(vkdu_command_execute_bundle(command,bundle));
        vkdu_object_destroy(readback); vkdu_object_destroy(fence); vkdu_object_destroy(queue);
    }
    /* No invalid configuration is ever submitted by the negative mode. */
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(bundle); vkdu_object_destroy(ba);
    vkdu_object_destroy(rtv); vkdu_object_destroy(target); vkdu_object_destroy(destination);
    for (i = 0; i < 4; ++i) vkdu_object_destroy(pipelines[i]);
    vkdu_object_destroy(single); vkdu_object_destroy(root); (void)x; (void)y;
    vkdu_object_destroy(so_pipeline); vkdu_object_destroy(so_root);
    if (failures) exit(1);
    printf("PASS MSAA bundle mode%u format%u samples4: %s\n",mode,format,
        !mode ? "all raster draw tuples checked before execution; invalid work never submitted" :
        "final PSO tuple, reset, fenced resolve and independent4096-byte pixel/guard readback");
}
static void check_msaa_bundles(vkdu_device *device, uint32_t mode)
{ check_msaa_bundle_case(device,28,mode); check_msaa_bundle_case(device,87,mode); }
