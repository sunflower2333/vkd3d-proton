/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Existing native buffer UAV forms only. Texture and structured clear
 * semantics remain explicit gaps, not silently accepted commands. */
int32_t vkdu_command_clear_uav(vkdu_object *command, vkdu_object *gpu_heap, uint32_t gpu_index,
        vkdu_object *cpu_heap, uint32_t cpu_index, vkdu_object *resource, int floating,
        const void *values, uint32_t count, const struct vkdu_rect *rects)
{
    const struct vkdu_uav_record *view;
    D3D12_RECT stack[16], *clipped = stack;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    ID3D12Resource *retained;
    uint32_t i, used = 0;
    HRESULT hr;
    if (!RECORDING(command) || (command->command_type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
            command->command_type != D3D12_COMMAND_LIST_TYPE_COMPUTE) || !values ||
            (floating != 0 && floating != 1) || (count && !rects) ||
            !VALID(resource, VKDU_BUFFER) || !vkdu_same_device(command, resource) ||
            !(resource->resource_flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
            !VALID(gpu_heap, VKDU_DESCRIPTOR_HEAP) || !VALID(cpu_heap, VKDU_DESCRIPTOR_HEAP) ||
            !vkdu_same_device(command, gpu_heap) || !vkdu_same_device(command, cpu_heap) ||
            gpu_heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
            cpu_heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
            !gpu_heap->shader_visible || cpu_heap->shader_visible ||
            gpu_index >= gpu_heap->descriptor_count || cpu_index >= cpu_heap->descriptor_count ||
            command->bound_heaps[0] != vkdu_heap_start(gpu_heap, 1)) return E_INVALIDARG;
    view = &cpu_heap->uav_records[cpu_index];
    if (view->resource_id != resource->identity || !view->count ||
            memcmp(view, &gpu_heap->uav_records[gpu_index], sizeof(*view))) return E_INVALIDARG;
    if (view->stride) return E_NOTIMPL;
    if (floating && view->format != DXGI_FORMAT_R32_FLOAT) return E_INVALIDARG;
    /* Validate the whole list before recording. Clip to the buffer view, not
     * the allocation: negative coordinates must never underflow GPU addresses.
     * Each buffer view is one row, and rectangles are relative to FirstElement. */
    for (i = 0; i < count; ++i)
        if (rects[i].left > rects[i].right || rects[i].top > rects[i].bottom) return E_INVALIDARG;
    if (count > sizeof(stack) / sizeof(stack[0]) && !(clipped = calloc(count, sizeof(*clipped))))
        return E_OUTOFMEMORY;
    for (i = 0; i < count; ++i) {
        int64_t left = rects[i].left < 0 ? 0 : rects[i].left;
        int64_t right = rects[i].right;
        if (right > view->count) right = view->count;
        if (left >= right || rects[i].top >= 1 || rects[i].bottom <= 0) continue;
        clipped[used++] = (D3D12_RECT){(LONG)left, 0, (LONG)right, 1};
    }
    if (count && !used) { if (clipped != stack) free(clipped); return S_OK; }
    retained = OBJ(ID3D12Resource, resource);
#if !defined(VKDU_ENABLE_TEST_DEVICE) || !defined(VKDU_TEST_UAV_CLEAR_DROP_OWNER)
    hr = vkd3d_wddm_command_retain_resources(OBJ(ID3D12GraphicsCommandList, command), 1, &retained);
#else
    hr = S_OK;
#endif
    if (SUCCEEDED(hr)) {
        gpu.ptr = vkdu_heap_start(gpu_heap, 1) + (uint64_t)gpu_index * gpu_heap->descriptor_stride;
        cpu.ptr = (SIZE_T)(vkdu_heap_start(cpu_heap, 0) + (uint64_t)cpu_index * cpu_heap->descriptor_stride);
#if !defined(VKDU_ENABLE_TEST_DEVICE) || !defined(VKDU_TEST_UAV_CLEAR_DROP)
        if (floating)
            ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(OBJ(ID3D12GraphicsCommandList, command),
                    gpu, cpu, retained, values, used, clipped);
        else
            ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(OBJ(ID3D12GraphicsCommandList, command),
                    gpu, cpu, retained, values, used, clipped);
#endif
        hr = ID3D12Device_GetDeviceRemovedReason(command->owner);
    }
    if (clipped != stack) free(clipped);
    return hr;
}
