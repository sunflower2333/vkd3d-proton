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

void native_query_command(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HQUERYHEAP heap,
        D3D12DDI_QUERY_TYPE type, UINT first, UINT count, D3D12DDI_HRESOURCE destination,
        UINT64 offset, int operation) {
    NativeGraphicsCommand command(h);
    if (!command.value) { command.report(E_INVALIDARG); return; }
    auto *ctx = command.op.ctx;
    HRESULT hr = ctx->native_adapter ? native_heap_generation(ctx) : S_OK;
    vkdu_object *query = nullptr, *buffer = nullptr;
    if (SUCCEEDED(hr)) {
        AcquireSRWLockShared(&ctx->resources_lock);
        // Opaque slots are compared before dereference, including foreign/stale handles.
        auto *entry = native_submission_find(ctx->query_heaps, heap.pDrvPrivate);
        auto *resource = operation == 2 ? find_buffer(ctx, destination.pDrvPrivate) : nullptr;
        if (!entry || (operation == 2 && !resource)) hr = E_INVALIDARG;
        else {
            if (vkdu_object_retain(entry->backend)) query = entry->backend;
            if (resource && vkdu_object_retain(resource->backend)) buffer = resource->backend;
            if (!query || (resource && !buffer)) hr = E_OUTOFMEMORY;
        }
        ReleaseSRWLockShared(&ctx->resources_lock);
    }
    {
        NativeBackendCall call(ctx);
        if (SUCCEEDED(hr))
            hr = operation == 2 ? vkdu_command_query_resolve(command.value, query, type, first, count, buffer, offset) :
                vkdu_command_query(command.value, query, type, first, operation == 0);
        vkdu_object_destroy(buffer); vkdu_object_destroy(query);
    }
    if (SUCCEEDED(hr) && ctx->native_adapter) hr = native_heap_generation(ctx);
    command.report(hr);
}
void APIENTRY native_begin_query(D3D12DDI_HCOMMANDLIST command, D3D12DDI_HQUERYHEAP heap,
        D3D12DDI_QUERY_TYPE type, UINT index) {
    native_query_command(command, heap, type, index, 0, {}, 0, 0);
}
void APIENTRY native_end_query(D3D12DDI_HCOMMANDLIST command, D3D12DDI_HQUERYHEAP heap,
        D3D12DDI_QUERY_TYPE type, UINT index) {
    native_query_command(command, heap, type, index, 0, {}, 0, 1);
}
void APIENTRY native_resolve_query(D3D12DDI_HCOMMANDLIST command, D3D12DDI_HQUERYHEAP heap,
        D3D12DDI_QUERY_TYPE type, UINT first, UINT count, D3D12DDI_HRESOURCE destination, UINT64 offset) {
    native_query_command(command, heap, type, first, count, destination, offset, 2);
}
void native_query_tables(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *commands) {
    commands->pfnBeginQuery = native_begin_query;
    commands->pfnEndQuery = native_end_query;
    commands->pfnResolveQueryData = native_resolve_query;
}
