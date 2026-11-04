/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Private Phase A: ordinary single-sample textures and typed RGBA/BGRA8
 * 2x/4x RT arrays. Query and creation use the same description. */
int32_t vkdu_private_color_samples(vkdu_device *device, uint32_t format, uint32_t count, uint32_t quality)
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS query = {0}; HRESULT hr;
    if (!device || quality || (count != 1 && count != 2 && count != 4)) return E_INVALIDARG;
    if (count == 1) return S_OK;
    if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM) return E_INVALIDARG;
    query.Format = format; query.SampleCount = count;
    hr = ID3D12Device_CheckFeatureSupport(device->object, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &query, sizeof(query));
    if (SUCCEEDED(hr)) hr = ID3D12Device_GetDeviceRemovedReason(device->object);
    return FAILED(hr) ? hr : query.NumQualityLevels ? S_OK : DXGI_ERROR_UNSUPPORTED;
}
static HRESULT vkdu_texture_shape_desc(const struct vkdu_texture2d_shape *shape, D3D12_RESOURCE_DESC *desc)
{
    if (!vkdu_texture2d_shape_valid(shape) || shape->flags > D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ||
            (shape->flags == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ?
                !vkdu_depth_format(shape->format) :
             shape->flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ? !vkdu_color_resource_format(shape->format) :
                shape->format != DXGI_FORMAT_R32_FLOAT && !vkdu_color_resource_format(shape->format))) return E_INVALIDARG;
    memset(desc, 0, sizeof(*desc));
    desc->Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc->Width = shape->width; desc->Height = shape->height;
    desc->Format = shape->format; desc->MipLevels = shape->mips; desc->DepthOrArraySize = shape->layers;
    desc->SampleDesc.Count = shape->sample_count; desc->SampleDesc.Quality = shape->sample_quality;
    desc->Alignment = vkdu_texture2d_alignment(shape);
    desc->Flags = shape->flags;
    return S_OK;
}
int32_t vkdu_texture2d_shape_allocation(vkdu_device *device, const struct vkdu_texture2d_shape *shape,
        uint64_t *bytes, uint64_t *alignment)
{
    D3D12_RESOURCE_DESC desc; D3D12_RESOURCE_ALLOCATION_INFO info; HRESULT hr;
    if (!bytes || !alignment) return E_POINTER;
    *bytes = *alignment = 0;
    if (!device || FAILED(vkdu_texture_shape_desc(shape, &desc))) return E_INVALIDARG;
    if (FAILED(hr = vkdu_private_color_samples(device, shape->format, shape->sample_count, shape->sample_quality))) return hr;
    info = ID3D12Device_GetResourceAllocationInfo(device->object, 0, 1, &desc);
    if (!info.SizeInBytes || info.SizeInBytes == UINT64_MAX || info.Alignment != desc.Alignment || info.SizeInBytes % desc.Alignment)
        return E_INVALIDARG;
    *bytes = info.SizeInBytes; *alignment = info.Alignment;
    return S_OK;
}
static void vkdu_texture_shape_metadata(vkdu_object *out, const struct vkdu_texture2d_shape *shape, uint64_t bytes)
{
    out->width = shape->width; out->height = shape->height; out->format = shape->format; out->bytes = bytes;
    out->resource_flags = shape->flags;
}
int32_t vkdu_texture2d_shape_create(vkdu_device *device, const struct vkdu_texture2d_shape *shape, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc; D3D12_HEAP_PROPERTIES heap = {0};
    ID3D12Resource *resource = NULL; uint64_t bytes, alignment; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (FAILED(hr = vkdu_texture2d_shape_allocation(device, shape, &bytes, &alignment))) return hr;
    vkdu_texture_shape_desc(shape, &desc);
    heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    hr = ID3D12Device_CreateCommittedResource(device->object, &heap, 0, &desc,
            shape->flags == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ? D3D12_RESOURCE_STATE_DEPTH_WRITE :
            shape->flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ? D3D12_RESOURCE_STATE_RENDER_TARGET :
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&resource);
    hr = wrap(device, VKDU_TEXTURE2D, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr)) vkdu_texture_shape_metadata(*out, shape, bytes);
    return hr;
}
int32_t vkdu_texture2d_shape_place(vkdu_device *device, vkdu_object *heap, uint64_t offset,
        const struct vkdu_texture2d_shape *shape, uint32_t state, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc; ID3D12Resource *resource = NULL;
    uint64_t bytes, alignment; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (FAILED(hr = vkdu_texture2d_shape_allocation(device, shape, &bytes, &alignment))) return hr;
    if (!VALID(heap, VKDU_MEMORY_HEAP) || heap->texture_heap != (shape->flags ? 2u : 1u) || heap->owner != device->object ||
            (state != 0 && state != D3D12_RESOURCE_STATE_COPY_DEST && state != D3D12_RESOURCE_STATE_COPY_SOURCE &&
             !(shape->flags == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ?
                 state == D3D12_RESOURCE_STATE_DEPTH_WRITE || state == D3D12_RESOURCE_STATE_DEPTH_READ :
                 (shape->flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET && state == D3D12_RESOURCE_STATE_RENDER_TARGET) ||
                 state == D3D12_RESOURCE_STATE_RESOLVE_SOURCE || state == D3D12_RESOURCE_STATE_RESOLVE_DEST ||
                 state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE || state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
                 state == (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)))) return E_INVALIDARG;
    if (ID3D12Heap_GetDesc(OBJ(ID3D12Heap, heap)).Alignment < alignment ||
            offset % alignment || offset > heap->bytes || bytes > heap->bytes - offset) return E_INVALIDARG;
    vkdu_texture_shape_desc(shape, &desc);
    hr = ID3D12Device_CreatePlacedResource(device->object, OBJ(ID3D12Heap, heap), offset, &desc,
            state, NULL, &IID_ID3D12Resource, (void **)&resource);
    hr = wrap(device, VKDU_TEXTURE2D, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr)) {
        vkdu_texture_shape_metadata(*out, shape, bytes);
        (*out)->placement_heap_id = heap->identity; (*out)->placement_offset = offset;
    }
    return hr;
}
