/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Physical, single-node D3D12 fence metadata. The runtime owns application
 * synchronization objects and emits its external packets on the context
 * associated with hRTCommandQueue. These DDIs select the adapter broadcast
 * mask; they must not create an unrelated Vulkan fence or cast a GPUVA. */
constexpr uint32_t native_fence_magic = 0x564b464e;
struct NativeFenceSlot { uint32_t magic; };
struct NativeFence {
    NativeFence *next = nullptr;
    void *slot = nullptr;
    D3D12DDI_FENCE description{};
};

HRESULT native_fence_description(const D3D12DDIARG_CREATE_FENCE *args,
        D3D12DDI_FENCE *snapshot) {
    if (!args || !args->FenceCount || !args->Fences) return E_INVALIDARG;
    if (args->FenceCount != 1) return DXGI_ERROR_UNSUPPORTED;
    const auto description = args->Fences[0];
    if (static_cast<UINT>(description.Flags) & ~static_cast<UINT>(D3D12DDI_FENCE_FLAG_BOTTOM_OF_PIPE))
        return DXGI_ERROR_UNSUPPORTED;
    // Addresses are opaque metadata, including zero for software-managed
    // fences. Neither field is a CPU mapping, native allocation or KMT handle.
    if (snapshot) *snapshot = description;
    return S_OK;
}

NativeFence *native_fence_find(Context *ctx, void *slot) {
    if (!slot) return nullptr;
    for (auto *fence = ctx->native_fences; fence; fence = fence->next)
        if (fence->slot == slot) return fence;
    return nullptr;
}

SIZE_T APIENTRY native_fence_size(D3D12DDI_HDEVICE device,
        const D3D12DDIARG_CREATE_FENCE *args) {
    NativeHeapOperation op(context(device));
    return op.live() && SUCCEEDED(native_fence_description(args, nullptr)) ? sizeof(NativeFenceSlot) : 0;
}

HRESULT APIENTRY native_fence_create(D3D12DDI_HDEVICE device,
        D3D12DDI_HFENCE handle, const D3D12DDIARG_CREATE_FENCE *args) {
    NativeHeapOperation op(context(device));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    D3D12DDI_FENCE description{};
    HRESULT hr = native_fence_description(args, &description);
    if (FAILED(hr)) return hr;
    if (!handle.pDrvPrivate || native_fence_find(op.ctx, handle.pDrvPrivate)) return E_INVALIDARG;
    auto *fence = new (std::nothrow) NativeFence;
    if (!fence) return E_OUTOFMEMORY;
    fence->slot = handle.pDrvPrivate;
    fence->description = description;
    fence->next = op.ctx->native_fences;
    op.ctx->native_fences = fence;
    ++op.ctx->references;
    // Runtime storage need not be initialized, and is never deleted here.
    new (handle.pDrvPrivate) NativeFenceSlot{native_fence_magic};
    return S_OK;
}

void APIENTRY native_fence_destroy(D3D12DDI_HDEVICE device, D3D12DDI_HFENCE handle) {
    NativeHeapOperation op(context(device), true);
    if (!op.retained) return;
    auto *fence = native_fence_find(op.ctx, handle.pDrvPrivate);
    if (!fence) { error(op.ctx, E_INVALIDARG); return; }
    auto **link = &op.ctx->native_fences;
    while (*link != fence) link = &(*link)->next;
    *link = fence->next;
    std::memset(fence->slot, 0, sizeof(NativeFenceSlot));
    delete fence;
    release(op.ctx);
}

void native_fences_retire(Context *ctx) {
    // These records own no backend/KMT object. On device retirement detach
    // metadata without touching runtime storage, which may already be expired.
    while (auto *fence = ctx->native_fences) {
        ctx->native_fences = fence->next;
        delete fence;
        release(ctx);
    }
}

void APIENTRY native_queue_fence(D3D12DDI_HCOMMANDQUEUE handle,
        D3D12DDIARG_FENCE_OPERATION *args) {
    // Clear the only output before any runtime callback. SetErrorCb may destroy
    // the device, queue, fence and this request, then return to this stack.
    if (args) args->PhysicalAdapterMask = 0;
    auto *queue = object(handle.pDrvPrivate);
    if (!queue) return;
    auto *ctx = queue->context;
    NativeHeapOperation op(ctx);
    if (!op.live()) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    const HRESULT prior = ctx->last_error.load();
    if (prior == DXGI_ERROR_DEVICE_REMOVED || prior == DXGI_ERROR_DEVICE_RESET ||
            prior == DXGI_ERROR_DEVICE_HUNG || prior == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        error(ctx, prior); return;
    }
    if (!args || queue->kind != VKDU_QUEUE ||
            !native_submission_find(ctx->native_queue_objects, queue)) {
        error(ctx, E_INVALIDARG); return;
    }
    auto *route = native_runtime_queue_token(ctx, queue->runtime_route);
    if (!route || route->pending || route->closing || !route->native_heap_context ||
            !route->runtime.handle || route->runtime.handle != queue->runtime_queue.handle) {
        error(ctx, DXGI_ERROR_DEVICE_REMOVED); return;
    }
    // A callback must not recursively publish an external packet while this
    // device is still inside RenderCb, before software enqueue is established.
    if (ctx->native_submitting) { error(ctx, DXGI_ERROR_WAS_STILL_DRAWING); return; }
    const bool owned = native_fence_find(ctx, args->Fence.pDrvPrivate) != nullptr;
#ifdef VKDU_TEST_NATIVE_FENCE_CONTROLS
    const bool accepted = owned || native_fence_ignore_owner;
#else
    const bool accepted = owned;
#endif
    if (!accepted) { error(ctx, E_INVALIDARG); return; }
    // Single physical node: EngineAffinity=1, NodeOrdinal=0. Execute already
    // drains all software translation/Render enqueue before returning, so the
    // runtime's following signal/wait packet cannot overtake that work.
    // Value (including a rewind) belongs to the runtime; leave it untouched.
    args->PhysicalAdapterMask = 1;
#ifdef VKDU_TEST_NATIVE_FENCE_CONTROLS
    if (native_fence_drop_mask) args->PhysicalAdapterMask = 0;
#endif
}

void native_fence_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *device,
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue) {
    device->pfnCalcPrivateFenceSize = native_fence_size;
    device->pfnCreateFence = native_fence_create;
    device->pfnDestroyFence = native_fence_destroy;
    queue->pfnSignalFence = native_queue_fence;
    queue->pfnWaitForFence = native_queue_fence;
}
