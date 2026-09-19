/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native residency through the runtime's own paging queue. The optional
 * provider enumerates actual descriptor/query backing allocations. */
#ifdef VKDU_TEST_NATIVE_RESIDENCY_CONTROLS
static bool native_residency_drop_pending, native_residency_skip_evict;
#endif
struct NativeResidencyBatch {
    Context *ctx;
    std::array<NativeHeap *, 192> heaps{};
    std::array<D3DKMT_HANDLE, 192> allocations{};
    std::array<vkdu_object *, 64> owners{};
    UINT count = 0;
    UINT owner_count = 0;
    explicit NativeResidencyBatch(Context *value) : ctx(value) {}
    HRESULT clear() {
        HRESULT result = S_OK;
        // Backend destructors can unmap/release their BOs. Drop exclusion first
        // while the batch still owns every allocation token independently.
        for (UINT i = 0; i < count; ++i) --heaps[i]->residency_operations;
        while (owner_count) {
            NativeBackendCall call(ctx);
            vkdu_object_destroy(owners[--owner_count]);
        }
        while (count) {
            auto *heap = heaps[--count];
            const HRESULT hr = native_runtime_release(ctx, heap);
            if (FAILED(hr) && SUCCEEDED(result)) result = hr;
        }
        return result;
    }
    ~NativeResidencyBatch() { clear(); }
    HRESULT add(NativeHeap *heap, bool evict, bool adopted) {
        if (!heap || !heap->allocation || heap->pending || !heap->users ||
                (!adopted && heap->users == UINT32_MAX)) return E_INVALIDARG;
        for (UINT j = 0; j < count; ++j) {
            if (heaps[j] != heap) continue;
            return adopted ? native_runtime_release(ctx, heap) : E_INVALIDARG;
        }
        if (count == heaps.size()) return E_OUTOFMEMORY;
        // An ICD-owned descriptor/query allocation can remain persistently CPU
        // mapped. Forward eviction to the runtime, which decides whether that
        // backing can be evicted; never fake success or silently unmap pointers
        // the ICD still uses. Direct native MapHeap owners keep the busy guard.
        if (heap->residency_operations || (evict && (heap->submitted || (heap->locked && !adopted))))
            return DXGI_ERROR_WAS_STILL_DRAWING;
        if (!adopted) ++heap->users;
        ++heap->residency_operations;
        heaps[count] = heap; allocations[count++] = heap->allocation;
        return S_OK;
    }
    HRESULT collect(UINT number, const D3D12DDI_HANDLE_AND_TYPE *objects, bool evict) {
        if (number > owners.size() || (number && !objects)) return E_INVALIDARG;
        std::array<D3D12DDI_HANDLE_AND_TYPE, 64> copied{};
        if (number) std::memcpy(copied.data(), objects, number * sizeof(*objects));
        objects = copied.data();
        for (UINT i = 0; i < number; ++i) {
            for (UINT j = 0; j < i; ++j)
                if (objects[j].Handle == objects[i].Handle && objects[j].Type == objects[i].Type) return E_INVALIDARG;
            if (objects[i].Type == D3D12DDI_HT_HEAP) {
                HRESULT hr = add(native_heap_find(ctx, objects[i].Handle), evict, false);
                if (FAILED(hr)) return hr;
                continue;
            }
            if (objects[i].Type != D3D12DDI_HT_DESCRIPTOR_HEAP && objects[i].Type != D3D12DDI_HT_QUERY_HEAP)
                return DXGI_ERROR_UNSUPPORTED;
            AcquireSRWLockShared(&ctx->resources_lock);
            auto *entry = objects[i].Type == D3D12DDI_HT_DESCRIPTOR_HEAP ? ctx->descriptor_heaps : ctx->query_heaps;
            while (entry && entry != objects[i].Handle) entry = entry->next;
            auto *value = entry ? entry->backend : nullptr;
            const bool retained = value && vkdu_object_retain(value);
            ReleaseSRWLockShared(&ctx->resources_lock);
            if (!retained) return E_INVALIDARG;
            owners[owner_count++] = value;
            std::array<mwd_allocation, 3> backing{};
            uint32_t backing_count = 0;
            HRESULT hr;
            {
                NativeBackendCall call(ctx);
                hr = vkdu_pageable_backing(value, &backing_count, backing.data());
            }
            if (FAILED(hr)) return hr;
            if (backing_count > backing.size()) {
                backing_count = static_cast<uint32_t>(backing.size());
                hr = DXGI_ERROR_DRIVER_INTERNAL_ERROR;
            }
            if (ctx->native_retiring) hr = DXGI_ERROR_DEVICE_REMOVED;
            for (uint32_t j = 0; j < backing_count; ++j) {
                auto *heap = native_token(ctx, backing[j].token);
                if (SUCCEEDED(hr)) {
                    if (!heap || heap->flags != backing[j].flags ||
                            heap->allocation != backing[j].handle || heap->address != backing[j].address ||
                            heap->bytes != backing[j].size || backing[j].generation != ctx->native_adapter->generation)
                        hr = E_INVALIDARG;
                    else hr = add(heap, evict, true);
                    if (SUCCEEDED(hr)) continue;
                }
                native_runtime_release(ctx, backing[j].token);
            }
            if (FAILED(hr)) return hr;
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
    NativeResidencyBatch batch(op.ctx);
    HRESULT hr = batch.collect(request.NumObjects, request.pObjects, false);
    if (FAILED(hr)) return hr;
    if (batch.count && !op.ctx->runtime_callbacks.pfnMakeResidentCb) return DXGI_ERROR_UNSUPPORTED;
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
    NativeResidencyBatch batch(op.ctx);
    HRESULT hr = batch.collect(request.NumObjects, request.pObjects, true);
    if (FAILED(hr)) return hr;
    if (batch.count && !op.ctx->runtime_callbacks.pfnEvictCb) return DXGI_ERROR_UNSUPPORTED;
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
    device->pfnCalcPrivateQueryHeapSize = native_query_size;
    device->pfnCreateQueryHeap = native_query_create;
    device->pfnDestroyQueryHeap = native_query_destroy;
}
