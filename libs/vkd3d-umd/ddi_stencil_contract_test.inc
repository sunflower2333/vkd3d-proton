/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int check_native_stencil_reference() {
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    graphics_context = &ctx; graphics_hook = nullptr; next_result = reported = S_OK;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{}; D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK);
    REQUIRE(commands.pfnOmSetStencilRef);
    Object command{}, wrong{}, dead{};
    REQUIRE(bind(&ctx, &command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
    REQUIRE(bind(&ctx, &wrong, make_peer(ctx.backend, VKDU_ALLOCATOR), VKDU_ALLOCATOR) == S_OK);
    REQUIRE(bind(&ctx, &dead, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
    VioGpuD3D12BridgeUnbindObject(&dead);
    auto *command_peer = command.backend;
    for (UINT ref : {0u, 1u, 0x123u, UINT32_MAX}) {
        unsigned before = stencil_ref_calls;
        commands.pfnOmSetStencilRef({&command}, ref);
        REQUIRE(stencil_ref_calls == before + 1 && stencil_ref_seen == ref && peer(command_peer)->references == 1);
    }
    unsigned before = stencil_ref_calls;
    Object malformed = command; malformed.backend = nullptr;
    for (Object *invalid : {&wrong, &dead, &malformed}) {
        commands.pfnOmSetStencilRef({invalid}, 7); REQUIRE(stencil_ref_calls == before);
    }
    reject_retain_peer = command_peer;
    commands.pfnOmSetStencilRef({&command}, 8); reject_retain_peer = nullptr;
    REQUIRE(stencil_ref_calls == before && peer(command_peer)->references == 1);
    ctx.native_retiring = true; commands.pfnOmSetStencilRef({&command}, 9); ctx.native_retiring = false;
    REQUIRE(stencil_ref_calls == before);
    command.runtime_command.handle = reinterpret_cast<HANDLE>(0x987651);
    ctx.runtime_device.handle = &ctx; ctx.runtime_callbacks.pfnSetCommandListErrorCb = graphics_command_error;
    graphics_slots[0] = &command; graphics_hook = graphics_poison;
    next_result = E_INVALIDARG; reported = S_OK;
    commands.pfnOmSetStencilRef({&command}, 0xfedcba98);
    REQUIRE(stencil_ref_calls == before + 1 && stencil_ref_seen == 0xfedcba98 && graphics_locks_free &&
        reported == E_INVALIDARG && graphics_runtime_seen == reinterpret_cast<HANDLE>(0x987651));
    graphics_hook = nullptr; for (auto &slot : graphics_slots) slot = nullptr;
    VioGpuD3D12BridgeUnbindObject(&wrong); next_result = reported = S_OK;
    REQUIRE(ctx.references == 1 && !ctx.resources);
    puts("PASS native stencil reference: full UINT, invalid/retired/retain rejection, unlocked recursive command retirement and copied error identity");
    return 0;
}

static int check_native_stencil_pso() {
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    graphics_context = &ctx; graphics_hook = nullptr; next_result = reported = S_OK;
    D3D12DDI_HDEVICE h{&ctx};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{}; D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK);
    Object root{}, vs{}, ps{}, blend{}, raster{}, depth{}, pipeline{};
    REQUIRE(bind(&ctx, &root, make_peer(ctx.backend, VKDU_ROOT), VKDU_ROOT) == S_OK);
    UINT vertex[] = {0x10050, 11, 0x04000060, 0x00101012, 0, 6, 0x04000067, 0x001020f2, 0, 1, 0x0100003e};
    UINT pixel[] = {0x50, 6, 0x03000065, 0x001020f2, 0, 0x0100003e};
    D3D12DDIARG_SIGNATURE_ENTRY input{}, output{};
    input.SystemValue = static_cast<D3D10_SB_NAME>(6); input.Mask = 1;
    input.RegisterComponentType = static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(1);
    output.SystemValue = static_cast<D3D10_SB_NAME>(1); output.Mask = 15;
    output.RegisterComponentType = static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(3);
    D3D12DDIARG_STAGE_IO_SIGNATURES io{}; io.NumInputSignatureEntries = io.NumOutputSignatureEntries = 1;
    io.pInputSignatureDeprecated = &input; io.pOutputSignatureDeprecated = &output;
    device.pfnCreateVertexShader(h, vertex, {&root}, {&vs}, &io, static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(0));
    io.NumInputSignatureEntries = 0; output.SystemValue = static_cast<D3D10_SB_NAME>(0);
    device.pfnCreatePixelShader(h, pixel, {&root}, {&ps}, &io, static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(0));
    REQUIRE(vs.magic && ps.magic);
    D3D12DDI_BLEND_DESC bs{}; bs.RenderTarget[0].RenderTargetWriteMask = 15;
    D3D12DDI_RASTERIZER_DESC rs{}; rs.FillMode = static_cast<D3D12DDI_FILL_MODE>(3);
    rs.CullMode = static_cast<D3D12DDI_CULL_MODE>(1); rs.ScissorEnable = rs.DepthClipEnable = TRUE;
    device.pfnCreateBlendState(h, &bs, {&blend}); device.pfnCreateRasterizerState(h, &rs, {&raster});
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pso{};
    pso.hRootSignature = {&root}; pso.hVertexShader = {&vs}; pso.hPixelShader = {&ps};
    pso.hBlendState = {&blend}; pso.hRasterizerState = {&raster}; pso.hDepthStencilState = {&depth};
    pso.PrimitiveTopologyType = D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = pso.SampleDesc.Count = 1; pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleMask = UINT32_MAX; pso.hDrvPipelineState = {&pipeline}; pso.NodeMask = 1;
    for (auto format : {DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM})
    for (unsigned face_bits = 0; face_bits < 4; ++face_bits)
    for (unsigned op = 1; op <= 8; ++op) {
        D3D12DDI_DEPTH_STENCIL_DESC ds{};
        ds.StencilEnable = face_bits != 0; ds.FrontEnable = (face_bits & 1) != 0; ds.BackEnable = (face_bits & 2) != 0;
        ds.StencilReadMask = 0xa5; ds.StencilWriteMask = 0x3c;
        ds.FrontFace.StencilFailOp = static_cast<D3D12DDI_STENCIL_OP>(op);
        ds.FrontFace.StencilDepthFailOp = static_cast<D3D12DDI_STENCIL_OP>(1 + op % 8);
        ds.FrontFace.StencilPassOp = static_cast<D3D12DDI_STENCIL_OP>(1 + (op + 1) % 8);
        ds.FrontFace.StencilFunc = static_cast<D3D12DDI_COMPARISON_FUNC>(op);
        ds.BackFace.StencilFailOp = static_cast<D3D12DDI_STENCIL_OP>(9 - op);
        ds.BackFace.StencilDepthFailOp = static_cast<D3D12DDI_STENCIL_OP>(1 + (op + 2) % 8);
        ds.BackFace.StencilPassOp = static_cast<D3D12DDI_STENCIL_OP>(1 + (op + 3) % 8);
        ds.BackFace.StencilFunc = static_cast<D3D12DDI_COMPARISON_FUNC>(9 - op);
        device.pfnCreateDepthStencilState(h, &ds, {&depth}); pso.DSVFormat = format;
        REQUIRE(device.pfnCreatePipelineState(h, &pso) == S_OK);
        const auto &got = graphics_state.stencil;
        REQUIRE(graphics_state.depth_format == unsigned(format) && got.enable == unsigned(ds.StencilEnable) &&
            got.front_enable == unsigned(ds.FrontEnable) && got.back_enable == unsigned(ds.BackEnable) &&
            got.read_mask == 0xa5 && got.write_mask == 0x3c);
        REQUIRE(got.front.fail_op == unsigned(ds.FrontFace.StencilFailOp) &&
            got.front.depth_fail_op == unsigned(ds.FrontFace.StencilDepthFailOp) &&
            got.front.pass_op == unsigned(ds.FrontFace.StencilPassOp) && got.front.func == unsigned(ds.FrontFace.StencilFunc));
        REQUIRE(got.back.fail_op == unsigned(ds.BackFace.StencilFailOp) &&
            got.back.depth_fail_op == unsigned(ds.BackFace.StencilDepthFailOp) &&
            got.back.pass_op == unsigned(ds.BackFace.StencilPassOp) && got.back.func == unsigned(ds.BackFace.StencilFunc));
        device.pfnDestroyPipelineState(h, {&pipeline}); device.pfnDestroyDepthStencilState(h, {&depth});
    }
    device.pfnDestroyBlendState(h, {&blend}); device.pfnDestroyRasterizerState(h, {&raster});
    VioGpuD3D12BridgeUnbindObject(&vs); VioGpuD3D12BridgeUnbindObject(&ps); VioGpuD3D12BridgeUnbindObject(&root);
    REQUIRE(ctx.references == 1 && !ctx.graphics_objects && !ctx.resources);
    puts("PASS native stencil PSO: 128 typed-format/face/operation cases preserve independent masks and both complete faces");
    return 0;
}
static int check_native_stencil_dsv_flags() {
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    graphics_hook = nullptr; next_result = reported = S_OK;
    D3D12DDI_HDEVICE h{&ctx}; D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{}; D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK);
    Object heap{}, command{};
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 hd{D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV, 8,
        D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
    REQUIRE(device.pfnCreateDescriptorHeap(h, &hd, {&heap}) == S_OK);
    REQUIRE(bind(&ctx, &command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
    for (auto format : {DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT}) {
        Object texture{};
        REQUIRE(bind(&ctx, &texture, make_peer(ctx.backend, VKDU_TEXTURE2D, format), VKDU_TEXTURE2D) == S_OK);
        D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW d{};
        d.hDrvResource = {&texture}; d.Format = format; d.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
        d.Tex2D.MipSlice = 1; d.Tex2D.FirstArraySlice = 2; d.Tex2D.ArraySize = 3;
        for (UINT flags = 0; flags < 4; ++flags) {
            d.Flags = static_cast<D3D12DDI_CREATE_DEPTH_STENCIL_VIEW_FLAGS>(flags);
            D3D12DDI_CPU_DESCRIPTOR_HANDLE handle{static_cast<SIZE_T>(test_start(heap.backend, 0) + flags * 32)};
            unsigned before = graphics_calls; device.pfnCreateDepthStencilView(h, &d, handle);
            REQUIRE(graphics_calls == before + 1 && depth_resource == texture.backend && depth_flags == flags &&
                depth_index == flags && attachment_view_mip == 1 && attachment_view_first_layer == 2 && attachment_view_layers == 3);
            for (UINT clear = 1; clear < 4; ++clear) {
                commands.pfnClearDepthStencilView({&command}, handle, clear, 0.75f, 0xab, 0, nullptr);
                REQUIRE(depth_flags == clear && depth_value == 0.75f && depth_stencil == 0xab && depth_index == flags);
            }
        }
        d.Flags = static_cast<D3D12DDI_CREATE_DEPTH_STENCIL_VIEW_FLAGS>(4);
        unsigned before = graphics_calls;
        device.pfnCreateDepthStencilView(h, &d, {static_cast<SIZE_T>(test_start(heap.backend, 0))});
        REQUIRE(graphics_calls == before && reported == E_INVALIDARG);
        VioGpuD3D12BridgeUnbindObject(&texture);
    }
    VioGpuD3D12BridgeUnbindObject(&heap); VioGpuD3D12BridgeUnbindObject(&command);
    REQUIRE(ctx.references == 1 && !ctx.descriptor_heaps && !ctx.resources);
    puts("PASS native combined DS: eight typed views preserve readonly flags and mip/layers; all clear aspect combinations reach backend; invalid flags reject");
    return 0;
}
static int check_native_stencil_contract() {
    REQUIRE(check_native_stencil_reference() == 0);
    REQUIRE(check_native_stencil_pso() == 0);
    REQUIRE(check_native_stencil_dsv_flags() == 0);
    REQUIRE(check_native_depth_format(DXGI_FORMAT_D24_UNORM_S8_UINT) == 0);
    REQUIRE(check_native_depth_format(DXGI_FORMAT_D32_FLOAT_S8X24_UINT) == 0);
    return 0;
}
