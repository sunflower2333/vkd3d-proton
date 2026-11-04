/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native heap-backed resources only. Never dereference an unregistered slot,
 * confuse runtime HANDLEs with KMT handles, or infer image GPUVAs/row strides. */
struct NativeResourceSnapshot {
    D3D12DDI_RESOURCE_ALLOCATION_INFO info{};
    D3DKMT_HANDLE allocation = 0;
    uint64_t address = 0;
    uint64_t buffer_bytes = 0;
    uint64_t row_pitch = 0, depth_pitch = 0;
};

HRESULT native_resource_snapshot(NativeHeapOperation &op, void *slot, NativeResourceSnapshot &out) {
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (!slot) return E_INVALIDARG;
    // QueryAdapterInfoCb can synchronously destroy/rewrite the resource slot.
    // Check epoch before looking up the live resource, then copy without any
    // further callbacks while holding both the native and resource locks.
    HRESULT hr = native_heap_generation(op.ctx);
    if (FAILED(hr)) return hr;
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    hr = E_INVALIDARG;
    AcquireSRWLockShared(&op.ctx->resources_lock);
    auto *resource = find_resource(op.ctx, slot);
    if (resource && resource->native_heap && resource->native_resource_info.ResourceDataSize) {
        auto *heap = op.ctx->native_heaps;
        while (heap && heap != resource->native_heap) heap = heap->next;
        if (heap && heap->allocation) {
            if (heap->pending) hr = DXGI_ERROR_WAS_STILL_DRAWING;
            else {
                out.info = resource->native_resource_info;
                out.allocation = heap->allocation;
                out.address = resource->kind == VKDU_BUFFER ? resource->address : 0;
                out.buffer_bytes = resource->kind == VKDU_BUFFER ? resource->bytes : 0;
                out.row_pitch = resource->native_row_pitch;
                out.depth_pitch = resource->native_depth_pitch;
                hr = S_OK;
            }
        }
    }
    ReleaseSRWLockShared(&op.ctx->resources_lock);
    return hr;
}

void APIENTRY native_existing_allocation_info(D3D12DDI_HDEVICE h, D3D12DDI_HRESOURCE resource,
        D3D12DDI_RESOURCE_ALLOCATION_INFO *out, D3DKMT_HANDLE *allocation) {
    NativeHeapOperation op(context(h));
    if (!out || !allocation) { error(op.ctx, E_INVALIDARG); return; }
    NativeResourceSnapshot snapshot;
    const HRESULT hr = native_resource_snapshot(op, resource.pDrvPrivate, snapshot);
    if (FAILED(hr)) { error(op.ctx, hr); return; }
    *out = snapshot.info;
    *allocation = snapshot.allocation;
}

D3DKMT_HANDLE APIENTRY native_resource_allocation_handle(D3D12DDI_HDEVICE h, D3D10DDI_HRESOURCE resource) {
    NativeHeapOperation op(context(h));
    NativeResourceSnapshot snapshot;
    const HRESULT hr = native_resource_snapshot(op, resource.pDrvPrivate, snapshot);
    error(op.ctx, hr);
    return SUCCEEDED(hr) ? snapshot.allocation : 0;
}

D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY native_resource_address(D3D12DDI_HDEVICE h, D3D12DDI_HRESOURCE resource) {
    NativeHeapOperation op(context(h));
    NativeResourceSnapshot snapshot;
    const HRESULT hr = native_resource_snapshot(op, resource.pDrvPrivate, snapshot);
    error(op.ctx, hr);
    return SUCCEEDED(hr) ? snapshot.address : 0;
}

void APIENTRY native_subresource_info(D3D12DDI_HDEVICE h, D3D12DDI_HRESOURCE resource,
        UINT subresource, D3D12DDI_SUBRESOURCE_INFO *out) {
    NativeHeapOperation op(context(h));
    if (!out || subresource) { error(op.ctx, E_INVALIDARG); return; }
    NativeResourceSnapshot snapshot;
    HRESULT hr = native_resource_snapshot(op, resource.pDrvPrivate, snapshot);
    // Device-dependent optimal images do not expose a CPU row/swizzle layout.
    // GetCopyableFootprints is a transfer layout and cannot substitute for it.
    if (SUCCEEDED(hr) && ((!snapshot.buffer_bytes && (!snapshot.row_pitch || !snapshot.depth_pitch)) ||
            snapshot.info.Layout != D3D12DDI_TL_ROW_MAJOR))
        hr = DXGI_ERROR_UNSUPPORTED;
    if (FAILED(hr)) { error(op.ctx, hr); return; }
    D3D12DDI_SUBRESOURCE_INFO result{};
    result.RowStride = snapshot.buffer_bytes ? snapshot.buffer_bytes : snapshot.row_pitch;
    result.DepthStride = snapshot.buffer_bytes ? snapshot.buffer_bytes : snapshot.depth_pitch;
    *out = result;
}
