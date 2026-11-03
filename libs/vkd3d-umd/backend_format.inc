/* SPDX-License-Identifier: LGPL-2.1-or-later */
int32_t vkdu_format_support(vkdu_device *device, uint32_t format, uint32_t *flags)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT info = {0};
    struct vkdu_input_element element = {0};
    uint32_t value = 0;
    HRESULT hr;
    if (!device || !flags) return E_INVALIDARG;
    info.Format = format;
    hr = ID3D12Device_CheckFeatureSupport(device->object, D3D12_FEATURE_FORMAT_SUPPORT, &info, sizeof(info));
    if (FAILED(ID3D12Device_GetDeviceRemovedReason(device->object))) return ID3D12Device_GetDeviceRemovedReason(device->object);
    /* The embedded engine rejects formats it cannot translate. For the DDI
     * that is "no support", not a device error. */
    if (FAILED(hr)) { *flags = 0; return S_OK; }
    // Intersect the actual typed resource/view/copy implementation with Vulkan.
    if (format == DXGI_FORMAT_R32_FLOAT || vkdu_color_target_format(format)) {
        if (info.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) value |= VKDU_FORMAT_SAMPLE;
        if (info.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_GATHER) value |= VKDU_FORMAT_GATHER;
    }
    if (vkdu_color_target_format(format) && (info.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
        value |= VKDU_FORMAT_RENDER_TARGET;
    if ((value & VKDU_FORMAT_RENDER_TARGET) && (info.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE))
        value |= VKDU_FORMAT_BLENDABLE;
    if (format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_R32_UINT || format == DXGI_FORMAT_R32_SINT) {
        if (info.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER) value |= VKDU_FORMAT_BUFFER;
        if (info.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) value |= VKDU_FORMAT_UAV_WRITE;
        if (info.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) value |= VKDU_FORMAT_UAV_READ;
    }
    element.format = format;
    if ((info.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER) && SUCCEEDED(vkdu_input_layout_validate(1, &element)))
        value |= VKDU_FORMAT_VERTEX;
    // No MSAA, display, tiled, video or unimplemented typed views leak
    // from the embedded engine's broader API capability table.
    *flags = value;
    return S_OK;
}
int32_t vkdu_multisample_quality(vkdu_device *device, uint32_t format, uint32_t samples,
        uint32_t flags, uint32_t *levels)
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS info = {0};
    HRESULT hr;
    if (!device || !levels || !samples || samples > 32 || flags > D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE)
        return E_INVALIDARG;
    info.Format = format; info.SampleCount = samples; info.Flags = flags;
    hr = ID3D12Device_CheckFeatureSupport(device->object, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
            &info, sizeof(info));
    if (FAILED(ID3D12Device_GetDeviceRemovedReason(device->object))) return ID3D12Device_GetDeviceRemovedReason(device->object);
    if (FAILED(hr)) { *levels = 0; return S_OK; }
    *levels = !flags && samples == 1 && (format == DXGI_FORMAT_R32_FLOAT || vkdu_color_resource_format(format) ||
        vkdu_depth_format(format))
        ? info.NumQualityLevels : 0;
    return S_OK;
}
