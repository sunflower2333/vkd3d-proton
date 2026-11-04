/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Independent integer byte oracle: actual multisample draw, resolve, transfer,
 * fence and readback. The API admission fixture is a separate controlled peer. */
static const uint8_t msaa_sample_colors[4][4] = {
    {32,64,96,128}, {96,128,160,192}, {160,192,224,64}, {224,32,64,96}
};
static const uint8_t msaa_clear_colors[3][4] = {{5,37,91,251}, {19,53,103,239}, {41,79,131,227}};
static const uint8_t msaa_destination_clear[4] = {9,21,45,255};
static void msaa_float_color(const uint8_t *bytes, float *color)
{ for (uint32_t i = 0; i < 4; ++i) color[i] = bytes[i] / 255.0f; }
static uint32_t check_msaa_case(vkdu_device *device, vkdu_device *other, uint32_t format, uint32_t samples, uint32_t placed, int clipped, uint32_t mask_mode, int copy_mode)
{
    const struct vkdu_texture2d_shape shape = {17,9,format,1,3,1,samples,0};
    const struct vkdu_texture2d_shape destination = {34,18,format,2,3,1,1,0};
    const struct vkdu_viewport viewport = {0,0,17,9,0,1};
    const struct vkdu_rect scissor = clipped ? (struct vkdu_rect){2,1,15,8} : (struct vkdu_rect){0,0,17,9};
    const struct vkdu_signature vin = {6,0,1,1}, vout = {1,0,15,3}, pout = {0,0,15,3};
    const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc), *p = graphics_tokens(ps_flat_color_code_dxbc);
    vkdu_object *src = NULL, *dst = NULL, *memory = NULL, *rtv = NULL, *root = NULL, *pipelines[4] = {0};
    vkdu_object *queue = NULL, *allocator = NULL, *command = NULL, *fence = NULL, *readback = NULL;
    vkdu_object *bad = NULL, *foreign = NULL, *single_pso = NULL, *predicate = NULL, *srv = NULL;
    vkdu_object *copied = NULL;
    uint64_t bytes, alignment; uint32_t quality, support; void *vs = NULL, *ps = NULL;
    size_t vs_size, ps_size; uint8_t *mapped; uint32_t i, layer, mip, x, y, channel, mismatches = 0;
    uint8_t *expected = malloc(65536), *pixel = calloc(65536, 1);
    if (!expected || !pixel) exit(1);
    memset(expected, 0xa5, 65536);
    {
        struct vkdu_texture2d_shape invalid = shape; uint64_t ignored_bytes, ignored_alignment;
        invalid.sample_count = 0; REJECT(vkdu_texture2d_shape_allocation(device,&invalid,&ignored_bytes,&ignored_alignment));
        invalid = shape; invalid.sample_count = 3; REJECT(vkdu_texture2d_shape_create(device,&invalid,&bad));
        invalid = shape; invalid.sample_quality = 1; REJECT(vkdu_texture2d_shape_create(device,&invalid,&bad));
        invalid = shape; invalid.mips = 2; REJECT(vkdu_texture2d_shape_create(device,&invalid,&bad));
        invalid = shape; invalid.format = 45; invalid.flags = 2; REJECT(vkdu_texture2d_shape_create(device,&invalid,&bad));
        if (bad) { fprintf(stderr,"FAIL MSAA rejected shape published object\n"); exit(1); }
    }
    CHECK(vkdu_texture2d_shape_allocation(device, &shape, &bytes, &alignment));
    if (alignment != 4194304 || !bytes || bytes % alignment) { fprintf(stderr,"FAIL MSAA allocation alignment/size\n"); exit(1); }
    if (placed) {
        CHECK(vkdu_test_attachment_heap_create(device, bytes * 3, &memory));
        REJECT(vkdu_texture2d_shape_place(device, memory, bytes, &shape, D3D12_RESOURCE_STATE_RENDER_TARGET, &bad));
        vkdu_object_destroy(memory); memory = NULL;
        CHECK(vkdu_test_attachment_heap_create_aligned(device, bytes * 3, 4194304, &memory));
        REJECT(vkdu_texture2d_shape_place(device, memory, 65536, &shape, D3D12_RESOURCE_STATE_RENDER_TARGET, &bad));
        CHECK(vkdu_texture2d_shape_place(device, memory, bytes, &shape, D3D12_RESOURCE_STATE_RENDER_TARGET, &src));
    } else CHECK(vkdu_texture2d_shape_create(device, &shape, &src));
    if (copy_mode) {
        if (placed) CHECK(vkdu_texture2d_shape_place(device,memory,0,&shape,D3D12_RESOURCE_STATE_COPY_DEST,&copied));
        else {
            CHECK(vkdu_texture2d_shape_create(device,&shape,&copied));
        }
    }
    CHECK(vkdu_texture2d_shape_create(device, &destination, &dst));
    CHECK(vkdu_texture2d_shape_create(other, &destination, &foreign));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 9, 0, &rtv));
    for (layer = 0; layer < 3; ++layer) {
        CHECK(vkdu_texture2d_rtv_range(rtv, layer, src, format, 0, layer, 1));
        for (mip = 0; mip < 2; ++mip) CHECK(vkdu_texture2d_rtv_range(rtv, 3+layer*2+mip, dst, format, mip, layer, 1));
    }
    REJECT(vkdu_texture2d_rtv_range(rtv, 0, src, format, 1, 0, 1));
    REJECT(vkdu_texture2d_rtv_range(rtv, 0, src, format, 0, 3, 1));
    REJECT(vkdu_texture2d_rtv_range(rtv, 0, src, format == 28 ? 29 : 91, 0, 0, 1));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, 0, &srv));
    REJECT(vkdu_texture2d_srv(srv, 0, src));
    CHECK(vkdu_shader_dxbc(1, v, v[1], &vin, 1, &vout, 1, &vs, &vs_size));
    CHECK(vkdu_root_create(device, NULL, 0, 0, &root));
    if (p[1] != 15 || p[9] != 0x00004002) { fprintf(stderr,"FAIL changed constant PS fixture\n"); exit(1); }
    for (i = 0; i < samples; ++i) {
        uint32_t tokens[15]; float rgba[4];
        struct vkdu_graphics_desc desc = {format, 1u << i, 3, 1, 0, 1, 15};
        if (mask_mode) desc.sample_mask = mask_mode == 1 ? 0 : UINT32_MAX;
        memcpy(tokens, p, sizeof(tokens)); msaa_float_color(msaa_sample_colors[i], rgba); memcpy(tokens+10, rgba, sizeof(rgba));
        CHECK(vkdu_shader_dxbc(0, tokens, 15, NULL, 0, &pout, 1, &ps, &ps_size));
        CHECK(vkdu_graphics_pipeline_create_sampled(device, root, vs, vs_size, ps, ps_size, &desc, samples, 0, &pipelines[i]));
        if (!i) {
            CHECK(vkdu_graphics_pipeline_create(device, root, vs, vs_size, ps, ps_size, &desc, &single_pso));
            REJECT(vkdu_graphics_pipeline_create_sampled(device, root, vs, vs_size, ps, ps_size, &desc, 0, 0, &bad));
            REJECT(vkdu_graphics_pipeline_create_sampled(device, root, vs, vs_size, ps, ps_size, &desc, samples, 1, &bad));
        }
        vkdu_shader_dxbc_free(ps); ps = NULL;
    }
    vkdu_shader_dxbc_free(vs); vs = NULL;
    CHECK(vkdu_multisample_quality(device, format, samples, 0, &quality)); CHECK(vkdu_format_support(device, format, &support));
    if (quality || (support & (8u|16u))) { fprintf(stderr,"FAIL MSAA advertised unfinished contract\n"); exit(1); }
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_buffer_create(device, 65536, D3D12_HEAP_TYPE_READBACK, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_buffer_create(device, 65536, D3D12_HEAP_TYPE_UPLOAD, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &predicate));
    CHECK(vkdu_buffer_map(predicate, 0, 0, (void **)&mapped)); memset(mapped, 0, 65536); CHECK(vkdu_buffer_unmap(predicate, 0, 65536));
    CHECK(vkdu_buffer_map(readback, 0, 0, (void **)&mapped)); memset(mapped, 0xa5, 65536); CHECK(vkdu_buffer_unmap(readback, 0, 65536));
    if (placed) {
        struct vkdu_texture2d_shape alias = shape; alias.sample_count = 1;
        CHECK(vkdu_texture2d_shape_place(device,memory,bytes,&alias,D3D12_RESOURCE_STATE_RESOLVE_DEST,&bad));
        REJECT(vkdu_command_resolve(command,bad,1,src,1,format)); vkdu_object_destroy(bad); bad = NULL;
    }
    REJECT(vkdu_command_resolve(command, dst, 0, src, 1, format)); /* unequal mip extent */
    REJECT(vkdu_command_resolve(command, dst, 3, src, 3, format));
    REJECT(vkdu_command_resolve(command, dst, 6, src, 1, format));
    REJECT(vkdu_command_resolve(command, foreign, 3, src, 1, format));
    REJECT(vkdu_command_resolve(command, src, 1, dst, 3, format));
    REJECT(vkdu_command_resolve(command, dst, 3, src, 1, format == 28 ? 87 : 28));
    REJECT(vkdu_command_resource_copy(command, dst, src));
    {
        struct vkdu_texture_copy_location a = {readback,512,1,format,17,9,1,256}, b = {src,1};
        REJECT(vkdu_command_texture_copy(command, &a, 0,0,0, &b, NULL));
        REJECT(vkdu_command_texture_upload(command, src, predicate, 0, 256));
    }
    CHECK(vkdu_command_predication(command, predicate, 0, 0));
    if (vkdu_command_resolve(command, dst, 3, src, 1, format) != (int32_t)E_NOTIMPL) { fprintf(stderr,"FAIL predicated resolve not closed\n"); exit(1); }
    CHECK(vkdu_command_predication(command, NULL, 0, 0));
    for (layer = 0; layer < 3; ++layer) {
        float rgba[4]; msaa_float_color(msaa_clear_colors[layer], rgba);
        CHECK(vkdu_command_clear_rtv(command, rtv, layer, rgba, 0, NULL));
        msaa_float_color(msaa_destination_clear, rgba);
        for (mip = 0; mip < 2; ++mip) CHECK(vkdu_command_clear_rtv(command, rtv, 3+layer*2+mip, rgba, 0, NULL));
    }
    CHECK(vkdu_command_graphics_root(command, root)); CHECK(vkdu_command_topology(command, 4));
    CHECK(vkdu_command_viewports(command, 1, &viewport)); CHECK(vkdu_command_scissors(command, 1, &scissor));
    CHECK(vkdu_command_targets(command, rtv, 1, NULL, 0)); CHECK(vkdu_command_pipeline(command, single_pso));
    REJECT(vkdu_command_draw(command, 3,1,0,0));
    /* Both binding orders must reject a sample mismatch before issuing a draw. */
    CHECK(vkdu_command_pipeline(command,pipelines[0])); CHECK(vkdu_command_targets(command,rtv,3,NULL,0));
    REJECT(vkdu_command_draw(command,3,1,0,0)); CHECK(vkdu_command_targets(command,rtv,1,NULL,0));
    {
        const struct vkdu_render_target_binding mixed[2] = {{rtv,1},{rtv,6}};
        REJECT(vkdu_command_render_targets(command, 2, mixed, NULL, 0));
    }
    for (i = 0; i < samples; ++i) { CHECK(vkdu_command_pipeline(command, pipelines[i])); CHECK(vkdu_command_draw(command,3,1,0,0)); }
    if (copy_mode) {
        /* Layer 2 retains a distinct clear sentinel until the final copy.
         * Dropping that copy must not leave already-correct pixels behind. */
        const struct vkdu_texture_copy_location same_dst = {copied,0}, same_src = {copied,1}, final_dst = {src,2};
        const struct vkdu_resource_barrier to_read1 = {VKDU_BARRIER_TRANSITION,copied,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE,NULL,2};
        const struct vkdu_resource_barrier to_read0 = {VKDU_BARRIER_TRANSITION,copied,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE,NULL,1};
        if (!placed) CHECK(vkdu_command_transition(command,copied,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST));
        CHECK(vkdu_command_transition(command,src,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_resource_copy(command,copied,src));
        CHECK(vkdu_command_transition(command,src,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST));
        CHECK(vkdu_command_barriers(command,1,&to_read1));
        CHECK(vkdu_command_texture_copy(command,&same_dst,0,0,0,&same_src,NULL));
        CHECK(vkdu_command_barriers(command,1,&to_read0));
#ifndef VKDU_TEST_MSAA_FINAL_COPY_DROP
        CHECK(vkdu_command_texture_copy(command,&final_dst,0,0,0,&same_dst,NULL));
#endif
        CHECK(vkdu_command_transition(command,src,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RESOLVE_SOURCE));
    } else CHECK(vkdu_command_transition(command, src, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE));
    CHECK(vkdu_command_transition(command, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST));
    for (layer = 0; layer < 3; ++layer) CHECK(vkdu_command_resolve(command, dst, 1+layer*2, src, layer, format));
    CHECK(vkdu_command_transition(command, dst, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
    for (layer = 0; layer < 3; ++layer) for (mip = 0; mip < 2; ++mip) {
        const uint32_t sub = mip+layer*2, width = 34 >> mip, height = 18 >> mip, offset = 512+sub*8192;
        struct vkdu_texture_copy_location a = {readback,offset,1,format,width,height,1,256}, b = {dst,sub};
        CHECK(vkdu_command_texture_copy(command, &a, 0,0,0, &b, NULL));
        for (y = 0; y < height; ++y) for (x = 0; x < width; ++x) for (channel = 0; channel < 4; ++channel) {
            const uint32_t c = format == 87 && channel < 3 ? 2-channel : channel;
            const uint32_t pos = offset+y*256+x*4+channel;
            uint32_t value = mip ? msaa_clear_colors[layer][c] : msaa_destination_clear[c];
            if (mip && (layer == 1 || (copy_mode && layer == 2)) && (!clipped || (x >= 2 && x < 15 && y >= 1 && y < 8))) {
                /* Literal independent expected arithmetic mean. */
                static const uint8_t avg2[4] = {64,96,128,160}, avg4[4] = {128,104,136,120};
                value = (samples == 2 ? avg2 : avg4)[c];
                if (mask_mode == 1) value = msaa_clear_colors[layer][c];
                else if (mask_mode == 2) value = msaa_sample_colors[samples-1][c];
            }
            expected[pos] = value; pixel[pos] = 1;
        }
    }
    CHECK(vkdu_command_close(command)); REJECT(vkdu_command_resolve(command, dst,3,src,1,format));
    /* Command recording retains actual engine resources, including heap memory. */
    vkdu_object_destroy(src); src = NULL; vkdu_object_destroy(dst); dst = NULL; vkdu_object_destroy(copied); copied = NULL; vkdu_object_destroy(memory); memory = NULL;
    vkdu_object_destroy(rtv); rtv = NULL;
    CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, fence, 1)); CHECK(vkdu_fence_wait(fence, 1, 10000));
    CHECK(vkdu_buffer_map(readback,0,65536,(void **)&mapped));
    for (i = 0; i < 65536; ++i) if (abs((int)mapped[i] - expected[i]) > (pixel[i] ? 1 : 0)) {
        if (mismatches++ < 4) fprintf(stderr,"FAIL MSAA pixel format%u count%u placed%u byte%u got%u expected%u guard%u\n",format,samples,placed,i,mapped[i],expected[i],!pixel[i]);
    }
#ifdef VKDU_TEST_MSAA_DIAGNOSTICS
    for (y=0;y<9;++y) { fprintf(stderr,"ACTUAL row%u:",y); for (x=0;x<17;++x) fprintf(stderr," %3u",mapped[25088+y*256+x*4]); fputc('\n',stderr); }
    fprintf(stderr,"MISMATCHES %u\n",mismatches);
#endif
    CHECK(vkdu_buffer_unmap(readback,0,0));
    CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command,allocator));
    CHECK(vkdu_command_graphics_root(command,root)); CHECK(vkdu_command_pipeline(command,pipelines[0]));
    CHECK(vkdu_command_topology(command,4)); CHECK(vkdu_command_viewports(command,1,&viewport)); CHECK(vkdu_command_scissors(command,1,&scissor));
    /* Reset must discard the previous attachment, even after its wrapper died. */
    REJECT(vkdu_command_draw(command,3,1,0,0)); CHECK(vkdu_command_close(command));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(readback); vkdu_object_destroy(predicate); vkdu_object_destroy(foreign); vkdu_object_destroy(srv);
    for (i = 0; i < samples; ++i) vkdu_object_destroy(pipelines[i]);
    vkdu_object_destroy(single_pso); vkdu_object_destroy(root); free(expected); free(pixel);
    printf("%s MSAA pixel format%u count%u quality0 placed%u clipped%u mask-mode%u copy-mode%u layers3 nonzero-mip resolve average sample-mask guard65536 retained-before-execute mismatches%u\n",mismatches ? "FAIL" : "PASS",format,samples,placed,clipped,mask_mode,copy_mode,mismatches);
    return mismatches;
}
static void check_msaa_pixels(vkdu_device *device, vkdu_device *other, int clipped)
{
    const uint32_t formats[2] = {28,87}, counts[2] = {2,4}; uint32_t f,n,completed = 0,unsupported = 0, mismatches = 0;
    for (f = 0; f < 2; ++f) for (n = 0; n < 2; ++n) {
        HRESULT hr = vkdu_private_color_samples(device,formats[f],counts[n],0);
        if (hr == DXGI_ERROR_UNSUPPORTED) { printf("UNSUPPORTED MSAA private backend format%u count%u quality0 (not PASS)\n",formats[f],counts[n]); ++unsupported; continue; }
        CHECK(hr);
        mismatches += check_msaa_case(device,other,formats[f],counts[n],0,clipped,0,0);
        mismatches += check_msaa_case(device,other,formats[f],counts[n],1,clipped,0,0); ++completed;
    }
    if (!completed) { fprintf(stderr,"FAIL no executable MSAA pixel case\n"); exit(1); }
    printf("MSAA CPU matrix supported-format-count-pairs=%u unsupported-format-count-pairs=%u; hardware=UNTESTED public-caps=CLOSED\n",completed,unsupported);
    if (mismatches) exit(1);
}
static void check_msaa_mask_controls(vkdu_device *device, vkdu_device *other)
{
    const uint32_t formats[2] = {28,87}, counts[2] = {2,4}; uint32_t f,n,m,p,completed = 0,mismatches = 0;
    for (f=0;f<2;++f) for (n=0;n<2;++n) {
        HRESULT hr = vkdu_private_color_samples(device,formats[f],counts[n],0);
        if (hr == DXGI_ERROR_UNSUPPORTED) { printf("UNSUPPORTED MSAA mask controls format%u count%u (not PASS)\n",formats[f],counts[n]); continue; }
        CHECK(hr);
        for (m=1;m<=2;++m) for (p=0;p<2;++p) { mismatches += check_msaa_case(device,other,formats[f],counts[n],p,0,m,0); ++completed; }
    }
    if (!completed || mismatches) exit(1);
    puts("PASS MSAA zero/all-bit sample mask controls and both binding orders/reset");
}
static void check_msaa_copy_samples(vkdu_device *device,vkdu_device *other)
{
    const uint32_t formats[2] = {28,87}, counts[2] = {2,4}; uint32_t f,n,p,completed=0,mismatches=0;
    for(f=0;f<2;++f) for(n=0;n<2;++n) {
        HRESULT hr=vkdu_private_color_samples(device,formats[f],counts[n],0);
        if(hr==DXGI_ERROR_UNSUPPORTED) { printf("UNSUPPORTED MSAA per-sample copies format%u count%u (not PASS)\n",formats[f],counts[n]); continue; }
        CHECK(hr);
        for(p=0;p<2;++p) { mismatches+=check_msaa_case(device,other,formats[f],counts[n],p,0,0,1); ++completed; }
    }
    if(!completed || mismatches) exit(1);
    puts("PASS MSAA distinct sample colors through whole copy, same-resource layer copy, cross-resource layer copy and resolve");
}
