/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Included by the production backend, using its actual object ownership. */
static int32_t vkdu_sampler_translate(const struct vkdu_sampler_desc *desc, D3D12_SAMPLER_DESC *native)
{
    uint32_t i, basic;
    if (!desc || (desc->filter & ~0x1d5u)) return E_INVALIDARG;
    basic = desc->filter & 0x7f;
    if ((basic & 0x40) && basic != 0x55) return E_INVALIDARG;
    /* Minimum/maximum reduction filters require a capability contract that
     * this R0 bridge does not expose yet. Never silently use standard filtering. */
    if (desc->filter & 0x100) return E_NOTIMPL;
    if (desc->address_u < 1 || desc->address_u > 5 || desc->address_v < 1 || desc->address_v > 5 ||
            desc->address_w < 1 || desc->address_w > 5 || !isfinite(desc->mip_bias) ||
            desc->mip_bias < -16.0f || desc->mip_bias > 15.99f ||
            desc->comparison < 1 || desc->comparison > 8 ||
            ((basic & 0x40) && (desc->max_anisotropy < 1 || desc->max_anisotropy > 16)) ||
            !isfinite(desc->min_lod) || !isfinite(desc->max_lod) || desc->min_lod > desc->max_lod)
        return E_INVALIDARG;
    for (i = 0; i < 4; ++i) if (!isfinite(desc->border[i])) return E_INVALIDARG;
    memset(native, 0, sizeof(*native));
    native->Filter = desc->filter;
    native->AddressU = desc->address_u; native->AddressV = desc->address_v; native->AddressW = desc->address_w;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_SAMPLER_DROP_ADDRESS)
    native->AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
#endif
    native->MipLODBias = desc->mip_bias; native->MaxAnisotropy = desc->max_anisotropy;
    native->ComparisonFunc = desc->comparison;
    memcpy(native->BorderColor, desc->border, sizeof(native->BorderColor));
    native->MinLOD = desc->min_lod; native->MaxLOD = desc->max_lod;
    return S_OK;
}

int32_t vkdu_sampler_create(vkdu_object *heap, uint32_t index, const struct vkdu_sampler_desc *desc)
{
    D3D12_SAMPLER_DESC native;
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    HRESULT hr;
    if (!VALID(heap, VKDU_DESCRIPTOR_HEAP) || heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ||
            index >= heap->descriptor_count) return E_INVALIDARG;
    if (FAILED(hr = vkdu_sampler_translate(desc, &native))) return hr;
    handle.ptr = (SIZE_T)(vkdu_heap_start(heap, 0) + (uint64_t)index * heap->descriptor_stride);
    ID3D12Device_CreateSampler(heap->owner, &native, handle);
    return ID3D12Device_GetDeviceRemovedReason(heap->owner);
}

static int vkdu_visibility_overlaps(uint32_t a, uint32_t b) { return !a || !b || a == b; }
static int32_t vkdu_static_samplers_translate(const struct vkdu_root_parameter *params, uint32_t count,
        const struct vkdu_static_sampler *samplers, uint32_t sampler_count, D3D12_STATIC_SAMPLER_DESC *native)
{
    uint32_t i, j, k;
    HRESULT hr;
    for (i = 0; i < sampler_count; ++i) {
        D3D12_SAMPLER_DESC desc;
        const struct vkdu_static_sampler *s = &samplers[i];
        if (s->visibility > 5 || s->border_color > 2) return E_INVALIDARG;
        if (FAILED(hr = vkdu_sampler_translate(&s->desc, &desc))) return hr;
        for (j = 0; j < i; ++j)
            if (samplers[j].shader_register == s->shader_register && samplers[j].register_space == s->register_space &&
                    vkdu_visibility_overlaps(samplers[j].visibility, s->visibility)) return E_INVALIDARG;
        for (j = 0; j < count; ++j) {
            if (params[j].type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
                    !vkdu_visibility_overlaps(params[j].visibility, s->visibility)) continue;
            for (k = 0; k < params[j].range_count; ++k) {
                const struct vkdu_descriptor_range *r = &params[j].ranges[k];
                if (r->type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER && r->register_space == s->register_space &&
                        s->shader_register >= r->shader_register &&
                        (r->count == UINT32_MAX || s->shader_register - r->shader_register < r->count)) return E_INVALIDARG;
            }
        }
        native[i].Filter = desc.Filter;
        native[i].AddressU = desc.AddressU; native[i].AddressV = desc.AddressV; native[i].AddressW = desc.AddressW;
        native[i].MipLODBias = desc.MipLODBias; native[i].MaxAnisotropy = desc.MaxAnisotropy;
        native[i].ComparisonFunc = desc.ComparisonFunc; native[i].MinLOD = desc.MinLOD; native[i].MaxLOD = desc.MaxLOD;
        native[i].BorderColor = s->border_color; native[i].ShaderRegister = s->shader_register;
        native[i].RegisterSpace = s->register_space; native[i].ShaderVisibility = s->visibility;
    }
    return S_OK;
}

int32_t vkdu_texture2d_create(vkdu_device *device, uint32_t width, uint32_t height,
        uint32_t format, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc = {0};
    D3D12_HEAP_PROPERTIES heap = {0};
    ID3D12Resource *resource = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !width || !height || width > 16384 || height > 16384 ||
            (format != DXGI_FORMAT_R32_FLOAT && format != DXGI_FORMAT_R8G8B8A8_UNORM)) return E_INVALIDARG;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1; desc.Format = format;
    heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    hr = ID3D12Device_CreateCommittedResource(device->object, &heap, 0, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&resource);
    hr = wrap(device, VKDU_TEXTURE2D, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr)) { (*out)->width = width; (*out)->height = height; (*out)->format = format; }
    return hr;
}

int32_t vkdu_texture2d_srv(vkdu_object *heap, uint32_t index, vkdu_object *texture)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    if (!VALID(heap, VKDU_DESCRIPTOR_HEAP) || heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
            index >= heap->descriptor_count || !VALID(texture, VKDU_TEXTURE2D) ||
            !vkdu_same_device(heap, texture)) return E_INVALIDARG;
    desc.Format = texture->format; desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; desc.Texture2D.MipLevels = 1;
    handle.ptr = (SIZE_T)(vkdu_heap_start(heap, 0) + (uint64_t)index * heap->descriptor_stride);
    ID3D12Device_CreateShaderResourceView(heap->owner, OBJ(ID3D12Resource, texture), &desc, handle);
    return ID3D12Device_GetDeviceRemovedReason(heap->owner);
}

int32_t vkdu_command_texture_upload(vkdu_object *command, vkdu_object *texture,
        vkdu_object *upload, uint64_t offset, uint32_t row_pitch)
{
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    uint64_t size;
    if (!RECORDING(command) || !VALID(texture, VKDU_TEXTURE2D) || !VALID(upload, VKDU_BUFFER) ||
            !vkdu_same_device(command, texture) || !vkdu_same_device(command, upload) ||
            upload->heap_type != D3D12_HEAP_TYPE_UPLOAD || offset % 512 || row_pitch % 256 ||
            row_pitch < texture->width * 4) return E_INVALIDARG;
    size = (uint64_t)row_pitch * (texture->height - 1) + texture->width * 4;
    if (offset > upload->bytes || size > upload->bytes - offset) return E_INVALIDARG;
    dst.pResource = OBJ(ID3D12Resource, texture); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = OBJ(ID3D12Resource, upload); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = offset; src.PlacedFootprint.Footprint.Format = texture->format;
    src.PlacedFootprint.Footprint.Width = texture->width; src.PlacedFootprint.Footprint.Height = texture->height;
    src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = row_pitch;
    ID3D12GraphicsCommandList_CopyTextureRegion(OBJ(ID3D12GraphicsCommandList, command), &dst, 0, 0, 0, &src, NULL);
    return S_OK;
}
