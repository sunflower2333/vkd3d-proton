/* SPDX-License-Identifier: LGPL-2.1-or-later */
HRESULT native_query_desc(const D3D12DDIARG_CREATE_QUERY_HEAP_0001 *args) {
    if (!args || !args->Count || args->NodeMask > 1) return E_INVALIDARG;
    return args->Type >= D3D12DDI_QUERY_HEAP_TYPE_OCCLUSION &&
        args->Type <= D3D12DDI_QUERY_HEAP_TYPE_SO_STATISTICS ? S_OK : DXGI_ERROR_UNSUPPORTED;
}
SIZE_T APIENTRY native_query_size(D3D12DDI_HDEVICE device,
        const D3D12DDIARG_CREATE_QUERY_HEAP_0001 *args) {
    return context(device) && SUCCEEDED(native_query_desc(args)) ? sizeof(Object) : 0;
}
HRESULT APIENTRY native_query_create(D3D12DDI_HDEVICE device,
        const D3D12DDIARG_CREATE_QUERY_HEAP_0001 *args, D3D12DDI_HQUERYHEAP slot) {
    NativeHeapOperation op(context(device));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    HRESULT hr = native_query_desc(args);
    if (FAILED(hr) || !slot.pDrvPrivate) return FAILED(hr) ? hr : E_INVALIDARG;
    const auto request = *args;
    if (object(slot.pDrvPrivate)) return E_INVALIDARG;
    if (FAILED(hr = native_heap_generation(op.ctx))) return hr;
    vkdu_object *value = nullptr;
    {
        NativeBackendCall call(op.ctx);
        hr = vkdu_query_heap_create(op.ctx->backend, request.Type, request.Count, &value);
    }
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) hr = native_heap_generation(op.ctx);
    if (SUCCEEDED(hr)) hr = bind(op.ctx, slot.pDrvPrivate, value, VKDU_QUERY_HEAP);
    if (FAILED(hr)) { NativeBackendCall call(op.ctx); vkdu_object_destroy(value); }
    return hr;
}
void APIENTRY native_query_destroy(D3D12DDI_HDEVICE device, D3D12DDI_HQUERYHEAP slot) {
    NativeHeapOperation op(context(device), true);
    if (!op.retained) return;
    AcquireSRWLockExclusive(&op.ctx->resources_lock);
    auto **link = &op.ctx->query_heaps;
    while (*link && *link != slot.pDrvPrivate) link = &(*link)->next;
    auto *entry = *link;
    vkdu_object *value = nullptr;
    if (entry) { *link = entry->next; value = entry->backend; std::memset(entry, 0, sizeof(*entry)); }
    ReleaseSRWLockExclusive(&op.ctx->resources_lock);
    if (!entry) { error(op.ctx, E_INVALIDARG); return; }
    { NativeBackendCall call(op.ctx); vkdu_object_destroy(value); }
    release(op.ctx);
}
