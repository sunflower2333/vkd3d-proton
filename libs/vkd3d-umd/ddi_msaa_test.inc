/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int check_native_msaa_pso() {
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    graphics_context = &ctx; graphics_hook = nullptr; next_result = reported = S_OK;
    D3D12DDI_HDEVICE h{&ctx}; D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{}; D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device,&commands,&queue) == S_OK);
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

    for (UINT samples : {2u,4u}) for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}) {
        pso.SampleDesc.Count=samples; pso.RTVFormats[0]=format; pso.SampleMask=0x5;
        REQUIRE(device.pfnCreatePipelineState(h,&pso) == S_OK);
        REQUIRE(pso_samples_seen == samples && !pso_quality_seen && graphics_state.sample_mask == 0x5 &&
            graphics_targets[0].format == unsigned(format));
        device.pfnDestroyPipelineState(h,{&pipeline});
    }
    for (UINT samples : {0u,3u,8u,UINT32_MAX}) {
        pso.SampleDesc.Count=samples; auto before=graphics_pipelines;
        REQUIRE(FAILED(device.pfnCreatePipelineState(h,&pso)) && graphics_pipelines==before && !pipeline.magic);
    }
    pso.SampleDesc.Count=4; pso.SampleDesc.Quality=1;
    auto before=graphics_pipelines; REQUIRE(FAILED(device.pfnCreatePipelineState(h,&pso)) && graphics_pipelines==before);
    pso.SampleDesc.Quality=0; pso.DSVFormat=DXGI_FORMAT_D24_UNORM_S8_UINT;
    REQUIRE(FAILED(device.pfnCreatePipelineState(h,&pso)) && graphics_pipelines==before);
    pso.DSVFormat=DXGI_FORMAT_UNKNOWN; pso.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    REQUIRE(FAILED(device.pfnCreatePipelineState(h,&pso)) && graphics_pipelines==before);
    device.pfnDestroyDepthStencilState(h,{&depth}); device.pfnDestroyBlendState(h,{&blend}); device.pfnDestroyRasterizerState(h,{&raster});
    VioGpuD3D12BridgeUnbindObject(&vs); VioGpuD3D12BridgeUnbindObject(&ps); VioGpuD3D12BridgeUnbindObject(&root);
    REQUIRE(ctx.references==1 && !ctx.resources && !ctx.graphics_objects);
    puts("PASS MSAA R0 PSO 2x/4x typed RGBA/BGRA, sample mask and closed quality/depth/sRGB/count contracts"); return 0;
}
static int check_native_msaa_resolve() {
    Context ctx, other; ctx.backend=reinterpret_cast<vkdu_device *>(&next_base); ctx.report=capture;
    other.backend=reinterpret_cast<vkdu_device *>(&other); other.report=capture;
    graphics_context=&ctx; graphics_hook=nullptr; next_result=reported=S_OK;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device,&commands,&queue)==S_OK && commands.pfnResourceResolveSubresource);
    Object command{},dst{},src{},buffer{},foreign{};
    REQUIRE(bind(&ctx,&command,make_peer(ctx.backend,VKDU_COMMAND_LIST),VKDU_COMMAND_LIST)==S_OK);
    REQUIRE(bind(&ctx,&dst,make_peer(ctx.backend,VKDU_TEXTURE2D,28),VKDU_TEXTURE2D)==S_OK);
    REQUIRE(bind(&ctx,&src,make_peer(ctx.backend,VKDU_TEXTURE2D,28),VKDU_TEXTURE2D)==S_OK);
    REQUIRE(bind(&ctx,&buffer,make_peer(ctx.backend,VKDU_BUFFER),VKDU_BUFFER)==S_OK);
    REQUIRE(bind(&other,&foreign,make_peer(other.backend,VKDU_TEXTURE2D,28),VKDU_TEXTURE2D)==S_OK);
    commands.pfnResourceResolveSubresource({&command},{&dst},3,{&src},1,DXGI_FORMAT_R8G8B8A8_UNORM);
    REQUIRE(resolve_calls==1 && resolve_dst_sub==3 && resolve_src_sub==1 && resolve_format==28 &&
        resource_copy_dst==dst.backend && resource_copy_src==src.backend);
    auto before=resolve_calls;
    for (Object *invalid : {&dst,&buffer,&foreign}) {
        commands.pfnResourceResolveSubresource({&command},{&dst},3,{invalid},1,DXGI_FORMAT_R8G8B8A8_UNORM);
        REQUIRE(resolve_calls==before);
    }
    reject_retain_peer=src.backend;
    commands.pfnResourceResolveSubresource({&command},{&dst},3,{&src},1,DXGI_FORMAT_R8G8B8A8_UNORM);
    reject_retain_peer=nullptr; REQUIRE(resolve_calls==before && peer(dst.backend)->references==1 && peer(src.backend)->references==1);
    ctx.native_retiring=true; commands.pfnResourceResolveSubresource({&command},{&dst},3,{&src},1,DXGI_FORMAT_R8G8B8A8_UNORM);
    ctx.native_retiring=false; REQUIRE(resolve_calls==before);
    command.runtime_command.handle=reinterpret_cast<HANDLE>(0x987650);
    ctx.runtime_device.handle=&ctx; ctx.runtime_callbacks.pfnSetCommandListErrorCb=graphics_command_error;
    graphics_slots[0]=&command; graphics_slots[1]=&dst; graphics_slots[2]=&src;
    graphics_hook=graphics_poison; next_result=E_INVALIDARG; reported=S_OK;
    commands.pfnResourceResolveSubresource({&command},{&dst},7,{&src},2,DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(resolve_calls==before+1 && resolve_dst_sub==7 && resolve_src_sub==2 && resolve_format==87 &&
        graphics_locks_free && reported==E_INVALIDARG && graphics_runtime_seen==reinterpret_cast<HANDLE>(0x987650));
    graphics_hook=nullptr; for (auto &slot:graphics_slots) slot=nullptr; next_result=reported=S_OK;
    VioGpuD3D12BridgeUnbindObject(&buffer); VioGpuD3D12BridgeUnbindObject(&foreign);
    REQUIRE(ctx.references==1 && !ctx.resources && other.references==1 && !other.resources);
    puts("PASS MSAA R0 resolve typed arguments, owner/retain rejection, unlocked recursive retirement and copied error identity"); return 0;
}
static int check_native_msaa() { return check_native_msaa_pso() || check_native_msaa_resolve(); }
