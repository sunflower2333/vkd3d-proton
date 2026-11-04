/* SPDX-License-Identifier: LGPL-2.1-or-later */
void APIENTRY native_resource_copy(D3D12DDI_HCOMMANDLIST h,
        D3D12DDI_HRESOURCE destination, D3D12DDI_HRESOURCE source) {
    NativeGraphicsCommand command(h);
    if (!command.value) return;
    auto *ctx = command.op.ctx;
    HRESULT hr = ctx->native_adapter ? native_heap_generation(ctx) : S_OK;
    if (FAILED(hr) || !sampler_operation_live(command.op)) { command.report(hr); return; }
    vkdu_object *dst = nullptr, *src = nullptr;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *dst_slot = find_resource(ctx, destination.pDrvPrivate);
    auto *src_slot = find_resource(ctx, source.pDrvPrivate);
    if (dst_slot && src_slot && dst_slot != src_slot && dst_slot->kind == src_slot->kind &&
            (dst_slot->kind == VKDU_BUFFER || dst_slot->kind == VKDU_TEXTURE2D)) {
        if (vkdu_object_retain(dst_slot->backend)) dst = dst_slot->backend;
        if (vkdu_object_retain(src_slot->backend)) src = src_slot->backend;
    }
    ReleaseSRWLockShared(&ctx->resources_lock);
    command.run([&] {
        const HRESULT result = dst && src ? vkdu_command_resource_copy(command.value, dst, src) : E_INVALIDARG;
        vkdu_object_destroy(dst); vkdu_object_destroy(src);
        return result;
    });
}
