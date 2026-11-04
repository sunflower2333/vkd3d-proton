/* SPDX-License-Identifier: LGPL-2.1-or-later */
void APIENTRY native_predication(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HRESOURCE resource,
        UINT64 offset, D3D12DDI_PREDICATION_OP operation) {
    NativeGraphicsCommand command(h);
    if (!command.value) return;
    vkdu_object *value = nullptr; HRESULT hr = S_OK;
    if (operation != D3D12DDI_PREDICATION_OP_EQUAL_ZERO && operation != D3D12DDI_PREDICATION_OP_NOT_EQUAL_ZERO)
        hr = E_INVALIDARG;
    if (resource.pDrvPrivate && SUCCEEDED(hr)) {
        auto *ctx = command.op.ctx;
        AcquireSRWLockShared(&ctx->resources_lock);
        auto *slot = find_buffer(ctx, resource.pDrvPrivate);
        if (slot && vkdu_object_retain(slot->backend)) value = slot->backend;
        ReleaseSRWLockShared(&ctx->resources_lock);
        if (!value) hr = E_INVALIDARG;
    }
    command.run([&] {
        if (SUCCEEDED(hr)) hr = vkdu_command_predication(command.value, value, offset, operation);
        vkdu_object_destroy(value); vkdu_object_destroy(command.value); command.value = nullptr;
        return hr;
    });
}
