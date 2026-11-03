/* SPDX-License-Identifier: LGPL-2.1-or-later */
static D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *mrt_pso_input;
static D3D12DDI_CPU_DESCRIPTOR_HANDLE *mrt_handle_input;
static void mrt_poison() {
    graphics_poison();
    if (mrt_pso_input) std::memset(mrt_pso_input,0xcc,sizeof(*mrt_pso_input));
    if (mrt_handle_input) std::memset(mrt_handle_input,0xcc,8*sizeof(*mrt_handle_input));
}
static int check_native_mrt_pipeline(Context *ctx, D3D12DDI_HDEVICE h,
        D3D12DDI_DEVICE_FUNCS_CORE_0003 &device, D3D12DDIARG_CREATE_PIPELINE_STATE_0001 &pso,
        D3D12DDI_BLEND_DESC &blend, Object &blend_object) {
    const auto original = pso; const auto original_blend = blend;
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    pso.NumRenderTargets=8; blend.IndependentBlendEnable=TRUE;
    for (UINT i=0;i<8;++i) {
        pso.RTVFormats[i]=formats[i%4];
        auto &rt=blend.RenderTarget[i]; rt={};rt.RenderTargetWriteMask=static_cast<UINT8>(i+1);rt.BlendEnable=i%2;
        rt.SrcBlend=D3D12DDI_BLEND_BLEND_FACTOR;rt.DestBlend=D3D12DDI_BLEND_INVBLEND_FACTOR;
        rt.SrcBlendAlpha=D3D12DDI_BLEND_ONE;rt.DestBlendAlpha=D3D12DDI_BLEND_ZERO;
        rt.BlendOp=D3D12DDI_BLEND_OP_SUBTRACT;rt.BlendOpAlpha=D3D12DDI_BLEND_OP_ADD;
    }
    device.pfnDestroyBlendState(h,{&blend_object});device.pfnCreateBlendState(h,&blend,{&blend_object});
    auto saved=pso;graphics_context=ctx;mrt_pso_input=&pso;graphics_hook=mrt_poison;
    REQUIRE(device.pfnCreatePipelineState(h,&pso)==S_OK);
    graphics_hook=nullptr;mrt_pso_input=nullptr;pso=saved;
    REQUIRE(graphics_locks_free && graphics_state.render_target_count==8);
    for (UINT i=0;i<8;++i) REQUIRE(graphics_targets[i].format==UINT(formats[i%4]) &&
        graphics_targets[i].write_mask==i+1 && graphics_targets[i].blend_enable==i%2 &&
        graphics_targets[i].src_blend==14 && graphics_targets[i].dst_blend==15 && graphics_targets[i].blend_op==2);
    device.pfnDestroyPipelineState(h,pso.hDrvPipelineState);
    pso.NumRenderTargets=9;REQUIRE(device.pfnCreatePipelineState(h,&pso)==E_INVALIDARG);
    pso.NumRenderTargets=UINT32_MAX;REQUIRE(device.pfnCreatePipelineState(h,&pso)==E_INVALIDARG);
    pso.NumRenderTargets=7;REQUIRE(device.pfnCreatePipelineState(h,&pso)==E_INVALIDARG);
    pso.NumRenderTargets=8;pso.RTVFormats[7]=DXGI_FORMAT_R32_FLOAT;
    REQUIRE(device.pfnCreatePipelineState(h,&pso)==DXGI_ERROR_UNSUPPORTED);pso.RTVFormats[7]=formats[3];
    blend.RenderTarget[7].RenderTargetWriteMask=16;
    device.pfnDestroyBlendState(h,{&blend_object});device.pfnCreateBlendState(h,&blend,{&blend_object});
    REQUIRE(device.pfnCreatePipelineState(h,&pso)==E_INVALIDARG);
    blend.RenderTarget[7].RenderTargetWriteMask=8;blend.RenderTarget[7].LogicOpEnable=TRUE;
    device.pfnDestroyBlendState(h,{&blend_object});device.pfnCreateBlendState(h,&blend,{&blend_object});
    REQUIRE(device.pfnCreatePipelineState(h,&pso)==DXGI_ERROR_UNSUPPORTED);
    blend.IndependentBlendEnable=FALSE;
    std::memset(&blend.RenderTarget[1],0xcc,7*sizeof(blend.RenderTarget[0]));
    device.pfnDestroyBlendState(h,{&blend_object});device.pfnCreateBlendState(h,&blend,{&blend_object});
    REQUIRE(device.pfnCreatePipelineState(h,&pso)==S_OK && graphics_state.render_target_count==8);
    for (UINT i=0;i<8;++i) REQUIRE(graphics_targets[i].format==UINT(formats[i%4]) &&
        graphics_targets[i].write_mask==1 && graphics_targets[i].blend_enable==0 && graphics_targets[i].blend_op==2);
    device.pfnDestroyPipelineState(h,pso.hDrvPipelineState);
    pso.NumRenderTargets=0;for(auto &format:pso.RTVFormats)format=DXGI_FORMAT_UNKNOWN;
    REQUIRE(device.pfnCreatePipelineState(h,&pso)==S_OK && !graphics_state.render_target_count);
    device.pfnDestroyPipelineState(h,pso.hDrvPipelineState);
    device.pfnDestroyBlendState(h,{&blend_object});blend=original_blend;
    device.pfnCreateBlendState(h,&blend,{&blend_object});pso=original;
    puts("PASS native MRT PSO:8 typed formats/effective blend masks,RT0 broadcast with ignored invalid entries,0/8/9/MAX counts,late format/mask/logic rejection,caller poison after unlock");
    return 0;
}
static int check_native_mrt_om(Context *ctx, Context *foreign,
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 &commands) {
    Object cmd{},heaps[2]{},other{},wrong{};
    REQUIRE(bind(ctx,&cmd,make_peer(ctx->backend,VKDU_COMMAND_LIST),VKDU_COMMAND_LIST)==S_OK);
    for(auto &heap:heaps) {
        REQUIRE(bind(ctx,&heap,make_peer(ctx->backend,VKDU_DESCRIPTOR_HEAP,2,8),VKDU_DESCRIPTOR_HEAP)==S_OK);
        heap.descriptor_type=2;heap.descriptor_flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    }
    REQUIRE(bind(foreign,&other,make_peer(foreign->backend,VKDU_DESCRIPTOR_HEAP,2,8),VKDU_DESCRIPTOR_HEAP)==S_OK);
    other.descriptor_type=2;other.descriptor_flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    REQUIRE(bind(ctx,&wrong,make_peer(ctx->backend,VKDU_DESCRIPTOR_HEAP,0,8),VKDU_DESCRIPTOR_HEAP)==S_OK);
    wrong.descriptor_type=0;wrong.descriptor_flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE handles[8];
    for(UINT i=0;i<8;++i) handles[i]={SIZE_T(test_start(heaps[i%2].backend,0)+(7-i)*32)};
    auto run=[&](UINT count,const D3D12DDI_CPU_DESCRIPTOR_HANDLE *p,BOOL contiguous,
            const D3D12DDI_CPU_DESCRIPTOR_HANDLE *dsv=nullptr) {
        reported=S_OK;commands.pfnOMSetRenderTargets({&cmd},count,p,contiguous,dsv);
    };
    run(8,handles,FALSE);REQUIRE(reported==S_OK && graphics_target_count==8);
    for(UINT i=0;i<8;++i) REQUIRE(graphics_bindings[i].heap==heaps[i%2].backend && graphics_bindings[i].index==7-i);
    auto *page=static_cast<unsigned char *>(VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    REQUIRE(page);DWORD old;
    REQUIRE(VirtualProtect(page+4096,4096,PAGE_NOACCESS,&old));
    auto *single=reinterpret_cast<D3D12DDI_CPU_DESCRIPTOR_HANDLE *>(page+4096-sizeof(D3D12DDI_CPU_DESCRIPTOR_HANDLE));
    *single={SIZE_T(test_start(heaps[0].backend,0))};
    run(8,single,TRUE);REQUIRE(reported==S_OK && graphics_target_count==8);
    for(UINT i=0;i<8;++i) REQUIRE(graphics_bindings[i].heap==heaps[0].backend && graphics_bindings[i].index==i);
    single->ptr+=32;run(8,single,TRUE);REQUIRE(reported==E_INVALIDARG && graphics_target_count==8);
    REQUIRE(VirtualFree(page,0,MEM_RELEASE));
    peer(heaps[0].backend)->count=UINT32_MAX;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE overflow{SIZE_T(test_start(heaps[0].backend,0)+uint64_t(UINT32_MAX-3)*32)};
    const auto overflow_before=graphics_calls;run(8,&overflow,TRUE);
    REQUIRE(reported==E_INVALIDARG && graphics_calls==overflow_before);
    peer(heaps[0].backend)->count=8;
    auto bad=handles[7];bad.ptr++;
    const auto before=graphics_calls;auto last=handles[7];handles[7]=bad;
    run(8,handles,FALSE);REQUIRE(reported==E_INVALIDARG && graphics_calls==before);handles[7]=last;
    bad.ptr=SIZE_T(test_start(other.backend,0));handles[7]=bad;
    run(8,handles,FALSE);REQUIRE(reported==E_INVALIDARG && graphics_calls==before);
    bad.ptr=SIZE_T(test_start(wrong.backend,0));handles[7]=bad;
    run(8,handles,FALSE);REQUIRE(reported==E_INVALIDARG && graphics_calls==before);handles[7]=last;
    heaps[1].descriptor_flags=0;run(8,handles,FALSE);REQUIRE(reported==E_INVALIDARG && graphics_calls==before);
    heaps[1].descriptor_flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    run(9,reinterpret_cast<const D3D12DDI_CPU_DESCRIPTOR_HANDLE *>(1),FALSE);REQUIRE(reported==E_INVALIDARG);
    run(UINT32_MAX,nullptr,FALSE);REQUIRE(reported==E_INVALIDARG);
    run(1,nullptr,FALSE);REQUIRE(reported==E_INVALIDARG);
    run(8,handles,2);REQUIRE(reported==E_INVALIDARG);
    bad={};run(8,handles,FALSE,&bad);REQUIRE(reported==E_INVALIDARG);
    run(0,reinterpret_cast<const D3D12DDI_CPU_DESCRIPTOR_HANDLE *>(1),TRUE);REQUIRE(reported==S_OK && !graphics_target_count);
    run(1,handles,FALSE);REQUIRE(reported==S_OK && graphics_target_count==1);
    VioGpuD3D12BridgeUnbindObject(&wrong);VioGpuD3D12BridgeUnbindObject(&other);
    Object retired{};
    REQUIRE(bind(ctx,&retired,make_peer(ctx->backend,VKDU_DESCRIPTOR_HEAP,2,8),VKDU_DESCRIPTOR_HEAP)==S_OK);
    retired.descriptor_type=2;retired.descriptor_flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE stale{SIZE_T(test_start(retired.backend,0))};
    VioGpuD3D12BridgeUnbindObject(&retired);const auto retired_before=graphics_calls;
    run(1,&stale,FALSE);REQUIRE(reported==E_INVALIDARG && graphics_calls==retired_before);
    const auto saved_runtime=ctx->runtime_device;const auto saved_error=ctx->runtime_callbacks.pfnSetCommandListErrorCb;
    cmd.runtime_command.handle=reinterpret_cast<HANDLE>(0x2468);ctx->runtime_device.handle=ctx;
    ctx->runtime_callbacks.pfnSetCommandListErrorCb=graphics_command_error;
    graphics_context=ctx;graphics_slots[0]=&cmd;graphics_slots[1]=&heaps[0];graphics_slots[2]=&heaps[1];
    vkdu_object *expected_heaps[2]={heaps[0].backend,heaps[1].backend};
    graphics_hook=mrt_poison;mrt_handle_input=handles;next_result=E_INVALIDARG;
    run(8,handles,FALSE);
    REQUIRE(graphics_locks_free && reported==E_INVALIDARG && graphics_runtime_seen==reinterpret_cast<HANDLE>(0x2468) && graphics_target_count==8);
    for(UINT i=0;i<8;++i) REQUIRE(graphics_bindings[i].heap==expected_heaps[i%2] && graphics_bindings[i].index==7-i);
    graphics_hook=nullptr;mrt_handle_input=nullptr;next_result=S_OK;for(auto &slot:graphics_slots)slot=nullptr;
    ctx->runtime_device=saved_runtime;ctx->runtime_callbacks.pfnSetCommandListErrorCb=saved_error;
    puts("PASS native MRT OM:guarded single range,8 separate handles,0/1/8 limits,late invalid/foreign/type/visibility/overflow rejects,caller poison and recursive command/heap retirement with correct error owner");
    return 0;
}
