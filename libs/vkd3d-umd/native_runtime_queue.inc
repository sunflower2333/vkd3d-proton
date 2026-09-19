/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Independent records survive runtime-owned slots and asynchronous ICD work. */
struct NativeRuntimeQueue : NativeContextBuffers {
    NativeRuntimeQueue *next = nullptr;
    D3D12DDI_HRTCOMMANDQUEUE runtime{};
    uint32_t users = 1, submissions = 0;
    bool pending = true, closing = false;
};

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
    return hr;
}

HRESULT native_runtime_queue_open(Context *ctx, D3D12DDI_HRTCOMMANDQUEUE runtime,
        NativeRuntimeQueue **out) {
    *out = nullptr;
    if (!ctx->runtime_callbacks.pfnCreateContextCb || !ctx->runtime_callbacks.pfnDestroyContextCb)
        return DXGI_ERROR_UNSUPPORTED;
    HRESULT hr = native_heap_open_context(ctx);
    if (FAILED(hr)) return hr;
    auto *queue = new (std::nothrow) NativeRuntimeQueue;
    if (!queue) return E_OUTOFMEMORY;
    queue->runtime = runtime;
    queue->next = ctx->native_runtime_queues; ctx->native_runtime_queues = queue;
    NativeContextCreateShared data{};
    data.base.header = native_header(sizeof(data));
    data.base.generation = ctx->native_adapter->generation;
    data.base.flags = 1; data.allocation_context_id = ctx->native_context_id;
    D3DDDICB_CREATECONTEXT args{};
    args.EngineAffinity = 1; args.pPrivateDriverData = &data; args.PrivateDriverDataSize = sizeof(data);
    hr = ctx->runtime_callbacks.pfnCreateContextCb(runtime, &args);
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
