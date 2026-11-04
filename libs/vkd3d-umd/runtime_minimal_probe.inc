/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Bounded actual native DDI -> embedded Vulkan -> KMT copy probe. Handwritten
 * runtime callbacks are explicit. No Microsoft D3D12 admission or presentation. */
struct MinimalBuffer {
    NativeSession &native;
    NativeHeapSlot heap_slot{};
    Object resource_slot{};
    D3D12DDI_HHEAP heap{};
    D3D12DDI_HRESOURCE resource{};
    explicit MinimalBuffer(NativeSession &n) : native(n) {}
    ~MinimalBuffer() { reset(); }
    void reset() {
        if (heap.pDrvPrivate || resource.pDrvPrivate) {
            native.table.pfnDestroyHeapAndResource(native.create.hDrvDevice, heap, resource);
            heap = {}; resource = {};
        }
    }
    void create(UINT state, D3D12DDI_CPU_PAGE_PROPERTY cpu_page = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK) {
        D3D12DDIARG_CREATERESOURCE_0003 r{};
        r.ResourceType = D3D12DDI_RT_BUFFER; r.Width = 65536;
        r.Height = r.DepthOrArraySize = r.MipLevels = r.SampleDesc.Count = 1;
        r.Layout = D3D12DDI_TL_ROW_MAJOR; r.InitialResourceState = static_cast<D3D12DDI_RESOURCE_STATES>(state);
        D3D12DDI_RESOURCE_ALLOCATION_INFO info{};
        native.table.pfnCheckResourceAllocationInfo(native.create.hDrvDevice, &r,
            D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native buffer allocation-info");
        require(info.ResourceDataSize == 65536 && info.ResourceDataAlignment == 65536,
            "native buffer exact allocation requirements");
        D3D12DDIARG_CREATEHEAP_0001 h{};
        h.ByteSize = info.ResourceDataSize; h.Alignment = info.ResourceDataAlignment;
        h.MemoryPool = D3D12DDI_MEMORY_POOL_L0; h.CPUPageProperty = cpu_page;
        h.Flags = D3D12DDI_HEAP_FLAG_BUFFERS; h.CreationNodeMask = h.VisibleNodeMask = 1;
        check(native.table.pfnCreateHeapAndResource(native.create.hDrvDevice, &h, {&heap_slot}, {}, &r,
            nullptr, {&resource_slot}), "native buffer allocation/import");
        heap = {&heap_slot}; resource = {&resource_slot};
    }
};
struct MinimalRecording {
    NativeSession &native;
    Object allocator{}, command{};
    bool allocator_live = false, command_live = false;
    explicit MinimalRecording(NativeSession &n) : native(n) {}
    ~MinimalRecording() { reset(); }
    void create(ProbeRuntime &runtime) {
        D3D12DDIARG_CREATECOMMANDALLOCATOR a{};
        a.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; a.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        a.hDrvCommandAllocator = {&allocator};
        check(native.table.pfnCreateCommandAllocator(native.create.hDrvDevice, &a), "native allocator");
        allocator_live = true;
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 c{};
        c.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; c.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        c.NodeMask = 1; c.hDrvCommandAllocator = {&allocator}; c.hDrvCommandList = {&command};
        c.hRTCommandList.handle = &runtime;
        check(native.table.pfnCreateCommandList(native.create.hDrvDevice, &c), "native command list");
        command_live = true;
    }
    void reopen() {
        native.table.pfnResetCommandAllocator({&allocator});
        D3D12DDIARG_RESETCOMMANDLIST r{}; r.hDrvCommandAllocator = {&allocator};
        native.commands.pfnResetCommandList({&command}, &r);
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native recording reset");
    }
    void reset() {
        if (command_live) { native.table.pfnDestroyCommandList(native.create.hDrvDevice, {&command}); command_live = false; }
        if (allocator_live) { native.table.pfnDestroyCommandAllocator(native.create.hDrvDevice, {&allocator}); allocator_live = false; }
    }
};
struct MinimalFence {
    NativeSession &native;
    NativeFenceSlot slot{};
    bool live = false;
    explicit MinimalFence(NativeSession &n) : native(n) {}
    ~MinimalFence() { reset(); }
    void create() {
        D3D12DDI_FENCE f{}; f.Flags = D3D12DDI_FENCE_FLAG_BOTTOM_OF_PIPE;
        D3D12DDIARG_CREATE_FENCE args{1, &f};
        check(native.table.pfnCreateFence(native.create.hDrvDevice, {&slot}, &args), "native software-fence metadata");
        live = true;
    }
    void reset() {
        if (live) { native.table.pfnDestroyFence(native.create.hDrvDevice, {&slot}); live = false; }
    }
};
void run_minimal_native_probe(LUID luid) {
    Deadline deadline;
    ProbeRuntime runtime; runtime.open(luid);
    NativeSession native; native.initialize(runtime);
    auto *ctx = context(native.create.hDrvDevice);
    require(ctx && ctx->native_context_id == runtime.context_id, "native allocation domain identity");
    const auto generation = ctx->native_adapter->generation;
    MinimalBuffer upload(native), readback(native);
    upload.create(0xac3); readback.create(0x400);
    auto *target = native_heap_find(ctx, readback.heap.pDrvPrivate);
    require(target && target->allocation && readback.resource_slot.address == target->address,
        "readback is exact native heap backing");
    const auto target_handle = target->allocation;
    runtime.target_handle = target_handle;
    Owned alias;
    const auto allocations = runtime.creates.load();
    check(vkdu_memory_heap_import(ctx->backend, ctx, target, target->bytes, 1, &alias.value), "native readback alias");
    require(runtime.creates == allocations, "duplicate import does not allocate replacement backing");
    NativeProbeQueue first(native, runtime), second(native, runtime);
    first.create(); second.create();
    require(first.runtime.context != second.runtime.context && first.runtime.context != runtime.context_handle &&
        second.runtime.context != runtime.context_handle, "two independent runtime-associated scheduler contexts");
    MinimalRecording recording(native); recording.create(runtime);
    MinimalFence fence(native); fence.create();
    std::printf("MINIMAL identity LUID=%08x:%08x generation=%llu owner=%u queue0=%u queue1=%u domain=%u allocation=%u\n",
        static_cast<UINT>(luid.HighPart), luid.LowPart, static_cast<unsigned long long>(generation),
        runtime.context_handle, first.runtime.context, second.runtime.context, runtime.context_id, target_handle);
    for (UINT round = 0; round < 8; ++round) {
        auto &selected = round & 1 ? second : first;
        auto &other = round & 1 ? first : second;
        const UINT sentinel = 0xfedc0000u ^ (round * 0x01010101u), first_word = 64 + round * 32;
        void *mapping = nullptr;
        check(native.table.pfnMapHeap(native.create.hDrvDevice, readback.heap, &mapping), "native map destination sentinel");
        auto *words = static_cast<UINT *>(mapping);
        for (UINT i = 0; i < 16384; ++i) words[i] = sentinel;
        native.table.pfnUnmapHeap(native.create.hDrvDevice, readback.heap);
        check(native.table.pfnMapHeap(native.create.hDrvDevice, upload.heap, &mapping), "native map source pattern");
        words = static_cast<UINT *>(mapping);
        for (UINT i = 0; i < 16384; ++i) words[i] = 0xdeadbeefu;
        for (UINT i = 0; i < 1024; ++i) words[32 + i] = (round + 1) * 0x1020304u ^ (i * 0x9e3779b9u);
        native.table.pfnUnmapHeap(native.create.hDrvDevice, upload.heap);
        if (round) recording.reopen();
        D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
        dst.BaseAddress.UMD.hResource = readback.resource; dst.BaseAddress.UMD.Offset = first_word * 4;
        src.BaseAddress.UMD.hResource = upload.resource; src.BaseAddress.UMD.Offset = 128;
        native.commands.pfnCopyBufferRegion({&recording.command}, dst, src, 4096);
        native.commands.pfnCloseCommandList({&recording.command});
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native copy/close");
        require(!runtime.command_errors, "no native command-list callback errors");
        const auto selected_before = runtime.counts(selected.runtime.context);
        const auto other_before = runtime.counts(other.runtime.context);
        const auto owner_before = runtime.counts(runtime.context_handle);
        const D3D12DDI_HCOMMANDLIST list{&recording.command};
        native.queues.pfnExecuteCommandLists({&selected.slot}, 1, &list);
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native ExecuteCommandLists");
        require(runtime.counts(selected.runtime.context).targets > selected_before.targets,
            "native Execute returns after target DMA is queued on selected scheduler");
        D3D12DDIARG_FENCE_OPERATION operation{{&fence.slot}, round + 1, 0};
        native.queues.pfnSignalFence({&selected.slot}, &operation);
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native SignalFence metadata");
        require(operation.PhysicalAdapterMask == 1 && operation.Value == round + 1,
            "native fence selects one physical node without inventing completion");
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(event != nullptr, "external runtime completion event");
        struct CloseEvent { HANDLE event; ~CloseEvent() { CloseHandle(event); } } event_owner{event};
        D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 signal{};
        signal.hContext = runtime_context(selected.runtime.context);
        signal.Flags.EnqueueCpuEvent = 1; signal.CpuEventHandle = event;
        check(probe_signal(&runtime, &signal), "real ordered OS event after native Execute/SignalFence");
        require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "real selected-context GPU completion event within10s");
        UINT completed = 0;
        check(native_runtime_completed(ctx, &completed), "retire actual OS completion prefix");
        require(!ctx->native_submissions && completed == ctx->native_last_submitted,
            "all accepted native submits really completed before CPU readback");
        check(runtime.observe_completions(0), "observe all actual GPU completion events");
        const auto after = runtime.counts(selected.runtime.context);
        require(after.events > selected_before.events && after.observed == after.events &&
            runtime.counts(other.runtime.context).targets == other_before.targets &&
            runtime.counts(runtime.context_handle).targets == owner_before.targets,
            "copy and completed events belong only to selected queue");
        check(native.table.pfnMapHeap(native.create.hDrvDevice, readback.heap, &mapping), "native map completed GPU readback");
        words = static_cast<UINT *>(mapping);
        for (UINT i = 0; i < 16384; ++i) {
            const UINT expected = i >= first_word && i < first_word + 1024 ?
                (round + 1) * 0x1020304u ^ ((i - first_word) * 0x9e3779b9u) : sentinel;
            if (words[i] != expected) {
                std::fprintf(stderr, "FAIL minimal round%u word%u actual=%08x expected=%08x\n", round, i, words[i], expected);
                native.table.pfnUnmapHeap(native.create.hDrvDevice, readback.heap);
                throw ProbeFailure("actual GPU copy readback");
            }
        }
        native.table.pfnUnmapHeap(native.create.hDrvDevice, readback.heap);
        check(native_heap_generation(ctx), "unchanged adapter generation");
        std::printf("PASS MINIMAL round=%u queue=%u renders=%u events=%u observed=%u completed=%u changed=1024 sentinel=15360\n",
            round, selected.runtime.context, after.renders, after.events, after.observed, completed);
    }
    /* The paired ICD still waits the entire context on imported BO retirement.
     * Stop every producer and prove the full completed prefix before releasing
     * any imported owner. This bounded test does not validate independent BO
     * retirement while unrelated later work is in flight. */
    check(runtime.observe_completions(0), "terminal completion before release");
    require(!ctx->native_submissions && ctx->native_os_completed == ctx->native_last_submitted,
        "terminal complete prefix, no later queue dependencies");
    runtime.retirement_sealed = true;
    const auto terminal_renders = runtime.renders.load();
    recording.reset(); fence.reset(); first.reset(); second.reset();
    require(!first.runtime.context && !second.runtime.context, "both completed queue contexts closed");
    native.table.pfnDestroyHeapAndResource(native.create.hDrvDevice, readback.heap, {}); readback.heap = {};
    require(runtime.has(target_handle), "resource holds native allocation after heap slot closes");
    native.table.pfnDestroyHeapAndResource(native.create.hDrvDevice, {}, readback.resource); readback.resource = {};
    require(runtime.has(target_handle), "duplicate imported owner retains native allocation");
    alias.reset();
    require(!runtime.has(target_handle), "last imported readback owner releases exact allocation");
    upload.reset();
    require(runtime.pageable_deallocations == 2, "exactly two public buffers close through Deallocate2 flags3");
    check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native clean release status");
    runtime.native_cleanup = true; native.finish();
    require(!runtime.context_handle && runtime.contexts.empty() && runtime.allocations.empty() &&
        !runtime.device_errors && !runtime.command_errors && runtime.renders == terminal_renders,
        "terminal cleanup releases all contexts/allocations without later Render or errors");
    check(runtime.close(), "final KMT device/adapter close");
    std::printf("PASS MINIMAL cleanup public_deallocate2=%u internal_deallocate=%u contexts=0 allocations=0 later_renders=0\n",
        runtime.pageable_deallocations.load(), runtime.internal_deallocations.load());
    std::puts("BOUNDARY real native DDI/KMT/GPU copy only, handwritten runtime callbacks; SYSTEM/PRIMARY closed; no monitored-fence/Present or independent BO retirement claim");
}
