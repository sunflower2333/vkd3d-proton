/* SPDX-License-Identifier: LGPL-2.1-or-later */
struct vkd3d_pageable_object { uint32_t type; uint64_t handle; };
static HRESULT vkd3d_wddm_collect_backing(struct d3d12_device *device,
        const struct vkd3d_pageable_object *objects, uint32_t object_count,
        uint32_t *count, struct mwd_allocation *allocations)
{
    struct mwd_allocation owned[3] = {{0}};
    struct mwd_context_info identity = {0};
    const struct mwd_callbacks callbacks = device->wddm_runtime_callbacks;
    void *owner = device->wddm_runtime_owner;
    uint32_t i, j, length = 0;
    HRESULT hr;
    if (!count || !allocations || object_count > 3) return E_INVALIDARG;
    *count = 0;
    if (!owner || !mwd_callbacks_valid(&callbacks)) return DXGI_ERROR_UNSUPPORTED;
    if (FAILED(hr = callbacks.context(owner, &identity))) return hr;
    for (i = 0; i < object_count; ++i)
    {
        struct mwd_pageable_backing reply = {0};
        if (!objects[i].handle) continue;
        if (!device->wddm_pageable_acquire) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }
        hr = device->wddm_pageable_acquire(device->vk_device, owner, identity.generation,
                objects[i].type, objects[i].handle, &reply);
        if (FAILED(hr)) goto fail;
        if (reply.flags == MWD_PAGEABLE_NO_BACKING && !reply.reserved && !reply.allocation.token &&
                !reply.allocation.handle && !reply.allocation.address && !reply.allocation.size &&
                !reply.allocation.generation && !reply.allocation.flags)
            continue;
        if (reply.flags || reply.reserved || !reply.allocation.token || !reply.allocation.handle ||
                reply.allocation.generation != identity.generation)
        {
            if (reply.allocation.token) callbacks.release(owner, reply.allocation.token);
            hr = (HRESULT)0x887a0020u; /* DXGI_ERROR_DRIVER_INTERNAL_ERROR */
            goto fail;
        }
        for (j = 0; j < length; ++j)
            if (owned[j].token == reply.allocation.token) break;
        if (j < length)
        {
            hr = callbacks.release(owner, reply.allocation.token);
            if (FAILED(hr)) goto fail;
        }
        else owned[length++] = reply.allocation;
    }
    if (FAILED(hr = callbacks.status(owner))) goto fail;
    memcpy(allocations, owned, length * sizeof(*owned));
    *count = length;
    return S_OK;
fail:
    while (length) callbacks.release(owner, owned[--length].token);
    return hr;
}

HRESULT vkd3d_wddm_descriptor_backing(ID3D12DescriptorHeap *iface,
        uint32_t *count, struct mwd_allocation *allocations)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);
    /* Snapshot every actual owned backing object before the first callback. */
    const struct vkd3d_pageable_object objects[] = {
        {MWD_PAGEABLE_MEMORY, (uint64_t)heap->descriptor_buffer.device_allocation.vk_memory},
        {MWD_PAGEABLE_MEMORY, (uint64_t)heap->device_allocation.vk_memory},
        {MWD_PAGEABLE_DESCRIPTOR_POOL, (uint64_t)heap->vk_descriptor_pool},
    };
    return vkd3d_wddm_collect_backing(heap->device, objects, ARRAY_SIZE(objects), count, allocations);
}

HRESULT vkd3d_wddm_query_backing(ID3D12QueryHeap *iface,
        uint32_t *count, struct mwd_allocation *allocations)
{
    struct d3d12_query_heap *heap = impl_from_ID3D12QueryHeap(iface);
    const struct vkd3d_pageable_object objects[] = {
        {MWD_PAGEABLE_MEMORY, (uint64_t)heap->device_allocation.vk_memory},
        {MWD_PAGEABLE_QUERY_POOL, (uint64_t)heap->vk_query_pool},
    };
    return vkd3d_wddm_collect_backing(heap->device, objects, ARRAY_SIZE(objects), count, allocations);
}
