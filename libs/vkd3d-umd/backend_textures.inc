/* SPDX-License-Identifier: LGPL-2.1-or-later */
uint32_t vkdu_texture2d_format(vkdu_object *texture) { return VALID(texture, VKDU_TEXTURE2D) ? texture->format : 0; }
static int32_t vkdu_texture_desc(uint32_t width, uint32_t height, uint32_t format, D3D12_RESOURCE_DESC *desc)
{
    if (!width || !height || width > 16384 || height > 16384 ||
            (format != DXGI_FORMAT_R32_FLOAT && format != DXGI_FORMAT_R8G8B8A8_UNORM)) return E_INVALIDARG;
    memset(desc, 0, sizeof(*desc));
    desc->Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc->Width = width; desc->Height = height;
    desc->DepthOrArraySize = desc->MipLevels = desc->SampleDesc.Count = 1; desc->Format = format;
    desc->Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    return S_OK;
}
int32_t vkdu_texture2d_allocation(vkdu_device *device, uint32_t width, uint32_t height,
        uint32_t format, uint64_t *bytes, uint64_t *alignment)
{
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_ALLOCATION_INFO info;
    if (!bytes || !alignment) return E_POINTER;
    *bytes = *alignment = 0;
    if (!device || FAILED(vkdu_texture_desc(width, height, format, &desc))) return E_INVALIDARG;
    info = ID3D12Device_GetResourceAllocationInfo(device->object, 0, 1, &desc);
    if (!info.SizeInBytes || info.SizeInBytes == UINT64_MAX || info.Alignment != 65536 ||
            info.SizeInBytes % info.Alignment) return E_INVALIDARG;
    *bytes = info.SizeInBytes; *alignment = info.Alignment;
    return S_OK;
}
int32_t vkdu_texture_heap_import(vkdu_device *device, void *owner, void *token, uint64_t bytes, vkdu_object **out)
{
    D3D12_HEAP_DESC desc = {0};
    ID3D12Heap *heap = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !owner || !token || !bytes || bytes % 65536) return E_INVALIDARG;
    desc.SizeInBytes = bytes; desc.Alignment = 65536;
    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Properties.CreationNodeMask = desc.Properties.VisibleNodeMask = 1;
    hr = vkd3d_create_heap_wddm(device->object, &desc, owner, token, &heap);
    hr = wrap(device, VKDU_MEMORY_HEAP, hr, (IUnknown *)heap, out);
    if (SUCCEEDED(hr)) {
        (*out)->bytes = bytes; (*out)->heap_type = desc.Properties.Type; (*out)->texture_heap = 1;
    }
    return hr;
}
int32_t vkdu_texture2d_place(vkdu_device *device, vkdu_object *heap, uint64_t offset,
        uint32_t width, uint32_t height, uint32_t format, uint32_t state, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc;
    ID3D12Resource *resource = NULL;
    uint64_t bytes, alignment;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !VALID(heap, VKDU_MEMORY_HEAP) || !heap->texture_heap || heap->owner != device->object ||
            (state != 0 && state != D3D12_RESOURCE_STATE_COPY_DEST && state != D3D12_RESOURCE_STATE_COPY_SOURCE &&
             state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE && state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE &&
             state != (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))) return E_INVALIDARG;
    if (FAILED(hr = vkdu_texture2d_allocation(device, width, height, format, &bytes, &alignment))) return hr;
    if (offset % alignment || offset > heap->bytes || bytes > heap->bytes - offset) return E_INVALIDARG;
    vkdu_texture_desc(width, height, format, &desc);
    hr = ID3D12Device_CreatePlacedResource(device->object, OBJ(ID3D12Heap, heap), offset, &desc, state, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    hr = wrap(device, VKDU_TEXTURE2D, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr)) { (*out)->width = width; (*out)->height = height; (*out)->format = format; (*out)->bytes = bytes; }
    return hr;
}
static int32_t vkdu_copy_location(vkdu_object *command, const struct vkdu_texture_copy_location *location,
        int destination, D3D12_TEXTURE_COPY_LOCATION *native, uint32_t *width, uint32_t *height, uint32_t *format)
{
    vkdu_object *resource = location->resource;
    uint64_t bytes;
    if (!resource || !vkdu_same_device(command, resource)) return E_INVALIDARG;
    memset(native, 0, sizeof(*native));
    if (!location->footprint) {
        if (!VALID(resource, VKDU_TEXTURE2D) || location->offset || location->format || location->width ||
                location->height || location->depth || location->row_pitch) return E_INVALIDARG;
        native->Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        *width = resource->width; *height = resource->height; *format = resource->format;
    } else {
        if (location->footprint != 1 || !VALID(resource, VKDU_BUFFER) || location->offset % 512 ||
                !location->width || location->width > 16384 || !location->height || location->height > 16384 ||
                location->depth != 1 || location->row_pitch % 256 || location->row_pitch < location->width * 4 ||
                (location->format != DXGI_FORMAT_R32_FLOAT && location->format != DXGI_FORMAT_R8G8B8A8_UNORM) ||
                (destination && resource->heap_type == D3D12_HEAP_TYPE_UPLOAD) ||
                (!destination && resource->heap_type == D3D12_HEAP_TYPE_READBACK)) return E_INVALIDARG;
        bytes = (uint64_t)location->row_pitch * (location->height - 1) + location->width * 4;
        if (location->offset > resource->bytes || bytes > resource->bytes - location->offset) return E_INVALIDARG;
        native->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        native->PlacedFootprint.Offset = location->offset; native->PlacedFootprint.Footprint.Format = location->format;
        native->PlacedFootprint.Footprint.Width = location->width; native->PlacedFootprint.Footprint.Height = location->height;
        native->PlacedFootprint.Footprint.Depth = 1; native->PlacedFootprint.Footprint.RowPitch = location->row_pitch;
        *width = location->width; *height = location->height; *format = location->format;
    }
    native->pResource = OBJ(ID3D12Resource, resource);
    return S_OK;
}
int32_t vkdu_command_texture_copy(vkdu_object *command, const struct vkdu_texture_copy_location *dst,
        uint32_t x, uint32_t y, uint32_t z, const struct vkdu_texture_copy_location *src, const struct vkdu_box *box)
{
    D3D12_TEXTURE_COPY_LOCATION native_dst, native_src;
    D3D12_BOX native_box;
    uint32_t sw, sh, sf, dw, dh, df;
    HRESULT hr;
    if (!RECORDING(command) || !dst || !src || dst->resource == src->resource ||
            (dst->footprint && src->footprint) || z) return E_INVALIDARG;
    if (FAILED(hr = vkdu_copy_location(command, dst, 1, &native_dst, &dw, &dh, &df)) ||
        FAILED(hr = vkdu_copy_location(command, src, 0, &native_src, &sw, &sh, &sf))) return hr;
    if (sf != df) return E_INVALIDARG;
    native_box.left = box ? box->left : 0; native_box.top = box ? box->top : 0;
    native_box.front = box ? box->front : 0; native_box.right = box ? box->right : sw;
    native_box.bottom = box ? box->bottom : sh; native_box.back = box ? box->back : 1;
    if (native_box.front || native_box.back != 1 || native_box.left >= native_box.right ||
            native_box.top >= native_box.bottom || native_box.right > sw || native_box.bottom > sh ||
            x > dw || native_box.right - native_box.left > dw - x ||
            y > dh || native_box.bottom - native_box.top > dh - y) return E_INVALIDARG;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_TEXTURE_DROP_OFFSET)
    /* Semantic negative control only: real GPU readback must observe this. */
    if (src->footprint) native_src.PlacedFootprint.Offset = 0;
#endif
    ID3D12GraphicsCommandList_CopyTextureRegion(OBJ(ID3D12GraphicsCommandList, command),
            &native_dst, x, y, 0, &native_src, &native_box);
    return S_OK;
}
