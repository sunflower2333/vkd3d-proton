/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "stage_test_shaders.h"
#include "stage_clip_test_tokens.h"
#include "stage_stream_test_tokens.h"
static D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW *stream_request;
static void stage_retire() { graphics_poison(); graphics_context->native_retiring = true; }
static void stream_poison() {
    graphics_poison();
    if (!VirtualFree(stream_request, 0, MEM_RELEASE)) std::abort();
    stream_request = nullptr;
}
static Object *stream_error_slot;
static Context *stream_error_context;
static unsigned stream_error_references, stream_error_calls;
static void APIENTRY stream_early_error(D3D12DDI_HRTCOMMANDLIST runtime, HRESULT hr) {
    ++stream_error_calls; reported = hr; graphics_runtime_seen = runtime.handle;
    stream_error_references = peer(stream_error_slot->backend)->references;
    VioGpuD3D12BridgeUnbindObject(stream_error_slot);
    if (!VirtualFree(stream_error_slot, 0, MEM_RELEASE)) std::abort();
    stream_error_slot = nullptr; stream_error_context->native_retiring = true;
}
static int check_stream_output_early_error() {
    for (unsigned i = 0; i < 2; ++i) {
        Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&stream_error_calls); ctx.report = capture;
        ctx.runtime_device.handle = &ctx; ctx.runtime_callbacks.pfnSetCommandListErrorCb = stream_early_error;
        stream_error_context = &ctx;
        stream_error_slot = static_cast<Object *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(stream_error_slot); new (stream_error_slot) Object{};
        REQUIRE(bind(&ctx, stream_error_slot, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
        stream_error_slot->runtime_command.handle = reinterpret_cast<HANDLE>(uintptr_t(0x57ea40 + i));
        const auto runtime = stream_error_slot->runtime_command.handle;
        const auto calls = stream_error_calls;
        D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
        REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK);
        commands.pfnSOSetTargets({stream_error_slot}, i ? 4 : 5, i ? 1 : 0, nullptr);
        REQUIRE(stream_error_references == 1 && stream_error_calls == calls + 1 && !stream_error_slot &&
            graphics_runtime_seen == runtime && reported == E_INVALIDARG && ctx.references == 1 && !ctx.resources);
    }
    stream_error_context = nullptr;
    puts("PASS SO invalid-range error retires backend borrow before callback Unbind/VirtualFree/cancel (controlled peers)");
    return 0;
}
static const UINT *stage_tokens(const uint32_t *container) {
    for (UINT i = 0; i < container[7]; ++i) {
        const auto *chunk = container + container[8 + i] / 4;
        if (chunk[0] == 0x58454853 || chunk[0] == 0x52444853) return chunk + 2;
    }
    std::abort();
}
static int check_native_shader_stages() {
    Context ctx, other; ctx.backend = reinterpret_cast<vkdu_device *>(&stream_calls); ctx.report = capture;
    other.backend = reinterpret_cast<vkdu_device *>(&geometry_bytes); other.report = capture;
    D3D12DDI_HDEVICE h{&ctx};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK);
    REQUIRE(device.pfnCreateGeometryShader && device.pfnCreateGeometryShaderWithStreamOutput && commands.pfnSOSetTargets);
    REQUIRE(device.pfnCalcPrivateTessellationShaderSize && device.pfnCreateHullShader && device.pfnCreateDomainShader);
    Object root{}, vs{}, gs{}, so{}, pass{}, bad{}, blend{}, raster{}, depth{}, pso{}, command{}, data{}, counter{}, foreign{};
    next_result = reported = S_OK; graphics_hook = nullptr;
    REQUIRE(bind(&ctx, &root, make_peer(ctx.backend, VKDU_ROOT), VKDU_ROOT) == S_OK);
    REQUIRE(bind(&other, &foreign, make_peer(other.backend, VKDU_ROOT), VKDU_ROOT) == S_OK);
    const auto *tokens = stage_tokens(stage_gs_code_dxbc);
    D3D12DDIARG_SIGNATURE_ENTRY link[2]{};
    link[0].SystemValue = D3D10_SB_NAME_POSITION; link[0].Mask = 15; link[0].RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    link[1] = link[0]; link[1].SystemValue = D3D10_SB_NAME_UNDEFINED; link[1].Register = 1;
    D3D12DDIARG_STAGE_IO_SIGNATURES io{};
    io.NumInputSignatureEntries = io.NumOutputSignatureEntries = 2;
    io.pInputSignatureDeprecated = io.pOutputSignatureDeprecated = link;
    device.pfnCreateGeometryShader(h, tokens, {&root}, {&gs}, &io, {});
    REQUIRE(reported == S_OK && gs.native_graphics_tag == 7 && gs.shader[0] == 0x43425844);
    device.pfnCreateGeometryShader(h, tokens, {&foreign}, {&bad}, &io, {});
    REQUIRE(reported == E_INVALIDARG && !bad.magic);
    UINT vertex[] = {0x10050, 11, 0x04000060, 0x00101012, 0, 6, 0x04000067, 0x001020f2, 0, 1, 0x0100003e};
    device.pfnCreateGeometryShader(h, vertex, {&root}, {&bad}, &io, {});
    REQUIRE(reported == E_INVALIDARG && !bad.magic);
    D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY entries[] = {{0,1,0,15},{0,1,UINT32_MAX,1},{0,2,1,15}};
    UINT strides[] = {0,20,16};
    D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT create{tokens,{&root},entries,3,strides,3,UINT32_MAX};
    REQUIRE(device.pfnCalcPrivateGeometryShaderWithStreamOutput(h,&create,&io) == sizeof(Object));
    reported = S_OK;
    device.pfnCreateGeometryShaderWithStreamOutput(h,&create,{&so},&io,{});
    REQUIRE(reported == S_OK && so.native_graphics_tag == 8);
    strides[1] = 16; device.pfnCreateGeometryShaderWithStreamOutput(h,&create,{&bad},&io,{});
    REQUIRE(reported == E_INVALIDARG && !bad.magic); strides[1] = 20;
    entries[2].Stream = 1; device.pfnCreateGeometryShaderWithStreamOutput(h,&create,{&bad},&io,{});
    REQUIRE(reported == E_INVALIDARG && !bad.magic); entries[2].Stream = 0;
    create.NumStrides = 5; device.pfnCreateGeometryShaderWithStreamOutput(h,&create,{&bad},&io,{});
    REQUIRE(reported == E_INVALIDARG && !bad.magic); create.NumStrides = 3;
    D3D12DDIARG_SIGNATURE_ENTRY vin{}; vin.SystemValue = D3D10_SB_NAME_VERTEX_ID; vin.Mask = 1;
    vin.RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_UINT32;
    D3D12DDIARG_STAGE_IO_SIGNATURES vio{}; vio.pInputSignatureDeprecated = &vin; vio.NumInputSignatureEntries = 1;
    vio.pOutputSignatureDeprecated = link; vio.NumOutputSignatureEntries = 1;
    device.pfnCreateVertexShader(h,vertex,{&root},{&vs},&vio,{}); REQUIRE(vs.magic);
    D3D12DDI_BLEND_DESC bs{};
    D3D12DDI_RASTERIZER_DESC rs{}; rs.FillMode = D3D12DDI_FILL_MODE_SOLID; rs.CullMode = D3D12DDI_CULL_MODE_NONE;
    rs.DepthClipEnable = rs.ScissorEnable = TRUE;
    D3D12DDI_DEPTH_STENCIL_DESC ds{};
    device.pfnCreateBlendState(h,&bs,{&blend}); device.pfnCreateRasterizerState(h,&rs,{&raster}); device.pfnCreateDepthStencilState(h,&ds,{&depth});
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pipeline{};
    pipeline.hRootSignature={&root}; pipeline.hVertexShader={&vs}; pipeline.hGeometryShader={&so};
    pipeline.hBlendState={&blend}; pipeline.hRasterizerState={&raster}; pipeline.hDepthStencilState={&depth};
    pipeline.PrimitiveTopologyType = D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_POINT; pipeline.SampleDesc.Count=1;
    pipeline.hDrvPipelineState={&pso}; pipeline.NodeMask=1;
#include "ddi_multistream_test.inc"
#include "ddi_clip_output_test.inc"
#include "ddi_distance_link_test.inc"
    {
        Object extended{};
        auto *page = static_cast<BYTE *>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        DWORD previous;
        REQUIRE(page && VirtualProtect(page+4096,4096,PAGE_NOACCESS,&previous));
        auto *large = reinterpret_cast<D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *>(page+4096) - 128;
        for (UINT i=0; i<127; ++i) large[i]={0,1,UINT32_MAX,1};
        large[127]={0,1,0,8};
        UINT large_strides[]{0,2048};
        D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT extended_create{tokens,{&root},large,128,large_strides,2,UINT32_MAX};
        reported=S_OK; device.pfnCreateGeometryShaderWithStreamOutput(h,&extended_create,{&extended},&io,{});
        REQUIRE(reported==S_OK && extended.native_graphics_tag==8);
        std::memset(large,0xcc,128*sizeof(*large)); REQUIRE(VirtualFree(page,0,MEM_RELEASE));
        large_strides[1]=0; pipeline.hGeometryShader={&extended};
        REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==S_OK && graphics_so.entry_count==128 &&
            graphics_so.entries[126].reg==UINT32_MAX && graphics_so.entries[127].mask==8 && graphics_so.strides[1]==2048);
        device.pfnDestroyPipelineState(h,{&pso}); device.pfnDestroyShader(h,{&extended});
        extended_create.NumEntries=513; extended_create.pOutputStreamDecl=reinterpret_cast<const D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *>(1);
        device.pfnCreateGeometryShaderWithStreamOutput(h,&extended_create,{&bad},&io,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);
        D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY over[129]{};
        for(UINT i=0;i<129;++i)over[i]={0,1,UINT32_MAX,1};
        over[128]={0,1,0,8}; extended_create.NumEntries=129; extended_create.pOutputStreamDecl=over; large_strides[1]=2048;
        device.pfnCreateGeometryShaderWithStreamOutput(h,&extended_create,{&bad},&io,{});
        REQUIRE(reported==E_INVALIDARG && !bad.magic);
        D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY partial[]={{0,1,UINT32_MAX,1},{0,1,0,8},{0,1,0,2},
            {0,1,1,4},{0,2,1,1},{0,2,0,1}};
        UINT partial_strides[]{0,20,12};
        extended_create={tokens,{&root},partial,6,partial_strides,3,UINT32_MAX};
        reported=S_OK;device.pfnCreateGeometryShaderWithStreamOutput(h,&extended_create,{&extended},&io,{});
        REQUIRE(reported==S_OK && extended.native_graphics_tag==8);
        std::memset(partial,0xcc,sizeof(partial));std::memset(partial_strides,0xcc,sizeof(partial_strides));
        REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==S_OK && graphics_so.entry_count==6 &&
            graphics_so.entries[1].mask==8 && graphics_so.entries[2].mask==2 && graphics_so.entries[4].slot==2 && graphics_so.strides[2]==12);
        device.pfnDestroyPipelineState(h,{&pso});device.pfnDestroyShader(h,{&extended});pipeline.hGeometryShader={&so};
    }
    std::memset(strides,0xcc,sizeof(strides)); std::memset(entries,0xcc,sizeof(entries)); std::memset(link,0xcc,sizeof(link));
    REQUIRE(device.pfnCreatePipelineState(h,&pipeline) == S_OK && geometry_bytes && graphics_has_so &&
        graphics_so.entries[1].reg == UINT32_MAX && graphics_so.strides[1] == 20 && graphics_so.rasterized_stream == UINT32_MAX);
    device.pfnDestroyPipelineState(h,{&pso});
    D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY pass_entry{0,1,0,15};
    const UINT pass_strides[]{0,16};
    D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT pass_create{nullptr,{&root},&pass_entry,1,pass_strides,2,UINT32_MAX};
    reported=S_OK; device.pfnCreateGeometryShaderWithStreamOutput(h,&pass_create,{&pass},nullptr,{});
    REQUIRE(reported==S_OK && pass.native_graphics_tag==8);
    pipeline.hGeometryShader={&pass};
    REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==S_OK && !geometry_bytes && graphics_has_so && graphics_so.entry_count==1);
    device.pfnDestroyPipelineState(h,{&pso}); device.pfnDestroyShader(h,{&pass}); pipeline.hGeometryShader={&so};
    pipeline.hRootSignature={&foreign}; REQUIRE(device.pfnCreatePipelineState(h,&pipeline) == E_INVALIDARG); pipeline.hRootSignature={&root};
    graphics_context=&ctx; graphics_hook=stage_retire;
    REQUIRE(device.pfnCreatePipelineState(h,&pipeline)==DXGI_ERROR_DEVICE_REMOVED && !pso.magic && graphics_locks_free);
    ctx.native_retiring=false; graphics_hook=nullptr;
    graphics_context=&ctx; graphics_slots[0]=&so; graphics_slots[1]=&vs; graphics_slots[2]=&blend;
    graphics_slots[3]=&raster; graphics_slots[4]=&depth; graphics_slots[5]=&root;
    graphics_hook=graphics_poison;
    REQUIRE(device.pfnCreatePipelineState(h,&pipeline) == S_OK && graphics_locks_free && geometry_bytes &&
        graphics_has_so && graphics_so.strides[2] == 16);
    graphics_hook=nullptr; std::memset(graphics_slots,0,sizeof(graphics_slots));
    device.pfnDestroyPipelineState(h,{&pso}); device.pfnDestroyShader(h,{&gs});
    REQUIRE(bind(&ctx,&command,make_peer(ctx.backend,VKDU_COMMAND_LIST),VKDU_COMMAND_LIST)==S_OK);
    REQUIRE(bind(&ctx,&data,make_peer(ctx.backend,VKDU_BUFFER),VKDU_BUFFER)==S_OK);
    REQUIRE(bind(&ctx,&counter,make_peer(ctx.backend,VKDU_BUFFER),VKDU_BUFFER)==S_OK);
    command.runtime_command.handle=reinterpret_cast<HANDLE>(0x50a1);
    ctx.runtime_device.handle=&ctx; ctx.runtime_callbacks.pfnSetCommandListErrorCb=graphics_command_error;
    D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW views[2]={{data.address+64,128,counter.address+12},{1,0,UINT64_MAX}};
    const auto calls_before=stream_calls;
    commands.pfnSOSetTargets({&command},2,2,views);
    REQUIRE(stream_calls==calls_before+1 && stream_first==2 && stream_count==2 && stream_views[0].buffer==data.backend &&
        stream_views[0].counter==counter.backend && stream_views[0].offset==64 && stream_views[0].counter_offset==12 && !stream_views[1].size);
    views[1]={data.address,64,counter.address+4093}; commands.pfnSOSetTargets({&command},2,2,views);
    REQUIRE(stream_calls==calls_before+1 && reported==E_INVALIDARG);
    views[1]={data.address,UINT64_MAX,counter.address}; commands.pfnSOSetTargets({&command},2,2,views);
    REQUIRE(stream_calls==calls_before+1 && reported==E_INVALIDARG);
    commands.pfnSOSetTargets({&command},3,2,nullptr); REQUIRE(stream_calls==calls_before+1 && reported==E_INVALIDARG);
    commands.pfnSOSetTargets({&command},2,2,nullptr); REQUIRE(stream_calls==calls_before+2 && !stream_views[0].size && !stream_views[1].size);
    stream_request=static_cast<D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW *>(VirtualAlloc(nullptr,sizeof(views),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    REQUIRE(stream_request); std::memcpy(stream_request,views,sizeof(views));
    graphics_slots[0]=&command; graphics_slots[1]=&data; graphics_slots[2]=&counter; graphics_hook=stream_poison;
    next_result=E_OUTOFMEMORY; commands.pfnSOSetTargets({&command},1,1,stream_request);
    REQUIRE(!stream_request && stream_calls==calls_before+3 && reported==E_OUTOFMEMORY && graphics_runtime_seen==reinterpret_cast<HANDLE>(0x50a1) && graphics_locks_free);
    graphics_hook=nullptr; next_result=S_OK; std::memset(graphics_slots,0,sizeof(graphics_slots));
    command={}; data={}; counter={};
    REQUIRE(bind(&ctx,&command,make_peer(ctx.backend,VKDU_COMMAND_LIST),VKDU_COMMAND_LIST)==S_OK);
    REQUIRE(bind(&ctx,&data,make_peer(ctx.backend,VKDU_BUFFER),VKDU_BUFFER)==S_OK);
    REQUIRE(bind(&ctx,&counter,make_peer(ctx.backend,VKDU_BUFFER),VKDU_BUFFER)==S_OK);
    views[0]={data.address,128,counter.address}; graphics_hook=stage_retire;
    commands.pfnSOSetTargets({&command},0,1,views);
    REQUIRE(graphics_locks_free && ctx.native_retiring && !ctx.native_backend_calls && ctx.references==4 &&
        peer(command.backend)->references==1 && peer(data.backend)->references==1 && peer(counter.backend)->references==1);
    ctx.native_retiring=false; graphics_hook=nullptr;
    VioGpuD3D12BridgeUnbindObject(&command); VioGpuD3D12BridgeUnbindObject(&data); VioGpuD3D12BridgeUnbindObject(&counter);
    VioGpuD3D12BridgeUnbindObject(&foreign);
    REQUIRE(ctx.references==1 && other.references==1 && !ctx.resources && !ctx.graphics_objects);
    puts("PASS native GS/SO: actual token/signature container, declaration validation, copied PSO shader/state, foreign owner rejects, whole-batch target validation, unlocked reentrant shader/target/command destruction (controlled backend)");
    return 0;
}
