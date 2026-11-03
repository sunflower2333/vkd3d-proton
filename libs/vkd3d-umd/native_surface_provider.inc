/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A native import is backed by an existing runtime allocation. NativeShare
 * retains its host mapping; only a successfully published runtime association
 * is ours to Deallocate2Cb. Never destroy a foreign raw allocation separately. */
#pragma pack(push, 4)
struct NativeSurfaceShare {
    NativeAbiHeader header;
    uint32_t opcode, flags;
    uint64_t generation, key, address, size;
    uint32_t context, reserved;
    uint64_t reserved2[3];
};
struct NativeSurfaceResolve {
    NativeAbiHeader header;
    uint32_t opcode, flags, allocation, resource;
    uint64_t key, size, generation, reserved2;
};
#pragma pack(pop)
static_assert(sizeof(NativeSurfaceShare) == 88 && sizeof(NativeSurfaceResolve) == 64,
    "existing v0 native surface ABI");

HRESULT native_surface_share(Context *ctx, NativeHeap *heap, uint32_t opcode, bool retirement = false) {
    if ((!retirement && ctx->native_retiring) || !ctx->runtime_device.handle ||
            !ctx->kernel_callbacks.pfnEscapeCb || !ctx->native_heap_context) return DXGI_ERROR_DEVICE_REMOVED;
    NativeSurfaceShare info{};
    info.header = native_header(sizeof(info)); info.opcode = opcode;
    info.generation = ctx->native_adapter->generation; info.key = heap->native_key;
    info.address = heap->address; info.size = heap->bytes;
    const auto expected = info;
    D3DDDICB_ESCAPE args{};
    args.hDevice = ctx->runtime_device.handle; args.hContext = ctx->native_heap_context;
    args.pPrivateDriverData = &info; args.PrivateDriverDataSize = sizeof(info);
    HRESULT hr = ctx->kernel_callbacks.pfnEscapeCb(ctx->native_adapter->runtime.handle, &args);
    if (!ctx->runtime_device.handle || (!retirement && ctx->native_retiring)) return DXGI_ERROR_DEVICE_REMOVED;
    if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
    const uint32_t returned_context = info.context;
    info.context = 0;
    return returned_context == ctx->native_context_id && !std::memcmp(&expected, &info, sizeof(info)) ? S_OK : E_FAIL;
}

HRESULT native_lease_escape(Context *ctx, NativeSurfaceLeaseInfo &info) {
    if (!ctx->kernel_callbacks.pfnEscapeCb || !ctx->native_adapter) return DXGI_ERROR_DEVICE_REMOVED;
    // Adapter-scoped: the KMD rejects a device or context on opcodes 8/9.
    D3DDDICB_ESCAPE args{};
    args.pPrivateDriverData = &info; args.PrivateDriverDataSize = sizeof(info);
    return ctx->kernel_callbacks.pfnEscapeCb(ctx->native_adapter->runtime.handle, &args);
}
HRESULT native_lease_free(Context *ctx, const NativeSurfaceLeaseInfo &lease) {
    NativeSurfaceLeaseInfo request = lease;
    request.header = native_header(sizeof(request)); request.flags = 0;
    request.reserved2[0] = request.reserved2[1] = request.reserved2[2] = 0;
    request.opcode = 9; request.expected = request.generation;
    const auto expected = request;
    HRESULT hr = native_lease_escape(ctx, request);
    native_trace("native lease free key=%llu hr=%08x", (unsigned long long)lease.key, (unsigned)hr);
    if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
    return !std::memcmp(&request, &expected, sizeof(request)) ? S_OK : E_FAIL;
}

HRESULT native_surface_cleanup(Context *ctx, NativeHeap *heap, bool retirement) {
    if (heap->pending || native_heap_residency_busy(heap)) return DXGI_ERROR_WAS_STILL_DRAWING;
    heap->pending = true;
    auto *mapping = heap->native_mapping.get();
    if (mapping) ++mapping->closing;
    HRESULT hr = S_OK;
    // A Deallocate callback may recursively close the other open. Do not count
    // associations already inside their closing callback as surviving owners:
    // release the mapping BEFORE the final actual runtime association closes.
    if (heap->native_key && mapping && !mapping->released &&
            heap->native_mapping.use_count() == mapping->closing) {
        hr = native_surface_share(ctx, heap, 7, retirement);
        if (SUCCEEDED(hr)) { mapping->released = true; heap->native_uncertain = false; }
    }
    if (SUCCEEDED(hr) && ctx->runtime_device.handle && (retirement || !ctx->native_retiring)) {
        bool close_association = heap->resource_created;
#ifdef VKDU_TEST_NATIVE_SURFACE_CONTROLS
        if (native_surface_negative_active && native_surface_skip_close) close_association = false;
#endif
        if (close_association) {
            hr = native_heap_deallocate(ctx, heap);
            if (hr == S_OK) {
                heap->resource_created = false;
                heap->runtime_resource = nullptr;
                heap->allocation = 0;
            } else if (SUCCEEDED(hr)) hr = E_FAIL;
        } else {
            // Open failed or was cancelled before publication. Runtime closes
            // that association; it has never transferred cleanup to this UMD.
            heap->allocation = 0;
            heap->runtime_resource = nullptr;
        }
    }
    if (SUCCEEDED(hr) && heap->lease_owned && !heap->resource_created && !heap->allocation &&
            native_lease_free(ctx, heap->lease) == S_OK)
        heap->lease_owned = false; // A failed free leaves the KMD to collect it at process exit.
    if (!ctx->runtime_device.handle) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (mapping) --mapping->closing;
    if (SUCCEEDED(hr)) { heap->native_key = 0; heap->native_mapping.reset(); }
    heap->pending = false;
    return hr;
}

HRESULT native_surface_import(Context *ctx, D3DKMT_HANDLE allocation, D3DKMT_HANDLE kernel_resource,
        uint64_t key, uint64_t size, HANDLE runtime_resource, NativeHeap **out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    NativeHeapOperation op(ctx);
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (!ctx->native_surfaces || !ctx->kernel_callbacks.pfnDeallocate2Cb) return DXGI_ERROR_UNSUPPORTED;
    if (!allocation || !kernel_resource || !key || key > UINT32_MAX || !runtime_resource ||
            !size || size % 4096 || size > (UINT32_MAX & ~uint64_t(4095))) return E_INVALIDARG;
    HRESULT hr = native_heap_generation(ctx);
    if (SUCCEEDED(hr)) hr = native_heap_open_context(ctx);
    if (FAILED(hr)) { native_trace("surface import: context hr=%08x", (unsigned)hr); return hr; }
    NativeHeap *existing = nullptr;
    for (auto *entry = ctx->native_heaps; entry; entry = entry->next) {
        if (entry->runtime_resource == runtime_resource ||
                (entry->allocation == allocation && entry->native_key != key)) return E_INVALIDARG;
        if (entry->native_key != key) continue;
        if (entry->pending || native_heap_residency_busy(entry) || entry->native_uncertain ||
                !entry->native_mapping || entry->native_mapping->released)
            return DXGI_ERROR_WAS_STILL_DRAWING;
        if (entry->bytes != size) return E_INVALIDARG;
        existing = entry;
    }
    auto *heap = new (std::nothrow) NativeHeap;
    if (!heap) return E_OUTOFMEMORY;
    heap->bytes = size; heap->alignment = 4096; heap->flags = 4u | MWD_ALLOCATION_NATIVE_SURFACE;
    heap->pageable = true;
    heap->allocation = allocation; heap->runtime_resource = runtime_resource;
    if (existing) {
        heap->native_mapping = existing->native_mapping; heap->native_key = key;
        heap->address = existing->address;
        heap->next = existing->next; existing->next = heap;
    } else {
        try { heap->native_mapping = std::make_shared<NativeSurfaceMapping>(); }
        catch (const std::bad_alloc &) { delete heap; return E_OUTOFMEMORY; }
        if (FAILED(hr = native_heap_reserve(ctx, heap))) { delete heap; return hr; }
    }
    heap->pending = true;
    NativeSurfaceResolve resolve{};
    resolve.header = native_header(sizeof(resolve)); resolve.opcode = 10;
    resolve.allocation = allocation; resolve.key = key; resolve.size = size;
    resolve.generation = ctx->native_adapter->generation;
    const auto expected = resolve;
    D3DDDICB_ESCAPE escape{};
    escape.hDevice = ctx->runtime_device.handle;
    escape.pPrivateDriverData = &resolve; escape.PrivateDriverDataSize = sizeof(resolve);
    hr = ctx->kernel_callbacks.pfnEscapeCb(ctx->native_adapter->runtime.handle, &escape);
    native_trace("surface import: resolve hr=%08x res=%x expected=%x", (unsigned)hr, resolve.resource, kernel_resource);
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (hr == S_OK) {
        const uint32_t returned_resource = resolve.resource;
        resolve.resource = 0;
        if (returned_resource != kernel_resource || std::memcmp(&resolve, &expected, sizeof(resolve))) hr = E_INVALIDARG;
    } else if (SUCCEEDED(hr)) hr = E_FAIL;
    if (SUCCEEDED(hr) && !existing) {
        // Even a failed escape may have reached the host. Quarantine its VA
        // until release or runtime context teardown resolves that uncertainty.
        heap->native_key = key; heap->native_uncertain = true;
        hr = native_surface_share(ctx, heap, 6);
        native_trace("surface import: share(6) hr=%08x va=%llx", (unsigned)hr, (unsigned long long)heap->address);
        if (hr == S_OK) heap->native_uncertain = false;
    }
    if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
    heap->pending = false;
    if (FAILED(hr)) {
        if (op.live()) native_surface_cleanup(ctx, heap, false);
        if (!heap->allocation && !heap->native_key) native_heap_erase(ctx, heap);
        return hr;
    }
    *out = heap;
    return S_OK;
}

HRESULT native_surface_complete_open(Context *ctx, NativeHeap *heap) {
    NativeHeapOperation op(ctx);
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (!heap || native_token(ctx, heap) != heap || heap->flags != (4u | MWD_ALLOCATION_NATIVE_SURFACE) ||
            !heap->native_key || !heap->native_mapping || heap->native_mapping->released || heap->native_uncertain ||
            heap->pending || !heap->allocation ||
            !heap->runtime_resource || heap->resource_created) return E_INVALIDARG;
    // The caller revalidates identity and all native slots immediately before
    // this no-callback commit. Only successful publication transfers ownership.
    heap->resource_created = true;
    return S_OK;
}
