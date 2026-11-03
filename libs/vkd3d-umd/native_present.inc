/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* CORE_0003 preparation records work into the runtime-provided command list.
 * It does not call PresentCb or submit a second queue behind the runtime. The
 * runtime consumes AddedGpuWork and the returned associated context. SYSTEM
 * execution/presentation remains an admission requirement, not fixture proof. */
void APIENTRY native_present(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HCOMMANDQUEUE queue_handle,
        const D3D12DDIARG_PRESENT_0001 *args, D3D12DDI_PRESENT_0003 *out) {
    NativeGraphicsCommand command(list);
    if (out) *out = {};
    if (!command.value || !out || !args) { command.report(E_INVALIDARG); return; }
    const auto request = *args;
    if (request.SurfacesToPresent != 1 || !request.phSurfacesToPresent ||
            request.DstSubResourceIndex || request.DirtyRects || request.pDirtyRects ||
            request.PrivateDriverDataSize || request.pPrivateDriverData || request.VidPnSourceID ||
            (request.Flags.Value & ~0x53u) || (request.Flags.Blt && request.Flags.Flip) ||
            static_cast<UINT>(request.FlipInterval) > 4 ||
            (request.OptimizeForComposition != FALSE && request.OptimizeForComposition != TRUE)) {
        command.report(DXGI_ERROR_UNSUPPORTED); return;
    }
    const auto source = *request.phSurfacesToPresent;
    if (!source.hSurface.pDrvPrivate || source.SubResourceIndex ||
            source.hSurface.pDrvPrivate == request.hDstResource.pDrvPrivate ||
            (request.Flags.Flip && request.hDstResource.pDrvPrivate)) {
        command.report(E_INVALIDARG); return;
    }
    auto *ctx = command.op.ctx;
    if (!ctx->native_surfaces) { command.report(DXGI_ERROR_UNSUPPORTED); return; }
    auto *queue = native_submission_find(ctx->native_queue_objects, queue_handle.pDrvPrivate);
    auto *route = queue ? queue->runtime_route : nullptr;
    if (!queue || queue->kind != VKDU_QUEUE || !route || route->pending || route->closing ||
            !route->native_heap_context || !route->runtime.handle || !route->users || route->users == UINT32_MAX ||
            route->runtime.handle != queue->runtime_queue.handle ||
            !native_submission_find(ctx->native_command_objects, list.pDrvPrivate)) {
        command.report(E_INVALIDARG); return;
    }
    if (ctx->native_submitting || route->preparing_present) {
        command.report(DXGI_ERROR_WAS_STILL_DRAWING); return;
    }
    auto *queue_backend = queue->backend;
    if (!vkdu_object_retain(queue_backend)) { command.report(E_OUTOFMEMORY); return; }
    ++route->users; route->preparing_present = true;
    const auto context_handle = route->native_heap_context;
    NativeResidencyBatch batch(ctx);
    vkdu_object *images[2]{};
    NativeHeap *heaps[2]{};
    void *slots[2] = {source.hSurface.pDrvPrivate, request.hDstResource.pDrvPrivate};
    const UINT count = slots[1] ? 2 : 1;
    HRESULT hr = S_OK;
    for (UINT i = 0; i < count && SUCCEEDED(hr); ++i) {
        AcquireSRWLockShared(&ctx->resources_lock);
        auto *resource = find_resource(ctx, slots[i]);
        auto *heap = resource ? resource->native_heap : nullptr;
        if (!resource || resource->kind != VKDU_TEXTURE2D || !heap || heap->flags != 20 ||
                !heap->resource_created || !heap->runtime_resource || !vkdu_object_retain(resource->backend)) {
            hr = E_INVALIDARG;
        } else {
            images[i] = resource->backend; heaps[i] = heap;
            batch.owners[batch.owner_count++] = images[i];
        }
        ReleaseSRWLockShared(&ctx->resources_lock);
        if (SUCCEEDED(hr)) hr = batch.add(heap, false, false);
    }
    if (SUCCEEDED(hr) && count == 2 && heaps[0]->native_mapping == heaps[1]->native_mapping)
        hr = E_INVALIDARG; // Aliases cannot be both source and destination.
    D3D12DDI_PRESENT_0003 result{};
    if (SUCCEEDED(hr)) {
        result.hSrcAllocation = heaps[0]->allocation;
        result.hDstAllocation = count == 2 ? heaps[1]->allocation : 0;
        result.hContext = route->native_heap_context;
        result.BackBufferMultiplicity = 1;
        // Everything below may call back into the runtime. No caller-owned
        // request, image slot or queue slot is read without registry validation.
        hr = native_heap_generation(ctx);
    }
    if (SUCCEEDED(hr) && !route->closing && route->runtime.handle && command.op.live()) {
        NativeBackendCall call(ctx);
        hr = vkdu_command_native_present(command.value, queue_backend, images[0], images[1]);
        if (hr != S_OK && SUCCEEDED(hr)) hr = E_FAIL;
    } else if (SUCCEEDED(hr)) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
    // Cleanup can reenter too. Clear all alias exclusions before dropping any
    // retained image/backend/token, then check publication after every cleanup.
    const HRESULT cleanup = batch.clear();
    if (SUCCEEDED(hr)) hr = cleanup;
    { NativeBackendCall call(ctx); vkdu_object_destroy(queue_backend); }
    if (!command.op.live() || route->closing || route->native_heap_context != context_handle ||
            !native_submission_find(ctx->native_queue_objects, queue_handle.pDrvPrivate) ||
            !native_submission_find(ctx->native_command_objects, list.pDrvPrivate)) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) {
        AcquireSRWLockShared(&ctx->resources_lock);
        for (UINT i = 0; i < count; ++i) {
            auto *resource = find_resource(ctx, slots[i]);
            if (!resource || resource->backend != images[i] || resource->native_heap != heaps[i]) hr = E_INVALIDARG;
        }
        ReleaseSRWLockShared(&ctx->resources_lock);
    }
    route->preparing_present = false;
    native_runtime_queue_release_record(ctx, route);
    if (FAILED(hr)) { command.report(hr); return; }
    result.AddedGpuWork = TRUE;
    *out = result; // No callbacks after output publication in this scope.
}
UINT APIENTRY native_present_private_size(D3D12DDI_HDEVICE, const D3D12DDIARG_PRESENT_0001 *) {
    return 0;
}
void native_present_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *table, D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *commands) {
    commands->pfnPresent = native_present;
    table->pfnGetPresentPrivateDriverDataSize = native_present_private_size;
}
