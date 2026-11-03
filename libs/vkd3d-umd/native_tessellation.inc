/* SPDX-License-Identifier: LGPL-2.1-or-later */
SIZE_T APIENTRY native_tessellation_size(D3D12DDI_HDEVICE, const UINT *, D3D12DDI_HROOTSIGNATURE,
        const D3D12DDIARG_TESSELLATION_IO_SIGNATURES *) {
    return sizeof(Object);
}
template<UINT Stage> void APIENTRY native_tessellation_shader(D3D12DDI_HDEVICE h, const UINT *tokens,
        D3D12DDI_HROOTSIGNATURE root, D3D12DDI_HSHADER shader,
        const D3D12DDIARG_TESSELLATION_IO_SIGNATURES *signatures, D3D12DDI_CREATE_SHADER_FLAGS flags) {
    NativeHeapOperation op(context(h)); auto *ctx = op.ctx;
    if (!sampler_operation_live(op)) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    if (!tokens || !signatures || flags || !belongs(ctx, root.pDrvPrivate) || !backend(root.pDrvPrivate, VKDU_ROOT)) {
        error(ctx, E_INVALIDARG); return;
    }
    const auto request = *signatures;
    const D3D12DDIARG_SIGNATURE_ENTRY *src[] = {request.pInputSignatureDeprecated,
        request.pOutputSignatureDeprecated, request.pPatchConstantSignatureDeprecated};
    const UINT counts[] = {request.NumInputSignatureEntries, request.NumOutputSignatureEntries, request.NumPatchConstantSignatureEntries};
    vkdu_signature entries[3][32]{};
    for (UINT group = 0; group < 3; ++group) {
        if (counts[group] > 32 || (counts[group] && !src[group])) { error(ctx, E_INVALIDARG); return; }
        for (UINT i = 0; i < counts[group]; ++i) {
            const auto e = src[group][i];
            if (e.MinPrecision != D3D11_SB_OPERAND_MIN_PRECISION_DEFAULT) { error(ctx, E_INVALIDARG); return; }
            entries[group][i] = {static_cast<UINT>(e.SystemValue), e.Register, e.Mask, static_cast<UINT>(e.RegisterComponentType)};
        }
    }
    void *code = nullptr; size_t bytes = 0;
    HRESULT hr = vkdu_shader_tess_dxbc(Stage, tokens, tokens[1], entries[0], counts[0], entries[1], counts[1],
        entries[2], counts[2], &code, &bytes);
    if (SUCCEEDED(hr)) hr = native_graphics_store(ctx, shader.pDrvPrivate, Stage == 3 ? 9 : 10, code, bytes);
    vkdu_shader_dxbc_free(code); error(ctx, hr);
}
