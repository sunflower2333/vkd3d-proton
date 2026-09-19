/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* WDDM 1.2 context-ordered CPU notification. This is not a D3D12 application
 * monitored fence, and neither GPUVA placement is interpreted as a pointer. */
constexpr unsigned native_submission_limit = 64;
struct NativeSubmission {
    NativeSubmission *next = nullptr;
    HANDLE event = nullptr;
    HANDLE context = nullptr;
    NativeRuntimeQueue *queue = nullptr;
    uint32_t fence = 0;
    std::unique_ptr<NativeHeap *[]> heaps;
    unsigned count = 0, released = 0;
    bool pending = true, armed = false;
    ~NativeSubmission() { if (event) CloseHandle(event); }
};

void native_submission_forget(Context *ctx) {
    // Runtime device teardown owns all residual KMT handles. Windows owns a
    // reference to accepted event objects; only our user handles close here.
    while (ctx->native_submissions) {
        auto *entry = ctx->native_submissions;
        ctx->native_submissions = entry->next;
        delete entry;
    }
    ctx->native_submission_count = 0;
}

HRESULT native_submission_reap(Context *ctx) {
    if (ctx->native_retiring) return DXGI_ERROR_DEVICE_REMOVED;
    if (ctx->native_reaping || ctx->native_submitting) return S_OK;
    struct Reaping {
        Context *ctx;
        explicit Reaping(Context *c) : ctx(c) { ctx->native_reaping = true; }
        ~Reaping() { ctx->native_reaping = false; }
    } reaping(ctx);
    while (auto *entry = ctx->native_submissions) {
        if (entry->pending) return S_OK;
        if (!entry->armed) {
            if (entry->queue && entry->queue->closing) return DXGI_ERROR_DEVICE_REMOVED;
            // A failed enqueue after Render must retain every owner. Retrying
            // on the same context is safe: it only moves completion later.
            D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 args{};
            args.hContext = entry->context;
            args.Flags.EnqueueCpuEvent = 1;
            args.CpuEventHandle = entry->event;
            HRESULT hr = ctx->kernel_callbacks.pfnSignalSynchronizationObject2Cb(ctx->runtime_device.handle, &args);
            if (ctx->native_retiring) return DXGI_ERROR_DEVICE_REMOVED;
            if (FAILED(hr)) return hr;
            entry->armed = true;
        }
        const DWORD state = WaitForSingleObject(entry->event, 0);
        if (state == WAIT_TIMEOUT) return S_OK;
        if (state != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(GetLastError());
        // FIFO retirement and the real OS event cover GPU completion, not
        // merely the return of RenderCb or the submission of a DMA packet.
        while (entry->released != entry->count) {
            auto *heap = entry->heaps[entry->released];
            if (heap->pending) return S_OK;
            if (heap->users == 1) {
                HRESULT hr = native_heap_cleanup(ctx, heap);
                if (ctx->native_retiring) return DXGI_ERROR_DEVICE_REMOVED;
                if (FAILED(hr)) return hr;
                native_heap_erase(ctx, heap);
            } else {
                --heap->users;
                --heap->submitted;
            }
            ++entry->released;
        }
        ctx->native_submissions = entry->next;
        --ctx->native_submission_count;
        ctx->native_os_completed = entry->fence;
        if (entry->queue) {
            --entry->queue->submissions;
            native_runtime_queue_release_record(ctx, entry->queue);
        }
        delete entry;
    }
    return S_OK;
}

HRESULT native_runtime_queue_drain(Context *ctx, NativeRuntimeQueue *queue) {
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (queue->submissions) {
        HRESULT hr = native_submission_reap(ctx);
        if (FAILED(hr) || !queue->submissions) return hr;
        auto *entry = ctx->native_submissions;
        if (!entry || entry->pending || !entry->armed || ctx->native_submitting || ctx->native_reaping)
            return DXGI_ERROR_DEVICE_REMOVED;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return DXGI_ERROR_DEVICE_HUNG;
        HANDLE event = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), entry->event, GetCurrentProcess(), &event,
                0, FALSE, DUPLICATE_SAME_ACCESS)) return HRESULT_FROM_WIN32(GetLastError());
        DWORD state;
        {
            NativeBackendCall call(ctx);
            state = WaitForSingleObject(event, static_cast<DWORD>(deadline - now));
        }
        CloseHandle(event);
        if (ctx->native_retiring || queue->closing) return DXGI_ERROR_DEVICE_REMOVED;
        if (state != WAIT_OBJECT_0) return DXGI_ERROR_DEVICE_HUNG;
    }
    return S_OK;
}
