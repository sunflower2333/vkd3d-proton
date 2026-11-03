/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Independent records survive runtime-owned slots and asynchronous ICD work. */
struct NativeRuntimeQueue : NativeContextBuffers {
    NativeRuntimeQueue *next = nullptr;
    D3D12DDI_HRTCOMMANDQUEUE runtime{};
    uint32_t users = 1, submissions = 0;
    bool pending = true, closing = false, preparing_present = false;
    // Proxy queues (native surfaces): the runtime's queue owns a standard
    // sync-only context. Native work runs on the single owner context, which
    // the KMD requires for host-surface imports (no domain children). Two
    // monitored fences order the runtime's queue packets against that work.
    bool proxy = false;
    D3DKMT_HANDLE hop_in = 0, hop_out = 0;
    uint64_t hop_value = 0;
};
HRESULT native_queue_hop(Context *ctx, NativeRuntimeQueue *queue, bool before) {
    // before: work already queued on the proxy (runtime waits) precedes ours.
    // after: the runtime's following signals/presents follow our work.
    auto &cb = ctx->kernel_callbacks;
    if (!cb.pfnSignalSynchronizationObjectFromGpuCb || !cb.pfnWaitForSynchronizationObjectFromGpuCb)
        return DXGI_ERROR_UNSUPPORTED;
    const uint64_t value = ++queue->hop_value;
    const D3DKMT_HANDLE fence = before ? queue->hop_in : queue->hop_out;
    D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU signal{};
    signal.hContext = before ? queue->native_heap_context : ctx->native_heap_context;
    signal.ObjectCount = 1; signal.ObjectHandleArray = &fence; signal.MonitoredFenceValueArray = &value;
    HRESULT hr = cb.pfnSignalSynchronizationObjectFromGpuCb(ctx->runtime_device.handle, &signal);
    if (FAILED(hr)) { native_trace("queue hop signal before=%d hr=%08x", before, (unsigned)hr); return hr; }
    D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait{};
    wait.hContext = before ? ctx->native_heap_context : queue->native_heap_context;
    wait.ObjectCount = 1; wait.ObjectHandleArray = &fence; wait.MonitoredFenceValueArray = &value;
    hr = cb.pfnWaitForSynchronizationObjectFromGpuCb(ctx->runtime_device.handle, &wait);
    if (FAILED(hr)) native_trace("queue hop wait before=%d hr=%08x", before, (unsigned)hr);
    return hr;
}
HRESULT native_queue_hop_fence(Context *ctx, D3DKMT_HANDLE *out) {
    if (!ctx->kernel_callbacks.pfnCreateSynchronizationObject2Cb) return DXGI_ERROR_UNSUPPORTED;
    D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 args{};
    args.Info.Type = D3DDDI_MONITORED_FENCE;
    args.Info.MonitoredFence.InitialFenceValue = 0;
    args.Info.MonitoredFence.EngineAffinity = 0;
    const HRESULT hr = ctx->kernel_callbacks.pfnCreateSynchronizationObject2Cb(ctx->runtime_device.handle, &args);
    *out = SUCCEEDED(hr) ? args.hSyncObject : 0;
    return SUCCEEDED(hr) && !*out ? E_FAIL : hr;
}
void native_queue_hop_destroy(Context *ctx, NativeRuntimeQueue *queue) {
    for (auto *fence : {&queue->hop_in, &queue->hop_out}) {
        if (!*fence || !ctx->kernel_callbacks.pfnDestroySynchronizationObjectCb) continue;
        D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT args{}; args.hSyncObject = *fence;
        if (SUCCEEDED(ctx->kernel_callbacks.pfnDestroySynchronizationObjectCb(ctx->runtime_device.handle, &args))) *fence = 0;
    }
}

NativeRuntimeQueue *native_runtime_queue_token(Context *ctx, void *token) {
    for (auto *queue = ctx->native_runtime_queues; queue; queue = queue->next)
        if (queue == token) return queue;
    return nullptr;
}
void native_runtime_queue_release_record(Context *ctx, NativeRuntimeQueue *queue) {
    if (--queue->users || queue->native_heap_context) return;
    auto **link = &ctx->native_runtime_queues;
    while (*link && *link != queue) link = &(*link)->next;
    if (*link) *link = queue->next;
    delete queue;
}
int32_t MWD_CALL native_runtime_queue_retain(void *owner, void *token) {
    NativeHeapOperation op(static_cast<Context *>(owner));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    auto *queue = native_runtime_queue_token(op.ctx, token);
    if (!queue || queue->pending || queue->closing || !queue->native_heap_context ||
            !queue->users || queue->users == UINT32_MAX) return E_INVALIDARG;
    ++queue->users;
    return S_OK;
}
int32_t MWD_CALL native_runtime_queue_release(void *owner, void *token) {
    NativeHeapOperation op(static_cast<Context *>(owner), true);
    if (!op.ctx) return E_INVALIDARG;
    auto *queue = native_runtime_queue_token(op.ctx, token);
    if (!queue || !queue->users) return E_INVALIDARG;
    native_runtime_queue_release_record(op.ctx, queue);
    return S_OK;
}

HRESULT native_runtime_queue_close(Context *ctx, NativeRuntimeQueue *queue) {
    queue->closing = true;
    const auto runtime = queue->runtime;
    queue->runtime = {}; // No retry can use an expired hRTCommandQueue.
    if (!queue->native_heap_context || ctx->native_retiring) return S_OK;
    if (queue->pending || queue->submissions || ctx->native_submitting || !runtime.handle) {
        // Abnormal recursive teardown cannot wait for the Render callback on
        // this same stack. Cancel the device; OS device teardown owns handles.
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    D3DDDICB_DESTROYCONTEXT args{};
    args.hContext = queue->native_heap_context;
    HRESULT hr = ctx->runtime_callbacks.pfnDestroyContextCb(runtime, &args);
    if (SUCCEEDED(hr)) queue->native_heap_context = nullptr;
    if (queue->proxy) native_queue_hop_destroy(ctx, queue);
    return hr;
}

HRESULT native_runtime_queue_open(Context *ctx, D3D12DDI_HRTCOMMANDQUEUE runtime,
        NativeRuntimeQueue **out) {
    *out = nullptr;
    if (!ctx->runtime_callbacks.pfnCreateContextCb || !ctx->runtime_callbacks.pfnDestroyContextCb)
        return DXGI_ERROR_UNSUPPORTED;
    NativeAdapterFacts current_facts{};
    HRESULT hr = native_query_adapter_facts(*ctx->native_adapter, current_facts, true);
    // QueryAdapterInfo may synchronously retire this device. The operation
    // retains Context, but no runtime handle may be used after that callback.
    if (ctx->native_retiring || ctx->native_destroying || hr == DXGI_ERROR_UNSUPPORTED)
        return DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(hr)) return hr;
    if (!native_owned_facts_valid(current_facts) ||
            !native_same_identity_facts(current_facts, ctx->native_facts))
        return DXGI_ERROR_DEVICE_REMOVED;
    hr = native_heap_open_context(ctx);
    if (FAILED(hr)) { native_trace("queue_open: heap context hr=%08x", (unsigned)hr); return hr; }
    auto *queue = new (std::nothrow) NativeRuntimeQueue;
    if (!queue) return E_OUTOFMEMORY;
    queue->runtime = runtime;
    queue->next = ctx->native_runtime_queues; ctx->native_runtime_queues = queue;
    if (ctx->native_surfaces) {
        // No private data selects the KMD's standard context: never a domain
        // child, so the owner context stays eligible for host-surface sharing.
        queue->proxy = true;
        D3DDDICB_CREATECONTEXT args{};
        args.EngineAffinity = 1;
        hr = ctx->runtime_callbacks.pfnCreateContextCb(runtime, &args);
        native_trace("queue_open: proxy CreateContextCb hr=%08x ctx=%x", (unsigned)hr, (unsigned)(uintptr_t)args.hContext);
        queue->native_heap_context = args.hContext;
        if (ctx->native_retiring) hr = DXGI_ERROR_DEVICE_REMOVED;
        if (SUCCEEDED(hr) && !args.hContext) hr = E_FAIL;
        if (SUCCEEDED(hr)) hr = native_queue_hop_fence(ctx, &queue->hop_in);
        if (SUCCEEDED(hr)) hr = native_queue_hop_fence(ctx, &queue->hop_out);
        native_trace("queue_open: proxy fences hr=%08x in=%x out=%x", (unsigned)hr, queue->hop_in, queue->hop_out);
        queue->pending = false;
        if (SUCCEEDED(hr)) { *out = queue; return hr; }
        native_runtime_queue_close(ctx, queue);
        native_queue_hop_destroy(ctx, queue);
        native_runtime_queue_release_record(ctx, queue);
        return hr;
    }
    NativeContextCreateShared data{};
    data.base.header = native_header(sizeof(data));
    data.base.generation = ctx->native_adapter->generation;
    data.base.flags = 1; data.allocation_context_id = ctx->native_context_id;
    D3DDDICB_CREATECONTEXT args{};
    args.EngineAffinity = 1; args.pPrivateDriverData = &data; args.PrivateDriverDataSize = sizeof(data);
    hr = ctx->runtime_callbacks.pfnCreateContextCb(runtime, &args);
    native_trace("queue_open: CreateContextCb hr=%08x ctx=%x cmd=%p/%u alloc=%p/%u patch=%p/%u", (unsigned)hr,
        (unsigned)args.hContext, args.pCommandBuffer, args.CommandBufferSize, (void *)args.pAllocationList,
        args.AllocationListSize, (void *)args.pPatchLocationList, args.PatchLocationListSize);
    queue->native_heap_context = args.hContext;
    if (ctx->native_retiring) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr) && (!args.hContext || !args.pCommandBuffer || !args.pAllocationList ||
            !args.pPatchLocationList || args.CommandBufferSize < 64 || args.CommandBufferSize > 65536 ||
            !args.AllocationListSize || args.AllocationListSize > 1024 ||
            !args.PatchLocationListSize || args.PatchLocationListSize > 1024)) hr = E_FAIL;
    NativeContextInfo info{};
    if (SUCCEEDED(hr)) {
        info.header = native_header(sizeof(info)); info.opcode = 1;
        info.expected_generation = ctx->native_adapter->generation;
        D3DDDICB_ESCAPE escape{};
        escape.hDevice = ctx->runtime_device.handle; escape.hContext = args.hContext;
        escape.pPrivateDriverData = &info; escape.PrivateDriverDataSize = sizeof(info);
        hr = ctx->kernel_callbacks.pfnEscapeCb(ctx->native_adapter->runtime.handle, &escape);
        native_trace("queue_open: context info escape hr=%08x", (unsigned)hr);
        if (ctx->native_retiring) hr = DXGI_ERROR_DEVICE_REMOVED;
    }
    if (SUCCEEDED(hr) && (!native_header_valid(info.header, sizeof(info)) || info.opcode != 1 || info.flags ||
            info.expected_generation != ctx->native_adapter->generation || info.generation != info.expected_generation ||
            info.context_id != ctx->native_context_id || info.queue_id != ctx->native_queue_id ||
            info.va_start != ctx->native_va_start || info.va_size != ctx->native_va_size)) hr = E_FAIL;
    queue->pending = false;
    if (SUCCEEDED(hr)) {
        queue->native_commands = args.pCommandBuffer; queue->native_command_capacity = args.CommandBufferSize;
        queue->native_allocation_list = args.pAllocationList; queue->native_allocation_capacity = args.AllocationListSize;
        queue->native_patch_list = args.pPatchLocationList; queue->native_patch_capacity = args.PatchLocationListSize;
        *out = queue;
    } else {
        native_runtime_queue_close(ctx, queue);
        native_runtime_queue_release_record(ctx, queue);
    }
    return hr;
}

void native_runtime_queues_forget(Context *ctx) {
    while (auto *queue = ctx->native_runtime_queues) {
        ctx->native_runtime_queues = queue->next;
        delete queue;
    }
}
