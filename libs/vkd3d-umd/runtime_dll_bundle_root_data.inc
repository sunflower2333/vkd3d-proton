/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Independent DLL consumer. Only public WDK types/opaque storage and the real
 * native KMT provider are used in hardware mode. CPU controls never call KMT. */
#include "../../tests/shaders/resource/headers/ps_store_buffer.h"
#include "../../tests/shaders/command/headers/execute_indirect_multi_dispatch_root_constants.h"
#include "../../tests/shaders/descriptors/headers/update_root_descriptors.h"

UINT dll_bundle_root_expected(UINT round, UINT i) {
    require(round < 3 && i < 16384, "bounded native bundle readback coordinates");
    UINT value = 0x13579bdf;
    if (i == 5) value = 0xa1b2c300 + round;
    if (i == 16 || i == 22) value = 0;
    if (i == 20) value = 0xdecaf000 + round;
    if (i == 32) value += 0x04020107 + round;
    if (i == 80) value += 0x00080000;
    if (i == 7) value = 0x2468ace0;
    if (i == 41) value = 0x89abcdef;
    return value;
}
UINT dll_bundle_root_verify(const UINT *words, UINT round, bool report) {
    require(words && round < 3, "native bundle complete readback input");
    UINT mismatches = 0;
    for (UINT i = 0; i < 16384; ++i) {
        const UINT expected = dll_bundle_root_expected(round,i);
        if (words[i] != expected && mismatches++ < 4 && report)
            std::printf("FAIL DLL BUNDLE ROOT round=%u word=%u actual=%08x expected=%08x\n",round,i,words[i],expected);
    }
    return mismatches;
}

struct DllBundleRoot {
    OpaqueStorage constant_storage, table_storage, heap_storage;
    D3D12DDI_HROOTSIGNATURE constants{}, table{};
    D3D12DDI_HDESCRIPTORHEAP heap{};
    UINT stride = 0;
    D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu{};
    void create(DllSession &s, const DllBuffer &input) {
        const auto h = s.create.hDrvDevice; const auto &d = s.api.device;
        D3D12DDI_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0,0,4};
        parameters[1].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_UAV;
        parameters[1].Descriptor = {0,0};
        D3D12DDI_ROOT_SIGNATURE signature{}; signature.NumParameters = 2; signature.pRootParameters = parameters;
        D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 args{&signature,1};
        constant_storage.allocate(d.pfnCalcPrivateRootSignatureSize(h,&args),"bundle-constants-root");
        check(d.pfnCreateRootSignature(h,&args,{constant_storage.data()}),"typed bundle constants root"); constants = {constant_storage.data()};
        const D3D12DDI_DESCRIPTOR_RANGE range{D3D12DDI_DESCRIPTOR_RANGE_TYPE_CBV,1,0,0,1};
        parameters[0].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = {1,&range};
        table_storage.allocate(d.pfnCalcPrivateRootSignatureSize(h,&args),"bundle-CBV-root");
        check(d.pfnCreateRootSignature(h,&args,{table_storage.data()}),"typed bundle CBV root"); table = {table_storage.data()};
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 desc{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,8,
            D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE | D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,1};
        heap_storage.allocate(d.pfnCalcPrivateDescriptorHeapSize(h,&desc),"bundle-CBV-heap");
        check(d.pfnCreateDescriptorHeap(h,&desc,{heap_storage.data()}),"typed bundle descriptor heap"); heap = {heap_storage.data()};
        stride = d.pfnGetDescriptorSizeInBytes(h,D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = d.pfnGetCPUDescriptorHandleForHeapStart(h,heap); gpu = d.pfnGetGPUDescriptorHandleForHeapStart(h,heap);
        require(stride && cpu.ptr && gpu.ptr && UINT64(stride)*8 <= SIZE_MAX &&
            cpu.ptr <= SIZE_MAX - SIZE_T(stride)*8 && gpu.ptr <= UINT64_MAX - UINT64(stride)*8,
            "bounded actual CBV handle stride/addresses");
        for (UINT index : {1u,4u,6u}) {
            auto destination = cpu; destination.ptr += SIZE_T(index)*stride;
            const UINT64 offset = index == 1 ? 0 : index == 4 ? 256 : 512;
            D3D12DDI_CONSTANT_BUFFER_VIEW_DESC cbv{};cbv.BufferLocation=input.address+offset;cbv.SizeInBytes=256;
            d.pfnCreateConstantBufferView(h,&cbv,destination);
        }
        s.status("typed native CBV descriptor creation");
    }
    void destroy(DllSession &s) {
        const auto h=s.create.hDrvDevice; const auto &d=s.api.device;
        if (heap.pDrvPrivate) { d.pfnDestroyDescriptorHeap(h,heap); heap={}; }
        if (table.pDrvPrivate) { d.pfnDestroyRootSignature(h,table); table={}; }
        if (constants.pDrvPrivate) { d.pfnDestroyRootSignature(h,constants); constants={}; }
        s.status("native bundle root/heap cleanup");
        require(constant_storage.intact() && table_storage.intact() && heap_storage.intact(),"bundle root/heap opaque canaries");
    }
};

struct DllBundlePipeline {
    OpaqueStorage vs_storage, shader_storage, blend_storage, raster_storage, depth_storage, pipeline_storage;
    D3D12DDI_HSHADER vs{}, shader{};
    D3D12DDI_HBLENDSTATE blend{};
    D3D12DDI_HRASTERIZERSTATE raster{};
    D3D12DDI_HDEPTHSTENCILSTATE depth{};
    D3D12DDI_HPIPELINESTATE pipeline{};
    template<size_t N> void create(DllSession &s,D3D12DDI_HROOTSIGNATURE root,const DWORD (&code)[N],bool graphics) {
        const auto h=s.create.hDrvDevice; const auto &d=s.api.device;
        D3D12DDIARG_STAGE_IO_SIGNATURES io{};
        D3D12DDIARG_SIGNATURE_ENTRY input{},output{};
        if (graphics) {
            input.SystemValue=static_cast<D3D10_SB_NAME>(6); input.Mask=1;
            input.RegisterComponentType=static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(1);
            output.SystemValue=static_cast<D3D10_SB_NAME>(1); output.Mask=15;
            output.RegisterComponentType=static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(3);
            io.NumInputSignatureEntries=io.NumOutputSignatureEntries=1;
            io.pInputSignatureDeprecated=&input; io.pOutputSignatureDeprecated=&output;
            const auto vertex=dll_graphics_tokens(vs_flat_color_code_dxbc);
            vs_storage.allocate(d.pfnCalcPrivateShaderSize(h,vertex.data(),root,&io),"bundle-VS");
            d.pfnCreateVertexShader(h,vertex.data(),root,{vs_storage.data()},&io,D3D12DDI_CREATE_SHADER_FLAG_NONE);
            vs={vs_storage.data()}; s.status("typed bundle VS");
            io={}; // UAV-only PS declares no input/output signature entries.
        }
        const auto tokens=dll_graphics_tokens(code);
        shader_storage.allocate(d.pfnCalcPrivateShaderSize(h,tokens.data(),root,&io),graphics?"bundle-PS":"bundle-CS");
        if (graphics) d.pfnCreatePixelShader(h,tokens.data(),root,{shader_storage.data()},&io,D3D12DDI_CREATE_SHADER_FLAG_NONE);
        else d.pfnCreateComputeShader(h,tokens.data(),root,{shader_storage.data()},D3D12DDI_CREATE_SHADER_FLAG_NONE);
        shader={shader_storage.data()}; s.status("typed bundle shader");
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pso{}; pso.hRootSignature=root; pso.NodeMask=1;
        if (graphics) {
            D3D12DDI_BLEND_DESC bs{}; bs.RenderTarget[0].RenderTargetWriteMask=15;
            D3D12DDI_RASTERIZER_DESC rs{}; rs.FillMode=static_cast<D3D12DDI_FILL_MODE>(3);
            rs.CullMode=static_cast<D3D12DDI_CULL_MODE>(1); rs.ScissorEnable=rs.DepthClipEnable=TRUE;
            D3D12DDI_DEPTH_STENCIL_DESC ds{};
            blend_storage.allocate(d.pfnCalcPrivateBlendStateSize(h,&bs),"bundle-blend");
            raster_storage.allocate(d.pfnCalcPrivateRasterizerStateSize(h,&rs),"bundle-raster");
            depth_storage.allocate(d.pfnCalcPrivateDepthStencilStateSize(h,&ds),"bundle-depth");
            d.pfnCreateBlendState(h,&bs,{blend_storage.data()}); blend={blend_storage.data()};
            d.pfnCreateRasterizerState(h,&rs,{raster_storage.data()}); raster={raster_storage.data()};
            d.pfnCreateDepthStencilState(h,&ds,{depth_storage.data()}); depth={depth_storage.data()};
            pso.hVertexShader=vs; pso.hPixelShader=shader; pso.hBlendState=blend; pso.hRasterizerState=raster; pso.hDepthStencilState=depth;
            pso.PrimitiveTopologyType=D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets=pso.SampleDesc.Count=1; pso.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; pso.SampleMask=UINT_MAX;
        } else pso.hComputeShader=shader;
        pipeline_storage.allocate(d.pfnCalcPrivatePipelineStateSize(h,&pso),"bundle-pipeline");
        pso.hDrvPipelineState={pipeline_storage.data()};
        check(d.pfnCreatePipelineState(h,&pso),"typed bundle pipeline"); pipeline=pso.hDrvPipelineState;
        s.status("typed bundle pipeline/state publication");
    }
    void destroy(DllSession &s) {
        const auto h=s.create.hDrvDevice; const auto &d=s.api.device;
        if(pipeline.pDrvPrivate){d.pfnDestroyPipelineState(h,pipeline);pipeline={};}
        if(shader.pDrvPrivate){d.pfnDestroyShader(h,shader);shader={};}
        if(vs.pDrvPrivate){d.pfnDestroyShader(h,vs);vs={};}
        if(blend.pDrvPrivate){d.pfnDestroyBlendState(h,blend);blend={};}
        if(raster.pDrvPrivate){d.pfnDestroyRasterizerState(h,raster);raster={};}
        if(depth.pDrvPrivate){d.pfnDestroyDepthStencilState(h,depth);depth={};}
        s.status("typed bundle pipeline cleanup");
        require(vs_storage.intact()&&shader_storage.intact()&&blend_storage.intact()&&raster_storage.intact()&&depth_storage.intact()&&pipeline_storage.intact(),"bundle shader/state opaque canaries");
    }
};

struct DllBundleRtv {
    OpaqueStorage storage; D3D12DDI_HDESCRIPTORHEAP heap{}; D3D12DDI_CPU_DESCRIPTOR_HANDLE handle{};
    void create(DllSession &s,const DllRenderTarget &target){
        const auto h=s.create.hDrvDevice; const auto &d=s.api.device;
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 desc{D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE,1};
        storage.allocate(d.pfnCalcPrivateDescriptorHeapSize(h,&desc),"bundle-RTV");
        check(d.pfnCreateDescriptorHeap(h,&desc,{storage.data()}),"typed bundle RTV heap");heap={storage.data()};
        handle=d.pfnGetCPUDescriptorHandleForHeapStart(h,heap);
        D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view{};view.hDrvResource=target.resource;
        view.ResourceDimension=D3D12DDI_RD_TEXTURE2D;view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;view.Tex2D.ArraySize=1;
        d.pfnCreateRenderTargetView(h,&view,handle);s.status("typed bundle RTV");
    }
    void destroy(DllSession &s){if(heap.pDrvPrivate){s.api.device.pfnDestroyDescriptorHeap(s.create.hDrvDevice,heap);heap={};}s.status("bundle RTV cleanup");require(storage.intact(),"RTV opaque canaries");}
};

void dll_bundle_transition(DllSession &s,D3D12DDI_HCOMMANDLIST command,D3D12DDI_HRESOURCE resource,UINT before,UINT after){
    D3D12DDIARG_RESOURCE_BARRIER_0003 b{};b.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.hResource=resource;b.Transition.Subresource=D3D12DDI_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=static_cast<D3D12DDI_RESOURCE_STATES>(before);b.Transition.StateAfter=static_cast<D3D12DDI_RESOURCE_STATES>(after);
    s.api.commands.pfnResourceBarrier(command,1,&b);
}
void dll_bundle_barrier(DllSession &s,D3D12DDI_HCOMMANDLIST command,D3D12DDI_HRESOURCE resource){
    D3D12DDIARG_RESOURCE_BARRIER_0003 b{};b.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_UAV;b.UAV.hResource=resource;
    s.api.commands.pfnResourceBarrier(command,1,&b);
}
void dll_bundle_copy(DllSession &s,D3D12DDI_HCOMMANDLIST command,D3D12DDI_HRESOURCE dst,D3D12DDI_HRESOURCE src){
    D3D12DDIARG_BUFFER_PLACEMENT d{},v{};d.BaseAddress.UMD.hResource=dst;v.BaseAddress.UMD.hResource=src;
    s.api.commands.pfnCopyBufferRegion(command,d,v,65536);
}

struct DllBundleWorkload {
    DllSession session;
    DllRenderTarget target;
    DllBuffer input,upload,output,readback;
    DllRecording direct,clear_bundle,table_bundle;
    DllBundleRoot roots;
    DllBundlePipeline graphics,table_graphics,compute,table_compute;
    DllBundleRtv rtv;
    std::array<DllBatch,3> batches;
    void quiescent()const{for(const auto &b:batches)if(b.gate.context)require(b.gate.release_allowed()&&!b.queue.pDrvPrivate&&!b.event,"native bundle all prior queues drained before reuse/map/release");}
    void record(UINT round){
        auto &s=session;const auto &c=s.api.commands;const auto cmd=direct.command;
        if(round){direct.reopen(s);clear_bundle.reopen(s);table_bundle.reopen(s);dll_bundle_transition(s,cmd,output.resource,0x800,0x400);}
        dll_bundle_copy(s,cmd,output.resource,upload.resource);dll_bundle_transition(s,cmd,output.resource,0x400,8);
        c.pfnSetGraphicsRootSignature(cmd,roots.constants);c.pfnSetComputeRootSignature(cmd,roots.constants);
        c.pfnOMSetRenderTargets(cmd,1,&rtv.handle,FALSE,nullptr);
        const D3D12DDI_VIEWPORT viewport{0,0,4,4,0,1};const D3D12DDI_RECT scissor{1,2,2,3};
        c.pfnRsSetViewports(cmd,1,&viewport);c.pfnRsSetScissorRects(cmd,1,&scissor);c.pfnIaSetTopology(cmd,D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT values[4]={20,0xa1b2c300+round,0x20000000,0x40000000};
        c.pfnSetGraphicsRoot32BitConstants(cmd,0,4,values,0);
        values[0]=7+round;values[1]=0x100;values[2]=0x20000;values[3]=0x4000000;
        c.pfnSetComputeRoot32BitConstants(cmd,0,4,values,0);
        c.pfnSetGraphicsRootUnorderedAccessView(cmd,1,output.address);c.pfnSetComputeRootUnorderedAccessView(cmd,1,output.address+128);
        c.pfnClearRootArguments(clear_bundle.command);c.pfnCloseCommandList(clear_bundle.command);
        c.pfnSetPipelineState(cmd,graphics.pipeline);c.pfnDrawInstanced(cmd,3,1,0,0);dll_bundle_barrier(s,cmd,output.resource);
        c.pfnSetPipelineState(cmd,compute.pipeline);c.pfnDispatch(cmd,1,1,1);dll_bundle_barrier(s,cmd,output.resource);
        c.pfnSetPipelineState(cmd,graphics.pipeline);c.pfnExecuteBundle(cmd,clear_bundle.command);
        values[0]=64;c.pfnSetGraphicsRoot32BitConstants(cmd,0,1,values,0);c.pfnSetGraphicsRootUnorderedAccessView(cmd,1,output.address);
        c.pfnDrawInstanced(cmd,3,1,0,0);dll_bundle_barrier(s,cmd,output.resource);
        const UINT patch=0xdecaf000+round;c.pfnSetGraphicsRoot32BitConstants(cmd,0,1,&patch,1);
        c.pfnSetGraphicsRootUnorderedAccessView(cmd,1,output.address+16);c.pfnDrawInstanced(cmd,3,1,0,0);dll_bundle_barrier(s,cmd,output.resource);
        c.pfnSetPipelineState(cmd,compute.pipeline);c.pfnSetComputeRootUnorderedAccessView(cmd,1,output.address+256);
        c.pfnDispatch(cmd,1,1,1);dll_bundle_barrier(s,cmd,output.resource);
        values[0]=0x00080000;c.pfnSetComputeRoot32BitConstants(cmd,0,1,values,2);
        c.pfnSetComputeRootUnorderedAccessView(cmd,1,output.address+320);c.pfnDispatch(cmd,1,1,1);dll_bundle_barrier(s,cmd,output.resource);
        c.pfnSetPipelineState(cmd,graphics.pipeline);c.pfnExecuteBundle(cmd,clear_bundle.command);
        values[0]=88;c.pfnSetGraphicsRoot32BitConstants(cmd,0,1,values,0);c.pfnSetGraphicsRootUnorderedAccessView(cmd,1,output.address);
        c.pfnDrawInstanced(cmd,3,1,0,0);dll_bundle_barrier(s,cmd,output.resource);
        c.pfnSetPipelineState(cmd,compute.pipeline);c.pfnSetComputeRootUnorderedAccessView(cmd,1,output.address+384);
        c.pfnDispatch(cmd,1,1,1);dll_bundle_barrier(s,cmd,output.resource);
        c.pfnClearRootArguments(table_bundle.command);
        if(round){c.pfnSetGraphicsRootSignature(table_bundle.command,roots.table);c.pfnSetComputeRootSignature(table_bundle.command,roots.table);}
        auto g=roots.gpu;g.ptr+=UINT64(3)*roots.stride;auto v=roots.gpu;v.ptr+=UINT64(5)*roots.stride;
        c.pfnSetGraphicsRootDescriptorTable(table_bundle.command,0,g);c.pfnSetComputeRootDescriptorTable(table_bundle.command,0,v);
        c.pfnCloseCommandList(table_bundle.command);
        c.pfnSetGraphicsRootSignature(cmd,roots.table);c.pfnSetComputeRootSignature(cmd,roots.table);
        // Production native error callbacks are sticky. Positive mode always
        // binds the required heap; refusal is checked only by CPU controls.
        c.pfnSetDescriptorHeaps(cmd,1,&roots.heap);c.pfnExecuteBundle(cmd,table_bundle.command);
        c.pfnSetPipelineState(cmd,table_graphics.pipeline);c.pfnSetGraphicsRootUnorderedAccessView(cmd,1,output.address);
        c.pfnDrawInstanced(cmd,3,1,0,0);dll_bundle_barrier(s,cmd,output.resource);
        c.pfnSetPipelineState(cmd,table_compute.pipeline);c.pfnSetComputeRootUnorderedAccessView(cmd,1,output.address);
        c.pfnDispatch(cmd,1,1,1);dll_bundle_barrier(s,cmd,output.resource);
        dll_bundle_transition(s,cmd,output.resource,8,0x800);dll_bundle_copy(s,cmd,readback.resource,output.resource);
        c.pfnCloseCommandList(cmd);s.status("native bundle full recording before submission");
    }
    void run(LUID luid){
        auto &s=session;auto &r=s.runtime;dll_graphics_slots(s);s.required_slots(true);
        require(s.api.commands.pfnClearRootArguments&&s.api.commands.pfnExecuteBundle&&s.api.commands.pfnSetGraphicsRoot32BitConstants&&
            s.api.commands.pfnSetComputeRoot32BitConstants&&s.api.commands.pfnSetGraphicsRootUnorderedAccessView&&
            s.api.commands.pfnSetComputeRootUnorderedAccessView&&s.api.commands.pfnSetGraphicsRootDescriptorTable&&s.api.device.pfnCreateConstantBufferView,
            "complete native bundle/root-data slots required before KMT open");
        s.initialize(luid);dll_graphics_public_gates(s);
        target.create(s);input.create(s,0xac3,D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK);upload.create(s,0xac3,D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK);
        output.create(s,0x400,D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE);readback.create(s,0x400,D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK);
        const std::set<D3DKMT_HANDLE> identities{target.allocation,input.allocation,upload.allocation,output.allocation,readback.allocation};
        require(identities.size()==5,"five distinct native KMT public allocation identities");r.target_handle=readback.allocation;
        UINT *words=input.map(s);std::fill(words,words+16384,0u);words[0]=44;words[1]=0x0badcafe;
        words[64]=28;words[65]=0x2468ace0;words[128]=41;words[129]=0x89abcdef;input.unmap(s);
        words=upload.map(s);std::fill(words,words+16384,0x13579bdfu);upload.unmap(s);
        roots.create(s,input);graphics.create(s,roots.constants,ps_store_buffer_code_dxbc,true);table_graphics.create(s,roots.table,ps_store_buffer_code_dxbc,true);
        compute.create(s,roots.constants,execute_indirect_multi_dispatch_root_constants_code_dxbc,false);
        table_compute.create(s,roots.table,update_root_descriptors_code_dxbc,false);rtv.create(s,target);
        direct.create(s);clear_bundle.create(s,D3D12DDI_COMMAND_LIST_TYPE_BUNDLE);table_bundle.create(s,D3D12DDI_COMMAND_LIST_TYPE_BUNDLE);
        for(UINT round=0;round<3;++round){
            quiescent();words=readback.map(s);std::fill(words,words+16384,0xfedc0000u^round);readback.unmap(s);
            record(round);auto &batch=batches[round];batch.create(s);batch.execute(s,direct,round);
            require(batch.gate.readback_allowed(),"bundle readback waits actual OS event plus typed queue drain");
            words=readback.map(s);const UINT mismatches=dll_bundle_root_verify(words,round,true);readback.unmap(s);
            require(!mismatches,"actual native DLL bundle root-data full64KiB shader readback");
            target.identity(s);output.identity(s,D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE);readback.identity(s,D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK);
            std::printf("PASS DLL BUNDLE ROOT batch=%u checked_words=16384 root_window=1024 sentinel_tail=15360 typed_drain_before_map=1 clear_replay=1 partial_graphics_compute=1 CBV_bases=3,5 range_offset=1\n",round);
        }
        cleanup();
    }
    void cleanup(){
        auto &s=session;auto &r=s.runtime;quiescent();check(r.observe_completions(0),"bundle terminal completion observation");
        dll_graphics_public_gates(s);r.retirement_sealed=true;const auto terminal_renders=r.renders.load();
        direct.destroy(s);clear_bundle.destroy(s);table_bundle.destroy(s);
        graphics.destroy(s);table_graphics.destroy(s);compute.destroy(s);table_compute.destroy(s);rtv.destroy(s);roots.destroy(s);
        target.destroy(s);input.destroy(s);upload.destroy(s);output.destroy(s);readback.destroy(s);
        require(r.pageable_deallocations==5,"exact five native bundle public Deallocate2 flags3");s.status("native bundle all public owners released");
        r.native_cleanup=true;s.adapter.pfnDestroyDevice(s.create.hDrvDevice);s.device_live=false;
        check(s.adapter.pfnCloseAdapter(s.open.hAdapter),"bundle typed CloseAdapter");s.open.hAdapter={};
        {std::lock_guard<std::mutex> lock(r.mutex);require(!r.context_handle&&r.contexts.empty()&&r.allocations.empty()&&!r.callback_exceptions&&
            !r.command_errors&&!r.device_errors&&r.renders==terminal_renders,"bundle native destroy leaves no context/allocation/fault/late render");
            for(const auto &e:r.completions)require(!e.event,"bundle no outstanding OS observer event");}
        require(s.device_storage.intact(),"bundle retired device canaries");check(r.close(),"bundle final real KMT provider close");
        require(!r.device&&!r.adapter&&!r.paging_queue&&r.resident_allocations.empty(),"bundle provider ownership closed");s.api.unload();
        std::printf("PASS DLL BUNDLE ROOT cleanup batches=3 public_deallocate2=%u internal_deallocate=%u callbacks=0 contexts=0 allocations=0 late_renders=0 core_reference_released=1\n",r.pageable_deallocations.load(),r.internal_deallocations.load());
        std::puts("BOUNDARY standalone changed DLL + handwritten runtime + real native KMT/GPU only; public0; no SYSTEM/Present/fullDX12 acceptance");
    }
};

void run_dll_bundle_root_data(const wchar_t *core,const wchar_t *hash,const wchar_t *manifest,const wchar_t *icd_hash,LUID luid){
#if defined(VKDU_PROBE_TEST_DROP_SYNCHRONOUS) || defined(VKDU_PROBE_TEST_WRONG_EVENT_CONTEXT) || defined(VKDU_PROBE_TEST_CPU_COMPLETION) || defined(VKDU_PROBE_TEST_EARLY_RELEASE)
    (void)core;(void)hash;(void)manifest;(void)icd_hash;(void)luid;throw ProbeFailure("CONTROL-ONLY binary refuses native bundle mode");
#else
    Deadline deadline;timeline.begin();TlCall run(TlKind::Run,{3,tl_luid(luid)});auto *workload=new DllBundleWorkload;
    try{
        tl_void(TlKind::CoreLoad,[&]{workload->session.api.load(core,hash);});
        require(workload->session.api.file.hash!="9f1440067d619234b65c1ce8d211d82484aef26e8200430e62b9161e206ee0b1","new bundle probe refuses frozen Core9f14 lacking bundle implementation");
        tl_void(TlKind::IcdConfigure,[&]{workload->session.icd.configure(manifest,icd_hash);});
        require(workload->session.icd.library.hash=="d002cfc7033d8fa2ccb57e7150514ca59f25c9e351103d720d2d088809d60969","bundle probe retains exact paired ICDd002");
        workload->run(luid);delete workload;run.end({0});timeline.emit(0,TlKind::Terminal,0,{0,0});timeline.finish(0,0);
    }catch(...){timeline.emit(0,TlKind::Retain,0,{1});run.end({1});timeline.emit(0,TlKind::Terminal,0,{1,1});timeline.finish(1,1);
        std::puts("FAIL retained native bundle session: all DLL/callback/opaque/provider owners remain until process exit");throw;}
#endif
}

/* These controls validate the actual new recorder/oracle with typed local
 * peers. They never initialize DllSession or call allocation/KMT/Vulkan. */
struct DllBundleRecordControl {
    DllBundleWorkload work;
    UINT round=0,draws=0,dispatches=0,clears=0,executes=0,copies=0,barriers=0,closes=0,resets=0;
    UINT graphics_tables=0,compute_tables=0,heap_binds=0,graphics_partials=0,compute_partials=0;
    bool clear_closed=false,table_closed=false,heaps=false,drop_heap=false,bad_table=false;
    static DllBundleRecordControl *active;
    static DllBundleRecordControl &self(){require(active!=nullptr,"active local bundle recorder control");return *active;}
    static HRESULT APIENTRY status(D3D12DDI_HDEVICE){return S_OK;}
    bool primary(D3D12DDI_HCOMMANDLIST h)const{return h.pDrvPrivate==&work.direct;}
    bool clear(D3D12DDI_HCOMMANDLIST h)const{return h.pDrvPrivate==&work.clear_bundle;}
    bool table(D3D12DDI_HCOMMANDLIST h)const{return h.pDrvPrivate==&work.table_bundle;}
    static void APIENTRY reset_allocator(D3D12DDI_HCOMMANDALLOCATOR){++self().resets;}
    static void APIENTRY reset_command(D3D12DDI_HCOMMANDLIST,const D3D12DDIARG_RESETCOMMANDLIST*){}
    static void APIENTRY root(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HROOTSIGNATURE r){
        auto &f=self();require((f.primary(h)||f.table(h))&&(r.pDrvPrivate==&f.work.roots.constants||r.pDrvPrivate==&f.work.roots.table),"control root/layout identity");
    }
    static void APIENTRY pipeline(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HPIPELINESTATE){require(self().primary(h),"pipeline only on primary");}
    static void APIENTRY targets(D3D12DDI_HCOMMANDLIST h,UINT n,const D3D12DDI_CPU_DESCRIPTOR_HANDLE *r,BOOL range,const D3D12DDI_CPU_DESCRIPTOR_HANDLE *d){
        auto &f=self();require(f.primary(h)&&n==1&&r&&r->ptr==f.work.rtv.handle.ptr&&!range&&!d,"control actual RTV slot");
    }
    static void APIENTRY viewport(D3D12DDI_HCOMMANDLIST,UINT n,const D3D12DDI_VIEWPORT *v){require(n==1&&v&&v->Width==4&&v->Height==4,"control root data viewport");}
    static void APIENTRY scissor(D3D12DDI_HCOMMANDLIST,UINT n,const D3D12DDI_RECT *r){require(n==1&&r&&r->left==1&&r->top==2&&r->right==2&&r->bottom==3,"control single-pixel scissor");}
    static void APIENTRY topology(D3D12DDI_HCOMMANDLIST,D3D12DDI_PRIMITIVE_TOPOLOGY t){require(t==D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST,"control triangle topology");}
    static void APIENTRY graphics_constants(D3D12DDI_HCOMMANDLIST h,UINT index,UINT n,const void *data,UINT offset){
        auto &f=self();require(f.primary(h)&&!index&&data&&(n==4||n==1)&&offset+n<=4,"control bounded graphics constants");
        if(n==1&&offset==1){require(*static_cast<const UINT*>(data)==0xdecaf000+f.round,"control partial graphics value");++f.graphics_partials;}
    }
    static void APIENTRY compute_constants(D3D12DDI_HCOMMANDLIST h,UINT index,UINT n,const void *data,UINT offset){
        auto &f=self();require(f.primary(h)&&!index&&data&&(n==4||n==1)&&offset+n<=4,"control bounded compute constants");
        if(n==1){require(offset==2&&*static_cast<const UINT*>(data)==0x00080000,"control partial compute lane");++f.compute_partials;}
    }
    static void APIENTRY uav(D3D12DDI_HCOMMANDLIST h,UINT index,D3D12DDI_GPU_VIRTUAL_ADDRESS va){
        auto &f=self();require(f.primary(h)&&index==1&&va>=f.work.output.address&&va<=f.work.output.address+384&&!(va&3),"control rebound queried output UAV address");
    }
    static void APIENTRY clear_root(D3D12DDI_HCOMMANDLIST h){
        auto &f=self();require(f.clear(h)||f.table(h),"clear recorded only in real bundle handles");++f.clears;
    }
    static void APIENTRY close(D3D12DDI_HCOMMANDLIST h){
        auto &f=self();if(f.clear(h))f.clear_closed=true;else if(f.table(h))f.table_closed=true;else require(f.primary(h),"known closed list");++f.closes;
    }
    static void APIENTRY execute(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HCOMMANDLIST b){
        auto &f=self();require(f.primary(h),"bundle executes through primary only");
        if(f.clear(b)){require(f.clear_closed&&f.draws>=1&&f.dispatches>=1,"seeded shader work precedes clear execute");}
        else require(f.table(b)&&f.table_closed&&f.heaps&&f.graphics_tables==1&&f.compute_tables==1,"table bundle needs matching bound heap and both nonzero tables");
        ++f.executes;
    }
    static void APIENTRY draw(D3D12DDI_HCOMMANDLIST h,UINT v,UINT i,UINT first,UINT instance){auto &f=self();require(f.primary(h)&&v==3&&i==1&&!first&&!instance,"control actual draw arguments");++f.draws;}
    static void APIENTRY dispatch(D3D12DDI_HCOMMANDLIST h,UINT x,UINT y,UINT z){auto &f=self();require(f.primary(h)&&x==1&&y==1&&z==1,"control dispatch arguments");++f.dispatches;}
    static void APIENTRY bind_heaps(D3D12DDI_HCOMMANDLIST h,UINT n,D3D12DDI_HDESCRIPTORHEAP *p){
        auto &f=self();require(f.primary(h)&&n==1&&p&&p->pDrvPrivate==&f.work.roots.heap,"control exact CBV heap");++f.heap_binds;f.heaps=!f.drop_heap;
    }
    static void APIENTRY graphics_table(D3D12DDI_HCOMMANDLIST h,UINT i,D3D12DDI_GPU_DESCRIPTOR_HANDLE p){
        auto &f=self();require(f.table(h)&&!i&&p.ptr==f.work.roots.gpu.ptr+UINT64(f.bad_table?0:3)*f.work.roots.stride,"control graphics table base3");++f.graphics_tables;
    }
    static void APIENTRY compute_table(D3D12DDI_HCOMMANDLIST h,UINT i,D3D12DDI_GPU_DESCRIPTOR_HANDLE p){
        auto &f=self();require(f.table(h)&&!i&&p.ptr==f.work.roots.gpu.ptr+UINT64(5)*f.work.roots.stride,"control compute table base5");++f.compute_tables;
    }
    static void APIENTRY barrier(D3D12DDI_HCOMMANDLIST h,UINT n,const D3D12DDIARG_RESOURCE_BARRIER_0003 *p){
        auto &f=self();require(f.primary(h)&&n==1&&p&&!p->Flags,"control barrier shape");
        require(p->Type==D3D12DDI_RESOURCE_BARRIER_TYPE_UAV||p->Type==D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION,"control UAV/transition only");++f.barriers;
    }
    static void APIENTRY copy(D3D12DDI_HCOMMANDLIST h,D3D12DDIARG_BUFFER_PLACEMENT dst,D3D12DDIARG_BUFFER_PLACEMENT src,UINT64 bytes){
        auto &f=self();require(f.primary(h)&&bytes==65536&&!dst.BaseAddress.UMD.Offset&&!src.BaseAddress.UMD.Offset,"control full64KiB typed copy");
        require((dst.BaseAddress.UMD.hResource.pDrvPrivate==&f.work.output&&src.BaseAddress.UMD.hResource.pDrvPrivate==&f.work.upload)||
            (dst.BaseAddress.UMD.hResource.pDrvPrivate==&f.work.readback&&src.BaseAddress.UMD.hResource.pDrvPrivate==&f.work.output),"control exact output/readback copy identities");++f.copies;
    }
    DllBundleRecordControl(){
        auto &s=work.session;auto &c=s.api.commands;s.device_live=true;s.api.status=status;
        work.direct.command={&work.direct};work.clear_bundle.command={&work.clear_bundle};work.table_bundle.command={&work.table_bundle};
        work.roots.constants={&work.roots.constants};work.roots.table={&work.roots.table};work.roots.heap={&work.roots.heap};work.roots.stride=32;work.roots.gpu.ptr=0x200000;
        work.output.address=0x100000;work.output.resource={&work.output};work.upload.resource={&work.upload};work.readback.resource={&work.readback};work.rtv.handle.ptr=0x300000;
        s.api.device.pfnResetCommandAllocator=reset_allocator;c.pfnResetCommandList=reset_command;
        c.pfnSetGraphicsRootSignature=root;c.pfnSetComputeRootSignature=root;c.pfnSetPipelineState=pipeline;c.pfnOMSetRenderTargets=targets;
        c.pfnRsSetViewports=viewport;c.pfnRsSetScissorRects=scissor;c.pfnIaSetTopology=topology;
        c.pfnSetGraphicsRoot32BitConstants=graphics_constants;c.pfnSetComputeRoot32BitConstants=compute_constants;
        c.pfnSetGraphicsRootUnorderedAccessView=uav;c.pfnSetComputeRootUnorderedAccessView=uav;
        c.pfnClearRootArguments=clear_root;c.pfnCloseCommandList=close;c.pfnExecuteBundle=execute;c.pfnDrawInstanced=draw;c.pfnDispatch=dispatch;
        c.pfnSetDescriptorHeaps=bind_heaps;c.pfnSetGraphicsRootDescriptorTable=graphics_table;c.pfnSetComputeRootDescriptorTable=compute_table;
        c.pfnResourceBarrier=barrier;c.pfnCopyBufferRegion=copy;
    }
    void run(UINT n){round=n;active=this;work.record(n);active=nullptr;
        require(draws==5&&dispatches==5&&clears==2&&executes==3&&copies==2&&closes==3&&graphics_tables==1&&compute_tables==1&&heap_binds==1&&
            graphics_partials==1&&compute_partials==1&&resets==(n?3u:0u),"actual bundle recorder counts and partial/replay/table coverage");}
};
DllBundleRecordControl *DllBundleRecordControl::active=nullptr;

void dll_bundle_root_data_controls(){
    for(UINT round=0;round<3;++round){auto f=std::make_unique<DllBundleRecordControl>();f->run(round);}
    unsigned rejected=0;
    for(unsigned fault=0;fault<2;++fault){auto f=std::make_unique<DllBundleRecordControl>();f->drop_heap=fault==0;f->bad_table=fault==1;
        try{f->run(0);}catch(const ProbeFailure&){++rejected;}DllBundleRecordControl::active=nullptr;}
    require(rejected==2,"recorder rejects missing heap and wrong CBV base");
    std::vector<UINT> words(16384);
    const UINT coordinates[]={0,5,7,16,20,22,32,41,64,80,96,1023,1024,16383};
    unsigned corruptions=0;
    for(UINT round=0;round<3;++round){
        for(UINT i=0;i<words.size();++i)words[i]=dll_bundle_root_expected(round,i);
        require(!dll_bundle_root_verify(words.data(),round,false),"all64KiB oracle positive");
        for(UINT index:coordinates){words[index]^=1;require(dll_bundle_root_verify(words.data(),round,false)==1,"whole-buffer oracle rejects changed/sentinel/tail corruption");words[index]^=1;++corruptions;}
    }
    // Exact inherited semantic RED observations must fail this same new oracle.
    for(UINT i=0;i<words.size();++i)words[i]=dll_bundle_root_expected(0,i);
    for(const auto &fault:std::array<std::pair<UINT,UINT>,3>{{{16,0xa1b2c300},{64,0x17599ce6},{7,0x13579bdf}}}){
        const UINT saved=words[fault.first];words[fault.first]=fault.second;
        require(dll_bundle_root_verify(words.data(),0,false)==1,"actual inherited semantic fault coordinate rejected");words[fault.first]=saved;++corruptions;
    }
    std::printf("PASS DLL BUNDLE ROOT CPU controls recording_rounds=3 refusal_controls=2 complete_oracles=3 corruptions=%u native_calls=0 DLL_load=0 KMT=0 Vulkan=0\n",corruptions);
}
