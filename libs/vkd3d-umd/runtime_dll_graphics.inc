/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Offscreen graphics through public WDK DDIs in the pinned standalone DLL.
 * No backend symbols, private layouts, aliases, Present or runtime admission. */
#include "../../tests/shaders/render_target/headers/vs_flat_color.h"
#include "../../tests/shaders/render_target/headers/ps_flat_color.h"

template<size_t N> std::vector<UINT> dll_graphics_tokens(const DWORD (&code)[N]) {
    require(N >= 8 && code[0] == 0x43425844 && code[6] == N * 4 && code[7] <= N - 8,
        "bounded frozen graphics DXBC header");
    for (UINT i = 0; i < code[7]; ++i) {
        const size_t offset = code[8 + i];
        require(!(offset % 4) && offset / 4 <= N - 2, "bounded graphics DXBC chunk offset");
        const size_t start = offset / 4, bytes = code[start + 1];
        require(!(bytes % 4) && bytes / 4 <= N - start - 2, "bounded graphics DXBC chunk size");
        if (code[start] == 0x58454853 || code[start] == 0x52444853) {
            require(bytes >= 8 && code[start + 3] == bytes / 4, "graphics token stream length agrees with chunk");
            return {code + start + 2, code + start + 2 + bytes / 4};
        }
    }
    throw ProbeFailure("graphics fixture has no shader tokens");
}

UINT dll_graphics_expected(UINT round, UINT i) {
    if (i >= 128 && i < 896 && (i - 128) % 64 < 16) {
        const UINT x = (i - 128) % 64, y = (i - 128) / 64;
        return round < 2 && x >= 2 + round && x < 12 && y >= 1 + round && y < 9 ?
            0xffffffffu : 0xff0000ffu;
    }
    return 0x71710000u ^ round;
}
UINT dll_graphics_verify(const UINT *words, UINT round, bool report) {
    require(words && round < 4, "bounded graphics oracle input");
    UINT mismatches = 0;
    for (UINT i = 0; i < 16384; ++i) {
        const UINT expected = dll_graphics_expected(round, i);
        if (words[i] != expected && mismatches++ < 4 && report)
            std::printf("FAIL DLL GRAPHICS round=%u word=%u actual=%08x expected=%08x\n", round, i, words[i], expected);
    }
    return mismatches;
}

struct DllRenderTarget {
    OpaqueStorage heap_storage, resource_storage;
    D3D12DDI_HHEAP heap{};
    D3D12DDI_HRESOURCE resource{};
    D3DKMT_HANDLE allocation = 0;
    UINT64 backing_iova = 0, bytes = 0;
    void identity(DllSession &s) {
        const auto &d = s.api.device; const auto device = s.create.hDrvDevice;
        D3D12DDI_RESOURCE_ALLOCATION_INFO info{}; D3DKMT_HANDLE queried = 0;
        d.pfnCheckExistingResourceAllocationInfo(device, resource, &info, &queried);
        const auto checked = d.pfnCheckResourceAllocationHandle(device, {resource.pDrvPrivate});
        const auto va = d.pfnCheckResourceVirtualAddress(device, resource);
        // A texture has no public buffer GPUVA. Its opaque backing allocation
        // still has a real provider IOVA, checked independently below.
        std::printf("RT_QUERY public_gpuva=%llx queried=%u checked=%u bytes=%llu expected_bytes=%llu alignment=%llu layout=%u\n",
            static_cast<unsigned long long>(va), queried, checked,
            static_cast<unsigned long long>(info.ResourceDataSize), static_cast<unsigned long long>(bytes),
            static_cast<unsigned long long>(info.ResourceDataAlignment), static_cast<UINT>(info.Layout));
        s.status("typed render-target identity queries");
        std::lock_guard<std::mutex> lock(s.runtime.mutex);
        const auto found = s.runtime.allocations.find(queried);
        const bool present = found != s.runtime.allocations.end();
        const ProbeWireAllocationInfo ledger = present ? found->second : ProbeWireAllocationInfo{};
        std::printf("RT_LEDGER present=%u allocation=%u backing_iova=%llx bytes=%llu context=%u expected_context=%u generation=%llu expected_generation=%llu\n",
            present ? 1u : 0u, queried, static_cast<unsigned long long>(ledger.iova),
            static_cast<unsigned long long>(ledger.size), ledger.context_id, s.runtime.context_id,
            static_cast<unsigned long long>(ledger.generation), static_cast<unsigned long long>(s.runtime.identity.generation));
        require(queried && queried == checked && !va && info.ResourceDataSize == bytes &&
            info.ResourceDataAlignment == 65536 && info.Layout == D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0,
            "render-target typed allocation info/handle and zero public GPUVA agree");
        require(present && ledger.size == bytes && ledger.iova &&
            ledger.context_id == s.runtime.context_id && ledger.generation == s.runtime.identity.generation,
            "render-target identity matches exact real Allocate ledger");
        if (allocation) require(allocation == queried && backing_iova == ledger.iova, "render-target identity unchanged across batches");
        require(heap_storage.intact() && resource_storage.intact(), "render-target opaque canaries intact");
        allocation = queried; backing_iova = ledger.iova;
        std::printf("RT allocation=%u public_gpuva=0 backing_iova=%llx bytes=%llu width=16 height=12 rgba8=1 cpu_map_allowed=0\n",
            allocation, static_cast<unsigned long long>(backing_iova), static_cast<unsigned long long>(bytes));
    }
    void create(DllSession &s) {
        const auto &d = s.api.device; const auto device = s.create.hDrvDevice;
        D3D12DDIARG_CREATERESOURCE_0003 r{};
        r.ResourceType = D3D12DDI_RT_TEXTURE2D; r.Width = 16; r.Height = 12;
        r.DepthOrArraySize = r.MipLevels = r.SampleDesc.Count = 1;
        r.Format = DXGI_FORMAT_R8G8B8A8_UNORM; r.Layout = D3D12DDI_TL_UNDEFINED;
        r.Flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;
        r.InitialResourceState = D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
        D3D12DDI_RESOURCE_ALLOCATION_INFO info{};
        TlCall requirements(TlKind::CheckAllocation,{4,0,r.Width,r.Height});
        d.pfnCheckResourceAllocationInfo(device, &r, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
        requirements.end({info.ResourceDataSize,info.ResourceDataAlignment});
        s.status("typed render-target allocation requirements");
        require(info.ResourceDataSize && info.ResourceDataSize <= 16 * 1024 * 1024 &&
            !(info.ResourceDataSize % 65536) && info.ResourceDataAlignment == 65536 &&
            info.Layout == D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0,
            "bounded actual render-target backing includes opaque image metadata");
        bytes = info.ResourceDataSize;
        D3D12DDIARG_CREATEHEAP_0001 h{};
        h.ByteSize = bytes; h.Alignment = info.ResourceDataAlignment; h.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
        h.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE; h.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
        h.CreationNodeMask = h.VisibleNodeMask = 1;
        const auto sizes = d.pfnCalcPrivateHeapAndResourceSizes(device, &h, &r);
        heap_storage.allocate(sizes.Heap, "RT-heap"); resource_storage.allocate(sizes.Resource, "RT-resource");
        const D3D12DDI_HHEAP new_heap{heap_storage.data()};
        const D3D12DDI_HRESOURCE new_resource{resource_storage.data()};
        check(tl_call(TlKind::CreateHeap,[&] { return d.pfnCreateHeapAndResource(device, &h, new_heap, {}, &r, nullptr, new_resource); },
            {4,0,bytes,h.Alignment,sizes.Heap,sizes.Resource}), "actual typed render-target creation");
        heap = new_heap; resource = new_resource; identity(s);
    }
    void destroy(DllSession &s) {
        s.api.device.pfnDestroyHeapAndResource(s.create.hDrvDevice, heap, resource); heap = {}; resource = {};
        s.status("typed render-target destroy");
        require(!s.runtime.has(allocation) && heap_storage.intact() && resource_storage.intact(),
            "exact public render-target allocation retired with canaries intact");
    }
};

struct DllGraphics {
    OpaqueStorage root_storage, vs_storage, ps_storage, blend_storage, raster_storage, depth_storage, pipeline_storage, rtv_storage;
    D3D12DDI_HROOTSIGNATURE root{};
    D3D12DDI_HSHADER vs{}, ps{};
    D3D12DDI_HBLENDSTATE blend{};
    D3D12DDI_HRASTERIZERSTATE raster{};
    D3D12DDI_HDEPTHSTENCILSTATE depth{};
    D3D12DDI_HPIPELINESTATE pipeline{};
    D3D12DDI_HDESCRIPTORHEAP rtv_heap{};
    D3D12DDI_CPU_DESCRIPTOR_HANDLE rtv{};
    void create(DllSession &s, const DllRenderTarget &target) {
        const auto device = s.create.hDrvDevice; const auto &d = s.api.device;
        D3D12DDI_ROOT_SIGNATURE signature{}; D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 args{&signature, 1};
        root_storage.allocate(d.pfnCalcPrivateRootSignatureSize(device, &args), "graphics-root");
        check(d.pfnCreateRootSignature(device, &args, {root_storage.data()}), "typed graphics root signature"); root = {root_storage.data()};
        D3D12DDIARG_SIGNATURE_ENTRY input{}, output{};
        input.SystemValue = static_cast<D3D10_SB_NAME>(6); input.Mask = 1;
        input.RegisterComponentType = static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(1);
        output.SystemValue = static_cast<D3D10_SB_NAME>(1); output.Mask = 15;
        output.RegisterComponentType = static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(3);
        D3D12DDIARG_STAGE_IO_SIGNATURES io{};
        io.NumInputSignatureEntries = io.NumOutputSignatureEntries = 1;
        io.pInputSignatureDeprecated = &input; io.pOutputSignatureDeprecated = &output;
        const auto vertex_tokens = dll_graphics_tokens(vs_flat_color_code_dxbc);
        const auto pixel_tokens = dll_graphics_tokens(ps_flat_color_code_dxbc);
        vs_storage.allocate(d.pfnCalcPrivateShaderSize(device, vertex_tokens.data(), root, &io), "graphics-VS");
        d.pfnCreateVertexShader(device, vertex_tokens.data(), root, {vs_storage.data()}, &io, D3D12DDI_CREATE_SHADER_FLAG_NONE);
        s.status("typed graphics VS publication"); vs = {vs_storage.data()};
        io.NumInputSignatureEntries = 0; io.pInputSignatureDeprecated = nullptr; output.SystemValue = static_cast<D3D10_SB_NAME>(0);
        ps_storage.allocate(d.pfnCalcPrivateShaderSize(device, pixel_tokens.data(), root, &io), "graphics-PS");
        d.pfnCreatePixelShader(device, pixel_tokens.data(), root, {ps_storage.data()}, &io, D3D12DDI_CREATE_SHADER_FLAG_NONE);
        s.status("typed graphics PS publication"); ps = {ps_storage.data()};
        D3D12DDI_BLEND_DESC bs{}; bs.RenderTarget[0].RenderTargetWriteMask = 15;
        D3D12DDI_RASTERIZER_DESC rs{}; rs.FillMode = static_cast<D3D12DDI_FILL_MODE>(3);
        rs.CullMode = static_cast<D3D12DDI_CULL_MODE>(1); rs.ScissorEnable = rs.DepthClipEnable = TRUE;
        D3D12DDI_DEPTH_STENCIL_DESC ds{};
        blend_storage.allocate(d.pfnCalcPrivateBlendStateSize(device, &bs), "graphics-blend");
        raster_storage.allocate(d.pfnCalcPrivateRasterizerStateSize(device, &rs), "graphics-raster");
        depth_storage.allocate(d.pfnCalcPrivateDepthStencilStateSize(device, &ds), "graphics-depth");
        d.pfnCreateBlendState(device, &bs, {blend_storage.data()}); blend = {blend_storage.data()};
        d.pfnCreateRasterizerState(device, &rs, {raster_storage.data()}); raster = {raster_storage.data()};
        d.pfnCreateDepthStencilState(device, &ds, {depth_storage.data()}); depth = {depth_storage.data()};
        s.status("typed graphics fixed states");
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 desc{};
        desc.hRootSignature = root; desc.hVertexShader = vs; desc.hPixelShader = ps;
        desc.hBlendState = blend; desc.hRasterizerState = raster; desc.hDepthStencilState = depth; desc.NodeMask = 1;
        desc.PrimitiveTopologyType = D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = desc.SampleDesc.Count = 1; desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleMask = UINT_MAX;
        pipeline_storage.allocate(d.pfnCalcPrivatePipelineStateSize(device, &desc), "graphics-pipeline");
        desc.hDrvPipelineState = {pipeline_storage.data()};
        check(d.pfnCreatePipelineState(device, &desc), "typed graphics pipeline"); pipeline = desc.hDrvPipelineState;
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap{D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV, 4,
            D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
        rtv_storage.allocate(d.pfnCalcPrivateDescriptorHeapSize(device, &heap), "graphics-RTV-heap");
        check(d.pfnCreateDescriptorHeap(device, &heap, {rtv_storage.data()}), "typed graphics RTV heap"); rtv_heap = {rtv_storage.data()};
        const UINT stride = d.pfnGetDescriptorSizeInBytes(device, D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV);
        rtv = d.pfnGetCPUDescriptorHandleForHeapStart(device, rtv_heap);
        require(stride && UINT64(stride) * 4 <= SIZE_MAX && rtv.ptr && rtv.ptr <= SIZE_MAX - SIZE_T(2) * stride,
            "bounded actual RTV descriptor stride and slot2");
        rtv.ptr += SIZE_T(2) * stride;
        D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view{};
        view.hDrvResource = target.resource; view.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM; view.Tex2D.ArraySize = 1;
        d.pfnCreateRenderTargetView(device, &view, rtv); s.status("typed graphics RTV publication");
    }
    void record(DllSession &s, D3D12DDI_HCOMMANDLIST command, const DllRenderTarget &target,
            const DllBuffer &indices, const DllBuffer &readback, UINT round) {
        require(round < 4, "graphics has exactly four rounds");
        const auto &c = s.api.commands;
        auto transition = [&](D3D12DDI_RESOURCE_STATES before, D3D12DDI_RESOURCE_STATES after) {
            D3D12DDIARG_RESOURCE_BARRIER_0003 barrier{};
            barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.hResource = target.resource;
            barrier.Transition.Subresource = UINT_MAX; barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
            c.pfnResourceBarrier(command, 1, &barrier); s.status("typed render-target transition");
        };
        if (round) transition(D3D12DDI_RESOURCE_STATE_COPY_SOURCE, D3D12DDI_RESOURCE_STATE_RENDER_TARGET);
        const float color[4] = {1, 0, 0, 1};
        const D3D12DDI_VIEWPORT viewport{0, 0, 16, 12, 0, 1};
        const D3D12DDI_RECT scissor{LONG(2 + round), LONG(1 + round), 12, 9};
        c.pfnClearRenderTargetView(command, rtv, color, 0, nullptr);
        c.pfnSetGraphicsRootSignature(command, root); c.pfnSetPipelineState(command, pipeline);
        c.pfnOMSetRenderTargets(command, 1, &rtv, FALSE, nullptr);
        c.pfnIaSetTopology(command, D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c.pfnRsSetViewports(command, 1, &viewport); c.pfnRsSetScissorRects(command, 1, &scissor);
        if (round & 1) {
            const D3D12DDI_INDEX_BUFFER_VIEW ib{indices.address, 10, DXGI_FORMAT_R16_UINT};
            c.pfnIASetIndexBuffer(command, &ib);
            c.pfnDrawIndexedInstanced(command, round == 3 ? 0 : 3, 1, 2, 13, 7);
        } else c.pfnDrawInstanced(command, 3, round == 2 ? 0 : 1, 0, 5);
        s.status("typed graphics draw recording");
        transition(D3D12DDI_RESOURCE_STATE_RENDER_TARGET, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        D3D12DDIARG_BUFFER_PLACEMENT source{}, destination{};
        source.BaseAddress.UMD.hResource = target.resource;
        destination.BaseAddress.UMD.hResource = readback.resource; destination.BaseAddress.UMD.Offset = 512;
        const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint{DXGI_FORMAT_R8G8B8A8_UNORM,16,12,1,256,3072};
        const D3D12DDIARG_PLACED_RESOURCE source_layout{D3D12DDI_RL_SELECT_SUBRESOURCE,nullptr};
        const D3D12DDIARG_PLACED_RESOURCE destination_layout{D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED,&footprint};
        c.pfnCopyTextureRegion(command, &destination, destination_layout, 0, 0, 0, &source, source_layout, nullptr);
        s.status("typed RGBA8 CopyTextureRegion into offset512 pitch256 readback");
    }
    void destroy(DllSession &s) {
        const auto device = s.create.hDrvDevice; const auto &d = s.api.device;
        d.pfnDestroyPipelineState(device, pipeline); pipeline = {};
        d.pfnDestroyDescriptorHeap(device, rtv_heap); rtv_heap = {}; rtv = {};
        d.pfnDestroyShader(device, vs); vs = {}; d.pfnDestroyShader(device, ps); ps = {};
        d.pfnDestroyBlendState(device, blend); blend = {}; d.pfnDestroyRasterizerState(device, raster); raster = {};
        d.pfnDestroyDepthStencilState(device, depth); depth = {}; d.pfnDestroyRootSignature(device, root); root = {};
        s.status("typed graphics object destroy");
        require(root_storage.intact() && vs_storage.intact() && ps_storage.intact() && blend_storage.intact() &&
            raster_storage.intact() && depth_storage.intact() && pipeline_storage.intact() && rtv_storage.intact(),
            "all graphics opaque storage canaries intact");
    }
};

void dll_graphics_slots(DllSession &s) {
    s.required_slots(false);
    const auto &d = s.api.device; const auto &c = s.api.commands;
    require(d.pfnCalcPrivateRootSignatureSize && d.pfnCreateRootSignature && d.pfnDestroyRootSignature &&
        d.pfnCalcPrivateShaderSize && d.pfnCreateVertexShader && d.pfnCreatePixelShader && d.pfnDestroyShader &&
        d.pfnCalcPrivateBlendStateSize && d.pfnCreateBlendState && d.pfnDestroyBlendState &&
        d.pfnCalcPrivateRasterizerStateSize && d.pfnCreateRasterizerState && d.pfnDestroyRasterizerState &&
        d.pfnCalcPrivateDepthStencilStateSize && d.pfnCreateDepthStencilState && d.pfnDestroyDepthStencilState &&
        d.pfnCalcPrivatePipelineStateSize && d.pfnCreatePipelineState && d.pfnDestroyPipelineState &&
        d.pfnCalcPrivateDescriptorHeapSize && d.pfnCreateDescriptorHeap && d.pfnDestroyDescriptorHeap &&
        d.pfnGetDescriptorSizeInBytes && d.pfnGetCPUDescriptorHandleForHeapStart && d.pfnCreateRenderTargetView &&
        c.pfnResourceBarrier && c.pfnCopyTextureRegion && c.pfnClearRenderTargetView && c.pfnSetGraphicsRootSignature &&
        c.pfnSetPipelineState && c.pfnOMSetRenderTargets && c.pfnIaSetTopology && c.pfnRsSetViewports &&
        c.pfnRsSetScissorRects && c.pfnIASetIndexBuffer && c.pfnDrawInstanced && c.pfnDrawIndexedInstanced,
        "all bounded graphics public R0 slots present");
}

void dll_graphics_public_gates(DllSession &s) {
    require(s.adapter.pfnGetCaps && s.adapter.pfnFillDDITable, "native public negotiation callbacks present");
    UINT count = 1; UINT64 version = 0x1122334455667788ull;
    check(s.adapter.pfnGetSupportedVersions(s.open.hAdapter, &count, &version), "graphics public versions check");
    require(!count && version == 0x1122334455667788ull, "graphics leaves public versions empty");
    const D3D12DDI_TABLE_TYPE types[] = {D3D12DDI_TABLE_TYPE_DEVICE_CORE,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D};
    const size_t sizes[] = {sizeof(s.api.device),sizeof(s.api.commands),sizeof(s.api.queues)};
    for (UINT i = 0; i < 3; ++i) {
        std::vector<unsigned char> table(sizes[i],0xa7);
        require(s.adapter.pfnFillDDITable(s.open.hAdapter,types[i],table.data(),table.size(),0,{}) == DXGI_ERROR_UNSUPPORTED &&
            std::all_of(table.begin(),table.end(),[](unsigned char b) { return b == 0xa7; }),
            "graphics public table publication remains rejected and untouched");
    }
    UINT data = 0x87654321;
    D3D12DDIARG_GETCAPS caps{}; caps.Type = static_cast<D3D12DDICAPS_TYPE>(0x7fffffff); caps.pData = &data; caps.DataSize = sizeof(data);
    require(s.adapter.pfnGetCaps(s.open.hAdapter,&caps) == DXGI_ERROR_UNSUPPORTED && data == 0x87654321,
        "graphics does not open unsupported caps");
    std::puts("PASS DLL GRAPHICS public_gates versions=0 tables_rejected=3 unsupported_caps_rejected=1");
}

struct DllGraphicsWorkload {
    DllSession session;
    DllRenderTarget target;
    DllBuffer indices, readback;
    DllRecording recording;
    DllGraphics graphics;
    std::array<DllBatch,4> batches;
    void require_quiescent() const {
        for (const auto &batch : batches)
            if (batch.gate.context) require(batch.gate.release_allowed() && !batch.queue.pDrvPrivate && !batch.event,
                "graphics has no live batch before reset/map/release");
    }
    void run(LUID luid) {
        auto &s = session; auto &r = s.runtime;
        dll_graphics_slots(s); s.initialize(luid); dll_graphics_public_gates(s);
        target.create(s); indices.create(s, 0xac3, D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK);
        readback.create(s, 0x400, D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK);
        require(target.allocation != indices.allocation && target.allocation != readback.allocation && indices.allocation != readback.allocation,
            "three distinct exact public graphics allocations");
        r.target_handle = readback.allocation;
        UINT *words = indices.map(s); std::fill(words,words + 16384,0x99999999u);
        auto *values = reinterpret_cast<uint16_t *>(words); values[0] = values[1] = 99; values[2] = 0; values[3] = 1; values[4] = 2;
        indices.unmap(s); recording.create(s); graphics.create(s,target);
        for (UINT round = 0; round < batches.size(); ++round) {
            require_quiescent();
            words = readback.map(s); std::fill(words,words + 16384,0x71710000u ^ round); readback.unmap(s);
            if (round) recording.reopen(s);
            graphics.record(s,recording.command,target,indices,readback,round);
            s.api.commands.pfnCloseCommandList(recording.command); s.status("typed graphics command close");
            auto &batch = batches[round]; batch.create(s); batch.execute(s,recording,round);
            require(batch.gate.readback_allowed(), "graphics readback requires actual event and typed drain");
            words = readback.map(s); const UINT mismatches = dll_graphics_verify(words,round,true); readback.unmap(s);
            require(!mismatches, "actual independent DLL graphics pixels and complete64KiB sentinel oracle");
            target.identity(s); readback.identity(s,D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK);
            std::printf("PASS DLL GRAPHICS batch=%u draw=%s checked_words=16384 pixels=192 sentinel=16192 queue_recreated=1 typed_drain_before_map=1\n",
                round,round == 0 ? "direct" : round == 1 ? "indexed" : round == 2 ? "zero_instance" : "zero_index");
        }
        cleanup();
    }
    void cleanup() {
        auto &s = session; auto &r = s.runtime;
        require_quiescent(); check(r.observe_completions(0), "graphics terminal completion observation");
        dll_graphics_public_gates(s); r.retirement_sealed = true; const auto terminal_renders = r.renders.load();
        recording.destroy(s); graphics.destroy(s); target.destroy(s); readback.destroy(s); indices.destroy(s);
        require(r.pageable_deallocations == 3, "graphics exact three public Deallocate2 flags3 calls");
        s.status("all graphics objects released after typed drains");
        r.native_cleanup = true; s.adapter.pfnDestroyDevice(s.create.hDrvDevice); s.device_live = false;
        check(s.adapter.pfnCloseAdapter(s.open.hAdapter), "graphics typed CloseAdapter"); s.open.hAdapter = {};
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            require(!r.context_handle && r.contexts.empty() && r.allocations.empty() && !r.callback_exceptions &&
                !r.command_errors && !r.device_errors && r.renders == terminal_renders,
                "graphics native destroy empties ledgers without late Render or errors");
            for (const auto &event : r.completions) require(!event.event, "graphics has no outstanding observer event");
        }
        require(s.device_storage.intact(), "graphics retired device canaries intact");
        check(r.close(), "graphics real provider teardown after all DLL children");
        require(!r.device && !r.adapter && !r.paging_queue && r.resident_allocations.empty(), "graphics provider ownership closed");
        s.api.unload();
        std::printf("PASS DLL GRAPHICS cleanup batches=4 public_deallocate2=%u internal_deallocate=%u callbacks=0 contexts=0 allocations=0 late_renders=0 core_reference_released=1\n",
            r.pageable_deallocations.load(),r.internal_deallocations.load());
        std::puts("BOUNDARY actual standalone DLL + handwritten runtime + real KMT/GPU; offscreen RGBA8 only; no Present/SYSTEM/feature-level/ordinary D3D12CreateDevice acceptance");
    }
};

void run_dll_graphics_workload(const wchar_t *core, const wchar_t *core_hash, const wchar_t *manifest, const wchar_t *icd_hash, LUID luid) {
    Deadline deadline; timeline.begin(); TlCall run_call(TlKind::Run,{2,tl_luid(luid)});
    // Failure deliberately retains all callback/provider/module/opaque owners
    // until process exit, identical to the preserved copy/compute containment.
    auto *workload = new DllGraphicsWorkload;
    try {
        tl_void(TlKind::CoreLoad,[&] { workload->session.api.load(core,core_hash); });
        require(workload->session.api.file.hash == "9f1440067d619234b65c1ce8d211d82484aef26e8200430e62b9161e206ee0b1",
            "graphics targets exact caps02 ARM64 DLL");
        tl_void(TlKind::IcdConfigure,[&] { workload->session.icd.configure(manifest,icd_hash); });
        workload->run(luid); delete workload;
        run_call.end({0}); timeline.emit(0,TlKind::Terminal,0,{0,0}); timeline.finish(0,0);
    } catch (...) {
        timeline.emit(0,TlKind::Retain,0,{1}); run_call.end({1}); timeline.emit(0,TlKind::Terminal,0,{1,1}); timeline.finish(1,1);
        std::puts("FAIL retained graphics session: no core unload or opaque/provider free on incomplete or failed drain; process exit owns final containment");
        throw;
    }
}

/* New-mode CPU controls call the same recording/oracle functions. All typed
 * peers below are local; no DLL is loaded and no KMT/Vulkan API is invoked. */
struct DllGraphicsRecordFixture {
    DllSession session;
    DllRenderTarget target;
    DllBuffer indices, readback;
    DllGraphics graphics;
    std::string trace;
    UINT round = 0, barriers = 0;
    static DllGraphicsRecordFixture &from(D3D12DDI_HCOMMANDLIST c) { return *static_cast<DllGraphicsRecordFixture *>(c.pDrvPrivate); }
    static HRESULT APIENTRY status(D3D12DDI_HDEVICE) { return S_OK; }
    static void APIENTRY barrier(D3D12DDI_HCOMMANDLIST c, UINT n, const D3D12DDIARG_RESOURCE_BARRIER_0003 *b) {
        auto &f = from(c);
        const bool initial = f.round && !f.barriers;
        require(n == 1 && b && b->Type == D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION && !b->Flags &&
            b->Transition.hResource.pDrvPrivate == &f.target && b->Transition.Subresource == UINT_MAX &&
            b->Transition.StateBefore == (initial ? D3D12DDI_RESOURCE_STATE_COPY_SOURCE : D3D12DDI_RESOURCE_STATE_RENDER_TARGET) &&
            b->Transition.StateAfter == (initial ? D3D12DDI_RESOURCE_STATE_RENDER_TARGET : D3D12DDI_RESOURCE_STATE_COPY_SOURCE),
            "graphics CPU typed barrier resource/state/flags/subresource");
        ++f.barriers; f.trace += 'B';
    }
    static void APIENTRY clear(D3D12DDI_HCOMMANDLIST c, D3D12DDI_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT *color, UINT n, const D3D12DDI_RECT *rects) {
        auto &f = from(c);
        require(rtv.ptr == f.graphics.rtv.ptr && color && color[0] == 1 && !color[1] && !color[2] && color[3] == 1 && !n && !rects,
            "graphics CPU clear red whole RTV"); f.trace += 'C';
    }
    static void APIENTRY root(D3D12DDI_HCOMMANDLIST c, D3D12DDI_HROOTSIGNATURE h) {
        auto &f = from(c); require(h.pDrvPrivate == &f.graphics.root, "graphics CPU root handle"); f.trace += 'R';
    }
    static void APIENTRY pipeline(D3D12DDI_HCOMMANDLIST c, D3D12DDI_HPIPELINESTATE h) {
        auto &f = from(c); require(h.pDrvPrivate == &f.graphics.pipeline, "graphics CPU pipeline handle"); f.trace += 'P';
    }
    static void APIENTRY targets(D3D12DDI_HCOMMANDLIST c, UINT n, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *rtv, BOOL range,
            const D3D12DDI_CPU_DESCRIPTOR_HANDLE *dsv) {
        auto &f = from(c); require(n == 1 && rtv && rtv->ptr == f.graphics.rtv.ptr && !range && !dsv,
            "graphics CPU exact RTV binding"); f.trace += 'T';
    }
    static void APIENTRY topology(D3D12DDI_HCOMMANDLIST c, D3D12DDI_PRIMITIVE_TOPOLOGY t) {
        require(t == D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST, "graphics CPU triangle topology"); from(c).trace += 'O';
    }
    static void APIENTRY viewport(D3D12DDI_HCOMMANDLIST c, UINT n, const D3D12DDI_VIEWPORT *v) {
        require(n == 1 && v && !v->TopLeftX && !v->TopLeftY && v->Width == 16 && v->Height == 12 && !v->MinDepth && v->MaxDepth == 1,
            "graphics CPU full viewport"); from(c).trace += 'V';
    }
    static void APIENTRY scissor(D3D12DDI_HCOMMANDLIST c, UINT n, const D3D12DDI_RECT *r) {
        auto &f = from(c); require(n == 1 && r && r->left == LONG(2 + f.round) && r->top == LONG(1 + f.round) && r->right == 12 && r->bottom == 9,
            "graphics CPU round scissor"); f.trace += 'S';
    }
    static void APIENTRY index(D3D12DDI_HCOMMANDLIST c, const D3D12DDI_INDEX_BUFFER_VIEW *v) {
        auto &f = from(c); require(v && v->BufferLocation == f.indices.address && v->SizeInBytes == 10 && v->Format == DXGI_FORMAT_R16_UINT,
            "graphics CPU queried index VA/format/range"); f.trace += 'I';
    }
    static void APIENTRY draw(D3D12DDI_HCOMMANDLIST c, UINT vertices, UINT instances, UINT first, UINT first_instance) {
        auto &f = from(c); require(!(f.round & 1) && vertices == 3 && instances == (f.round == 2 ? 0u : 1u) && !first && first_instance == 5,
            "graphics CPU direct/zero-instance draw arguments"); f.trace += 'D';
    }
    static void APIENTRY draw_indexed(D3D12DDI_HCOMMANDLIST c, UINT indices, UINT instances, UINT first, INT base, UINT first_instance) {
        auto &f = from(c); require((f.round & 1) && indices == (f.round == 3 ? 0u : 3u) && instances == 1 && first == 2 && base == 13 && first_instance == 7,
            "graphics CPU indexed/zero-index draw arguments"); f.trace += 'J';
    }
    static void APIENTRY copy(D3D12DDI_HCOMMANDLIST c, const D3D12DDIARG_BUFFER_PLACEMENT *dst, D3D12DDIARG_PLACED_RESOURCE dl,
            UINT x, UINT y, UINT z, const D3D12DDIARG_BUFFER_PLACEMENT *src, D3D12DDIARG_PLACED_RESOURCE sl, const D3D12DDI_BOX *box) {
        auto &f = from(c);
        require(dst && src && dst->BaseAddress.UMD.hResource.pDrvPrivate == &f.readback && dst->BaseAddress.UMD.Offset == 512 &&
            src->BaseAddress.UMD.hResource.pDrvPrivate == &f.target && !src->BaseAddress.UMD.Offset && !x && !y && !z && !box &&
            sl.Layout == D3D12DDI_RL_SELECT_SUBRESOURCE && !sl.pLayout &&
            dl.Layout == D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED && dl.pLayout,
            "graphics CPU WDK typed texture copy placement and selected subresource");
        const auto &p = *static_cast<const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *>(dl.pLayout);
        require(p.Format == DXGI_FORMAT_R8G8B8A8_UNORM && p.PhysicalWidth == 16 && p.PhysicalHeight == 12 && p.PhysicalDepth == 1 &&
            p.Pitch == 256 && p.SlicePitch == 3072, "graphics CPU exact RGBA8 footprint and padding"); f.trace += 'X';
    }
    DllGraphicsRecordFixture() {
        session.device_live = true; session.create.hDrvDevice = {this}; session.api.status = status;
        target.resource = {&target}; readback.resource = {&readback}; indices.address = 0x45002a0000ull;
        graphics.root = {&graphics.root}; graphics.pipeline = {&graphics.pipeline}; graphics.rtv.ptr = 0x12340;
        auto &c = session.api.commands;
        c.pfnResourceBarrier = barrier; c.pfnClearRenderTargetView = clear; c.pfnSetGraphicsRootSignature = root;
        c.pfnSetPipelineState = pipeline; c.pfnOMSetRenderTargets = targets; c.pfnIaSetTopology = topology;
        c.pfnRsSetViewports = viewport; c.pfnRsSetScissorRects = scissor; c.pfnIASetIndexBuffer = index;
        c.pfnDrawInstanced = draw; c.pfnDrawIndexedInstanced = draw_indexed; c.pfnCopyTextureRegion = copy;
    }
    void run() {
        for (round = 0; round < 4; ++round) {
            trace.clear(); barriers = 0; graphics.record(session,{this},target,indices,readback,round);
            const std::string expected = std::string(round ? "B" : "") + "CRPTOVS" + (round & 1 ? "IJ" : "D") + "BX";
            require(trace == expected && barriers == (round ? 2u : 1u), "graphics CPU actual recording order");
        }
    }
};

void dll_graphics_controls() {
    const auto vs = dll_graphics_tokens(vs_flat_color_code_dxbc), ps = dll_graphics_tokens(ps_flat_color_code_dxbc);
    require(vs.size() == 55 && vs[0] == 0x00010050 && ps.size() == 15 && ps[0] == 0x00000050,
        "graphics CPU frozen SM5 VS/PS token extraction");
    DWORD broken[sizeof(vs_flat_color_code_dxbc)/sizeof(DWORD)];
    unsigned shader_rejections = 0;
    for (UINT mutation = 0; mutation < 4; ++mutation) {
        std::copy(std::begin(vs_flat_color_code_dxbc),std::end(vs_flat_color_code_dxbc),broken);
        if (!mutation) broken[0] = 0;
        else if (mutation == 1) broken[8] = UINT_MAX;
        else if (mutation == 2) broken[12] = UINT_MAX;
        else broken[40] = 1;
        try { dll_graphics_tokens(broken); } catch (const ProbeFailure &) { ++shader_rejections; }
    }
    require(shader_rejections == 4, "graphics CPU rejects malformed shader headers/chunks/tokens");
    std::array<UINT,16384> words{};
    UINT oracle_rejections = 0;
    for (UINT round = 0; round < 4; ++round) {
        const UINT sentinel = 0x71710000u ^ round;
        words.fill(sentinel);
        require(dll_graphics_verify(words.data(),round,false) == 192, "graphics CPU rejects absent texture copy"); ++oracle_rejections;
        for (UINT y = 0; y < 12; ++y) for (UINT x = 0; x < 16; ++x)
            words[128 + y*64 + x] = round < 2 && x >= 2 + round && x < 12 && y >= 1 + round && y < 9 ? 0xffffffffu : 0xff0000ffu;
        require(!dll_graphics_verify(words.data(),round,false), "graphics CPU accepts reference image and all padding");
        UINT white = 0, red = 0, padding = 0;
        for (UINT word : words) { white += word == 0xffffffffu; red += word == 0xff0000ffu; padding += word == sentinel; }
        require(white == (round == 0 ? 80u : round == 1 ? 63u : 0u) && white + red == 192 && padding == 16192,
            "graphics CPU independent coverage/scissor/zero-draw counts");
        for (UINT at : {0u,127u,128u,144u,847u,848u,896u,16383u}) {
            words[at] ^= 1; require(dll_graphics_verify(words.data(),round,false) == 1, "graphics CPU detects pixel/prefix/row-padding/tail corruption");
            words[at] ^= 1; ++oracle_rejections;
        }
        words[128 + 2*64 + 4] ^= 0x00ffff00u;
        require(dll_graphics_verify(words.data(),round,false) == 1, "graphics CPU detects missing or unexpected white draw"); ++oracle_rejections;
    }
    DllGraphicsRecordFixture recording; recording.run();
    std::printf("PASS GRAPHICS CPU rounds=4 checked_words=65536 oracle_rejections=%u shader_rejections=%u typed_recording_rounds=4 no_DLL_KMT_Vulkan=1\n",
        oracle_rejections,shader_rejections);
}

/* Only the new identity-specific CPU mode enters this fixture. It calls the
 * actual DllRenderTarget::identity through typed local peers, never DLL/KMT/Vulkan. */
struct DllGraphicsIdentityFixture {
    DllSession session;
    DllRenderTarget target;
    D3D12DDI_RESOURCE_ALLOCATION_INFO info{};
    D3DKMT_HANDLE queried = 17, checked = 17;
    UINT64 public_va = 0;
    static DllGraphicsIdentityFixture &from(D3D12DDI_HDEVICE h) {
        return *static_cast<DllGraphicsIdentityFixture *>(h.pDrvPrivate);
    }
    static HRESULT APIENTRY status(D3D12DDI_HDEVICE) { return S_OK; }
    static void APIENTRY existing(D3D12DDI_HDEVICE h, D3D12DDI_HRESOURCE,
            D3D12DDI_RESOURCE_ALLOCATION_INFO *out, D3DKMT_HANDLE *allocation) {
        auto &f = from(h); *out = f.info; *allocation = f.queried;
    }
    static D3DKMT_HANDLE APIENTRY handle(D3D12DDI_HDEVICE h, D3D10DDI_HRESOURCE) { return from(h).checked; }
    static D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY address(D3D12DDI_HDEVICE h, D3D12DDI_HRESOURCE) { return from(h).public_va; }
    DllGraphicsIdentityFixture() {
        session.device_live = true; session.create.hDrvDevice = {this}; session.api.status = status;
        session.api.device.pfnCheckExistingResourceAllocationInfo = existing;
        session.api.device.pfnCheckResourceAllocationHandle = handle;
        session.api.device.pfnCheckResourceVirtualAddress = address;
        session.runtime.context_id = 13; session.runtime.identity.generation = 2;
        target.bytes = info.ResourceDataSize = 65536; info.ResourceDataAlignment = 65536;
        info.Layout = D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0;
        target.heap_storage.allocate(64,"CPU-RT-heap"); target.resource_storage.allocate(64,"CPU-RT-resource");
        target.resource = {target.resource_storage.data()};
        ProbeWireAllocationInfo record{}; record.size = 65536; record.iova = 0x4500010000ull;
        record.context_id = 13; record.generation = 2; session.runtime.allocations[queried] = record;
    }
    ProbeWireAllocationInfo &ledger() { return session.runtime.allocations.at(queried); }
    void run() { target.identity(session); }
};

template<typename Mutation> void dll_graphics_identity_reject(const char *name, Mutation mutation, bool seeded = false) {
    DllGraphicsIdentityFixture f;
    if (seeded) f.run();
    mutation(f);
    bool rejected = false;
    try { f.run(); } catch (const ProbeFailure &) { rejected = true; }
    require(rejected, "graphics identity CPU rejects each mutated contract");
    std::printf("CONTROL_REJECT GRAPHICS_IDENTITY case=%s\n",name);
}

void dll_graphics_identity_controls() {
    DllGraphicsIdentityFixture positive;
    positive.run(); positive.run();
    require(positive.target.allocation == 17 && positive.target.backing_iova == 0x4500010000ull && !positive.public_va,
        "graphics identity CPU keeps public zero and stable nonzero backing separate");
    DllGraphicsIdentityFixture different_backing;
    different_backing.ledger().iova += 65536; different_backing.run();
    require(different_backing.target.backing_iova != positive.target.backing_iova && !different_backing.public_va,
        "graphics identity CPU accepts independent valid allocation IOVAs");
    dll_graphics_identity_reject("nonzero_public_va",[](auto &f){f.public_va = f.ledger().iova;});
    dll_graphics_identity_reject("missing_queried_handle",[](auto &f){f.queried = 0;});
    dll_graphics_identity_reject("mismatched_checked_handle",[](auto &f){++f.checked;});
    dll_graphics_identity_reject("typed_size",[](auto &f){f.info.ResourceDataSize += 65536;});
    dll_graphics_identity_reject("typed_alignment",[](auto &f){f.info.ResourceDataAlignment = 4096;});
    dll_graphics_identity_reject("typed_layout",[](auto &f){f.info.Layout = D3D12DDI_TL_ROW_MAJOR;});
    dll_graphics_identity_reject("missing_ledger",[](auto &f){f.session.runtime.allocations.clear();});
    dll_graphics_identity_reject("ledger_size",[](auto &f){f.ledger().size += 65536;});
    dll_graphics_identity_reject("zero_backing_iova",[](auto &f){f.ledger().iova = 0;});
    dll_graphics_identity_reject("ledger_context",[](auto &f){++f.ledger().context_id;});
    dll_graphics_identity_reject("ledger_generation",[](auto &f){++f.ledger().generation;});
    dll_graphics_identity_reject("current_context",[](auto &f){++f.session.runtime.context_id;});
    dll_graphics_identity_reject("current_generation",[](auto &f){++f.session.runtime.identity.generation;});
    dll_graphics_identity_reject("changed_backing_iova",[](auto &f){f.ledger().iova += 65536;},true);
    dll_graphics_identity_reject("changed_allocation_handle",[](auto &f){auto record=f.ledger();++f.queried;f.checked=f.queried;f.session.runtime.allocations[f.queried]=record;},true);
    dll_graphics_identity_reject("heap_canary",[](auto &f){f.target.heap_storage.base[0] ^= 1;});
    dll_graphics_identity_reject("resource_canary",[](auto &f){f.target.resource_storage.base[0] ^= 1;});
    dll_graphics_identity_reject("device_canary",[](auto &f){f.session.device_storage.allocate(64,"CPU-device");f.session.device_storage.base[0] ^= 1;});
    dll_graphics_identity_reject("command_error",[](auto &f){++f.session.runtime.command_errors;});
    dll_graphics_identity_reject("device_error",[](auto &f){++f.session.runtime.device_errors;});
    dll_graphics_identity_reject("callback_exception",[](auto &f){++f.session.runtime.callback_exceptions;});
    std::puts("PASS GRAPHICS IDENTITY CPU positive_calls=3 rejected_cases=21 actual_typed_identity=1 public_texture_va=0 separate_stable_backing_iova=1 no_DLL_KMT_Vulkan=1");
}
