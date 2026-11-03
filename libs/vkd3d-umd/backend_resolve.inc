/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Private Phase A typed color resolve. Public quality/features remain closed
 * until predication, sample loads and the complete coverage contract pass. */
int32_t vkdu_command_resolve(vkdu_object *command, vkdu_object *dst, uint32_t dst_subresource,
        vkdu_object *src, uint32_t src_subresource, uint32_t format)
{
    D3D12_RESOURCE_DESC a, b;
    uint32_t amip, bmip;
    HRESULT hr;
    if (!RECORDING(command) || command->command_type || !VALID(dst, VKDU_TEXTURE2D) ||
            !VALID(src, VKDU_TEXTURE2D) || dst->object == src->object ||
            !vkdu_same_device(command, dst) || !vkdu_same_device(command, src) ||
            (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM) ||
            src->format != format || dst->format != format) return E_INVALIDARG;
    /* The existing engine's vkCmdResolveImage path is not conditional. Never
     * silently ignore active D3D12 predication; Phase B owns that fallback. */
    if (command->predicate_enabled) return E_NOTIMPL;
    a = ID3D12Resource_GetDesc(OBJ(ID3D12Resource, dst));
    b = ID3D12Resource_GetDesc(OBJ(ID3D12Resource, src));
    if (a.SampleDesc.Count != 1 || a.SampleDesc.Quality || b.SampleDesc.Quality ||
            (b.SampleDesc.Count != 2 && b.SampleDesc.Count != 4) || b.MipLevels != 1 ||
            dst_subresource >= (uint32_t)a.MipLevels * a.DepthOrArraySize ||
            src_subresource >= (uint32_t)b.MipLevels * b.DepthOrArraySize) return E_INVALIDARG;
    amip = dst_subresource % a.MipLevels; bmip = src_subresource % b.MipLevels;
    if (max(1ull, a.Width >> amip) != max(1ull, b.Width >> bmip) ||
            max(1u, a.Height >> amip) != max(1u, b.Height >> bmip)) return E_INVALIDARG;
    if (vkdu_placed_overlap(dst, src)) return E_INVALIDARG;
    {
        ID3D12Resource *owners[2] = {OBJ(ID3D12Resource, dst), OBJ(ID3D12Resource, src)};
        if (FAILED(hr = vkd3d_wddm_command_retain_resources(OBJ(ID3D12GraphicsCommandList, command), 2, owners))) return hr;
    }
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_MSAA_DROP_RESOLVE)
    return S_OK;
#endif
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_MSAA_WRONG_LAYER)
    if (src_subresource) src_subresource = 0;
#endif
    ID3D12GraphicsCommandList_ResolveSubresource(OBJ(ID3D12GraphicsCommandList, command),
        OBJ(ID3D12Resource, dst), dst_subresource, OBJ(ID3D12Resource, src), src_subresource, format);
    return S_OK;
}
