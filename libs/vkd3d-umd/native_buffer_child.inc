/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ResourceHeaps: Capture Resource GPU VA. Only ordinary 64 KiB-aligned buffer
 * aliases are implemented; no new allocation, GPUVA reservation or remapping.
 * Re-import the same allocation to give each resource independent state while
 * retaining the parent's existing per-open kernel association. */
struct NativeBufferChild {
    NativeHeap *heap = nullptr;
    vkdu_object *parent = nullptr;
    uint64_t offset = 0, address = 0;
};

HRESULT native_child_request(Context *ctx, const D3D12DDIARG_CREATERESOURCE_0003 &resource,
        NativeBufferChild &out, bool retain) {
    const auto &placement = resource.ReuseBufferGPUVA.BaseAddress.UMD;
    if (!placement.hResource.pDrvPrivate) return DXGI_ERROR_UNSUPPORTED;
    if (placement.Offset % native_heap_alignment) return E_INVALIDARG;
    HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *parent = find_buffer(ctx, placement.hResource.pDrvPrivate);
    auto *heap = parent ? parent->native_heap : nullptr;
    auto *registered = ctx->native_heaps;
    while (registered && registered != heap) registered = registered->next;
    if (registered && heap->allocation && !heap->pending && !heap->native_uncertain &&
            !(heap->flags & MWD_ALLOCATION_NATIVE_SURFACE) && parent->address >= heap->address &&
            parent->address - heap->address <= heap->bytes &&
            parent->bytes <= heap->bytes - (parent->address - heap->address) &&
            placement.Offset < parent->bytes && resource.Width <= parent->bytes - placement.Offset) {
        D3D12DDIARG_CREATEHEAP_0001 description{};
        description.ByteSize = heap->bytes; description.Alignment = native_heap_alignment;
        description.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
        description.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
        description.CreationNodeMask = description.VisibleNodeMask = 1;
        description.CPUPageProperty = heap->cpu_visible ? D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK :
            D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        auto child = resource; child.ReuseBufferGPUVA = {};
        hr = native_heap_desc(&description, &child);
        if (SUCCEEDED(hr)) {
            out.heap = heap; out.offset = parent->address - heap->address + placement.Offset;
            out.address = heap->address + out.offset;
            if (out.offset % native_heap_alignment) hr = E_INVALIDARG;
            else if (retain) {
                if (vkdu_object_retain(parent->backend)) out.parent = parent->backend;
                else hr = E_INVALIDARG;
            }
        }
    }
    ReleaseSRWLockShared(&ctx->resources_lock);
    return hr;
}

D3D12DDI_HEAP_AND_RESOURCE_SIZES native_child_sizes(Context *ctx,
        const D3D12DDIARG_CREATERESOURCE_0003 *resource) {
    NativeBufferChild child;
    if (!resource || FAILED(native_child_request(ctx, *resource, child, false))) return {};
    return {0, sizeof(Object)};
}

bool native_child_slot_free(Context *ctx, void *slot) {
    const auto address = reinterpret_cast<uintptr_t>(slot);
    if (!address || address > UINTPTR_MAX - sizeof(Object) || slot == ctx || native_open_slot_pending(ctx, slot)) return false;
    const auto overlaps = [&](void *storage, size_t bytes) {
        const auto start = reinterpret_cast<uintptr_t>(storage);
        return start && address < start + bytes && start < address + sizeof(Object);
    };
    for (auto *pending = ctx->native_open_reservations; pending; pending = pending->next)
        if (overlaps(pending->heap, sizeof(NativeHeapSlot)) || overlaps(pending->resource, sizeof(Object))) return false;
    for (auto *heap = ctx->native_heaps; heap; heap = heap->next) {
        if (overlaps(heap->slot, sizeof(NativeHeapSlot))) return false;
    }
    bool free = true;
    AcquireSRWLockShared(&ctx->resources_lock);
    for (auto *head : {ctx->resources, ctx->descriptor_heaps, ctx->query_heaps})
        for (auto *p = head; p; p = p->next)
            if (overlaps(p, sizeof(Object))) free = false;
    ReleaseSRWLockShared(&ctx->resources_lock);
    return free;
}

HRESULT native_child_create(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP heap_slot,
        const D3D12DDIARG_CREATERESOURCE_0003 *resource, const D3D12DDI_CLEAR_VALUES *clear,
        D3D12DDI_HRESOURCE slot) {
    NativeHeapOperation op(context(device));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    auto *ctx = op.ctx;
    if (!resource || heap_slot.pDrvPrivate || clear || slot.pDrvPrivate == device.pDrvPrivate ||
            !native_child_slot_free(ctx, slot.pDrvPrivate)) return E_INVALIDARG;
    const auto request = *resource; // Never revisit caller storage after a callback.
    NativeBufferChild child;
    HRESULT hr = native_child_request(ctx, request, child, true);
    if (FAILED(hr)) return hr;
    NativeOpenReservation reservation(ctx, nullptr, slot.pDrvPrivate);
    vkdu_object *memory = nullptr, *buffer = nullptr;
    uint64_t bytes = 0, alignment = 0;
    {
        NativeBackendCall call(ctx);
        hr = vkdu_buffer_allocation(ctx->backend, request.Width, &bytes, &alignment);
    }
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr) && (!bytes || alignment != native_heap_alignment ||
            bytes % native_heap_alignment || bytes > child.heap->bytes - child.offset)) hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
    {
        NativeBackendCall call(ctx);
        if (SUCCEEDED(hr)) hr = vkdu_memory_heap_import(ctx->backend, ctx, child.heap,
                child.heap->bytes, child.heap->cpu_visible, &memory);
    }
    if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
    {
        NativeBackendCall call(ctx);
        if (SUCCEEDED(hr)) hr = vkdu_buffer_place(ctx->backend, memory, child.offset, request.Width,
                static_cast<UINT>(request.InitialResourceState), &buffer);
        vkdu_object_destroy(memory);
        // Cleanup can synchronously retire the runtime too. Finish it before
        // checking generation and publishing to runtime-owned output memory.
        vkdu_object_destroy(child.parent);
    }
    if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr) && (vkdu_buffer_address(buffer) != child.address ||
            vkdu_buffer_size(buffer) != request.Width)) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        hr = bind(ctx, slot.pDrvPrivate, buffer, VKDU_BUFFER, child.heap);
        if (SUCCEEDED(hr)) {
            auto *entry = object(slot.pDrvPrivate);
            entry->native_resource_info.ResourceDataSize = bytes;
            entry->native_resource_info.ResourceDataAlignment = alignment;
            entry->native_resource_info.Layout = D3D12DDI_TL_ROW_MAJOR;
            return S_OK;
        }
    }
    { NativeBackendCall call(ctx); vkdu_object_destroy(buffer); }
    return !op.live() ? DXGI_ERROR_DEVICE_REMOVED : hr;
}
