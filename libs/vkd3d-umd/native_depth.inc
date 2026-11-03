/* SPDX-License-Identifier: LGPL-2.1-or-later */
static_assert(DXGI_FORMAT_D24_UNORM_S8_UINT == 45 && DXGI_FORMAT_D32_FLOAT_S8X24_UINT == 20 &&
    DXGI_FORMAT_D32_FLOAT == 40 && DXGI_FORMAT_D16_UNORM == 55, "Typed DS format ABI");
static_assert(D3D12DDI_CREATE_DSV_FLAG_READ_ONLY_DEPTH == 1 &&
    D3D12DDI_CREATE_DSV_FLAG_READ_ONLY_STENCIL == 2, "DSV plane flags ABI");
void APIENTRY native_create_dsv(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW *args,
        D3D12DDI_CPU_DESCRIPTOR_HANDLE handle) {
    NativeHeapOperation op(context(h)); auto *ctx = op.ctx;
    if (!sampler_operation_live(op)) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    if (!args || args->ResourceDimension != D3D12DDI_RD_TEXTURE2D ||
            !vkdu_depth_format(args->Format) ||
            args->Flags > (vkdu_stencil_format(args->Format) ? 3u : 1u) ||
            !args->Tex2D.ArraySize) { error(ctx, E_INVALIDARG); return; }
    const auto request = *args; uint32_t index = 0;
    auto *heap = native_graphics_view_heap(ctx, handle, index, D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV);
    vkdu_object *texture = nullptr;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *resource = find_resource(ctx, request.hDrvResource.pDrvPrivate);
    if (resource && resource->kind == VKDU_TEXTURE2D && vkdu_texture2d_format(resource->backend) == request.Format &&
            vkdu_object_retain(resource->backend)) texture = resource->backend;
    ReleaseSRWLockShared(&ctx->resources_lock);
    HRESULT hr;
    { NativeBackendCall call(ctx); hr = heap && texture ? vkdu_texture2d_dsv_range(heap, index, texture, request.Flags,
            request.Tex2D.MipSlice, request.Tex2D.FirstArraySlice, request.Tex2D.ArraySize) : E_INVALIDARG;
        vkdu_object_destroy(heap); vkdu_object_destroy(texture); }
    if (SUCCEEDED(hr) && ctx->native_adapter) hr = native_heap_generation(ctx);
    if (!sampler_operation_live(op)) hr = DXGI_ERROR_DEVICE_REMOVED;
    error(ctx, hr);
}
void APIENTRY native_clear_dsv(D3D12DDI_HCOMMANDLIST h, D3D12DDI_CPU_DESCRIPTOR_HANDLE handle,
        UINT flags, FLOAT depth, UINT8 stencil, UINT count, const D3D12DDI_RECT *rects) {
    NativeGraphicsCommand command(h); if (!command.value) return;
    vkdu_rect copy[16]{};
    if (!native_graphics_rects(count, rects, copy)) {
        command.run([&] { vkdu_object_destroy(command.value); command.value = nullptr; return E_INVALIDARG; });
        return;
    }
    uint32_t index = 0;
    auto *heap = native_graphics_view_heap(command.op.ctx, handle, index, D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV);
    command.run([&] { auto hr = vkdu_command_clear_dsv(command.value, heap, index, flags, depth, stencil, count, copy);
        vkdu_object_destroy(heap); vkdu_object_destroy(command.value); command.value = nullptr; return hr; });
}
