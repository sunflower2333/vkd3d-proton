/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Only typed R0 calls into the independently loaded image. All storage and
 * callback owners live in one heap-owned session until safe explicit cleanup. */
#include "../../tests/shaders/command/headers/execute_indirect_cs.h"

struct DllSession {
    DllApi api;
    IcdPin icd;
    ProbeRuntime runtime;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    D3DDDI_ADAPTERCALLBACKS adapter_callbacks{};
    D3DDDI_DEVICECALLBACKS kernel_callbacks{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 runtime_callbacks{};
    OpaqueStorage device_storage;
    bool device_live = false;
    void status(const char *label) {
        require(device_live, "status requires published native device");
        check(tl_call(TlKind::Status,[&] { return api.status(create.hDrvDevice); }), label);
        require(!runtime.command_errors && !runtime.device_errors && !runtime.callback_exceptions,
            "zero command/device/callback exception faults");
        require(device_storage.intact(), "opaque device canaries intact");
    }
    void initialize(LUID luid) {
        tl_void(TlKind::RuntimeOpen,[&] { runtime.open(luid); },{tl_luid(luid)});
        adapter_callbacks.pfnQueryAdapterInfoCb = safe_query;
        open.hRTAdapter.handle = &runtime; open.pAdapterCallbacks = &adapter_callbacks; open.pAdapterFuncs = &adapter;
        check(tl_call(TlKind::OpenAdapter,[&] { return api.open(&open); }), "actual DLL typed OpenAdapter12");
        require(open.hAdapter.pDrvPrivate && runtime.identity.seen, "actual KMD query published adapter identity");
        api.audit_table(adapter, "adapter");
        require(adapter.pfnCalcPrivateDeviceSize && adapter.pfnCreateDevice && adapter.pfnDestroyDevice &&
            adapter.pfnCloseAdapter && adapter.pfnGetSupportedVersions, "required native adapter callbacks present");
        UINT count = 123;
        check(adapter.pfnGetSupportedVersions(open.hAdapter, &count, nullptr), "native supported versions");
        require(count == 0, "public native admission remains disabled");
        D3D12DDIARG_CALCPRIVATEDEVICESIZE size{};
        size.Interface = D3D12DDI_INTERFACE_VERSION_R0; size.Version = D3D12DDI_BUILD_VERSION << 16;
        device_storage.allocate(adapter.pfnCalcPrivateDeviceSize(open.hAdapter, &size), "device");
        kernel_callbacks.pfnCreateContextCb = safe_context_create; kernel_callbacks.pfnDestroyContextCb = safe_context_destroy;
        kernel_callbacks.pfnEscapeCb = safe_escape; kernel_callbacks.pfnAllocateCb = safe_allocate;
        kernel_callbacks.pfnDeallocateCb = safe_deallocate; kernel_callbacks.pfnDeallocate2Cb = safe_deallocate2;
        kernel_callbacks.pfnLockCb = safe_lock; kernel_callbacks.pfnUnlockCb = safe_unlock;
        kernel_callbacks.pfnRenderCb = safe_render; kernel_callbacks.pfnSignalSynchronizationObject2Cb = safe_signal;
        runtime_callbacks.pfnSetErrorCb = probe_error; runtime_callbacks.pfnSetCommandListErrorCb = probe_command_error;
        runtime_callbacks.pfnCreateContextCb = safe_queue_context_create;
        runtime_callbacks.pfnDestroyContextCb = safe_queue_context_destroy;
        runtime_callbacks.pfnMakeResidentCb = safe_r0_make_resident;
        runtime_callbacks.pfnEvictCb = safe_r0_evict;
        create.hDrvDevice = {device_storage.data()}; create.hRTDevice.handle = &runtime;
        create.Interface = size.Interface; create.Version = size.Version;
        create.pKTCallbacks = &kernel_callbacks; create.p12UMCallbacks = &runtime_callbacks;
        check(tl_call(TlKind::CreateDevice,[&] { return adapter.pfnCreateDevice(open.hAdapter, &create); }), "actual DLL typed native CreateDevice");
        device_live = true;
        status("native device created");
        require(runtime.identity.queue_protocol && runtime.context_handle && runtime.context_id,
            "actual validated VQDC plus KMT owner context/domain established");
        tl_void(TlKind::IcdVerify,[&] { icd.verify_loaded(); });
        std::printf("IDENTITY luid=%08x:%08x generation=%llu owner_context=%u allocation_domain=%u\n",
            static_cast<UINT>(luid.HighPart), static_cast<UINT>(luid.LowPart),
            static_cast<unsigned long long>(runtime.identity.generation), runtime.context_handle, runtime.context_id);
    }
    void required_slots(bool compute) {
        const auto &d = api.device; const auto &c = api.commands; const auto &q = api.queues;
        require(d.pfnCheckResourceAllocationInfo && d.pfnCalcPrivateHeapAndResourceSizes && d.pfnCreateHeapAndResource &&
            d.pfnDestroyHeapAndResource && d.pfnCheckExistingResourceAllocationInfo && d.pfnCheckResourceAllocationHandle &&
            d.pfnCheckResourceVirtualAddress && d.pfnMapHeap && d.pfnUnmapHeap && d.pfnCalcPrivateCommandQueueSize &&
            d.pfnCreateCommandQueue && d.pfnDestroyCommandQueue && d.pfnCalcPrivateCommandAllocatorSize &&
            d.pfnCreateCommandAllocator && d.pfnDestroyCommandAllocator && d.pfnResetCommandAllocator &&
            d.pfnCalcPrivateCommandListSize && d.pfnCreateCommandList && d.pfnDestroyCommandList &&
            d.pfnCalcPrivateFenceSize && d.pfnCreateFence && d.pfnDestroyFence &&
            c.pfnResetCommandList && c.pfnCopyBufferRegion && c.pfnCloseCommandList &&
            q.pfnExecuteCommandLists && q.pfnSignalFence, "all bounded copy/lifecycle R0 slots present");
        if (compute) require(d.pfnCalcPrivateRootSignatureSize && d.pfnCreateRootSignature && d.pfnDestroyRootSignature &&
            d.pfnCalcPrivateShaderSize && d.pfnCreateComputeShader && d.pfnDestroyShader &&
            d.pfnCalcPrivatePipelineStateSize && d.pfnCreatePipelineState && d.pfnDestroyPipelineState &&
            d.pfnCalcPrivateDescriptorHeapSize && d.pfnCreateDescriptorHeap && d.pfnDestroyDescriptorHeap &&
            d.pfnGetDescriptorSizeInBytes && d.pfnGetCPUDescriptorHandleForHeapStart && d.pfnGetGPUDescriptorHandleForHeapStart &&
            d.pfnCreateUnorderedAccessView && d.pfnCopyDescriptorsSimple && c.pfnResourceBarrier &&
            c.pfnSetComputeRootSignature && c.pfnSetPipelineState && c.pfnSetDescriptorHeaps &&
            c.pfnSetComputeRootDescriptorTable && c.pfnDispatch, "all bounded compute R0 slots present");
    }
};

struct DllBuffer {
    OpaqueStorage heap_storage, resource_storage;
    D3D12DDI_HHEAP heap{};
    D3D12DDI_HRESOURCE resource{};
    D3DKMT_HANDLE allocation = 0;
    UINT64 address = 0;
    bool mapped = false;
    void create(DllSession &s, UINT state, D3D12DDI_CPU_PAGE_PROPERTY property) {
        D3D12DDIARG_CREATERESOURCE_0003 r{};
        r.ResourceType = D3D12DDI_RT_BUFFER; r.Width = 65536;
        r.Height = r.DepthOrArraySize = r.MipLevels = r.SampleDesc.Count = 1;
        r.Layout = D3D12DDI_TL_ROW_MAJOR; r.InitialResourceState = static_cast<D3D12DDI_RESOURCE_STATES>(state);
        D3D12DDI_RESOURCE_ALLOCATION_INFO info{};
        TlCall requirements(TlKind::CheckAllocation,{state,static_cast<uint64_t>(property),r.Width});
        s.api.device.pfnCheckResourceAllocationInfo(s.create.hDrvDevice, &r,
            D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
        requirements.end({info.ResourceDataSize,info.ResourceDataAlignment});
        s.status("typed buffer allocation requirements");
        require(info.ResourceDataSize == 65536 && info.ResourceDataAlignment == 65536,
            "buffer requires exact64KiB backing/alignment");
        D3D12DDIARG_CREATEHEAP_0001 h{};
        h.ByteSize = info.ResourceDataSize; h.Alignment = info.ResourceDataAlignment;
        h.MemoryPool = D3D12DDI_MEMORY_POOL_L0; h.CPUPageProperty = property;
        h.Flags = D3D12DDI_HEAP_FLAG_BUFFERS; h.CreationNodeMask = h.VisibleNodeMask = 1;
        TlCall sizing(TlKind::CalcSizes,{state,static_cast<uint64_t>(property),h.ByteSize,h.Alignment});
        const auto sizes = s.api.device.pfnCalcPrivateHeapAndResourceSizes(s.create.hDrvDevice, &h, &r);
        sizing.end({sizes.Heap,sizes.Resource});
        heap_storage.allocate(sizes.Heap, "heap"); resource_storage.allocate(sizes.Resource, "resource");
        const D3D12DDI_HHEAP new_heap{heap_storage.data()};
        const D3D12DDI_HRESOURCE new_resource{resource_storage.data()};
        check(tl_call(TlKind::CreateHeap,[&] { return s.api.device.pfnCreateHeapAndResource(s.create.hDrvDevice, &h, new_heap, {}, &r, nullptr, new_resource); },
            {state,static_cast<uint64_t>(property),h.ByteSize,h.Alignment,sizes.Heap,sizes.Resource}), "actual typed paired buffer import");
        heap = new_heap; resource = new_resource;
        identity(s, property);
    }
    void identity(DllSession &s, D3D12DDI_CPU_PAGE_PROPERTY property) {
        D3D12DDI_RESOURCE_ALLOCATION_INFO info{}; D3DKMT_HANDLE queried = 0;
        s.api.device.pfnCheckExistingResourceAllocationInfo(s.create.hDrvDevice, resource, &info, &queried);
        const auto checked = s.api.device.pfnCheckResourceAllocationHandle(s.create.hDrvDevice, {resource.pDrvPrivate});
        const auto va = s.api.device.pfnCheckResourceVirtualAddress(s.create.hDrvDevice, resource);
        s.status("typed resource identity queries");
        require(queried && queried == checked && va && info.ResourceDataSize == 65536 && info.ResourceDataAlignment == 65536,
            "typed allocation handle/GPUVA/info agree");
        std::lock_guard<std::mutex> lock(s.runtime.mutex);
        auto found = s.runtime.allocations.find(queried);
        require(found != s.runtime.allocations.end() && found->second.size == 65536 && found->second.iova == va &&
            found->second.context_id == s.runtime.context_id && found->second.generation == s.runtime.identity.generation,
            "typed resource identity matches real Allocate ledger");
        // Backing CPU_VISIBLE is not permission to call public MapHeap.
        // Default heaps may use host-visible UMA memory while the DDI forbids
        // mapping; WRITE_BACK heaps must have CPU-visible backing.
        const bool cpu_map_allowed = property == D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
        require((property == D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE || cpu_map_allowed) &&
            (!cpu_map_allowed || (found->second.flags & 2)),
            "requested CPU page permission supported by backing");
        if (allocation) require(allocation == queried && address == va, "buffer identity unchanged across batches");
        allocation = queried; address = va;
        require(heap_storage.intact() && resource_storage.intact(), "heap/resource opaque canaries");
        std::printf("BUFFER allocation=%u gpuva=%llx bytes=%llu cpu_visible=%u\n", allocation,
            static_cast<unsigned long long>(address), static_cast<unsigned long long>(info.ResourceDataSize),
            property != D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
    }
    UINT *map(DllSession &s) {
        require(!mapped, "map each buffer once");
        void *mapping = nullptr;
        TlCall map_call(TlKind::Map,{allocation,address});
        const HRESULT map_result=s.api.device.pfnMapHeap(s.create.hDrvDevice, heap, &mapping);
        map_call.end({tl_u32(map_result),tl_ptr(mapping),allocation});
        check(map_result, "typed MapHeap");
        mapped = true;
        require(mapping != nullptr, "actual nonzero MapHeap pointer");
        {
            std::lock_guard<std::mutex> lock(s.runtime.mutex);
            auto found = s.runtime.lock_addresses.find(allocation);
            require(found != s.runtime.lock_addresses.end() && found->second == mapping,
                "MapHeap matches real LockCb pointer for queried allocation");
        }
        s.status("typed MapHeap status");
        return static_cast<UINT *>(mapping);
    }
    void unmap(DllSession &s) {
        require(mapped, "Unmap pairs successful Map");
        s.api.device.pfnUnmapHeap(s.create.hDrvDevice, heap); mapped = false;
        s.status("typed UnmapHeap");
    }
    void destroy(DllSession &s) {
        require(!mapped, "no mapped buffer released");
        if (heap.pDrvPrivate || resource.pDrvPrivate) {
            s.api.device.pfnDestroyHeapAndResource(s.create.hDrvDevice, heap, resource); heap = {}; resource = {};
            s.status("typed paired buffer destroy");
            require(!s.runtime.has(allocation), "exact public buffer allocation retired");
            require(heap_storage.intact() && resource_storage.intact(), "retired heap/resource opaque canaries");
        }
    }
};

struct DllRecording {
    OpaqueStorage allocator_storage, command_storage, fence_storage;
    D3D12DDI_HCOMMANDALLOCATOR allocator{};
    D3D12DDI_HCOMMANDLIST command{};
    D3D12DDI_HFENCE fence{};
    void create(DllSession &s, D3D12DDI_COMMAND_LIST_TYPE type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT) {
        D3D12DDIARG_CREATECOMMANDALLOCATOR a{};
        a.Type = type; a.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        allocator_storage.allocate(s.api.device.pfnCalcPrivateCommandAllocatorSize(s.create.hDrvDevice, &a), "allocator");
        a.hDrvCommandAllocator = {allocator_storage.data()};
        check(s.api.device.pfnCreateCommandAllocator(s.create.hDrvDevice, &a), "typed allocator create"); allocator = a.hDrvCommandAllocator;
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 c{};
        c.Type = a.Type; c.QueueFlags = a.QueueFlags; c.NodeMask = 1;
        c.hDrvCommandAllocator = allocator; c.hRTCommandList.handle = &s.runtime;
        command_storage.allocate(s.api.device.pfnCalcPrivateCommandListSize(s.create.hDrvDevice, &c), "command-list");
        c.hDrvCommandList = {command_storage.data()};
        check(s.api.device.pfnCreateCommandList(s.create.hDrvDevice, &c), "typed command-list create"); command = c.hDrvCommandList;
        D3D12DDI_FENCE f{}; f.Flags = D3D12DDI_FENCE_FLAG_BOTTOM_OF_PIPE;
        D3D12DDIARG_CREATE_FENCE args{1, &f};
        fence_storage.allocate(s.api.device.pfnCalcPrivateFenceSize(s.create.hDrvDevice, &args), "fence-metadata");
        check(s.api.device.pfnCreateFence(s.create.hDrvDevice, {fence_storage.data()}, &args), "typed fence metadata create");
        fence = {fence_storage.data()}; s.status("recording lifecycle creation");
    }
    void reopen(DllSession &s) {
        s.api.device.pfnResetCommandAllocator(allocator);
        D3D12DDIARG_RESETCOMMANDLIST args{}; args.hDrvCommandAllocator = allocator;
        s.api.commands.pfnResetCommandList(command, &args); s.status("typed recording reset after prior queue drain");
    }
    void destroy(DllSession &s) {
        if (command.pDrvPrivate) { s.api.device.pfnDestroyCommandList(s.create.hDrvDevice, command); command = {}; }
        if (allocator.pDrvPrivate) { s.api.device.pfnDestroyCommandAllocator(s.create.hDrvDevice, allocator); allocator = {}; }
        if (fence.pDrvPrivate) { s.api.device.pfnDestroyFence(s.create.hDrvDevice, fence); fence = {}; }
        s.status("typed recording/fence destroy");
        require(command_storage.intact() && allocator_storage.intact() && fence_storage.intact(), "recording/fence opaque canaries");
    }
};

struct DllBatch {
    OpaqueStorage storage;
    D3D12DDI_HCOMMANDQUEUE queue{};
    ProbeRuntimeQueue owner{};
    BatchGate gate;
    HANDLE event = nullptr;
    void create(DllSession &s) {
        owner.runtime = &s.runtime;
        D3D12DDIARG_CREATECOMMANDQUEUE_0001 args{};
        args.NodeMask = 1; args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D; args.hRTCommandQueue.handle = &owner;
        storage.allocate(s.api.device.pfnCalcPrivateCommandQueueSize(s.create.hDrvDevice, &args), "batch-queue");
        args.hDrvCommandQueue = {storage.data()};
        check(s.api.device.pfnCreateCommandQueue(s.create.hDrvDevice, &args), "typed independent batch queue create");
        queue = args.hDrvCommandQueue; s.status("batch queue create status");
        require(owner.creates == 1 && owner.context && owner.context != s.runtime.context_handle &&
            s.runtime.has_context(owner.context), "queue has exactly one real distinct scheduler-context callback");
        gate.begin(owner.context);
    }
    void execute(DllSession &s, const DllRecording &recording, UINT round) {
        const auto before = s.runtime.counts(owner.context), owner_before = s.runtime.counts(s.runtime.context_handle);
        gate.execute();
        tl_void(TlKind::Execute,[&] { s.api.queues.pfnExecuteCommandLists(queue, 1, &recording.command); },{round,owner.context});
        s.status("actual DLL ExecuteCommandLists");
        const auto submitted = s.runtime.counts(owner.context);
        require(submitted.targets > before.targets && submitted.renders > before.renders,
            "Execute returns after target DMA reaches selected real scheduler context");
        require(s.runtime.counts(s.runtime.context_handle).renders == owner_before.renders &&
            s.runtime.counts(s.runtime.context_handle).targets == owner_before.targets, "work does not use allocation-owner context");
        D3D12DDIARG_FENCE_OPERATION operation{recording.fence, round + 1, 0};
        tl_void(TlKind::SignalFence,[&] { s.api.queues.pfnSignalFence(queue, &operation); },{round,owner.context,operation.Value}); s.status("typed SignalFence metadata only");
        require(operation.PhysicalAdapterMask == 1 && operation.Value == round + 1, "fence metadata chooses one node");
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr); require(event != nullptr, "real batch external event");
        D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 signal{};
        signal.hContext = runtime_context(owner.context); signal.Flags.EnqueueCpuEvent = 1; signal.CpuEventHandle = event;
        check(safe_signal(&s.runtime, &signal), "actual KMT ordered external completion event");
        require(tl_call(TlKind::BatchWait,[&] { return WaitForSingleObject(event, 10000); },
            {round,owner.context,tl_ptr(event),10000}) == WAIT_OBJECT_0, "actual batch OS completion observed within10s");
        check(s.runtime.observe_completions(owner.context), "all selected-context callback and external events observed");
        const auto after = s.runtime.counts(owner.context);
        require(after.events > before.events && after.observed == after.events, "every selected-context completion event observed");
        gate.observed(owner.context);
        /* This typed destroy is the only available public drain operation for
         * the bounded probe. Never use BridgeStatus/SignalFence as a reap. */
        gate.destroy_start();
        tl_void(TlKind::DestroyQueue,[&] { s.api.device.pfnDestroyCommandQueue(s.create.hDrvDevice, queue); },{round,owner.context}); queue = {};
        s.status("typed DestroyCommandQueue drains pending native FIFO");
        require(owner.destroys == 1 && owner.context == 0 && owner.last_destroyed == gate.context &&
            !s.runtime.has_context(gate.context), "exact real DestroyContext callback completed during typed queue drain");
        bool pending = false;
        {
            std::lock_guard<std::mutex> lock(s.runtime.mutex);
            for (const auto &item : s.runtime.completions)
                if (item.context == gate.context && item.event) pending = true;
        }
        gate.destroy_finish(owner.last_destroyed, !pending && owner.closed.events == owner.closed.observed,
            !s.runtime.callback_exceptions && !s.runtime.command_errors && !s.runtime.device_errors);
        require(gate.readback_allowed() && storage.intact(), "typed drain permits readback with opaque queue storage intact");
        CloseHandle(event); event = nullptr;
        std::printf("DRAIN batch=%u context=%u creates=%u destroys=%u renders=%u targets=%u events=%u observed=%u map_allowed=1\n",
            round, gate.context, owner.creates, owner.destroys, owner.closed.renders, owner.closed.targets,
            owner.closed.events, owner.closed.observed);
    }
};

struct DllCompute {
    OpaqueStorage root_storage, shader_storage, pipeline_storage, cpu_storage, gpu_storage;
    D3D12DDI_HROOTSIGNATURE root{};
    D3D12DDI_HSHADER shader{};
    D3D12DDI_HPIPELINESTATE pipeline{};
    D3D12DDI_HDESCRIPTORHEAP cpu{}, gpu{};
    UINT stride = 0;
    void create(DllSession &s) {
        const auto device = s.create.hDrvDevice; const auto &d = s.api.device;
        D3D12DDI_DESCRIPTOR_RANGE range{D3D12DDI_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1};
        D3D12DDI_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &range};
        D3D12DDI_ROOT_SIGNATURE signature{}; signature.NumParameters = 1; signature.pRootParameters = &parameter;
        D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 args{&signature, 1};
        root_storage.allocate(d.pfnCalcPrivateRootSignatureSize(device, &args), "compute-root");
        check(d.pfnCreateRootSignature(device, &args, {root_storage.data()}), "typed compute root table"); root = {root_storage.data()};
        static_assert(sizeof(execute_indirect_cs_code_dxbc) == 280, "frozen SM5 fixture size");
        require(execute_indirect_cs_code_dxbc[19] == 0x58454853 && execute_indirect_cs_code_dxbc[20] == 49 * sizeof(UINT) &&
            execute_indirect_cs_code_dxbc[21] == 0x00050050 && execute_indirect_cs_code_dxbc[22] == 49,
            "exact legal SM5 SHEX token fixture");
        const auto tokens = reinterpret_cast<const UINT *>(execute_indirect_cs_code_dxbc + 21);
        D3D12DDIARG_STAGE_IO_SIGNATURES io{};
        shader_storage.allocate(d.pfnCalcPrivateShaderSize(device, tokens, root, &io), "compute-shader");
        d.pfnCreateComputeShader(device, tokens, root, {shader_storage.data()}, D3D12DDI_CREATE_SHADER_FLAG_NONE);
        s.status("typed CreateComputeShader status/callback publication"); shader = {shader_storage.data()};
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pso{};
        pso.hRootSignature = root; pso.hComputeShader = shader; pso.NodeMask = 1;
        pipeline_storage.allocate(d.pfnCalcPrivatePipelineStateSize(device, &pso), "compute-pipeline");
        pso.hDrvPipelineState = {pipeline_storage.data()};
        check(d.pfnCreatePipelineState(device, &pso), "typed compute pipeline"); pipeline = pso.hDrvPipelineState;
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            8, D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
        cpu_storage.allocate(d.pfnCalcPrivateDescriptorHeapSize(device, &heap), "CPU-descriptor-heap");
        check(d.pfnCreateDescriptorHeap(device, &heap, {cpu_storage.data()}), "typed CPU descriptors"); cpu = {cpu_storage.data()};
        heap.Flags |= D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        gpu_storage.allocate(d.pfnCalcPrivateDescriptorHeapSize(device, &heap), "GPU-descriptor-heap");
        check(d.pfnCreateDescriptorHeap(device, &heap, {gpu_storage.data()}), "typed GPU descriptors"); gpu = {gpu_storage.data()};
        stride = d.pfnGetDescriptorSizeInBytes(device, D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        require(stride && UINT64(stride) * 8 <= SIZE_MAX, "bounded actual descriptor stride");
        s.status("typed compute object creation");
    }
    void bind(DllSession &s, D3D12DDI_HCOMMANDLIST command, DllBuffer &scratch, UINT first) {
        const auto device = s.create.hDrvDevice; const auto &d = s.api.device; const auto &c = s.api.commands;
        D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 view{};
        view.hDrvResource = scratch.resource; view.ResourceDimension = D3D12DDI_RD_BUFFER;
        view.Format = DXGI_FORMAT_R32_TYPELESS; view.Buffer.FirstElement = first;
        view.Buffer.NumElements = 1024; view.Buffer.Flags = D3D12DDI_BUFFER_UAV_FLAG_RAW;
        auto source = d.pfnGetCPUDescriptorHandleForHeapStart(device, cpu);
        auto destination = d.pfnGetCPUDescriptorHandleForHeapStart(device, gpu);
        require(source.ptr && destination.ptr && source.ptr <= SIZE_MAX - SIZE_T(2) * stride &&
            destination.ptr <= SIZE_MAX - SIZE_T(4) * stride, "CPU descriptor handle arithmetic bounded");
        source.ptr += SIZE_T(2) * stride; destination.ptr += SIZE_T(4) * stride;
        d.pfnCreateUnorderedAccessView(device, &view, source);
        d.pfnCopyDescriptorsSimple(device, 1, destination, source, D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        c.pfnSetComputeRootSignature(command, root); c.pfnSetPipelineState(command, pipeline);
        c.pfnSetDescriptorHeaps(command, 1, &gpu);
        auto table = d.pfnGetGPUDescriptorHandleForHeapStart(device, gpu);
        require(table.ptr && table.ptr <= UINT64_MAX - UINT64(3) * stride, "GPU descriptor handle arithmetic bounded");
        table.ptr += UINT64(3) * stride;
        c.pfnSetComputeRootDescriptorTable(command, 0, table); s.status("typed compute view/copy/root bindings");
    }
    void destroy(DllSession &s) {
        const auto device = s.create.hDrvDevice; const auto &d = s.api.device;
        if (pipeline.pDrvPrivate) { d.pfnDestroyPipelineState(device, pipeline); pipeline = {}; }
        if (shader.pDrvPrivate) { d.pfnDestroyShader(device, shader); shader = {}; }
        if (root.pDrvPrivate) { d.pfnDestroyRootSignature(device, root); root = {}; }
        if (gpu.pDrvPrivate) { d.pfnDestroyDescriptorHeap(device, gpu); gpu = {}; }
        if (cpu.pDrvPrivate) { d.pfnDestroyDescriptorHeap(device, cpu); cpu = {}; }
        s.status("typed compute object destroy");
        require(root_storage.intact() && shader_storage.intact() && pipeline_storage.intact() && cpu_storage.intact() &&
            gpu_storage.intact(), "compute opaque storage canaries");
    }
};

struct DllWorkload {
    DllSession session;
    DllBuffer upload, scratch, readback;
    DllRecording recording;
    DllCompute compute;
    /* Addresses never move after passing HRT/opaque handles to the DLL. */
    std::array<DllBatch, 8> batches;
    bool compute_mode = false, residency_mode = false;
    void residency_cycle(UINT round) {
        auto &s = session; auto &r = s.runtime;
        const D3D12DDI_HANDLE_AND_TYPE objects[] = {
            {upload.heap.pDrvPrivate, D3D12DDI_HT_HEAP},
            {readback.heap.pDrvPrivate, D3D12DDI_HT_HEAP}};
        require(s.api.device.pfnMakeResident && s.api.device.pfnEvict,
            "exact native R0 residency slots present");
        D3D12DDIARG_EVICT evict{}; evict.NumObjects = 2; evict.pObjects = objects;
        check(s.api.device.pfnEvict(s.create.hDrvDevice, &evict), "actual native R0 Evict");
        s.status("native R0 Evict status");
        {
            std::lock_guard<std::mutex> lock(r.residency_mutex);
            require(!r.resident_allocations.count(upload.allocation) &&
                !r.resident_allocations.count(readback.allocation), "R0 Evict removes final explicit buffer references");
        }
        const auto paging = r.runtime_paging_queue();
        UINT64 fence = 0xa5a5;
        D3D12DDIARG_MAKERESIDENT_0001 make{};
        make.NumAdapters = 1; make.pRTPagingQueue = &paging;
        make.NumObjects = 2; make.pObjects = objects; make.pPagingFenceValue = &fence;
        const HRESULT hr = s.api.device.pfnMakeResident(s.create.hDrvDevice, &make);
        require(hr == S_OK || hr == E_PENDING, "actual native R0 MakeResident accepted");
        require((hr == S_OK && !make.WaitMask && !fence) ||
            (hr == E_PENDING && make.WaitMask == 1 && fence), "native R0 pending fence and wait mask agree");
        s.status("native R0 MakeResident status");
        // The existing Render provider waits for the actual monitored paging
        // fence before KMT Render, including a returned E_PENDING target.
        std::printf("R0 RESIDENCY round=%u objects=2 allocations=2 evict_then_resident=1 hr=%08x wait_mask=%u fence=%llu\n",
            round, static_cast<UINT>(hr), make.WaitMask, static_cast<unsigned long long>(fence));
    }
    void require_quiescent() const {
        for (const auto &batch : batches)
            if (batch.gate.context) require(batch.gate.release_allowed() && !batch.queue.pDrvPrivate && !batch.event,
                "no live batch before resource/provider/module release");
    }
    void run(bool use_compute, LUID luid, bool use_residency = false) {
        compute_mode = use_compute; residency_mode = use_residency;
        auto &s = session; auto &r = s.runtime;
        s.required_slots(use_compute); s.initialize(luid);
        upload.create(s, 0xac3, D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK);
        if (use_compute) scratch.create(s, 0x400, D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
        readback.create(s, 0x400, D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK);
        require(upload.allocation != readback.allocation && (!use_compute ||
            (scratch.allocation != readback.allocation && scratch.allocation != upload.allocation)), "distinct real public buffer allocations");
        r.target_handle = readback.allocation;
        recording.create(s); if (use_compute) compute.create(s);
        const auto command = recording.command;
        auto transition = [&](UINT before, UINT after) {
            D3D12DDIARG_RESOURCE_BARRIER_0003 barrier{}; barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.hResource = scratch.resource; barrier.Transition.Subresource = UINT_MAX;
            barrier.Transition.StateBefore = static_cast<D3D12DDI_RESOURCE_STATES>(before);
            barrier.Transition.StateAfter = static_cast<D3D12DDI_RESOURCE_STATES>(after);
            s.api.commands.pfnResourceBarrier(command, 1, &barrier); s.status("typed compute transition");
        };
        auto copy = [&](D3D12DDI_HRESOURCE dst, UINT64 dst_offset, D3D12DDI_HRESOURCE src, UINT64 src_offset, UINT64 bytes) {
            D3D12DDIARG_BUFFER_PLACEMENT destination{}, source{};
            destination.BaseAddress.UMD.hResource = dst; destination.BaseAddress.UMD.Offset = dst_offset;
            source.BaseAddress.UMD.hResource = src; source.BaseAddress.UMD.Offset = src_offset;
            s.api.commands.pfnCopyBufferRegion(command, destination, source, bytes); s.status("typed buffer copy");
        };
        for (UINT round = 0; round < batches.size(); ++round) {
            require_quiescent();
            const UINT first = 64 + round * 32, sentinel = 0xfedc0000u ^ (round * 0x01010101u);
            UINT *words = upload.map(s);
            for (UINT i = 0; i < 16384; ++i) words[i] = use_compute ? sentinel : 0xdeadbeefu;
            if (!use_compute) for (UINT i = 0; i < 1024; ++i) words[32 + i] = (round + 1) * 0x1020304u ^ (i * 0x9e3779b9u);
            upload.unmap(s);
            words = readback.map(s);
            for (UINT i = 0; i < 16384; ++i) words[i] = use_compute ? 0x13579bdfu ^ (round * 0x11111111u) : sentinel;
            readback.unmap(s);
            if (residency_mode) residency_cycle(round);
            if (round) recording.reopen(s);
            if (use_compute) {
                if (round) transition(0x800, 0x400);
                copy(scratch.resource, 0, upload.resource, 0, 65536);
                transition(0x400, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
                compute.bind(s, command, scratch, first);
                s.api.commands.pfnDispatch(command, 1024, 1, 1); s.status("actual typed Dispatch");
                transition(D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, 0x800);
                copy(readback.resource, 0, scratch.resource, 0, 65536);
            } else copy(readback.resource, UINT64(first) * 4, upload.resource, 128, 4096);
            s.api.commands.pfnCloseCommandList(command); s.status("typed command close");
            auto &batch = batches[round]; batch.create(s); batch.execute(s, recording, round);
            require(batch.gate.readback_allowed(), "readback only after actual event and typed drain");
            words = readback.map(s);
            for (UINT i = 0; i < 16384; ++i) {
                const UINT expected = i >= first && i < first + 1024 ?
                    (use_compute ? i - first : (round + 1) * 0x1020304u ^ ((i - first) * 0x9e3779b9u)) : sentinel;
                if (words[i] != expected) {
                    std::fprintf(stdout, "FAIL DLL %s batch=%u word=%u actual=%08x expected=%08x\n",
                        use_compute ? "COMPUTE" : "COPY", round, i, words[i], expected);
                    readback.unmap(s); throw ProbeFailure("actual independent DLL GPU data oracle");
                }
            }
            readback.unmap(s);
            readback.identity(s, D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK);
            std::printf("PASS DLL %s batch=%u changed=1024 sentinel=15360 queue_recreated=1 typed_drain_before_map=1\n",
                use_compute ? "COMPUTE" : "COPY", round);
        }
        cleanup();
    }
    void cleanup() {
        auto &s = session; auto &r = s.runtime;
        require_quiescent();
        check(r.observe_completions(0), "terminal provider completion observation");
        r.retirement_sealed = true;
        const auto terminal_renders = r.renders.load();
        recording.destroy(s); if (compute_mode) compute.destroy(s);
        readback.destroy(s); if (compute_mode) scratch.destroy(s); upload.destroy(s);
        require(r.pageable_deallocations == (compute_mode ? 3u : 2u), "exact public Deallocate2 flags3 count");
        s.status("all typed public objects released after batches drained");
        r.native_cleanup = true;
        s.adapter.pfnDestroyDevice(s.create.hDrvDevice); s.device_live = false;
        check(s.adapter.pfnCloseAdapter(s.open.hAdapter), "typed native CloseAdapter"); s.open.hAdapter = {};
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            require(!r.context_handle && r.contexts.empty() && r.allocations.empty() && !r.callback_exceptions &&
                !r.command_errors && !r.device_errors && r.renders == terminal_renders,
                "native destroy empties active context/allocation ledgers without late Render or errors");
            for (const auto &event : r.completions) require(!event.event, "no outstanding observer event handle");
        }
        require(s.device_storage.intact(), "retired native device opaque canaries");
        check(r.close(), "real provider paging/device/adapter teardown after DLL children");
        require(!r.device && !r.adapter && !r.paging_queue && r.resident_allocations.empty(), "all provider ownership closed");
        s.api.unload();
        std::printf("PASS DLL %s cleanup batches=8 public_deallocate2=%u internal_deallocate=%u callbacks=0 contexts=0 allocations=0 late_renders=0 core_reference_released=1\n",
            compute_mode ? "COMPUTE" : "COPY", r.pageable_deallocations.load(), r.internal_deallocations.load());
        std::puts("BOUNDARY actual standalone DLL + handwritten runtime + real KMT/GPU; batch queue recreation, not persistent-two-queue/private-prefix proof; no aliases/SYSTEM/PRIMARY/Present/monitored-fence/independent-BO-retirement claim");
    }
};

void run_dll_workload(bool compute, const wchar_t *core, const wchar_t *core_hash,
        const wchar_t *manifest, const wchar_t *icd_hash, LUID luid, bool residency = false) {
    Deadline deadline;
    timeline.begin();
    TlCall run_call(TlKind::Run,{compute,tl_luid(luid),residency});
    /* Deliberately no automatic owner: on any unconfirmed lifetime edge the
     * provider, module, callback tables, opaque objects and events stay alive.
     * Process exit is the final containment boundary, not FreeLibrary. */
    auto *workload = new DllWorkload;
    try {
        tl_void(TlKind::CoreLoad,[&] { workload->session.api.load(core, core_hash); });
        tl_void(TlKind::IcdConfigure,[&] { workload->session.icd.configure(manifest, icd_hash); });
        workload->run(compute, luid, residency);
        delete workload;
        run_call.end({0}); timeline.emit(0,TlKind::Terminal,0,{0,0}); timeline.finish(0,0);
    } catch (...) {
        timeline.emit(0,TlKind::Retain,0,{1});
        run_call.end({1}); timeline.emit(0,TlKind::Terminal,0,{1,1}); timeline.finish(1,1);
        std::fputs("FAIL retained session: no core unload, no opaque/provider free on incomplete or failed drain; process exit owns final containment\n", stdout);
        throw;
    }
}
