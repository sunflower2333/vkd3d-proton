/* SPDX-License-Identifier: LGPL-2.1-or-later */
static_assert(DXGI_FORMAT_R8G8B8A8_TYPELESS == 27 && DXGI_FORMAT_B8G8R8A8_TYPELESS == 90 &&
    DXGI_FORMAT_R8G8B8A8_UNORM == 28 && DXGI_FORMAT_R8G8B8A8_UNORM_SRGB == 29 &&
    DXGI_FORMAT_B8G8R8A8_UNORM == 87 && DXGI_FORMAT_B8G8R8A8_UNORM_SRGB == 91, "typed color format ABI");
static_assert(VKDU_FORMAT_SAMPLE == D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE &&
    VKDU_FORMAT_RENDER_TARGET == D3D12DDI_FORMAT_SUPPORT_RENDERTARGET &&
    VKDU_FORMAT_BLENDABLE == D3D12DDI_FORMAT_SUPPORT_BLENDABLE &&
    VKDU_FORMAT_VERTEX == D3D12DDI_FORMAT_SUPPORT_VERTEX_BUFFER &&
    VKDU_FORMAT_UAV_WRITE == D3D12DDI_FORMAT_SUPPORT_UAV_WRITES &&
    VKDU_FORMAT_BUFFER == D3D12DDI_FORMAT_SUPPORT_BUFFER &&
    VKDU_FORMAT_GATHER == D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER &&
    VKDU_FORMAT_UAV_READ == D3D12DDI_FORMAT_SUPPORT_UAV_READS, "WDK format contract");
template<class Query> void native_format_query(D3D12DDI_HDEVICE h, UINT *out, Query query) {
    NativeHeapOperation op(context(h));
    if (!out) { native_trace("format query: null output"); error(op.ctx, E_INVALIDARG); return; }
    if (!sampler_operation_live(op)) { native_trace("format query: operation not live"); error(op.ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    uint32_t value = 0; HRESULT hr;
    { NativeBackendCall call(op.ctx); hr = query(op.ctx->backend, &value); }
    if (SUCCEEDED(hr) && op.ctx->native_adapter) hr = native_heap_generation(op.ctx);
    if (!sampler_operation_live(op)) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) *out = value;
    else native_trace("format query failed hr=%08x", (unsigned)hr);
    error(op.ctx, hr);
}
void APIENTRY native_format_support(D3D12DDI_HDEVICE h, DXGI_FORMAT format, UINT *out) {
    native_format_query(h, out, [&](vkdu_device *device, uint32_t *value) {
        return vkdu_format_support(device, format, value);
    });
}
void APIENTRY native_multisample_quality(D3D12DDI_HDEVICE h, DXGI_FORMAT format, UINT samples,
        D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags, UINT *out) {
    native_format_query(h, out, [&](vkdu_device *device, uint32_t *value) {
        return vkdu_multisample_quality(device, format, samples, flags, value);
    });
}
