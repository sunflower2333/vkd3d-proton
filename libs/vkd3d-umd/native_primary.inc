/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* DXGI swapchain buffers (D3D12DDI_HEAP_FLAG_PRIMARY committed textures).
 * DWM opens them in another process through its own UMD, so they use the
 * creator half of the existing VIOGPU/DXVK host-surface protocol:
 *   opcode 8 leases a host surface (key, stride, size),
 *   AllocateCb carries NativeResourceShare + NativeAllocationInfo,
 *   the allocation is then imported exactly like an opened surface,
 *   opcode 9 frees the lease after the wrapper allocation is gone. */
bool native_primary_format(DXGI_FORMAT format, uint32_t &fourcc, uint32_t &wire) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        fourcc = 0x34325241; wire = 1; return true; // AR24
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        fourcc = 0x34324241; wire = 3; return true; // AB24
    default: return false;
    }
}
bool native_lease_valid(const NativeSurfaceLeaseInfo &v, uint64_t generation, uint32_t width, uint32_t height,
        uint32_t fourcc) {
    return native_header_valid(v.header, sizeof(v)) && v.opcode == 8 && !v.flags && !v.expected &&
        v.key && v.key <= UINT32_MAX && v.size && v.size % 4096 == 0 && v.size <= (UINT32_MAX & ~uint64_t(4095)) &&
        v.generation == generation && !v.modifier && !v.offset && v.resource && !v.context &&
        v.width == width && v.height == height && v.fourcc == fourcc &&
        v.stride >= uint64_t(width) * 4 && v.stride % 4 == 0 && v.planes == 1 && v.layout_flags == 1 &&
        v.size >= uint64_t(v.stride) * height && !v.reserved2[0] && !v.reserved2[1] && !v.reserved2[2];
}

HRESULT native_primary_create(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATEHEAP_0001 *args,
        D3D12DDI_HHEAP heap_slot, D3D12DDI_HRTRESOURCE runtime, const D3D12DDIARG_CREATERESOURCE_0003 *resource,
        D3D12DDI_HRESOURCE resource_slot) {
    NativeHeapOperation op(context(device));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    auto *ctx = op.ctx;
    if (!ctx->native_surfaces || !ctx->kernel_callbacks.pfnDeallocate2Cb || !ctx->kernel_callbacks.pfnEscapeCb ||
            !ctx->kernel_callbacks.pfnAllocateCb) return DXGI_ERROR_UNSUPPORTED;
    if (!args || !resource) return E_INVALIDARG;
    const auto heap_request = *args;
    const auto request = *resource;
    const UINT flags = static_cast<UINT>(request.Flags);
    uint32_t fourcc = 0, wire = 0;
    if (request.ResourceType != D3D12DDI_RT_TEXTURE2D || !request.Width || request.Width > 16384 ||
            !request.Height || request.Height > 16384 || request.DepthOrArraySize != 1 || request.MipLevels != 1 ||
            request.SampleDesc.Count != 1 || request.SampleDesc.Quality ||
            !(flags & D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET) ||
            (flags & ~static_cast<UINT>(D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET | D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE)) ||
            request.pRowMajorLayout || !native_buffer_placement_empty(request.ReuseBufferGPUVA) ||
            !native_primary_format(request.Format, fourcc, wire) ||
            heap_request.MemoryPool != D3D12DDI_MEMORY_POOL_L0 ||
            heap_request.CPUPageProperty != D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE ||
            heap_request.CreationNodeMask != 1 || heap_request.VisibleNodeMask != 1 ||
            (static_cast<UINT>(heap_request.Flags) & ~static_cast<UINT>(D3D12DDI_HEAP_FLAG_PRIMARY | D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES))) {
        native_trace("primary: unsupported shape fmt=%u flags=%x heap=%x", (unsigned)request.Format, flags,
            (unsigned)heap_request.Flags);
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (!runtime.handle || heap_slot.pDrvPrivate == device.pDrvPrivate || resource_slot.pDrvPrivate == device.pDrvPrivate ||
            !native_open_slots_free(ctx, heap_slot.pDrvPrivate, resource_slot.pDrvPrivate) ||
            native_open_slot_pending(ctx, heap_slot.pDrvPrivate) || native_open_slot_pending(ctx, resource_slot.pDrvPrivate))
        return E_INVALIDARG;
    for (auto *existing = ctx->native_heaps; existing; existing = existing->next)
        if (existing->runtime_resource == runtime.handle) return E_INVALIDARG;
    NativeOpenReservation reservation(ctx, heap_slot.pDrvPrivate, resource_slot.pDrvPrivate);
    const auto width = static_cast<uint32_t>(request.Width), height = request.Height;
    HRESULT hr = native_heap_generation(ctx);
    if (FAILED(hr)) return hr;

    NativeSurfaceLeaseInfo lease{};
    lease.header = native_header(sizeof(lease)); lease.opcode = 8;
    lease.width = width; lease.height = height; lease.fourcc = fourcc;
    hr = native_lease_escape(ctx, lease);
    // Preserve returned ownership before validation; free by the exact reply.
    const bool owned = hr == S_OK && lease.key && lease.generation;
    native_trace("primary: lease %ux%u fourcc=%08x hr=%08x key=%llu size=%llu stride=%u gen=%llu res=%u",
        width, height, fourcc, (unsigned)hr, (unsigned long long)lease.key, (unsigned long long)lease.size,
        lease.stride, (unsigned long long)lease.generation, lease.resource);
    if (hr == S_OK && !native_lease_valid(lease, ctx->native_adapter->generation, width, height, fourcc)) hr = E_FAIL;
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (hr != S_OK) {
        if (owned && op.live()) native_lease_free(ctx, lease);
        return FAILED(hr) ? hr : E_FAIL;
    }

    NativeAllocationInfo info{};
    info.header = native_header(sizeof(info));
    info.size = lease.size; info.alignment = 4096; info.flags = 0; info.format = wire;
    info.width = width; info.height = height; info.pitch = lease.stride;
    NativeResourceShare share{};
    share.header = native_header(sizeof(share)); share.key = lease.key; share.stride = lease.stride; share.flags = 1;
    D3DDDI_ALLOCATIONINFO allocation{};
    allocation.pPrivateDriverData = &info; allocation.PrivateDriverDataSize = sizeof(info);
    D3DDDICB_ALLOCATE create{};
    create.pPrivateDriverData = &share; create.PrivateDriverDataSize = sizeof(share);
    create.hResource = runtime.handle; create.NumAllocations = 1; create.pAllocationInfo = &allocation;
    hr = ctx->kernel_callbacks.pfnAllocateCb(ctx->runtime_device.handle, &create);
    native_trace("primary: AllocateCb hr=%08x alloc=%x km=%x", (unsigned)hr, allocation.hAllocation, create.hKMResource);
    const bool allocated = SUCCEEDED(hr) || allocation.hAllocation || create.hKMResource;
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr) && !allocation.hAllocation) hr = E_FAIL;
    D3DKMT_HANDLE kernel_resource = create.hKMResource;
    if (SUCCEEDED(hr) && !kernel_resource) {
        // The 0022 core-layer callback does not report the kernel resource.
        // The KMD names this allocation's parent for the owning process.
        NativeSurfaceResolve query{};
        query.header = native_header(sizeof(query)); query.opcode = 10;
        query.allocation = allocation.hAllocation; query.key = lease.key; query.size = lease.size;
        query.generation = ctx->native_adapter->generation;
        D3DDDICB_ESCAPE escape{};
        escape.hDevice = ctx->runtime_device.handle;
        escape.pPrivateDriverData = &query; escape.PrivateDriverDataSize = sizeof(query);
        hr = ctx->kernel_callbacks.pfnEscapeCb(ctx->native_adapter->runtime.handle, &escape);
        if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
        if (hr == S_OK) kernel_resource = query.resource;
        native_trace("primary: resolve hr=%08x km=%x", (unsigned)hr, kernel_resource);
        if (SUCCEEDED(hr) && !kernel_resource) hr = E_FAIL;
    }
    auto release = [&](HRESULT result) {
        if (op.live() && allocated) {
            D3DDDICB_DEALLOCATE2 free_args{};
            free_args.hResource = runtime.handle;
            free_args.Flags.AssumeNotInUse = 1; free_args.Flags.SynchronousDestroy = 1;
            const HRESULT freed = ctx->kernel_callbacks.pfnDeallocate2Cb(ctx->runtime_device.handle, &free_args);
            // Only a released wrapper lets the KMD retire the lease's AHB.
            if (freed == S_OK) native_lease_free(ctx, lease);
        } else if (op.live()) native_lease_free(ctx, lease);
        return result;
    };
    if (FAILED(hr)) return release(hr);

    NativeHeap *heap = nullptr;
    hr = native_surface_import(ctx, allocation.hAllocation, kernel_resource, lease.key, lease.size,
        runtime.handle, &heap);
    native_trace("primary: import hr=%08x", (unsigned)hr);
    if (FAILED(hr)) return release(hr);
    heap->lease = lease; heap->lease_owned = true;
    vkdu_object *image = nullptr;
    {
        NativeBackendCall call(ctx);
        hr = vkdu_native_image_import(ctx->backend, ctx, heap, width, height, request.Format, lease.stride, 1, 0, &image);
    }
    native_trace("primary: image import hr=%08x", (unsigned)hr);
    if (hr != S_OK && SUCCEEDED(hr)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
    if (SUCCEEDED(hr) && !native_open_slots_free(ctx, heap_slot.pDrvPrivate, resource_slot.pDrvPrivate)) hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) hr = bind(ctx, resource_slot.pDrvPrivate, image, VKDU_TEXTURE2D);
    const bool bound = SUCCEEDED(hr);
    if (bound) hr = native_surface_complete_open(ctx, heap);
    if (SUCCEEDED(hr)) {
        auto *entry = object(resource_slot.pDrvPrivate);
        entry->native_heap = heap;
        entry->native_resource_info.ResourceDataSize = lease.size;
        entry->native_resource_info.ResourceDataAlignment = 4096;
        entry->native_resource_info.Layout = D3D12DDI_TL_ROW_MAJOR;
        entry->native_row_pitch = lease.stride;
        entry->native_depth_pitch = uint64_t(lease.stride) * height;
        heap->slot = heap_slot.pDrvPrivate;
        new (heap_slot.pDrvPrivate) NativeHeapSlot{native_heap_magic};
        return S_OK;
    }
    {
        NativeBackendCall call(ctx);
        if (bound) VioGpuD3D12BridgeUnbindObject(resource_slot.pDrvPrivate);
        else vkdu_object_destroy(image);
    }
    if (!heap->resource_created) {
        // The import published no runtime association: the wrapper is still
        // this creator's, so release it here and then the lease.
        heap->lease_owned = false;
        native_runtime_release(ctx, heap);
        return release(hr);
    }
    native_runtime_release(ctx, heap);
    return hr;
}
