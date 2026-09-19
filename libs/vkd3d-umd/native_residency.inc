/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native heap residency through the runtime's own paging queue. Descriptor/
 * query backing enumeration and complete public admission remain separate. */
#ifdef VKDU_TEST_NATIVE_RESIDENCY_CONTROLS
static bool native_residency_drop_pending, native_residency_skip_evict;
#endif
struct NativeResidencyBatch {
    Context *ctx;
    std::array<NativeHeap *, 64> heaps{};
    std::array<D3DKMT_HANDLE, 64> allocations{};
    UINT count = 0;
    explicit NativeResidencyBatch(Context *value) : ctx(value) {}
    HRESULT clear() {
        HRESULT result = S_OK;
        while (count) {
            auto *heap = heaps[--count];
            --heap->residency_operations;
            const HRESULT hr = native_runtime_release(ctx, heap);
            if (FAILED(hr) && SUCCEEDED(result)) result = hr;
        }
        return result;
    }
    ~NativeResidencyBatch() { clear(); }
    HRESULT collect(UINT number, const D3D12DDI_HANDLE_AND_TYPE *objects, bool evict) {
        if (number > heaps.size() || (number && !objects)) return E_INVALIDARG;
        for (UINT i = 0; i < number; ++i) {
            if (objects[i].Type != D3D12DDI_HT_HEAP) return DXGI_ERROR_UNSUPPORTED;
            auto *heap = native_heap_find(ctx, objects[i].Handle);
            if (!heap || !heap->allocation || heap->pending || !heap->users || heap->users == UINT32_MAX)
                return E_INVALIDARG;
            for (UINT j = 0; j < count; ++j)
                if (heaps[j] == heap) return E_INVALIDARG;
            if (heap->residency_operations || (evict && (heap->submitted || heap->locked)))
                return DXGI_ERROR_WAS_STILL_DRAWING;
            ++heap->users;
            ++heap->residency_operations;
            heaps[count] = heap; allocations[count++] = heap->allocation;
        }
        return S_OK;
    }
};

HRESULT APIENTRY native_make_resident(D3D12DDI_HDEVICE device, D3D12DDIARG_MAKERESIDENT_0001 *args) {
    NativeHeapOperation op(context(device));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (!args || args->NumAdapters != 1 || !args->pRTPagingQueue ||
            !args->pRTPagingQueue[0].handle || !args->pPagingFenceValue) return E_INVALIDARG;
    const auto request = *args;
    const auto paging_queue = request.pRTPagingQueue[0];
    if ((request.Flags.Value & ~3u) || ((request.Flags.Value & 2u) && !(request.Flags.Value & 1u)))
        return E_INVALIDARG;
    if (request.NumObjects && !op.ctx->runtime_callbacks.pfnMakeResidentCb) return DXGI_ERROR_UNSUPPORTED;
    NativeResidencyBatch batch(op.ctx);
    HRESULT hr = batch.collect(request.NumObjects, request.pObjects, false);
    if (FAILED(hr)) return hr;
    hr = native_heap_generation(op.ctx);
    if (FAILED(hr)) return hr;
    D3DDDI_MAKERESIDENT make{};
    make.NumAllocations = batch.count;
    make.AllocationList = batch.allocations.data();
    make.Flags = request.Flags;
    const UINT count = batch.count;
    // The runtime translates its opaque paging-queue handle. hPagingQueue in
    // the kernel request stays zero; no pointer is reinterpreted as a KMT ID.
    if (count) hr = op.ctx->runtime_callbacks.pfnMakeResidentCb(op.ctx->runtime_device, paging_queue, &make);
#ifdef VKDU_TEST_NATIVE_RESIDENCY_CONTROLS
    if (native_residency_drop_pending && hr == E_PENDING) hr = S_OK;
#endif
    const bool accepted = hr == S_OK || hr == E_PENDING;
    const bool invalid_reply = accepted && make.NumAllocations != count;
    const UINT64 paging_fence = hr == E_PENDING ? make.PagingFenceValue : 0;
    const HRESULT cleanup = batch.clear();
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (invalid_reply || (SUCCEEDED(hr) && hr != S_OK)) {
        error(op.ctx, DXGI_ERROR_DRIVER_INTERNAL_ERROR);
        return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
    if (FAILED(cleanup)) return cleanup;
    if (accepted) {
        // E_PENDING has its HRESULT failure bit set, but conveys an accepted
        // residency request with a paging fence owned/waited by the runtime.
        *request.pPagingFenceValue = paging_fence;
        args->WaitMask = hr == E_PENDING ? 1u : 0u;
    }
    return hr;
}

HRESULT APIENTRY native_evict(D3D12DDI_HDEVICE device, const D3D12DDIARG_EVICT *args) {
    NativeHeapOperation op(context(device));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (!args || (args->Flags.Value & ~1u)) return E_INVALIDARG;
    // NotWrittenTo requires an explicit driver opt-in that this slice lacks.
    const auto request = *args;
    if (request.NumObjects && !op.ctx->runtime_callbacks.pfnEvictCb) return DXGI_ERROR_UNSUPPORTED;
    NativeResidencyBatch batch(op.ctx);
    HRESULT hr = batch.collect(request.NumObjects, request.pObjects, true);
    if (FAILED(hr)) return hr;
    hr = native_heap_generation(op.ctx);
    if (FAILED(hr)) return hr;
    D3DDDICB_EVICT evict{};
    evict.NumAllocations = batch.count;
    evict.AllocationList = batch.allocations.data();
    evict.Flags = request.Flags;
    bool dispatch = batch.count != 0;
#ifdef VKDU_TEST_NATIVE_RESIDENCY_CONTROLS
    dispatch = dispatch && !native_residency_skip_evict;
#endif
    if (dispatch) hr = op.ctx->runtime_callbacks.pfnEvictCb(op.ctx->runtime_device, &evict);
    const HRESULT cleanup = batch.clear();
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr) && hr != S_OK) {
        error(op.ctx, DXGI_ERROR_DRIVER_INTERNAL_ERROR);
        return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
    return FAILED(cleanup) ? cleanup : hr;
}

void native_residency_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *device) {
    device->pfnMakeResident = native_make_resident;
    device->pfnEvict = native_evict;
}
