/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* R0 buffer alias activation plus existing transition/UAV batches. Runtime
 * slots and arrays cannot survive arbitrary backend callbacks; only retained
 * backend peers cross that boundary. Explicit texture aliases stay closed. */
void APIENTRY barriers(D3D12DDI_HCOMMANDLIST h, UINT count,
        const D3D12DDIARG_RESOURCE_BARRIER_0003 *args) {
    NativeGraphicsCommand command(h);
    if (!command.value) return;
    if ((count && !args) || count > 65536) { command.report(E_INVALIDARG); return; }
    vkdu_resource_barrier stack[16]{};
    std::unique_ptr<vkdu_resource_barrier[]> allocated;
    auto *translated = stack;
    if (count > 16) {
        allocated.reset(new (std::nothrow) vkdu_resource_barrier[count]{});
        if (!allocated) { command.report(E_OUTOFMEMORY); return; }
        translated = allocated.get();
    }
    auto *ctx = command.op.ctx;
    HRESULT result = S_OK;
    AcquireSRWLockShared(&ctx->resources_lock);
    for (UINT i = 0; i < count && SUCCEEDED(result); ++i) {
        const auto request = args[i];
        if (request.Flags) { result = E_NOTIMPL; break; }
        void *before = nullptr, *after = nullptr;
        auto &out = translated[i];
        if (request.Type == D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION) {
            before = request.Transition.hResource.pDrvPrivate;
            if (!before) { result = E_INVALIDARG; break; }
            out.type = VKDU_BARRIER_TRANSITION;
            out.before = request.Transition.StateBefore; out.after = request.Transition.StateAfter;
            out.subresource_plus_one = request.Transition.Subresource == UINT_MAX ? 0 : request.Transition.Subresource + 1;
        } else if (request.Type == D3D12DDI_RESOURCE_BARRIER_TYPE_UAV) {
            before = request.UAV.hResource.pDrvPrivate; out.type = VKDU_BARRIER_UAV;
        } else if (request.Type == D3D12DDI_RESOURCE_BARRIER_TYPE_ALIASING) {
            before = request.Aliasing.hResourceBefore.pDrvPrivate;
            after = request.Aliasing.hResourceAfter.pDrvPrivate;
            out.type = VKDU_BARRIER_ALIASING;
        } else { result = E_NOTIMPL; break; }
        auto *a = find_resource(ctx, before), *b = find_resource(ctx, after);
        if ((before && !a) || (after && !b)) { result = E_INVALIDARG; break; }
        if (out.type == VKDU_BARRIER_ALIASING &&
                ((a && a->kind != VKDU_BUFFER) || (b && b->kind != VKDU_BUFFER))) { result = E_NOTIMPL; break; }
        if (a) {
            if (!vkdu_object_retain(a->backend)) { result = E_INVALIDARG; break; }
            out.resource = a->backend;
        }
        if (b) {
            if (!vkdu_object_retain(b->backend)) { result = E_INVALIDARG; break; }
            out.alias_after = b->backend;
        }
    }
    ReleaseSRWLockShared(&ctx->resources_lock);
    command.run([&] {
        if (SUCCEEDED(result)) result = vkdu_command_barriers(command.value, count, translated);
        for (UINT i = 0; i < count; ++i) {
            vkdu_object_destroy(translated[i].resource);
            vkdu_object_destroy(translated[i].alias_after);
        }
        // No backend work follows. Drop the temporary command borrow before
        // SetCommandListErrorCb can destroy command/allocator/device in order.
        // The copied error owner and operation's Context reference suffice.
        vkdu_object_destroy(command.value);
        command.value = nullptr;
        return result;
    });
}
