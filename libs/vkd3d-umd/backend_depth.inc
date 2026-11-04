/* SPDX-License-Identifier: LGPL-2.1-or-later */
static HRESULT vkdu_depth_desc(uint32_t width, uint32_t height, uint32_t format, D3D12_RESOURCE_DESC *desc)
{
    if (!vkdu_depth_format(format)) return E_INVALIDARG;
    HRESULT hr = vkdu_texture_desc(width, height, DXGI_FORMAT_R32_FLOAT, desc);
    if (FAILED(hr)) return hr;
    desc->Format = format; desc->Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    return S_OK;
}
static void vkdu_depth_metadata(vkdu_object *out, uint32_t width, uint32_t height, uint32_t format, uint64_t bytes)
{
    out->width = width; out->height = height; out->bytes = bytes;
    out->format = format; out->resource_flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
}
int32_t vkdu_depth_allocation(vkdu_device *device, uint32_t width, uint32_t height, uint32_t format, uint64_t *bytes, uint64_t *alignment)
{
    D3D12_RESOURCE_DESC desc; D3D12_RESOURCE_ALLOCATION_INFO info;
    if (!bytes || !alignment) return E_POINTER;
    *bytes = *alignment = 0;
    if (!device || FAILED(vkdu_depth_desc(width, height, format, &desc))) return E_INVALIDARG;
    info = ID3D12Device_GetResourceAllocationInfo(device->object, 0, 1, &desc);
    if (!info.SizeInBytes || info.SizeInBytes == UINT64_MAX || info.Alignment != 65536 || info.SizeInBytes % 65536)
        return E_INVALIDARG;
    *bytes = info.SizeInBytes; *alignment = info.Alignment;
    return S_OK;
}
int32_t vkdu_depth_create(vkdu_device *device, uint32_t width, uint32_t height, uint32_t format, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc; D3D12_HEAP_PROPERTIES heap = {0};
    ID3D12Resource *resource = NULL; HRESULT hr; uint64_t bytes, alignment;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || FAILED(hr = vkdu_depth_allocation(device, width, height, format, &bytes, &alignment))) return E_INVALIDARG;
    vkdu_depth_desc(width, height, format, &desc);
    heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    hr = ID3D12Device_CreateCommittedResource(device->object, &heap, 0, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        NULL, &IID_ID3D12Resource, (void **)&resource);
    hr = wrap(device, VKDU_TEXTURE2D, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr)) vkdu_depth_metadata(*out, width, height, format, bytes);
    return hr;
}
int32_t vkdu_depth_place(vkdu_device *device, vkdu_object *heap, uint32_t width, uint32_t height, uint32_t format,
        uint32_t state, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc; ID3D12Resource *resource = NULL;
    HRESULT hr; uint64_t bytes, alignment;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !VALID(heap, VKDU_MEMORY_HEAP) || heap->texture_heap != 2 || heap->owner != device->object ||
            (state != D3D12_RESOURCE_STATE_COMMON && state != D3D12_RESOURCE_STATE_DEPTH_WRITE &&
             state != D3D12_RESOURCE_STATE_DEPTH_READ && state != D3D12_RESOURCE_STATE_COPY_SOURCE && state != D3D12_RESOURCE_STATE_COPY_DEST))
        return E_INVALIDARG;
    if (FAILED(hr = vkdu_depth_allocation(device, width, height, format, &bytes, &alignment))) return hr;
    if (bytes > heap->bytes) return E_INVALIDARG;
    vkdu_depth_desc(width, height, format, &desc);
    hr = ID3D12Device_CreatePlacedResource(device->object, OBJ(ID3D12Heap, heap), 0, &desc, state,
        NULL, &IID_ID3D12Resource, (void **)&resource);
    hr = wrap(device, VKDU_TEXTURE2D, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr)) vkdu_depth_metadata(*out, width, height, format, bytes);
    return hr;
}
int32_t vkdu_texture2d_dsv(vkdu_object *heap, uint32_t index, vkdu_object *texture, uint32_t flags)
{ return vkdu_texture2d_dsv_range(heap, index, texture, flags, 0, 0, 1); }
int32_t vkdu_texture2d_dsv_range(vkdu_object *heap, uint32_t index, vkdu_object *texture, uint32_t flags,
        uint32_t mip, uint32_t first_layer, uint32_t layers)
{
    D3D12_DEPTH_STENCIL_VIEW_DESC desc = {0}; D3D12_CPU_DESCRIPTOR_HANDLE handle;
    D3D12_RESOURCE_DESC resource;
    if (!VALID(heap, VKDU_DESCRIPTOR_HEAP) || heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV ||
            index >= heap->descriptor_count || !VALID(texture, VKDU_TEXTURE2D) || !vkdu_same_device(heap, texture) ||
            !vkdu_depth_format(texture->format) ||
            !(texture->resource_flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
            flags > (vkdu_stencil_format(texture->format) ?
                D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL : D3D12_DSV_FLAG_READ_ONLY_DEPTH)) return E_INVALIDARG;
    resource = ID3D12Resource_GetDesc(OBJ(ID3D12Resource, texture));
    if (mip >= resource.MipLevels || !layers || first_layer >= resource.DepthOrArraySize ||
            layers > resource.DepthOrArraySize - first_layer || resource.SampleDesc.Count != 1) return E_INVALIDARG;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_ATTACHMENT_VIEW_MIP)
    if (mip == 1 && layers == 2 && resource.MipLevels > 2) mip = 2;
#endif
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_ATTACHMENT_VIEW_LAYER)
    if (first_layer == 1 && layers == 2) first_layer = 0;
#endif
    desc.Format = texture->format; desc.Flags = flags;
    if (resource.DepthOrArraySize > 1) {
        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.MipSlice = mip; desc.Texture2DArray.FirstArraySlice = first_layer;
        desc.Texture2DArray.ArraySize = layers;
    } else { desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D; desc.Texture2D.MipSlice = mip; }
    handle.ptr = (SIZE_T)(vkdu_heap_start(heap, 0) + (uint64_t)index * heap->descriptor_stride);
    ID3D12Device_CreateDepthStencilView(heap->owner, OBJ(ID3D12Resource, texture), &desc, handle);
    HRESULT status = ID3D12Device_GetDeviceRemovedReason(heap->owner);
    vkdu_attachment_store(heap, index, texture, &resource, texture->format, mip, first_layer, layers, status);
    return status;
}
static int vkdu_dsv_handle(vkdu_object *command, vkdu_object *heap, uint32_t index, D3D12_CPU_DESCRIPTOR_HANDLE *out)
{
    if (!RECORDING(command) || command->command_type || !VALID(heap, VKDU_DESCRIPTOR_HEAP) ||
            heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || index >= heap->descriptor_count ||
            !vkdu_same_device(command, heap)) return 0;
    out->ptr = (SIZE_T)(vkdu_heap_start(heap, 0) + (uint64_t)index * heap->descriptor_stride);
    return 1;
}
int32_t vkdu_command_render_targets(vkdu_object *command, uint32_t count,
        const struct vkdu_render_target_binding *targets, vkdu_object *depth_heap, uint32_t depth_index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE colors[VKDU_MAX_RENDER_TARGETS], depth;
    struct vkdu_attachment_record records[VKDU_MAX_RENDER_TARGETS + 1];
    vkdu_object *resources[VKDU_MAX_RENDER_TARGETS + 1] = {0};
    uint32_t i, j, attachments;
    HRESULT hr = E_INVALIDARG;
    if (!RECORDING(command) || command->command_type || count > VKDU_MAX_RENDER_TARGETS ||
            (count && !targets) || (!depth_heap && depth_index)) return E_INVALIDARG;
    attachments = count + !!depth_heap;
    for (i = 0; i < count; ++i) {
        if (!vkdu_rtv_handle(command, targets[i].heap, targets[i].index, &colors[i])) goto done;
        resources[i] = vkdu_attachment_acquire(targets[i].heap, targets[i].index, &records[i]);
        if (!resources[i]) {
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_MRT_ACCEPT_RETIRED)
            /* Safe negative control: lie about success without dereferencing a stale descriptor. */
            if (records[i].lifetime) hr = S_OK;
#endif
            goto done;
        }
    }
    if (depth_heap) {
        if (!vkdu_dsv_handle(command, depth_heap, depth_index, &depth)) goto done;
        resources[count] = vkdu_attachment_acquire(depth_heap, depth_index, &records[count]);
        if (!resources[count]) goto done;
    }
    for (i = 0; i < attachments; ++i) {
        if (!vkdu_same_device(command, resources[i]) ||
                (i && !vkdu_attachment_dimensions_equal(&records[0], &records[i]))) goto done;
        for (j = 0; j < i && i < count; ++j)
            if (resources[i]->identity == resources[j]->identity && records[i].mip == records[j].mip &&
                    records[i].first_layer < (uint64_t)records[j].first_layer + records[j].layers &&
                    records[j].first_layer < (uint64_t)records[i].first_layer + records[i].layers) goto done;
    }
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_MRT_DROP_TARGETS)
    if (count > 1) count = 1;
#endif
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_MRT_DUPLICATE_LAST)
    if (count == VKDU_MAX_RENDER_TARGETS) colors[count - 1] = colors[0];
#endif
    ID3D12GraphicsCommandList_OMSetRenderTargets(OBJ(ID3D12GraphicsCommandList, command), count,
        count ? colors : NULL, FALSE, depth_heap ? &depth : NULL);
    command->render_target = !!count || !!depth_heap;
    command->attachment_samples = attachments ? records[0].sample_count : 0;
    command->attachment_quality = attachments ? records[0].sample_quality : 0;
    hr = S_OK;
done:
    for (i = 0; i < VKDU_MAX_RENDER_TARGETS + 1; ++i) vkdu_object_destroy(resources[i]);
    return hr;
}
int32_t vkdu_command_targets(vkdu_object *command, vkdu_object *color_heap, uint32_t color_index,
        vkdu_object *depth_heap, uint32_t depth_index)
{
    const struct vkdu_render_target_binding target = {color_heap, color_index};
    if (!color_heap && color_index) return E_INVALIDARG;
    return vkdu_command_render_targets(command, !!color_heap, color_heap ? &target : NULL, depth_heap, depth_index);
}
int32_t vkdu_command_clear_dsv(vkdu_object *command, vkdu_object *heap, uint32_t index,
        uint32_t flags, float depth, uint32_t stencil, uint32_t count, const struct vkdu_rect *rects)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle; D3D12_RECT native[16]; HRESULT hr;
    if (!vkdu_dsv_handle(command, heap, index, &handle) || !flags ||
            (flags & ~(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL)) ||
            ((flags & D3D12_CLEAR_FLAG_DEPTH) && (!isfinite(depth) || depth < 0 || depth > 1)) ||
            stencil > 255 || !vkdu_rects(count, rects, native)) return E_INVALIDARG;
    struct vkdu_attachment_record record;
    vkdu_object *resource = vkdu_attachment_acquire(heap, index, &record);
    if (!resource) return E_INVALIDARG;
    if ((flags & D3D12_CLEAR_FLAG_STENCIL) && !vkdu_stencil_format(record.format)) {
        vkdu_object_destroy(resource); return E_INVALIDARG;
    }
    /* Read-only DSV flags constrain draws, not explicit clears. Only the
     * requested aspect(s) are cleared; the other plane remains untouched.
     * Canonicalize an unused depth value before the Vulkan-facing engine. */
    hr = vkd3d_wddm_command_prepare_attachment_clear(OBJ(ID3D12GraphicsCommandList, command), handle, TRUE);
    if (SUCCEEDED(hr))
        ID3D12GraphicsCommandList_ClearDepthStencilView(OBJ(ID3D12GraphicsCommandList, command), handle,
            flags, flags & D3D12_CLEAR_FLAG_DEPTH ? depth : 0.0f, (uint8_t)stencil, count, native);
    vkdu_object_destroy(resource);
    return hr;
}
int32_t vkdu_command_stencil_ref(vkdu_object *command, uint32_t reference)
{
    if (!RECORDING(command) || command->command_type > 1) return E_INVALIDARG;
    ID3D12GraphicsCommandList_OMSetStencilRef(OBJ(ID3D12GraphicsCommandList, command), reference);
    return S_OK;
}
