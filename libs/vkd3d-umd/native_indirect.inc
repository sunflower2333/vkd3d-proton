/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Included after native heap ownership and registered-slot helpers. */
HRESULT indirect_signature_desc(const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *args) {
    if (!args || !args->NumArgumentDescs || !args->pArgumentDescs || args->NodeMask > 1 ||
            args->ByteStride < 12 || (args->ByteStride & 3)) return E_INVALIDARG;
    // Dispatch alone modifies no root arguments. Its signature has no root
    // signature association; draw and indirect root changes need their own DDIs.
    if (args->NumArgumentDescs != 1 || args->pArgumentDescs[0].Type != D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH)
        return DXGI_ERROR_UNSUPPORTED;
    return args->hRootSignature.pDrvPrivate ? E_INVALIDARG : S_OK;
}

SIZE_T APIENTRY indirect_signature_size(D3D12DDI_HDEVICE h,
        const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *args) {
    return context(h) && SUCCEEDED(indirect_signature_desc(args)) ? sizeof(Object) : 0;
}

HRESULT APIENTRY indirect_signature_create(D3D12DDI_HDEVICE h,
        const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *args, D3D12DDI_HCOMMANDSIGNATURE slot) {
    auto *ctx = context(h);
    if (!ctx || !slot.pDrvPrivate) return E_INVALIDARG;
    NativeHeapOperation op(ctx);
    if (!op.retained || (ctx->native_adapter && !op.live())) return DXGI_ERROR_DEVICE_REMOVED;
    HRESULT hr = indirect_signature_desc(args);
    if (FAILED(hr)) return hr;
    if (native_submission_find(ctx->indirect_signatures, slot.pDrvPrivate)) return E_INVALIDARG;
    const auto stride = args->ByteStride;
    vkdu_object *value = nullptr;
    {
        NativeBackendCall call(ctx);
        hr = vkdu_dispatch_signature_create(ctx->backend, stride, &value);
    }
    if (ctx->native_adapter && !op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) hr = bind(ctx, slot.pDrvPrivate, value, VKDU_COMMAND_SIGNATURE);
    if (FAILED(hr)) {
        NativeBackendCall call(ctx);
        vkdu_object_destroy(value);
        return hr;
    }
    auto *signature = object(slot.pDrvPrivate);
    signature->next = ctx->indirect_signatures;
    ctx->indirect_signatures = signature;
    return S_OK;
}

void APIENTRY indirect_signature_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDSIGNATURE slot) {
    auto *ctx = context(h);
    NativeHeapOperation op(ctx, true);
    if (!op.retained) return;
    auto *signature = native_submission_find(ctx->indirect_signatures, slot.pDrvPrivate);
    if (!signature) { error(ctx, E_INVALIDARG); return; }
    auto *value = signature->backend;
    native_submission_remove(ctx->indirect_signatures, signature);
    std::memset(signature, 0, sizeof(*signature));
    {
        NativeBackendCall call(ctx);
        vkdu_object_destroy(value);
    }
    release(ctx);
}

void APIENTRY execute_indirect(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HCOMMANDSIGNATURE signature_handle,
        UINT maximum, D3D12DDIARG_BUFFER_PLACEMENT arguments, D3D12DDIARG_BUFFER_PLACEMENT count) {
    auto *command = object(h.pDrvPrivate);
    if (!command) return;
    auto *ctx = command->context;
    NativeHeapOperation op(ctx);
    // Error ownership survives a backend callback destroying the runtime slot.
    Object report_owner{};
    report_owner.context = ctx; report_owner.kind = command->kind;
    report_owner.runtime_command = command->runtime_command;
    HRESULT hr = S_OK;
    if (!op.retained || (ctx->native_adapter && !op.live())) hr = DXGI_ERROR_DEVICE_REMOVED;
    else if (command->kind != VKDU_COMMAND_LIST || (ctx->native_adapter &&
            !native_submission_find(ctx->native_command_objects, command))) hr = E_INVALIDARG;
    auto *signature = native_submission_find(ctx->indirect_signatures, signature_handle.pDrvPrivate);
    if (SUCCEEDED(hr) && !signature) hr = E_INVALIDARG;
    vkdu_object *owners[4]{};
    unsigned retained = 0;
    if (SUCCEEDED(hr)) {
        AcquireSRWLockShared(&ctx->resources_lock);
        auto *argument_buffer = find_buffer(ctx, arguments.BaseAddress.UMD.hResource.pDrvPrivate);
        auto *count_buffer = find_buffer(ctx, count.BaseAddress.UMD.hResource.pDrvPrivate);
        if (!argument_buffer || (count.BaseAddress.UMD.hResource.pDrvPrivate && !count_buffer)) hr = E_INVALIDARG;
        else {
            owners[0] = command->backend; owners[1] = signature->backend;
            owners[2] = argument_buffer->backend; owners[3] = count_buffer ? count_buffer->backend : nullptr;
            const unsigned needed = count_buffer ? 4 : 3;
            for (; retained < needed; ++retained)
                if (!vkdu_object_retain(owners[retained])) { hr = E_OUTOFMEMORY; break; }
        }
        ReleaseSRWLockShared(&ctx->resources_lock);
    }
    {
        // No runtime-owned input or resource registry lock survives this call.
        NativeBackendCall call(ctx);
        if (SUCCEEDED(hr)) hr = vkdu_command_execute_indirect(owners[0], owners[1], maximum,
                owners[2], arguments.BaseAddress.UMD.Offset, owners[3], count.BaseAddress.UMD.Offset);
        for (unsigned i = 0; i < retained; ++i) vkdu_object_destroy(owners[i]);
    }
    error(&report_owner, hr);
}
