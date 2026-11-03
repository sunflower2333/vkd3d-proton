/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Source-linked production UMD through its DDI tables. This controlled runtime
 * is not SYSTEM D3D12 admission and does not load the standalone UMD DLL. */
#include "../../tests/shaders/command/headers/execute_indirect_cs.h"

struct ComputeObjects {
    NativeSession &native;
    Object root{}, shader{}, pipeline{}, cpu_heap{}, gpu_heap{};
    bool root_live = false, shader_live = false, pipeline_live = false;
    bool cpu_live = false, gpu_live = false;
    UINT stride = 0;
    explicit ComputeObjects(NativeSession &n) : native(n) {}
    ~ComputeObjects() { reset(); }
    void create() {
        const auto device = native.create.hDrvDevice;
        D3D12DDI_DESCRIPTOR_RANGE range{D3D12DDI_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1};
        D3D12DDI_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &range};
        D3D12DDI_ROOT_SIGNATURE signature{};
        signature.NumParameters = 1; signature.pRootParameters = &parameter;
        D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args{&signature, 1};
        check(native.table.pfnCreateRootSignature(device, &root_args, {&root}), "compute root table");
        root_live = true;
        // Exact existing legal SM5 fixture, not a whole DXBC passed as tokens.
        static_assert(sizeof(execute_indirect_cs_code_dxbc) == 280, "SM5 fixture bounds");
        require(execute_indirect_cs_code_dxbc[19] == 0x58454853 &&
                execute_indirect_cs_code_dxbc[20] == 49 * sizeof(UINT) &&
                execute_indirect_cs_code_dxbc[21] == 0x00050050 &&
                execute_indirect_cs_code_dxbc[22] == 49, "SM5 fixture SHEX identity and token length");
        native.table.pfnCreateComputeShader(device,
            reinterpret_cast<const UINT *>(execute_indirect_cs_code_dxbc + 21), {&root}, {&shader},
            static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(0));
        shader_live = object(&shader) != nullptr;
        check(VioGpuD3D12BridgeStatus(device), "compute SM5 shader");
        require(shader_live, "native shader storage published");
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pso{};
        pso.hRootSignature = {&root}; pso.hComputeShader = {&shader};
        pso.hDrvPipelineState = {&pipeline}; pso.NodeMask = 1;
        check(native.table.pfnCreatePipelineState(device, &pso), "compute pipeline");
        pipeline_live = true;
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 desc{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            8, D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
        check(native.table.pfnCreateDescriptorHeap(device, &desc, {&cpu_heap}), "compute CPU descriptors");
        cpu_live = true;
        desc.Flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE | D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(native.table.pfnCreateDescriptorHeap(device, &desc, {&gpu_heap}), "compute visible descriptors");
        gpu_live = true;
        stride = native.table.pfnGetDescriptorSizeInBytes(device, D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        require(stride != 0, "native descriptor stride");
    }
    void view(MinimalBuffer &scratch, UINT first) {
        const auto device = native.create.hDrvDevice;
        D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 view{};
        view.hDrvResource = scratch.resource; view.ResourceDimension = D3D12DDI_RD_BUFFER;
        view.Format = DXGI_FORMAT_R32_TYPELESS; view.Buffer.FirstElement = first;
        view.Buffer.NumElements = 1024; view.Buffer.Flags = D3D12DDI_BUFFER_UAV_FLAG_RAW;
        auto source = native.table.pfnGetCPUDescriptorHandleForHeapStart(device, {&cpu_heap});
        auto destination = native.table.pfnGetCPUDescriptorHandleForHeapStart(device, {&gpu_heap});
        source.ptr += SIZE_T(2) * stride; destination.ptr += SIZE_T(4) * stride;
        native.table.pfnCreateUnorderedAccessView(device, &view, source);
        check(VioGpuD3D12BridgeStatus(device), "compute raw UAV view");
        native.table.pfnCopyDescriptorsSimple(device, 1, destination, source, D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        check(VioGpuD3D12BridgeStatus(device), "compute staging descriptor copy");
    }
    void bind(D3D12DDI_HCOMMANDLIST command) {
        native.commands.pfnSetComputeRootSignature(command, {&root});
        native.commands.pfnSetPipelineState(command, {&pipeline});
        D3D12DDI_HDESCRIPTORHEAP visible{&gpu_heap};
        native.commands.pfnSetDescriptorHeaps(command, 1, &visible);
        auto table = native.table.pfnGetGPUDescriptorHandleForHeapStart(native.create.hDrvDevice, visible);
        table.ptr += UINT64(3) * stride;
        native.commands.pfnSetComputeRootDescriptorTable(command, 0, table);
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "compute root/PSO/descriptor table binding");
    }
    void reset() {
        const auto device = native.create.hDrvDevice;
        if (pipeline_live) { native.table.pfnDestroyPipelineState(device, {&pipeline}); pipeline_live = false; }
        if (shader_live) { native.table.pfnDestroyShader(device, {&shader}); shader_live = false; }
        if (root_live) { native.table.pfnDestroyRootSignature(device, {&root}); root_live = false; }
        if (gpu_live) { native.table.pfnDestroyDescriptorHeap(device, {&gpu_heap}); gpu_live = false; }
        if (cpu_live) { native.table.pfnDestroyDescriptorHeap(device, {&cpu_heap}); cpu_live = false; }
    }
};

void run_compute_native_probe(LUID luid) {
    Deadline deadline;
    ProbeRuntime runtime; runtime.open(luid);
    NativeSession native; native.initialize(runtime);
    auto *ctx = context(native.create.hDrvDevice);
    require(ctx && ctx->native_context_id == runtime.context_id, "compute native allocation domain identity");
    const auto generation = ctx->native_adapter->generation;
    MinimalBuffer upload(native), scratch(native), readback(native);
    upload.create(0xac3); scratch.create(0x400, D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE); readback.create(0x400);
    auto *target = native_heap_find(ctx, readback.heap.pDrvPrivate);
    auto *uav = native_heap_find(ctx, scratch.heap.pDrvPrivate);
    require(target && target->allocation && target->cpu_visible && uav && uav->allocation && !uav->cpu_visible &&
            target->allocation != uav->allocation && readback.resource_slot.address == target->address &&
            scratch.resource_slot.address == uav->address, "compute exact default UAV and cached readback backing");
    const auto target_handle = target->allocation;
    runtime.target_handle = target_handle;
    NativeProbeQueue first(native, runtime), second(native, runtime);
    first.create(); second.create();
    require(first.runtime.context != second.runtime.context && first.runtime.context != runtime.context_handle &&
            second.runtime.context != runtime.context_handle, "compute two independent runtime-associated contexts");
    ComputeObjects objects(native); objects.create();
    MinimalRecording recording(native); recording.create(runtime);
    MinimalFence fence(native); fence.create();
    std::printf("COMPUTE identity LUID=%08x:%08x generation=%llu owner=%u queue0=%u queue1=%u domain=%u allocation=%u\n",
        static_cast<UINT>(luid.HighPart), static_cast<UINT>(luid.LowPart), static_cast<unsigned long long>(generation),
        runtime.context_handle, first.runtime.context, second.runtime.context, runtime.context_id, target_handle);
    const D3D12DDI_HCOMMANDLIST command{&recording.command};
    auto transition = [&](UINT before, UINT after) {
        D3D12DDIARG_RESOURCE_BARRIER_0003 barrier{};
        barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.hResource = scratch.resource; barrier.Transition.Subresource = UINT_MAX;
        barrier.Transition.StateBefore = static_cast<D3D12DDI_RESOURCE_STATES>(before);
        barrier.Transition.StateAfter = static_cast<D3D12DDI_RESOURCE_STATES>(after);
        native.commands.pfnResourceBarrier(command, 1, &barrier);
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "compute resource transition");
    };
    auto copy = [&](D3D12DDI_HRESOURCE destination, D3D12DDI_HRESOURCE source) {
        D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
        dst.BaseAddress.UMD.hResource = destination; src.BaseAddress.UMD.hResource = source;
        native.commands.pfnCopyBufferRegion(command, dst, src, 65536);
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "compute whole-buffer DDI copy");
    };
    for (UINT round = 0; round < 8; ++round) {
        auto &selected = round & 1 ? second : first;
        auto &other = round & 1 ? first : second;
        const UINT first_word = 64 + round * 32;
        const UINT sentinel = 0xfedc0000u ^ (round * 0x01010101u);
        const UINT poison = 0x13579bdfu ^ (round * 0x11111111u);
        void *mapping = nullptr;
        check(native.table.pfnMapHeap(native.create.hDrvDevice, upload.heap, &mapping), "compute map upload sentinel");
        auto *words = static_cast<UINT *>(mapping);
        for (UINT i = 0; i < 16384; ++i) words[i] = sentinel;
        native.table.pfnUnmapHeap(native.create.hDrvDevice, upload.heap);
        check(native.table.pfnMapHeap(native.create.hDrvDevice, readback.heap, &mapping), "compute map readback poison");
        words = static_cast<UINT *>(mapping);
        for (UINT i = 0; i < 16384; ++i) words[i] = poison;
        native.table.pfnUnmapHeap(native.create.hDrvDevice, readback.heap);
        objects.view(scratch, first_word);
        if (round) { recording.reopen(); transition(0x800, 0x400); }
        copy(scratch.resource, upload.resource);
        transition(0x400, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
        objects.bind(command);
        native.commands.pfnDispatch(command, 1024, 1, 1);
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native compute Dispatch");
        transition(D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, 0x800);
        copy(readback.resource, scratch.resource);
        native.commands.pfnCloseCommandList(command);
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native compute close");
        require(!runtime.command_errors && !runtime.device_errors, "no native compute callback errors");
        const auto selected_before = runtime.counts(selected.runtime.context);
        const auto other_before = runtime.counts(other.runtime.context);
        const auto owner_before = runtime.counts(runtime.context_handle);
        native.queues.pfnExecuteCommandLists({&selected.slot}, 1, &command);
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native compute ExecuteCommandLists");
        require(runtime.counts(selected.runtime.context).targets > selected_before.targets,
                "compute Execute queues readback DMA on selected scheduler");
        D3D12DDIARG_FENCE_OPERATION operation{{&fence.slot}, round + 1, 0};
        native.queues.pfnSignalFence({&selected.slot}, &operation);
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native compute SignalFence metadata");
        require(operation.PhysicalAdapterMask == 1 && operation.Value == round + 1,
                "compute fence metadata selects one physical node");
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(event != nullptr, "compute external runtime completion event");
        struct CloseEvent { HANDLE event; ~CloseEvent() { CloseHandle(event); } } event_owner{event};
        D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 signal{};
        signal.hContext = runtime_context(selected.runtime.context);
        signal.Flags.EnqueueCpuEvent = 1; signal.CpuEventHandle = event;
        check(probe_signal(&runtime, &signal), "compute real ordered OS event");
        require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "compute selected-context GPU event within10s");
        UINT completed = 0;
        check(native_runtime_completed(ctx, &completed), "compute retire actual OS completion prefix");
        require(!ctx->native_submissions && completed == ctx->native_last_submitted,
                "compute all accepted submissions completed before CPU readback");
        check(runtime.observe_completions(0), "compute observe actual GPU completion events");
        const auto after = runtime.counts(selected.runtime.context);
        const auto other_after = runtime.counts(other.runtime.context);
        const auto owner_after = runtime.counts(runtime.context_handle);
        require(after.renders > selected_before.renders && after.events > selected_before.events &&
                after.observed == after.events && other_after.targets == other_before.targets &&
                other_after.renders == other_before.renders && owner_after.targets == owner_before.targets &&
                owner_after.renders == owner_before.renders, "compute work and completion stay on selected queue");
        check(native.table.pfnMapHeap(native.create.hDrvDevice, readback.heap, &mapping), "compute map completed readback");
        words = static_cast<UINT *>(mapping);
        // This independent integer oracle is never written to any GPU input.
        for (UINT i = 0; i < 16384; ++i) {
            const UINT expected = i >= first_word && i < first_word + 1024 ? i - first_word : sentinel;
            if (words[i] != expected) {
                std::fprintf(stderr, "FAIL compute round%u word%u actual=%08x expected=%08x\n", round, i, words[i], expected);
                native.table.pfnUnmapHeap(native.create.hDrvDevice, readback.heap);
                throw ProbeFailure("actual GPU integer/padding readback");
            }
        }
        native.table.pfnUnmapHeap(native.create.hDrvDevice, readback.heap);
        check(native_heap_generation(ctx), "compute unchanged adapter generation");
        std::printf("PASS COMPUTE round=%u queue=%u renders=%u events=%u observed=%u completed=%u first=%u outputs=1024 padding=15360\n",
                round, selected.runtime.context, after.renders, after.events, after.observed, completed, first_word);
    }
    check(runtime.observe_completions(0), "compute terminal completion before release");
    require(!ctx->native_submissions && ctx->native_os_completed == ctx->native_last_submitted,
            "compute terminal completed prefix without later producers");
    runtime.retirement_sealed = true;
    const auto terminal_renders = runtime.renders.load();
    recording.reset(); fence.reset(); first.reset(); second.reset(); objects.reset();
    require(!first.runtime.context && !second.runtime.context, "compute both queue contexts closed");
    readback.reset(); scratch.reset(); upload.reset();
    require(!runtime.has(target_handle) && runtime.pageable_deallocations == 3,
            "compute exactly three public Deallocate2 flags3 releases");
    check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "compute native clean release status");
    runtime.native_cleanup = true; native.finish();
    require(!runtime.context_handle && runtime.contexts.empty() && runtime.allocations.empty() &&
            runtime.resident_allocations.empty() && !runtime.device_errors && !runtime.command_errors &&
            runtime.renders == terminal_renders, "compute no contexts/allocations/residency/errors/later Render");
    check(runtime.close(), "compute final KMT device/adapter close");
    require(!runtime.device && !runtime.adapter && !runtime.paging_queue && !runtime.paging_sync &&
            !runtime.paging_cpu_fence && !runtime.pending_paging_fence, "compute all KMT paging/device handles closed");
    std::printf("PASS COMPUTE cleanup public_deallocate2=%u internal_deallocate=%u contexts=0 allocations=0 later_renders=0\n",
            runtime.pageable_deallocations.load(), runtime.internal_deallocations.load());
    std::puts("BOUNDARY real native DDI/KMT/GPU compute only, handwritten runtime callbacks; SYSTEM/PRIMARY closed; no monitored-fence/Present or independent BO retirement claim");
}
