/* SPDX-License-Identifier: LGPL-2.1-or-later */
bool sampler_operation_live(const NativeHeapOperation &op) {
    const auto *ctx = op.ctx;
    return op.retained && !ctx->native_retiring && !ctx->native_destroying &&
        (!ctx->native_adapter || ctx->runtime_device.handle);
}
void APIENTRY create_sampler(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SAMPLER *args,
        D3D12DDI_CPU_DESCRIPTOR_HANDLE destination) {
    auto *ctx = context(h);
    if (!ctx) return;
    NativeHeapOperation op(ctx);
    if (!sampler_operation_live(op)) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    if (!args || !args->pSamplerDesc) { error(ctx, E_INVALIDARG); return; }
    const auto desc = *args->pSamplerDesc;
    vkdu_sampler_desc sampler{};
    sampler.filter = desc.Filter; sampler.address_u = desc.AddressU;
    sampler.address_v = desc.AddressV; sampler.address_w = desc.AddressW;
    sampler.mip_bias = desc.MipLODBias; sampler.max_anisotropy = desc.MaxAnisotropy;
    sampler.comparison = desc.ComparisonFunc;
    std::memcpy(sampler.border, desc.BorderColor, sizeof(sampler.border));
    sampler.min_lod = desc.MinLOD; sampler.max_lod = desc.MaxLOD;
    uint32_t index = 0;
    vkdu_object *value = nullptr;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *heap = find_heap(ctx, destination.ptr, false, &index);
    if (heap && heap->descriptor_type == D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
            vkdu_object_retain(heap->backend)) value = heap->backend;
    ReleaseSRWLockShared(&ctx->resources_lock);
    if (!value) { error(ctx, E_INVALIDARG); return; }
    HRESULT hr;
    {
        // No registry/callback locks or borrowed descriptor/heap slots survive
        // backend entry. Turnip may enter runtime allocation/error callbacks.
        NativeBackendCall call(ctx);
        hr = vkdu_sampler_create(value, index, &sampler);
        vkdu_object_destroy(value);
    }
    if (!sampler_operation_live(op)) hr = DXGI_ERROR_DEVICE_REMOVED;
    error(ctx, hr);
}

HRESULT native_root_backend(Context *ctx, void *slot, const vkdu_root_parameter *parameters,
        UINT count, UINT flags, const vkdu_static_sampler *samplers, UINT sampler_count) {
    NativeHeapOperation op(ctx);
    if (!sampler_operation_live(op)) return DXGI_ERROR_DEVICE_REMOVED;
    if (!slot || object(slot)) return E_INVALIDARG;
    vkdu_object *value = nullptr;
    HRESULT hr;
    {
        NativeBackendCall call(ctx);
        hr = sampler_count ? vkdu_root_create_samplers(ctx->backend, parameters, count, flags,
                samplers, sampler_count, &value) : vkdu_root_create(ctx->backend, parameters, count, flags, &value);
    }
    if (!sampler_operation_live(op)) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) hr = bind(ctx, slot, value, VKDU_ROOT);
    if (FAILED(hr)) {
        NativeBackendCall call(ctx);
        vkdu_object_destroy(value);
    }
    return hr;
}
