/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The runtime owns private shader slots, while this inline header and bytecode
 * copy are owned by the existing shader storage. No borrowed declaration or
 * signature pointer survives creation, including SO without a GS. */
struct NativeGeometryOutput {
    UINT code_words, shader_stage;
    vkdu_stream_output_desc output;
};
SIZE_T APIENTRY native_geometry_output_size(D3D12DDI_HDEVICE,
        const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT *, const D3D12DDIARG_STAGE_IO_SIGNATURES *) {
    return sizeof(Object);
}
void APIENTRY native_geometry_output(D3D12DDI_HDEVICE h,
        const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT *args,
        D3D12DDI_HSHADER shader, const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures,
        D3D12DDI_CREATE_SHADER_FLAGS flags) {
    NativeHeapOperation op(context(h)); auto *ctx = op.ctx;
    if (!sampler_operation_live(op)) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    if (!args || (args->pShaderCode && !signatures) || flags || args->NumEntries > VKDU_MAX_SO_ENTRIES || args->NumStrides > 4 ||
            (args->NumEntries && !args->pOutputStreamDecl) || (args->NumStrides && !args->BufferStridesInBytes) ||
            !belongs(ctx, args->hRootSignature.pDrvPrivate) || !backend(args->hRootSignature.pDrvPrivate, VKDU_ROOT) ||
            (signatures && (signatures->NumInputSignatureEntries > 32 || signatures->NumOutputSignatureEntries > 32))) {
        error(ctx, E_INVALIDARG); return;
    }
    const auto request = *args; const auto io = signatures ? *signatures : D3D12DDIARG_STAGE_IO_SIGNATURES{};
    NativeGeometryOutput header{};
    header.output.entry_count = request.NumEntries; header.output.stride_count = request.NumStrides;
    header.output.rasterized_stream = request.RasterizedStream;
    for (UINT i = 0; i < request.NumEntries; ++i) {
        const auto e = request.pOutputStreamDecl[i];
        header.output.entries[i] = {e.Stream, e.OutputSlot, e.RegisterIndex, e.RegisterMask};
    }
    for (UINT i = 0; i < request.NumStrides; ++i) header.output.strides[i] = request.BufferStridesInBytes[i];
    vkdu_signature inputs[32]{}, outputs[32]{};
    const auto translate = [](const D3D12DDIARG_SIGNATURE_ENTRY *src, UINT count, vkdu_signature *dst) {
        if (count && !src) return false;
        for (UINT i = 0; i < count; ++i) {
            if (src[i].MinPrecision != D3D11_SB_OPERAND_MIN_PRECISION_DEFAULT) return false;
            dst[i] = {static_cast<UINT>(src[i].SystemValue), src[i].Register, src[i].Mask,
                static_cast<UINT>(src[i].RegisterComponentType)};
        }
        return true;
    };
    if (!translate(io.pInputSignatureDeprecated, io.NumInputSignatureEntries, inputs) ||
            !translate(io.pOutputSignatureDeprecated, io.NumOutputSignatureEntries, outputs)) {
        error(ctx, E_INVALIDARG); return;
    }
    void *code = nullptr; size_t bytes = 0; HRESULT hr = S_OK;
    if (request.pShaderCode) {
        header.shader_stage = request.pShaderCode[0] >> 16;
        if (header.shader_stage == 1 || header.shader_stage == 2)
            hr = vkdu_shader_dxbc(header.shader_stage, request.pShaderCode, request.pShaderCode[1],
                inputs, io.NumInputSignatureEntries, outputs, io.NumOutputSignatureEntries, &code, &bytes);
        else hr = E_INVALIDARG;
    }
    if (SUCCEEDED(hr) && (!request.pShaderCode || header.shader_stage == 1)) {
        if (header.output.rasterized_stream != 0 && header.output.rasterized_stream != UINT32_MAX) hr = E_INVALIDARG;
        for (UINT i=0;i<header.output.entry_count;++i)
            if (header.output.entries[i].stream) hr = E_INVALIDARG;
    }
    if (SUCCEEDED(hr)) hr = vkdu_stream_output_validate(&header.output, code, bytes);
    if (SUCCEEDED(hr)) {
        header.code_words = static_cast<UINT>(bytes / 4);
        const auto words = sizeof(header) / 4 + header.code_words;
        std::unique_ptr<uint32_t[]> copy(new (std::nothrow) uint32_t[words]);
        if (!copy) hr = E_OUTOFMEMORY;
        else {
            std::memcpy(copy.get(), &header, sizeof(header));
            if (bytes) std::memcpy(reinterpret_cast<char *>(copy.get()) + sizeof(header), code, bytes);
            hr = native_graphics_store(ctx, shader.pDrvPrivate, 8, copy.get(), words * 4);
        }
    }
    vkdu_shader_dxbc_free(code); error(ctx, hr);
}
