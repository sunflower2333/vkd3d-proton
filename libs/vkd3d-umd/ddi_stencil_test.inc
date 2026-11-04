/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Catch missing R0 entry and rejection in real native PSO validation. The
 * existing backend peer is below that validation; a disabled-stencil PSO
 * succeeds first to prove that the rest of this native fixture is valid. */
static int check_native_stencil_baseline(unsigned which)
{
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    next_result = reported = S_OK; graphics_hook = nullptr;
    D3D12DDI_HDEVICE h{&ctx};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queue) == S_OK);
    if (!which) {
        if (!commands.pfnOmSetStencilRef) {
            std::fprintf(stderr, "FAIL stencil R0 OmSetStencilRef is NULL\n"); return 1;
        }
        puts("PASS stencil R0 OmSetStencilRef entry"); return 0;
    }
    Object root{}, vs{}, ps{}, blend{}, raster{}, depth{}, pipeline{};
    REQUIRE(bind(&ctx, &root, make_peer(ctx.backend, VKDU_ROOT), VKDU_ROOT) == S_OK);
    UINT vertex[] = {0x10050, 11, 0x04000060, 0x00101012, 0, 6, 0x04000067, 0x001020f2, 0, 1, 0x0100003e};
    UINT pixel[] = {0x50, 6, 0x03000065, 0x001020f2, 0, 0x0100003e};
    D3D12DDIARG_SIGNATURE_ENTRY input{}, output{};
    input.SystemValue = static_cast<D3D10_SB_NAME>(6); input.Mask = 1;
    input.RegisterComponentType = static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(1);
    output.SystemValue = static_cast<D3D10_SB_NAME>(1); output.Mask = 15;
    output.RegisterComponentType = static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(3);
    D3D12DDIARG_STAGE_IO_SIGNATURES io{};
    io.NumInputSignatureEntries = io.NumOutputSignatureEntries = 1;
    io.pInputSignatureDeprecated = &input; io.pOutputSignatureDeprecated = &output;
    device.pfnCreateVertexShader(h, vertex, {&root}, {&vs}, &io, static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(0));
    io.NumInputSignatureEntries = 0; output.SystemValue = static_cast<D3D10_SB_NAME>(0);
    device.pfnCreatePixelShader(h, pixel, {&root}, {&ps}, &io, static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(0));
    REQUIRE(vs.magic && ps.magic);
    D3D12DDI_BLEND_DESC bs{}; bs.RenderTarget[0].RenderTargetWriteMask = 15;
    D3D12DDI_RASTERIZER_DESC rs{}; rs.FillMode = static_cast<D3D12DDI_FILL_MODE>(3);
    rs.CullMode = static_cast<D3D12DDI_CULL_MODE>(1); rs.ScissorEnable = rs.DepthClipEnable = TRUE;
    D3D12DDI_DEPTH_STENCIL_DESC ds{};
    device.pfnCreateBlendState(h, &bs, {&blend}); device.pfnCreateRasterizerState(h, &rs, {&raster});
    device.pfnCreateDepthStencilState(h, &ds, {&depth});
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pso{};
    pso.hRootSignature = {&root}; pso.hVertexShader = {&vs}; pso.hPixelShader = {&ps};
    pso.hBlendState = {&blend}; pso.hRasterizerState = {&raster}; pso.hDepthStencilState = {&depth};
    pso.PrimitiveTopologyType = D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = pso.SampleDesc.Count = 1; pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleMask = UINT32_MAX; pso.hDrvPipelineState = {&pipeline}; pso.NodeMask = 1;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    REQUIRE(device.pfnCreatePipelineState(h, &pso) == S_OK);
    device.pfnDestroyPipelineState(h, {&pipeline}); device.pfnDestroyDepthStencilState(h, {&depth});
    ds.StencilEnable = ds.FrontEnable = ds.BackEnable = TRUE;
    ds.StencilReadMask = ds.StencilWriteMask = 255;
    ds.FrontFace.StencilFailOp = ds.FrontFace.StencilDepthFailOp = ds.FrontFace.StencilPassOp = D3D12DDI_STENCIL_OP_KEEP;
    ds.FrontFace.StencilFunc = D3D12DDI_COMPARISON_FUNC_ALWAYS; ds.BackFace = ds.FrontFace;
    device.pfnCreateDepthStencilState(h, &ds, {&depth});
    if (which == 2) pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    if (which == 3) pso.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    const auto before = graphics_pipelines;
    HRESULT hr = device.pfnCreatePipelineState(h, &pso);
    if (FAILED(hr)) std::fprintf(stderr, "FAIL stencil native PSO format%u: HRESULT %08x expected S_OK; backend_calls=%u\n",
        unsigned(pso.DSVFormat), unsigned(hr), graphics_pipelines - before);
    else device.pfnDestroyPipelineState(h, {&pipeline});
    device.pfnDestroyDepthStencilState(h, {&depth}); device.pfnDestroyBlendState(h, {&blend});
    device.pfnDestroyRasterizerState(h, {&raster});
    VioGpuD3D12BridgeUnbindObject(&vs); VioGpuD3D12BridgeUnbindObject(&ps); VioGpuD3D12BridgeUnbindObject(&root);
    REQUIRE(ctx.references == 1 && !ctx.graphics_objects && !ctx.resources);
    if (FAILED(hr)) return 1;
    printf("PASS stencil native PSO format%u\n", unsigned(pso.DSVFormat)); return 0;
}
