/* SPDX-License-Identifier: LGPL-2.1-or-later */
void native_texture_srv(Context *ctx, const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 *args,
        D3D12DDI_CPU_DESCRIPTOR_HANDLE destination) {
    NativeHeapOperation op(ctx);
    if (!sampler_operation_live(op)) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    const auto desc = *args;
    if (desc.Shader4ComponentMapping != 5768 || desc.Tex2D.MostDetailedMip || desc.Tex2D.FirstArraySlice ||
            desc.Tex2D.MipLevels != 1 || desc.Tex2D.ArraySize != 1 || desc.Tex2D.PlaneSlice ||
            desc.Tex2D.ResourceMinLODClamp != 0 ||
            (desc.Format != DXGI_FORMAT_R32_FLOAT && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)) {
        error(ctx, E_INVALIDARG); return;
    }
    vkdu_object *texture = nullptr, *heap_value = nullptr;
    uint32_t index = 0;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *resource = find_resource(ctx, desc.hDrvResource.pDrvPrivate);
    auto *heap = find_heap(ctx, destination.ptr, false, &index);
    if (resource && resource->kind == VKDU_TEXTURE2D && heap &&
            heap->descriptor_type == D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            vkdu_object_retain(resource->backend)) {
        texture = resource->backend;
        if (vkdu_object_retain(heap->backend)) heap_value = heap->backend;
    }
    ReleaseSRWLockShared(&ctx->resources_lock);
    HRESULT hr = E_INVALIDARG;
    {
        NativeBackendCall call(ctx);
        if (texture && heap_value && vkdu_texture2d_format(texture) == static_cast<UINT>(desc.Format))
            hr = vkdu_texture2d_srv(heap_value, index, texture);
        vkdu_object_destroy(texture); vkdu_object_destroy(heap_value);
    }
    if (!sampler_operation_live(op)) hr = DXGI_ERROR_DEVICE_REMOVED;
    error(ctx, hr);
}

HRESULT native_copy_location(Context *ctx, const D3D12DDIARG_BUFFER_PLACEMENT *placement,
        D3D12DDIARG_PLACED_RESOURCE layout, vkdu_texture_copy_location &out) {
    if (!placement) return E_INVALIDARG;
    auto *resource = find_resource(ctx, placement->BaseAddress.UMD.hResource.pDrvPrivate);
    if (!resource) return E_INVALIDARG;
    out.offset = placement->BaseAddress.UMD.Offset;
    if (layout.Layout == D3D12DDI_RL_SELECT_SUBRESOURCE) {
        if (layout.pLayout || resource->kind != VKDU_TEXTURE2D || out.offset) return E_INVALIDARG;
    } else if (layout.Layout == D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED) {
        if (!layout.pLayout || resource->kind != VKDU_BUFFER) return E_INVALIDARG;
        const auto &p = *static_cast<const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *>(layout.pLayout);
        if ((p.Format != DXGI_FORMAT_R32_FLOAT && p.Format != DXGI_FORMAT_R8G8B8A8_UNORM) ||
                p.PhysicalDepth != 1 || uint64_t(p.Pitch) * p.PhysicalHeight != p.SlicePitch) return E_INVALIDARG;
        out.footprint = 1; out.format = p.Format; out.width = p.PhysicalWidth;
        out.height = p.PhysicalHeight; out.depth = p.PhysicalDepth; out.row_pitch = p.Pitch;
    } else return E_NOTIMPL;
    if (!vkdu_object_retain(resource->backend)) return E_OUTOFMEMORY;
    out.resource = resource->backend;
    return S_OK;
}

void APIENTRY copy_texture(D3D12DDI_HCOMMANDLIST c, const D3D12DDIARG_BUFFER_PLACEMENT *dst,
        D3D12DDIARG_PLACED_RESOURCE dst_layout, UINT x, UINT y, UINT z,
        const D3D12DDIARG_BUFFER_PLACEMENT *src, D3D12DDIARG_PLACED_RESOURCE src_layout, const D3D12DDI_BOX *box) {
    auto *cmd = object(c.pDrvPrivate);
    if (!cmd || cmd->kind != VKDU_COMMAND_LIST) return;
    auto *ctx = cmd->context;
    const auto runtime = cmd->runtime_command;
    NativeHeapOperation op(ctx);
    if (!sampler_operation_live(op)) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    vkdu_texture_copy_location destination{}, source{};
    vkdu_box region{};
    if (box) region = {box->left, box->top, box->front, box->right, box->bottom, box->back};
    auto *command = cmd->backend;
    if (!vkdu_object_retain(command)) { error(ctx, E_OUTOFMEMORY); return; }
    AcquireSRWLockShared(&ctx->resources_lock);
    HRESULT hr = native_copy_location(ctx, dst, dst_layout, destination);
    if (SUCCEEDED(hr)) hr = native_copy_location(ctx, src, src_layout, source);
    ReleaseSRWLockShared(&ctx->resources_lock);
    {
        NativeBackendCall call(ctx);
        if (SUCCEEDED(hr)) hr = vkdu_command_texture_copy(command, &destination, x, y, z, &source, box ? &region : nullptr);
        vkdu_object_destroy(destination.resource); vkdu_object_destroy(source.resource); vkdu_object_destroy(command);
    }
    if (!sampler_operation_live(op)) hr = DXGI_ERROR_DEVICE_REMOVED;
    // Copy the error owner because the runtime command slot may have expired.
    Object error_owner{};
    error_owner.context = ctx; error_owner.kind = VKDU_COMMAND_LIST; error_owner.runtime_command = runtime;
    error(&error_owner, hr);
}
