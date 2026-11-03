/* SPDX-License-Identifier: LGPL-2.1-or-later */

static D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *tess_request;
static void tess_poison() {
    graphics_poison();
    if (!VirtualFree(tess_request,0,MEM_RELEASE)) std::abort();
    tess_request=nullptr;
}
static int check_native_tessellation() {
    Context ctx, other; ctx.backend=reinterpret_cast<vkdu_device *>(&hull_bytes); ctx.report=capture;
    other.backend=reinterpret_cast<vkdu_device *>(&domain_bytes); other.report=capture;
    D3D12DDI_HDEVICE h{&ctx}; D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{}; D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device,&commands,&queues)==S_OK);
    REQUIRE(device.pfnCalcPrivateTessellationShaderSize && device.pfnCreateHullShader && device.pfnCreateDomainShader);
    Object root{}, foreign{}, vs{}, hs{}, ds{}, bad{}, so{}, blend{}, raster{}, depth{}, pso{};
    next_result=reported=S_OK; graphics_hook=nullptr;
    REQUIRE(bind(&ctx,&root,make_peer(ctx.backend,VKDU_ROOT),VKDU_ROOT)==S_OK);
    REQUIRE(bind(&other,&foreign,make_peer(other.backend,VKDU_ROOT),VKDU_ROOT)==S_OK);
    D3D12DDIARG_SIGNATURE_ENTRY position{}, patch[4]{};
    position.SystemValue=D3D10_SB_NAME_POSITION; position.Mask=15; position.RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    for(UINT i=0;i<4;++i) { patch[i].SystemValue=static_cast<D3D10_SB_NAME>(17+i); patch[i].Register=i;
        patch[i].Mask=1; patch[i].RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32; }
    D3D12DDIARG_TESSELLATION_IO_SIGNATURES io{};
    io.pInputSignatureDeprecated=io.pOutputSignatureDeprecated=&position;
    io.NumInputSignatureEntries=io.NumOutputSignatureEntries=1;
    io.pPatchConstantSignatureDeprecated=patch; io.NumPatchConstantSignatureEntries=4;
    const UINT *ht=stage_tokens(stage_nop_hs_code_dxbc), *dt=stage_tokens(stage_nop_ds_code_dxbc);
    REQUIRE(device.pfnCalcPrivateTessellationShaderSize(h,ht,{&root},&io)==sizeof(Object));
    auto mutable_tokens=std::unique_ptr<UINT[]>(new UINT[ht[1]]);
    std::memcpy(mutable_tokens.get(),ht,ht[1]*4);
    device.pfnCreateHullShader(h,mutable_tokens.get(),{&root},{&hs},&io,{});
    std::memset(mutable_tokens.get(),0xcc,ht[1]*4); mutable_tokens.reset();
    REQUIRE(reported==S_OK && hs.native_graphics_tag==9 && hs.shader[0]==0x43425844);
    device.pfnCreateDomainShader(h,dt,{&root},{&ds},&io,{});
    REQUIRE(reported==S_OK && ds.native_graphics_tag==10 && ds.shader[0]==0x43425844);
    for(auto *shader : {&hs,&ds}) {
        const UINT *pc=shader->shader+shader->shader[10]/4;
        REQUIRE(pc[0]==0x47534350 && pc[2]==4);
        for(UINT i=0;i<4;++i) {
            const UINT *e=pc+4+i*6;
            REQUIRE(e[1]==(i==3 ? 0u : i) && e[2]==(i==3 ? 14u : 13u) && e[4]==i && (e[5]&255)==1);
        }
    }
    device.pfnCreateHullShader(h,dt,{&root},{&bad},&io,{}); REQUIRE(reported==E_INVALIDARG && !bad.magic);
    device.pfnCreateDomainShader(h,ht,{&root},{&bad},&io,{}); REQUIRE(reported==E_INVALIDARG && !bad.magic);
    device.pfnCreateHullShader(h,ht,{&foreign},{&bad},&io,{}); REQUIRE(reported==E_INVALIDARG && !bad.magic);
    device.pfnCreateHullShader(h,ht,{&root},{&bad},&io,static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(1)); REQUIRE(reported==E_INVALIDARG && !bad.magic);
    io.NumPatchConstantSignatureEntries=33; device.pfnCreateHullShader(h,ht,{&root},{&bad},&io,{});
    REQUIRE(reported==E_INVALIDARG && !bad.magic); io.NumPatchConstantSignatureEntries=4;
    io.pPatchConstantSignatureDeprecated=nullptr; device.pfnCreateHullShader(h,ht,{&root},{&bad},&io,{});
    REQUIRE(reported==E_INVALIDARG && !bad.magic); io.pPatchConstantSignatureDeprecated=patch;
    patch[0].MinPrecision=D3D11_SB_OPERAND_MIN_PRECISION_FLOAT_16;
    device.pfnCreateHullShader(h,ht,{&root},{&bad},&io,{}); REQUIRE(reported==E_INVALIDARG && !bad.magic);
    patch[0].MinPrecision=D3D11_SB_OPERAND_MIN_PRECISION_DEFAULT;
    patch[0].Register=32; device.pfnCreateHullShader(h,ht,{&root},{&bad},&io,{});
    REQUIRE(reported==E_INVALIDARG && !bad.magic); patch[0].Register=0;
    patch[0].Mask=3; device.pfnCreateHullShader(h,ht,{&root},{&bad},&io,{});
    REQUIRE(reported==E_INVALIDARG && !bad.magic); patch[0].Mask=1;
    patch[0].SystemValue=static_cast<D3D10_SB_NAME>(11); device.pfnCreateHullShader(h,ht,{&root},{&bad},&io,{});
    REQUIRE(reported==E_INVALIDARG && !bad.magic); patch[0].SystemValue=static_cast<D3D10_SB_NAME>(17);
    UINT malformed[]={0x30050,4,0x02000065,0x001020f2};
    device.pfnCreateHullShader(h,malformed,{&root},{&bad},&io,{}); REQUIRE(reported==E_INVALIDARG && !bad.magic);
    // Every inside-instruction truncation ends at an inaccessible page, so
    // declaration operands can never be consumed before the length check.
    SYSTEM_INFO info{}; GetSystemInfo(&info); DWORD protection;
    auto pages=static_cast<char *>(VirtualAlloc(nullptr,info.dwPageSize*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    REQUIRE(pages && VirtualProtect(pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&protection));
    UINT guarded=0;
    for(const UINT *original : {ht,dt}) {
        for(UINT at=2;at<original[1];) {
            UINT length=(original[at]>>24)&127; if((original[at]&0x7ff)==53)length=original[at+1];
            REQUIRE(length && length<=original[1]-at);
            for(UINT cut=at+1;cut<at+length;++cut) {
                REQUIRE(cut*4<info.dwPageSize);
                auto bounded=reinterpret_cast<UINT *>(pages+info.dwPageSize-cut*4);
                std::memcpy(bounded,original,cut*4); bounded[1]=cut;
                if(original==ht) device.pfnCreateHullShader(h,bounded,{&root},{&bad},&io,{});
                else device.pfnCreateDomainShader(h,bounded,{&root},{&bad},&io,{});
                REQUIRE(reported==E_INVALIDARG && !bad.magic); ++guarded;
            }
            at+=length;
        }
    }
    REQUIRE(guarded>32 && VirtualFree(pages,0,MEM_RELEASE));
    D3D12DDIARG_SIGNATURE_ENTRY input=position; input.SystemValue=D3D10_SB_NAME_UNDEFINED;
    D3D12DDIARG_STAGE_IO_SIGNATURES vio{}; vio.pInputSignatureDeprecated=&input; vio.NumInputSignatureEntries=1;
    vio.pOutputSignatureDeprecated=&position; vio.NumOutputSignatureEntries=1;
    reported=S_OK; device.pfnCreateVertexShader(h,stage_tokens(stage_quad_tess_vs_code_dxbc),{&root},{&vs},&vio,{});
    REQUIRE(reported==S_OK && vs.magic);
    D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY entry{0,0,0,15}; const UINT stride=16;
    D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT pass{nullptr,{&root},&entry,1,&stride,1,UINT32_MAX};
    device.pfnCreateGeometryShaderWithStreamOutput(h,&pass,{&so},nullptr,{}); REQUIRE(reported==S_OK && so.magic);
    D3D12DDI_BLEND_DESC bs{}; D3D12DDI_RASTERIZER_DESC rs{};
    rs.FillMode=D3D12DDI_FILL_MODE_SOLID; rs.CullMode=D3D12DDI_CULL_MODE_NONE; rs.ScissorEnable=rs.DepthClipEnable=TRUE;
    D3D12DDI_DEPTH_STENCIL_DESC zs{};
    device.pfnCreateBlendState(h,&bs,{&blend}); device.pfnCreateRasterizerState(h,&rs,{&raster}); device.pfnCreateDepthStencilState(h,&zs,{&depth});
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pipeline{};
    pipeline.hRootSignature={&root}; pipeline.hVertexShader={&vs}; pipeline.hHullShader={&hs}; pipeline.hDomainShader={&ds};
    pipeline.hGeometryShader={&so}; pipeline.hBlendState={&blend}; pipeline.hRasterizerState={&raster}; pipeline.hDepthStencilState={&depth};
    pipeline.PrimitiveTopologyType=D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_PATCH; pipeline.SampleDesc.Count=1;
    pipeline.hDrvPipelineState={&pso}; pipeline.NodeMask=1;
    std::memset(patch,0xcc,sizeof(patch)); std::memset(&position,0xcc,sizeof(position));
    REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==S_OK && hull_bytes && domain_bytes && !geometry_bytes && graphics_has_so);
    device.pfnDestroyPipelineState(h,{&pso});
    pipeline.hDomainShader={}; REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==E_INVALIDARG); pipeline.hDomainShader={&ds};
    pipeline.hHullShader={}; REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==E_INVALIDARG); pipeline.hHullShader={&hs};
    pipeline.hHullShader={&ds}; REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==E_INVALIDARG); pipeline.hHullShader={&hs};
    pipeline.PrimitiveTopologyType=D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==E_INVALIDARG); pipeline.PrimitiveTopologyType=D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pipeline.hRootSignature={&foreign}; REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==E_INVALIDARG); pipeline.hRootSignature={&root};
    graphics_context=&ctx; graphics_hook=stage_retire;
    REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==DXGI_ERROR_DEVICE_REMOVED && !pso.magic && graphics_locks_free);
    ctx.native_retiring=false; graphics_hook=nullptr;
    graphics_slots[0]=&root; graphics_slots[1]=&vs; graphics_slots[2]=&hs; graphics_slots[3]=&ds;
    graphics_slots[4]=&so; graphics_slots[5]=&blend; graphics_slots[6]=&raster; graphics_slots[7]=&depth;
    tess_request=static_cast<D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *>(VirtualAlloc(nullptr,sizeof(pipeline),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    REQUIRE(tess_request); *tess_request=pipeline;
    graphics_hook=tess_poison;
    REQUIRE(device.pfnCreatePipelineState(h,tess_request)==S_OK && !tess_request && hull_bytes && domain_bytes && graphics_has_so && graphics_locks_free);
    graphics_hook=nullptr; std::memset(graphics_slots,0,sizeof(graphics_slots));
    device.pfnDestroyPipelineState(h,{&pso}); VioGpuD3D12BridgeUnbindObject(&foreign);
    REQUIRE(ctx.references==1 && other.references==1 && !ctx.graphics_objects && !ctx.resources && !ctx.native_backend_calls);
    printf("PASS native HS/DS %u guarded truncations: real R0 token/PCSG translation, bounds/domain/wrong-stage/foreign-root rejects, HS+DS patch PSO, DS SO passthrough, copied shader storage and unlocked reentrant cancellation/teardown (controlled backend)\n",guarded);
    return 0;
}
