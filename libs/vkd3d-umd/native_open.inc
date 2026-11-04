/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Existing VIOGPU/DXVK shared image wire format, not arbitrary D3D12 heaps.
 * All private metadata is copied before runtime or Vulkan callbacks. */
#pragma pack(push, 4)
struct NativeResourceShare {
    NativeAbiHeader header;
    uint64_t key;
    uint32_t stride, flags;
    uint64_t reserved[2];
};
#pragma pack(pop)
static_assert(sizeof(NativeResourceShare) == 48, "native resource share v0");
struct NativeOpenRequest {
    D3DKMT_HANDLE allocation = 0, kernel_resource = 0;
    NativeAllocationInfo info{};
    NativeResourceShare share{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
HRESULT native_open_request(const D3D12DDIARG_OPENHEAP_0003 *args, NativeOpenRequest &out) {
    if (!args) return E_INVALIDARG;
    const auto request = *args;
    if (request.NumAllocations != 1 || request.InitialResourceState != D3D12DDI_RESOURCE_STATE_COMMON)
        return DXGI_ERROR_UNSUPPORTED;
    if (!request.pOpenAllocationInfo || !request.hKMResource.handle || !request.pPrivateDriverData ||
            request.PrivateDriverDataSize != sizeof(NativeResourceShare)) return E_INVALIDARG;
    const auto opened = *request.pOpenAllocationInfo;
    if (!opened.hAllocation || !opened.pPrivateDriverData || opened.PrivateDriverDataSize != sizeof(NativeAllocationInfo))
        return E_INVALIDARG;
    std::memcpy(&out.info, opened.pPrivateDriverData, sizeof(out.info));
    std::memcpy(&out.share, request.pPrivateDriverData, sizeof(out.share));
    const auto &info = out.info; const auto &share = out.share;
    if (!native_header_valid(info.header, sizeof(info)) || !native_header_valid(share.header, sizeof(share)) ||
            !info.width || !info.height || info.width > 16384 || info.height > 16384 ||
            info.flags > 1 || !info.size || info.size % 4096 || info.size > (UINT32_MAX & ~uint64_t(4095)) ||
            info.alignment != 4096 || info.iova || info.generation || info.context_id ||
            info.pitch != share.stride || share.stride < uint64_t(info.width) * 4 || share.stride % 4 ||
            info.size < uint64_t(share.stride) * info.height || !share.key || share.key > UINT32_MAX ||
            share.flags != 1 || share.reserved[0] || share.reserved[1] ||
            (info.flags ? (!info.refresh_numerator || !info.refresh_denominator) :
                (info.refresh_numerator || info.refresh_denominator))) return E_INVALIDARG;
    if (info.format != 1 && info.format != 3) return DXGI_ERROR_UNSUPPORTED;
    out.allocation = opened.hAllocation; out.kernel_resource = request.hKMResource.handle;
    out.format = info.format == 1 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    return S_OK;
}

struct NativeOpenReservation {
    Context *context;
    void *heap, *resource;
    NativeOpenReservation *next;
    NativeOpenReservation(Context *ctx, void *h, void *r) : context(ctx), heap(h), resource(r), next(ctx->native_open_reservations) {
        ctx->native_open_reservations = this;
    }
    ~NativeOpenReservation() {
        auto **link = &context->native_open_reservations;
        while (*link && *link != this) link = &(*link)->next;
        if (*link) *link = next;
    }
};
bool native_open_slot_pending(Context *ctx, void *slot) {
    if (!ctx || !slot) return false;
    for (auto *entry = ctx->native_open_reservations; entry; entry = entry->next)
        if (entry->heap == slot || entry->resource == slot) return true;
    return false;
}
bool native_open_slots_free(Context *ctx, void *heap, void *resource) {
    const auto h = reinterpret_cast<uintptr_t>(heap), r = reinterpret_cast<uintptr_t>(resource);
    if (!h || !r || h > UINTPTR_MAX - sizeof(NativeHeapSlot) || r > UINTPTR_MAX - sizeof(Object) ||
            (h < r + sizeof(Object) && r < h + sizeof(NativeHeapSlot)) ||
            heap == ctx || resource == ctx || native_heap_find(ctx, heap) || native_heap_find(ctx, resource)) return false;
    AcquireSRWLockShared(&ctx->resources_lock);
    const bool free = !find_resource(ctx, resource) && !find_resource(ctx, heap);
    ReleaseSRWLockShared(&ctx->resources_lock);
    return free;
}
D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY native_open_sizes(D3D12DDI_HDEVICE device,
        const D3D12DDIARG_OPENHEAP_0003 *args) {
    NativeHeapOperation op(context(device));
    NativeOpenRequest request;
    if (!op.live() || !op.ctx->native_surfaces || !op.ctx->kernel_callbacks.pfnDeallocate2Cb ||
            FAILED(native_open_request(args, request))) return {};
    return {sizeof(NativeHeapSlot), sizeof(Object)};
}
HRESULT APIENTRY native_open_heap(D3D12DDI_HDEVICE device, const D3D12DDIARG_OPENHEAP_0003 *args,
        D3D12DDI_HHEAP heap_slot, D3D12DDI_HRTRESOURCE runtime, D3D12DDI_HRESOURCE resource_slot) {
    NativeHeapOperation op(context(device));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    auto *ctx = op.ctx;
    if (!ctx->native_surfaces || !ctx->kernel_callbacks.pfnDeallocate2Cb) return DXGI_ERROR_UNSUPPORTED;
    NativeOpenRequest request;
    HRESULT hr = native_open_request(args, request);
    if (FAILED(hr)) return hr;
    if (!runtime.handle || heap_slot.pDrvPrivate == device.pDrvPrivate || resource_slot.pDrvPrivate == device.pDrvPrivate ||
            !native_open_slots_free(ctx, heap_slot.pDrvPrivate, resource_slot.pDrvPrivate) ||
            native_open_slot_pending(ctx, heap_slot.pDrvPrivate) || native_open_slot_pending(ctx, resource_slot.pDrvPrivate))
        return E_INVALIDARG;
    NativeOpenReservation reservation(ctx, heap_slot.pDrvPrivate, resource_slot.pDrvPrivate);
    NativeHeap *heap = nullptr;
    hr = native_surface_import(ctx, request.allocation, request.kernel_resource,
        request.share.key, request.info.size, runtime.handle, &heap);
    if (FAILED(hr)) return hr;
    vkdu_object *image = nullptr;
    {
        NativeBackendCall call(ctx);
        hr = vkdu_native_image_import(ctx->backend, ctx, heap, request.info.width, request.info.height,
            request.format, request.info.pitch, 1, 1, &image);
    }
    if (hr != S_OK && SUCCEEDED(hr)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
    if (SUCCEEDED(hr) && !native_open_slots_free(ctx, heap_slot.pDrvPrivate, resource_slot.pDrvPrivate)) hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) hr = bind(ctx, resource_slot.pDrvPrivate, image, VKDU_TEXTURE2D);
    bool bound = SUCCEEDED(hr);
    if (bound) hr = native_surface_complete_open(ctx, heap);
    if (SUCCEEDED(hr)) {
        auto *entry = object(resource_slot.pDrvPrivate);
        entry->native_heap = heap;
        entry->native_resource_info.ResourceDataSize = request.info.size;
        entry->native_resource_info.ResourceDataAlignment = 4096;
        entry->native_resource_info.Layout = D3D12DDI_TL_ROW_MAJOR;
        entry->native_row_pitch = request.info.pitch;
        entry->native_depth_pitch = uint64_t(request.info.pitch) * request.info.height;
        heap->slot = heap_slot.pDrvPrivate;
        new (heap_slot.pDrvPrivate) NativeHeapSlot{native_heap_magic};
        return S_OK;
    }
    {
        NativeBackendCall call(ctx);
        if (bound) VioGpuD3D12BridgeUnbindObject(resource_slot.pDrvPrivate);
        else vkdu_object_destroy(image);
    }
    native_runtime_release(ctx, heap);
    return hr;
}
void native_open_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *table) {
    table->pfnCalcPrivateOpenedHeapAndResourceSizes = native_open_sizes;
    table->pfnOpenHeapAndResource = native_open_heap;
}
