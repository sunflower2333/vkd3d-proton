/* SPDX-License-Identifier: LGPL-2.1-or-later */
int32_t vkdu_command_resource_copy(vkdu_object *command, vkdu_object *dst, vkdu_object *src)
{
    if (!RECORDING(command) || command->command_type == 1 || (!VALID(dst, VKDU_BUFFER) && !VALID(dst, VKDU_TEXTURE2D)) ||
            (!VALID(src, VKDU_BUFFER) && !VALID(src, VKDU_TEXTURE2D)) ||
            dst->object == src->object || dst->kind != src->kind ||
            !vkdu_same_device(command, dst) || !vkdu_same_device(command, src)) return E_INVALIDARG;
    if (vkdu_placed_overlap(dst, src)) return E_INVALIDARG;
    if (dst->kind == VKDU_BUFFER) {
        if (dst->bytes != src->bytes || dst->heap_type == D3D12_HEAP_TYPE_UPLOAD ||
                src->heap_type == D3D12_HEAP_TYPE_READBACK) return E_INVALIDARG;
    } else {
        if (command->predicate_enabled) return E_NOTIMPL;
        D3D12_RESOURCE_DESC a = ID3D12Resource_GetDesc(OBJ(ID3D12Resource, dst));
        D3D12_RESOURCE_DESC b = ID3D12Resource_GetDesc(OBJ(ID3D12Resource, src));
        /* Color copies may reinterpret
         * bytes within one implemented typeless family, without conversion. */
        if (dst->width != src->width || dst->height != src->height || !vkdu_copy_format_compatible(dst->format, src->format) ||
                !vkdu_copy_pixel_bytes(dst->format) || a.MipLevels != b.MipLevels ||
                a.DepthOrArraySize != b.DepthOrArraySize || a.SampleDesc.Count != b.SampleDesc.Count ||
                a.SampleDesc.Quality != b.SampleDesc.Quality) return E_INVALIDARG;
    }
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_RESOURCE_COPY_DROP)
    return S_OK;
#endif
    {
        ID3D12Resource *owners[2] = {OBJ(ID3D12Resource, dst), OBJ(ID3D12Resource, src)};
        HRESULT hr = vkd3d_wddm_command_retain_resources(OBJ(ID3D12GraphicsCommandList, command), 2, owners);
        if (FAILED(hr)) return hr;
    }
    ID3D12GraphicsCommandList_CopyResource(OBJ(ID3D12GraphicsCommandList, command),
        OBJ(ID3D12Resource, dst), OBJ(ID3D12Resource, src));
    return S_OK;
}
