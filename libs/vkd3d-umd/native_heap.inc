/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Included inside runtime.inc's anonymous namespace. This is a native KMD
 * heap owner and shared Turnip allocation domain. Not a WDDM2 GPUVA DDI. */

// Exact packed v0 subset of viogpu/shared/viogpu_wddm_abi.h. No host pointers.
#pragma pack(push, 4)
struct NativeAbiHeader { uint32_t magic, version, size, reserved; };
struct NativeContextCreate {
    NativeAbiHeader header;
    uint64_t generation;
    uint32_t flags, reserved;
};
struct NativeContextCreateShared {
    NativeContextCreate base;
    uint32_t allocation_context_id, reserved;
};
struct NativeContextInfo {
    NativeAbiHeader header;
    uint32_t opcode, flags;
    uint64_t expected_generation, va_start, va_size, generation;
    uint32_t context_id, queue_id;
};
struct NativeAllocationInfo {
    NativeAbiHeader header;
    uint64_t size, alignment, iova, generation;
    uint32_t flags, format, width, height, pitch, refresh_numerator, refresh_denominator, context_id;
};
#pragma pack(pop)
static_assert(sizeof(NativeContextCreate) == 32 && sizeof(NativeContextInfo) == 64, "v0 context ABI");
static_assert(sizeof(NativeContextCreateShared) == 40, "shared domain context ABI");
static_assert(sizeof(NativeAllocationInfo) == 80 && offsetof(NativeAllocationInfo, context_id) == 76, "v0 allocation ABI");
#pragma pack(push, 4)
/* Host surface lease (KMD escape opcodes 8/9), the creator half of the
 * VIOGPU/DXVK native surface protocol. Same 128-byte wire as DXVK's
 * NativeSurfaceInfo. */
struct NativeSurfaceLeaseInfo {
    NativeAbiHeader header;
    uint32_t opcode, flags;
    uint64_t expected, key, size, generation, modifier, offset;
    uint32_t resource, context, width, height, fourcc;
    uint32_t stride, planes, layout_flags;
    uint64_t reserved2[3];
};
#pragma pack(pop)
static_assert(sizeof(NativeSurfaceLeaseInfo) == 128, "native surface lease v0");
constexpr uint64_t native_heap_alignment = 65536;
static_assert((D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12DDI_RESOURCE_STATE_INDEX_BUFFER |
        D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12DDI_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12DDI_RESOURCE_STATE_COPY_SOURCE) == 0xac3, "buffer read states");
struct NativeHeapSlot { uint32_t magic; };
constexpr uint32_t native_heap_magic = 0x564b484d;
// Each runtime open has its own NativeHeap/association, while all opens of a
// native key share one GPU mapping and VA reservation until the final owner.
struct NativeSurfaceMapping {
    long closing = 0;
    uint32_t residency_operations = 0;
    bool released = false;
};
struct NativeHeap {
    NativeHeap *next = nullptr;
    void *slot = nullptr;
    D3DKMT_HANDLE allocation = 0;
    HANDLE runtime_resource = nullptr;
    uint64_t address = 0, bytes = 0;
    void *mapped = nullptr;
    uint32_t map_count = 0;
    uint32_t users = 1, flags = 4;
    uint32_t submitted = 0;
    uint32_t residency_operations = 0;
    uint64_t alignment = native_heap_alignment;
    bool cpu_visible = false, locked = false, pending = false, resource_created = false;
    // Public pageable heaps/resources have synchronous destruction semantics.
    // This survives slot retirement and backend token references. Internal ICD
    // scratch allocations retain their separate legacy deallocation policy.
    bool pageable = false;
    uint64_t native_key = 0;
    bool native_uncertain = false;
    std::shared_ptr<NativeSurfaceMapping> native_mapping;
    // Creator-owned host surface lease (swapchain buffers). Freed only after
    // the wrapper allocation is gone; the KMD retires the AHB after that.
    NativeSurfaceLeaseInfo lease{};
    bool lease_owned = false;
};
HRESULT native_surface_cleanup(Context *, NativeHeap *, bool);

bool native_heap_residency_busy(const NativeHeap *heap) {
    return heap->residency_operations ||
        (heap->native_mapping && heap->native_mapping->residency_operations);
}

// One retained context and recursive lock cover every callback. A synchronous
// DestroyDevice may retire callbacks while the call is on the stack. Check
// native_retiring after each callback and never use retired callback handles.
struct NativeHeapOperation {
    Context *ctx;
    bool retained;
    bool cleanup;
    explicit NativeHeapOperation(Context *value, bool allow_cleanup = false) : ctx(value), cleanup(allow_cleanup) {
        retained = retain_live(ctx);
        if (ctx) ctx->error_mutex.lock();
    }
    ~NativeHeapOperation() {
        if (ctx) { ctx->error_mutex.unlock(); if (retained) release(ctx); }
    }
    bool live() const {
        return ctx && !ctx->native_retiring && (!ctx->native_destroying || cleanup) &&
            ctx->runtime_device.handle && ctx->native_adapter;
    }
};

// Vulkan can wait for queue workers which enter the runtime callback lock.
// Keep the operation's Context reference, but never that lock across a call
// into Vulkan or a backend destructor.
struct NativeBackendCall {
    Context *ctx;
    unsigned depth;
    explicit NativeBackendCall(Context *value) : ctx(value) {
        ++ctx->native_backend_calls;
        depth = ctx->error_mutex.suspend();
    }
    ~NativeBackendCall() {
        ctx->error_mutex.resume(depth);
        --ctx->native_backend_calls;
    }
};

NativeAbiHeader native_header(uint32_t size) { return {0x504d5644, 0, size, 0}; }
bool native_header_valid(const NativeAbiHeader &h, uint32_t size) {
    return h.magic == 0x504d5644 && !h.version && h.size == size && !h.reserved;
}

HRESULT native_heap_generation(Context *ctx) {
    if (ctx->native_retiring || !ctx->runtime_device.handle) return DXGI_ERROR_DEVICE_REMOVED;
    NativeAdapterFacts current_facts{};
    HRESULT hr = native_query_adapter_facts(*ctx->native_adapter, current_facts);
    if (ctx->native_retiring || hr == DXGI_ERROR_UNSUPPORTED) return DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(hr)) return hr;
    return native_same_identity_facts(current_facts, ctx->native_facts)
        ? S_OK : DXGI_ERROR_DEVICE_REMOVED;
}

vkdu_texture2d_shape native_texture_shape(const D3D12DDIARG_CREATERESOURCE_0003 &r) {
    return {static_cast<UINT>(r.Width), r.Height, static_cast<UINT>(r.Format), r.MipLevels, r.DepthOrArraySize,
        static_cast<UINT>(r.Flags) & 3u, r.SampleDesc.Count, r.SampleDesc.Quality};
}
bool native_texture_subresources(const D3D12DDIARG_CREATERESOURCE_0003 &r) {
    return r.ResourceType == D3D12DDI_RT_TEXTURE2D && (r.MipLevels != 1 || r.DepthOrArraySize != 1 || r.SampleDesc.Count != 1);
}
bool native_buffer_placement_empty(const D3D12DDIARG_BUFFER_PLACEMENT &placement) {
    // The i386 ABI pads the 32-bit resource handle before the 64-bit offset.
    // Padding is not part of the DDI request and need not be initialized by
    // the runtime or by a field-wise copy of the placement structure.
    const auto &base = placement.BaseAddress.UMD;
    return !base.hResource.pDrvPrivate && !base.Offset;
}
HRESULT native_heap_desc(const D3D12DDIARG_CREATEHEAP_0001 *heap,
        const D3D12DDIARG_CREATERESOURCE_0003 *resource) {
    // Preserve unsupported-shape rejection before applying the MSAA heap contract.
    if (resource && resource->ResourceType == D3D12DDI_RT_TEXTURE2D && resource->SampleDesc.Count != 1) {
        const auto shape = native_texture_shape(*resource);
        if (resource->Width > 16384 || !vkdu_texture2d_shape_valid(&shape)) return DXGI_ERROR_UNSUPPORTED;
    }
    const uint64_t alignment = resource && resource->ResourceType == D3D12DDI_RT_TEXTURE2D &&
        resource->SampleDesc.Count > 1 ? 4194304 : native_heap_alignment;
    if (!heap || !heap->ByteSize || heap->ByteSize % alignment ||
            heap->ByteSize > (UINT32_MAX & ~uint64_t(4095)) ||
            (alignment == 4194304 ? heap->Alignment != alignment : heap->Alignment && heap->Alignment != alignment) ||
            heap->CreationNodeMask != 1 || heap->VisibleNodeMask != 1) return E_INVALIDARG;
    if ((heap->Flags != D3D12DDI_HEAP_FLAG_BUFFERS && heap->Flags != D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES &&
             heap->Flags != D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES) ||
            heap->MemoryPool != D3D12DDI_MEMORY_POOL_L0 ||
            (heap->CPUPageProperty != D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
             heap->CPUPageProperty != D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK)) return DXGI_ERROR_UNSUPPORTED;
    if (heap->Flags == D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES || heap->Flags == D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES) {
        const bool rt = heap->Flags == D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
        if (!resource || heap->CPUPageProperty != D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE) return DXGI_ERROR_UNSUPPORTED;
        const UINT resource_flags = static_cast<UINT>(resource->Flags);
        // The runtime includes SHADER_RESOURCE unless the API denies shader reads.
        // Normalize this independent binding bit for both color and depth resources.
        const UINT binding_flags = resource_flags & ~static_cast<UINT>(D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE);
        const bool depth = rt && binding_flags == D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL;
        const auto shape = native_texture_shape(*resource);
        if (resource->ResourceType != D3D12DDI_RT_TEXTURE2D || resource->Width > 16384 ||
                !vkdu_texture2d_shape_valid(&shape) ||
                (depth ? !vkdu_depth_format(resource->Format) :
                 resource->Format != DXGI_FORMAT_R32_FLOAT && !vkdu_color_resource_format(resource->Format)) ||
                (depth && resource->SampleDesc.Count != 1) ||
                (depth ? binding_flags != 2u : binding_flags != (rt ? 1u : 0u)) ||
                (rt && !depth && !vkdu_color_resource_format(resource->Format)) || resource->pRowMajorLayout ||
                (resource->Layout != D3D12DDI_TL_UNDEFINED && resource->Layout != D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0) ||
                !native_buffer_placement_empty(resource->ReuseBufferGPUVA)) return DXGI_ERROR_UNSUPPORTED;
        const UINT state = resource->InitialResourceState;
        if (depth) return !state || state == D3D12DDI_RESOURCE_STATE_DEPTH_WRITE ||
            state == D3D12DDI_RESOURCE_STATE_DEPTH_READ || state == 0x400 || state == 0x800 ? S_OK : DXGI_ERROR_UNSUPPORTED;
        return !state || (rt && state == 4) || state == 0x400 || state == 0x800 || state == 0x40 || state == 0x80 || state == 0xc0 || state == 0x1000 || state == 0x2000 ? S_OK : DXGI_ERROR_UNSUPPORTED;
    }
    if (resource) {
        if (resource->ResourceType != D3D12DDI_RT_BUFFER || !resource->Width || resource->Width > heap->ByteSize ||
                resource->Height != 1 || resource->DepthOrArraySize != 1 || resource->MipLevels != 1 ||
                resource->Format != DXGI_FORMAT_UNKNOWN || resource->SampleDesc.Count != 1 || resource->SampleDesc.Quality ||
                resource->Layout != D3D12DDI_TL_ROW_MAJOR ||
                (static_cast<UINT>(resource->Flags) & ~static_cast<UINT>(D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE)) || resource->pRowMajorLayout ||
                !native_buffer_placement_empty(resource->ReuseBufferGPUVA)) return DXGI_ERROR_UNSUPPORTED;
        const uint32_t state = static_cast<uint32_t>(resource->InitialResourceState);
        if (!vkdu_buffer_read_state(state) && state != 0x400 && state != D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS &&
                state != D3D12DDI_RESOURCE_STATE_STREAM_OUT)
            return DXGI_ERROR_UNSUPPORTED;
        if (heap->CPUPageProperty == D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK &&
                !vkdu_buffer_read_state(state) && state != 0x400 && state != D3D12DDI_RESOURCE_STATE_STREAM_OUT)
            return DXGI_ERROR_UNSUPPORTED;
    }
    return S_OK;
}

NativeHeap *native_heap_find(Context *ctx, void *slot) {
    if (!slot) return nullptr;
    for (auto *heap = ctx->native_heaps; heap; heap = heap->next)
        if (heap->slot == slot) return heap;
    return nullptr;
}

void native_heap_erase(Context *ctx, NativeHeap *heap) {
    auto **cursor = &ctx->native_heaps;
    while (*cursor && *cursor != heap) cursor = &(*cursor)->next;
    if (*cursor) *cursor = heap->next;
    delete heap;
}

HRESULT native_heap_unlock(Context *ctx, NativeHeap *heap) {
    if (!heap->locked) return S_OK;
    D3DDDICB_UNLOCK args{};
    args.NumAllocations = 1; args.phAllocations = &heap->allocation;
    HRESULT hr = ctx->kernel_callbacks.pfnUnlockCb(ctx->runtime_device.handle, &args);
    if (SUCCEEDED(hr)) { heap->locked = false; heap->mapped = nullptr; heap->map_count = 0; }
    return hr;
}

HRESULT native_heap_deallocate(Context *ctx, NativeHeap *heap) {
    if (heap->pageable) {
        if (!ctx->kernel_callbacks.pfnDeallocate2Cb) return DXGI_ERROR_UNSUPPORTED;
        D3DDDICB_DEALLOCATE2 args{};
        if (heap->resource_created) args.hResource = heap->runtime_resource;
        else { args.NumAllocations = 1; args.HandleList = &heap->allocation; }
        args.Flags.AssumeNotInUse = 1;
        args.Flags.SynchronousDestroy = 1;
        const HRESULT hr = ctx->kernel_callbacks.pfnDeallocate2Cb(ctx->runtime_device.handle, &args);
        return hr == S_OK ? S_OK : FAILED(hr) ? hr : E_FAIL;
    }
    D3DDDICB_DEALLOCATE args{};
    if (heap->resource_created) args.hResource = heap->runtime_resource;
    else { args.NumAllocations = 1; args.HandleList = &heap->allocation; }
    return ctx->kernel_callbacks.pfnDeallocateCb(ctx->runtime_device.handle, &args);
}

HRESULT native_heap_cleanup(Context *ctx, NativeHeap *heap, bool retirement = false) {
    if (heap->flags & MWD_ALLOCATION_NATIVE_SURFACE)
        return native_surface_cleanup(ctx, heap, retirement);
    if (heap->pending || native_heap_residency_busy(heap)) return DXGI_ERROR_WAS_STILL_DRAWING;
    heap->pending = true;
    HRESULT hr = native_heap_unlock(ctx, heap);
    // During normal operation DestroyDevice may have run inside UnlockCb.
    if (SUCCEEDED(hr) && (retirement || !ctx->native_retiring) && (heap->allocation || heap->resource_created)) {
        // This slice owns exactly one heap per runtime resource. Releasing only
        // its allocation leaks the kernel resource (see DeallocateCb contract).
        hr = native_heap_deallocate(ctx, heap);
        if (SUCCEEDED(hr)) { heap->allocation = 0; heap->resource_created = false; }
    }
    heap->pending = false;
    return hr;
}

HRESULT native_heap_close_context(Context *ctx) {
    if (!ctx->native_heap_context) return S_OK;
    D3DDDICB_DESTROYCONTEXT args{};
    args.hContext = ctx->native_heap_context;
    HRESULT hr = ctx->kernel_callbacks.pfnDestroyContextCb(ctx->runtime_device.handle, &args);
    if (SUCCEEDED(hr)) {
        ctx->native_heap_context = nullptr;
        ctx->native_va_start = ctx->native_va_size = 0; ctx->native_context_id = 0;
        ctx->native_queue_id = 0;
        ctx->native_commands = nullptr; ctx->native_allocation_list = nullptr; ctx->native_patch_list = nullptr;
        ctx->native_command_capacity = ctx->native_allocation_capacity = ctx->native_patch_capacity = 0;
    }
    return hr;
}

HRESULT native_heap_open_context(Context *ctx) {
    auto &cb = ctx->kernel_callbacks;
    if (!cb.pfnCreateContextCb || !cb.pfnDestroyContextCb || !cb.pfnEscapeCb ||
            !cb.pfnAllocateCb || !cb.pfnDeallocateCb || !cb.pfnLockCb || !cb.pfnUnlockCb)
        return DXGI_ERROR_UNSUPPORTED;
    if (ctx->native_context_pending) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (ctx->native_context_id) return S_OK;
    if (ctx->native_heap_context) {
        HRESULT hr = native_heap_close_context(ctx);
        if (ctx->native_retiring) return DXGI_ERROR_DEVICE_REMOVED;
        if (FAILED(hr)) return hr;
    }
    ctx->native_context_pending = true;
    NativeContextCreate data{};
    data.header = native_header(sizeof(data)); data.generation = ctx->native_adapter->generation;
    D3DDDICB_CREATECONTEXT args{};
    args.EngineAffinity = 1; args.pPrivateDriverData = &data; args.PrivateDriverDataSize = sizeof(data);
    HRESULT hr = cb.pfnCreateContextCb(ctx->runtime_device.handle, &args);
    ctx->native_heap_context = args.hContext;
    if (ctx->native_retiring) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr) && !args.hContext) hr = E_FAIL;
    NativeContextInfo info{};
    if (SUCCEEDED(hr)) {
        info.header = native_header(sizeof(info)); info.opcode = 1;
        info.expected_generation = ctx->native_adapter->generation;
        D3DDDICB_ESCAPE escape{};
        escape.hDevice = ctx->runtime_device.handle; escape.hContext = args.hContext;
        escape.pPrivateDriverData = &info; escape.PrivateDriverDataSize = sizeof(info);
        hr = cb.pfnEscapeCb(ctx->native_adapter->runtime.handle, &escape);
        if (ctx->native_retiring) hr = DXGI_ERROR_DEVICE_REMOVED;
    }
    if (SUCCEEDED(hr) && (!native_header_valid(info.header, sizeof(info)) || info.opcode != 1 || info.flags ||
            info.expected_generation != ctx->native_adapter->generation || info.generation != info.expected_generation ||
            !info.context_id || !info.queue_id || !info.va_start || !info.va_size ||
            ((info.va_start | info.va_size) & 4095) || info.va_size > UINT64_MAX - info.va_start)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
    if (SUCCEEDED(hr)) {
        ctx->native_va_start = info.va_start; ctx->native_va_size = info.va_size;
        ctx->native_context_id = info.context_id;
        ctx->native_queue_id = info.queue_id;
        ctx->native_commands = args.pCommandBuffer; ctx->native_command_capacity = args.CommandBufferSize;
        ctx->native_allocation_list = args.pAllocationList; ctx->native_allocation_capacity = args.AllocationListSize;
        ctx->native_patch_list = args.pPatchLocationList; ctx->native_patch_capacity = args.PatchLocationListSize;
    } else if (!ctx->native_retiring && ctx->native_heap_context) {
        // Keep ownership if cleanup fails, and retry before opening another.
        native_heap_close_context(ctx);
    }
    ctx->native_context_pending = false;
    return hr;
}

HRESULT native_heap_reserve(Context *ctx, NativeHeap *heap, uint64_t requested = 0) {
    uint64_t cursor = ctx->native_va_start;
    const uint64_t end = cursor + ctx->native_va_size;
    auto **position = &ctx->native_heaps;
    while (true) {
        if (cursor > UINT64_MAX - (heap->alignment - 1)) return E_OUTOFMEMORY;
        cursor = (cursor + heap->alignment - 1) & ~(heap->alignment - 1);
        const uint64_t limit = *position ? (*position)->address : end;
        if (requested && requested >= cursor && requested <= limit) cursor = requested;
        if ((!requested || requested == cursor) && cursor <= limit && heap->bytes <= limit - cursor) break;
        if (requested && requested < limit) return E_OUTOFMEMORY;
        if (!*position) return E_OUTOFMEMORY;
        cursor = (*position)->address + (*position)->bytes;
        position = &(*position)->next;
    }
    heap->address = cursor; heap->next = *position; *position = heap;
    return S_OK;
}

void APIENTRY native_allocation_info(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATERESOURCE_0003 *resource,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags, UINT64 restriction, UINT visible,
        D3D12DDI_RESOURCE_ALLOCATION_INFO *out) {
    NativeHeapOperation op(context(h));
    if (!op.live()) { error(op.ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    // CheckResourceAllocationInfo accepts zero as the single-GPU node mask,
    // equivalent to bit zero (PFND3D12DDI_CHECKRESOURCEALLOCATIONINFO_0088).
    const UINT visible_mask = visible ? visible : 1;
    const uint64_t required_alignment = resource && resource->ResourceType == D3D12DDI_RT_TEXTURE2D &&
        resource->SampleDesc.Count > 1 ? 4194304 : native_heap_alignment;
    if (!resource || !out || visible_mask != 1 || (restriction && restriction != required_alignment)) {
        error(op.ctx, E_INVALIDARG); return;
    }
    auto request = *resource;
    const bool texture = request.ResourceType == D3D12DDI_RT_TEXTURE2D;
    const UINT allowed = D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_SHADER_RESOURCE |
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_DETERMINISTIC |
        (texture && op.ctx->native_surfaces ? static_cast<UINT>(D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_PRIMARY) : 0u) |
        (texture ? 0u : static_cast<UINT>(D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_UNORDERED_ACCESS));
    if (static_cast<UINT>(flags) & ~allowed) { error(op.ctx, DXGI_ERROR_UNSUPPORTED); return; }
    // Validate the same resource subset as creation, independently of its
    // initial state. This description is only a validation bound, not a heap.
    D3D12DDIARG_CREATEHEAP_0001 bound{};
    bound.ByteSize = UINT32_MAX & ~(required_alignment - 1); bound.Alignment = required_alignment;
    bound.MemoryPool = D3D12DDI_MEMORY_POOL_L0; bound.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    const bool render_target = texture && (request.Flags & D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET);
    const bool depth = texture && (request.Flags & D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL);
    bound.Flags = render_target || depth ? D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES :
        texture ? D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES : D3D12DDI_HEAP_FLAG_BUFFERS;
    bound.CreationNodeMask = bound.VisibleNodeMask = 1;
    request.InitialResourceState = D3D12DDI_RESOURCE_STATE_COMMON;
    HRESULT hr = native_heap_desc(&bound, &request);
    if (FAILED(hr)) { error(op.ctx, hr); return; }
    D3D12DDI_RESOURCE_ALLOCATION_INFO result{};
    const auto shape = native_texture_shape(request);
    {
        NativeBackendCall call(op.ctx);
        hr = native_texture_subresources(request) ? vkdu_texture2d_shape_allocation(op.ctx->backend, &shape,
                &result.ResourceDataSize, &result.ResourceDataAlignment) :
            depth ? vkdu_depth_allocation(op.ctx->backend, static_cast<UINT>(request.Width), request.Height, request.Format,
                &result.ResourceDataSize, &result.ResourceDataAlignment) :
            render_target ? vkdu_render_target_allocation_format(op.ctx->backend, static_cast<UINT>(request.Width), request.Height, request.Format,
                &result.ResourceDataSize, &result.ResourceDataAlignment) :
            texture ? vkdu_texture2d_allocation(op.ctx->backend, static_cast<UINT>(request.Width), request.Height,
                request.Format, &result.ResourceDataSize, &result.ResourceDataAlignment) :
            vkdu_buffer_allocation(op.ctx->backend, request.Width, &result.ResourceDataSize, &result.ResourceDataAlignment);
    }
    if (SUCCEEDED(hr) && op.ctx->native_adapter) hr = native_heap_generation(op.ctx);
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr) && (!result.ResourceDataSize || result.ResourceDataSize > bound.ByteSize ||
            result.ResourceDataAlignment != required_alignment || result.ResourceDataSize % required_alignment)) hr = E_INVALIDARG;
    if (FAILED(hr)) { error(op.ctx, hr); return; }
    result.Layout = texture ? D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0 : D3D12DDI_TL_ROW_MAJOR;
    // Publish once, after any backend callback, without touching retired caller
    // storage. Auxiliary metadata is part of the opaque image's allocation.
    *out = result;
}

HRESULT native_heap_optimized_clear(const D3D12DDIARG_CREATERESOURCE_0003 *resource,
        const D3D12DDI_CLEAR_VALUES *clear) {
    if (!clear) return S_OK;
    if (!resource || resource->ResourceType != D3D12DDI_RT_TEXTURE2D)
        return E_INVALIDARG;
    if (resource->Flags & D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET)
        return vkdu_color_target_format(clear->Format) && vkdu_texture_view_compatible(resource->Format, clear->Format)
            ? S_OK : E_INVALIDARG;
    if (resource->Flags & D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL)
        return clear->Format == resource->Format &&
            vkdu_depth_format(resource->Format) &&
            clear->DepthStencil.Depth >= 0 && clear->DepthStencil.Depth <= 1 ? S_OK : E_INVALIDARG;
    return E_INVALIDARG;
}

D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY native_heap_sizes(D3D12DDI_HDEVICE h,
        const D3D12DDIARG_CREATEHEAP_0001 *heap, const D3D12DDIARG_CREATERESOURCE_0003 *resource) {
    NativeHeapOperation op(context(h));
    D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes{};
    if (op.live() && !heap) return native_child_sizes(op.ctx, resource);
    // Private storage must always fit the driver objects: the runtime
    // allocates exactly what is reported here before CreateHeapAndResource,
    // which performs all descriptor validation (including driver-sized
    // committed heaps whose ByteSize is UINT64_MAX).
    sizes.Heap = sizeof(NativeHeapSlot);
    if (resource) sizes.Resource = sizeof(Object);
    return sizes;
}

HRESULT native_heap_create_impl(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001 *args,
        D3D12DDI_HHEAP slot, D3D12DDI_HRTRESOURCE runtime_resource,
        const D3D12DDIARG_CREATERESOURCE_0003 *resource, const D3D12DDI_CLEAR_VALUES *clear,
        D3D12DDI_HRESOURCE resource_slot);
HRESULT native_primary_create(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATEHEAP_0001 *args,
        D3D12DDI_HHEAP heap_slot, D3D12DDI_HRTRESOURCE runtime, const D3D12DDIARG_CREATERESOURCE_0003 *resource,
        D3D12DDI_HRESOURCE resource_slot);
HRESULT APIENTRY native_heap_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001 *args,
        D3D12DDI_HHEAP slot, D3D12DDI_HRTRESOURCE runtime_resource,
        const D3D12DDIARG_CREATERESOURCE_0003 *resource, const D3D12DDI_CLEAR_VALUES *clear,
        D3D12DDI_HRESOURCE resource_slot) {
    if (args) native_trace("CreateHeapAndResource heap bytes=%llu align=%llu pool=%u cpu=%u flags=%x nodes=%u/%u",
        (unsigned long long)args->ByteSize, (unsigned long long)args->Alignment, (unsigned)args->MemoryPool,
        (unsigned)args->CPUPageProperty, (unsigned)args->Flags, args->CreationNodeMask, args->VisibleNodeMask);
    if (resource) native_trace("  resource type=%u w=%llu h=%u d=%u mips=%u fmt=%u samples=%u/%u layout=%u flags=%x state=%x rowmajor=%p",
        (unsigned)resource->ResourceType, (unsigned long long)resource->Width, resource->Height, resource->DepthOrArraySize,
        resource->MipLevels, (unsigned)resource->Format, resource->SampleDesc.Count, resource->SampleDesc.Quality,
        (unsigned)resource->Layout, (unsigned)resource->Flags, (unsigned)resource->InitialResourceState,
        (const void *)resource->pRowMajorLayout);
    const HRESULT hr = native_heap_create_impl(h, args, slot, runtime_resource, resource, clear, resource_slot);
    native_trace("CreateHeapAndResource hr=%08x", (unsigned)hr);
    return hr;
}
HRESULT native_heap_create_impl(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001 *args,
        D3D12DDI_HHEAP slot, D3D12DDI_HRTRESOURCE runtime_resource,
        const D3D12DDIARG_CREATERESOURCE_0003 *resource, const D3D12DDI_CLEAR_VALUES *clear,
        D3D12DDI_HRESOURCE resource_slot) {
    // Capture Resource GPU VA creates only a logical resource. It does not
    // create an allocation or acquire another runtime hResource association.
    if (!args) return native_child_create(h, slot, resource, clear, resource_slot);
    if ((args->Flags & D3D12DDI_HEAP_FLAG_PRIMARY) && resource)
        return native_primary_create(h, args, slot, runtime_resource, resource, resource_slot);
    NativeHeapOperation op(context(h));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    D3D12DDIARG_CREATEHEAP_0001 implicit_heap;
    if (args->ByteSize == UINT64_MAX && resource) {
        // Committed resource: the runtime leaves the heap size to the driver.
        // Size the implicit heap to the resource's own allocation.
        const bool is_texture = resource->ResourceType == D3D12DDI_RT_TEXTURE2D;
        const bool is_rt = is_texture && (resource->Flags & D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET);
        const bool is_depth = is_texture && (resource->Flags & D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL);
        const auto implicit_shape = native_texture_shape(*resource);
        uint64_t bytes = 0, alignment = 0;
        HRESULT size_hr;
        {
            NativeBackendCall call(op.ctx);
            size_hr = native_texture_subresources(*resource) ?
                vkdu_texture2d_shape_allocation(op.ctx->backend, &implicit_shape, &bytes, &alignment) :
                is_depth ? vkdu_depth_allocation(op.ctx->backend, static_cast<UINT>(resource->Width), resource->Height,
                    resource->Format, &bytes, &alignment) :
                is_rt ? vkdu_render_target_allocation_format(op.ctx->backend, static_cast<UINT>(resource->Width),
                    resource->Height, resource->Format, &bytes, &alignment) :
                is_texture ? vkdu_texture2d_allocation(op.ctx->backend, static_cast<UINT>(resource->Width), resource->Height,
                    resource->Format, &bytes, &alignment) :
                vkdu_buffer_allocation(op.ctx->backend, resource->Width, &bytes, &alignment);
        }
        if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
        if (FAILED(size_hr)) return size_hr;
        if (!bytes || !alignment) return E_INVALIDARG;
        implicit_heap = *args;
        implicit_heap.Alignment = alignment;
        implicit_heap.ByteSize = (bytes + alignment - 1) / alignment * alignment;
        args = &implicit_heap;
    }
    HRESULT hr = native_heap_desc(args, resource);
    if (FAILED(hr)) return hr;
    if (!op.ctx->kernel_callbacks.pfnDeallocate2Cb) return DXGI_ERROR_UNSUPPORTED;
    // This is an optional performance hint, never an initial clear. Validate
    // all used fields before any callback can expire the caller's storage.
    // Vulkan has no matching creation hint and the embedded VKD3D engine also
    // ignores optimized_clear_value (resource.c). Do not invent GPU work or
    // a dependency on a clear value after creation.
    if (FAILED(hr = native_heap_optimized_clear(resource, clear))) return hr;
    auto *ctx = op.ctx;
    const auto heap_request = *args;
    const auto resource_request = resource ? *resource : D3D12DDIARG_CREATERESOURCE_0003{};
    args = &heap_request;
    if (resource) resource = &resource_request;
    if (!slot.pDrvPrivate || (resource != nullptr) != (resource_slot.pDrvPrivate != nullptr) ||
            native_heap_find(ctx, slot.pDrvPrivate) || native_open_slot_pending(ctx, slot.pDrvPrivate) ||
            native_open_slot_pending(ctx, resource_slot.pDrvPrivate)) return E_INVALIDARG;
    const bool texture = resource && resource->ResourceType == D3D12DDI_RT_TEXTURE2D;
    const bool render_target = texture && (resource->Flags & D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET);
    const bool depth = texture && (resource->Flags & D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL);
    const bool subresources = resource && native_texture_subresources(*resource);
    const auto shape = resource ? native_texture_shape(*resource) : vkdu_texture2d_shape{};
    D3D12DDI_RESOURCE_ALLOCATION_INFO resource_info{};
    if (resource) {
        uint64_t bytes = 0, alignment = 0;
        {
            NativeBackendCall call(ctx);
            hr = subresources ? vkdu_texture2d_shape_allocation(ctx->backend, &shape, &bytes, &alignment) :
                depth ? vkdu_depth_allocation(ctx->backend, static_cast<UINT>(resource->Width), resource->Height, resource->Format,
                    &bytes, &alignment) : render_target ? vkdu_render_target_allocation_format(ctx->backend, static_cast<UINT>(resource->Width), resource->Height, resource->Format,
                    &bytes, &alignment) : texture ? vkdu_texture2d_allocation(ctx->backend, static_cast<UINT>(resource->Width), resource->Height,
                    resource->Format, &bytes, &alignment) : vkdu_buffer_allocation(ctx->backend, resource->Width, &bytes, &alignment);
        }
        if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
        if (FAILED(hr)) return hr;
        if (!bytes || bytes % alignment || alignment != (args->Alignment ? args->Alignment : native_heap_alignment) || bytes > args->ByteSize)
            return E_INVALIDARG;
        resource_info.ResourceDataSize = bytes;
        resource_info.ResourceDataAlignment = alignment;
        resource_info.Layout = texture ? D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0 : D3D12DDI_TL_ROW_MAJOR;
    }
    if (runtime_resource.handle)
        for (auto *existing = ctx->native_heaps; existing; existing = existing->next)
            if (existing->runtime_resource == runtime_resource.handle) return E_INVALIDARG;
    if (FAILED(hr = native_heap_generation(ctx)) || FAILED(hr = native_heap_open_context(ctx))) return hr;
    auto *heap = new (std::nothrow) NativeHeap;
    if (!heap) return E_OUTOFMEMORY;
    heap->bytes = args->ByteSize; heap->alignment = args->Alignment ? args->Alignment : native_heap_alignment;
    heap->pageable = true;
    heap->slot = slot.pDrvPrivate; heap->runtime_resource = runtime_resource.handle;
    heap->cpu_visible = args->CPUPageProperty == D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    // Turnip's coherent Vulkan memory types are CPU visible even when the
    // native D3D heap prohibits MapHeap. Keep the DDI permission separate.
    heap->flags = 6;
    if (FAILED(hr = native_heap_reserve(ctx, heap))) { delete heap; return hr; }
    heap->pending = true;
    NativeAllocationInfo data{};
    data.header = native_header(sizeof(data)); data.size = heap->bytes; data.alignment = 4096;
    data.iova = heap->address; data.generation = ctx->native_adapter->generation;
    data.flags = heap->flags; data.context_id = ctx->native_context_id;
    D3DDDI_ALLOCATIONINFO allocation{};
    allocation.pPrivateDriverData = &data; allocation.PrivateDriverDataSize = sizeof(data);
    D3DDDICB_ALLOCATE create{};
    create.hResource = runtime_resource.handle; create.NumAllocations = 1; create.pAllocationInfo = &allocation;
    hr = ctx->kernel_callbacks.pfnAllocateCb(ctx->runtime_device.handle, &create);
    heap->allocation = allocation.hAllocation; heap->pending = false;
    heap->resource_created = runtime_resource.handle && (SUCCEEDED(hr) || create.hKMResource || heap->allocation);
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr) && !heap->allocation) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
    if (SUCCEEDED(hr) && resource) {
        vkdu_object *memory = nullptr, *buffer = nullptr;
        heap->slot = nullptr; // No native slot is published while Vulkan imports.
        {
            NativeBackendCall backend_call(ctx);
            hr = render_target || depth ? (heap->alignment == 4194304 ?
                vkdu_render_target_heap_import_aligned(ctx->backend, ctx, heap, heap->bytes, heap->alignment, &memory) :
                vkdu_render_target_heap_import(ctx->backend, ctx, heap, heap->bytes, &memory)) :
                texture ? vkdu_texture_heap_import(ctx->backend, ctx, heap, heap->bytes, &memory) :
                vkdu_memory_heap_import(ctx->backend, ctx, heap, heap->bytes, heap->cpu_visible, &memory);
        }
        // Import can synchronously retire the runtime. Do not begin placement
        // against that device after reacquiring the callback lock.
        if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
        {
            NativeBackendCall backend_call(ctx);
            if (SUCCEEDED(hr)) hr = subresources ? vkdu_texture2d_shape_place(ctx->backend, memory, 0, &shape,
                    static_cast<UINT>(resource->InitialResourceState), &buffer) :
                depth ? vkdu_depth_place(ctx->backend, memory,
                    static_cast<UINT>(resource->Width), resource->Height, resource->Format, static_cast<UINT>(resource->InitialResourceState), &buffer) :
                render_target ? vkdu_render_target_place_format(ctx->backend, memory,
                    static_cast<UINT>(resource->Width), resource->Height, resource->Format, static_cast<UINT>(resource->InitialResourceState), &buffer) :
                texture ? vkdu_texture2d_place(ctx->backend, memory, 0,
                    static_cast<UINT>(resource->Width), resource->Height, resource->Format,
                    static_cast<UINT>(resource->InitialResourceState), &buffer) :
                vkdu_buffer_place(ctx->backend, memory, 0, resource->Width,
                    static_cast<uint32_t>(resource->InitialResourceState), &buffer);
            vkdu_object_destroy(memory);
        }
        if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
        if (SUCCEEDED(hr) && (!op.live() || (!texture && vkdu_buffer_address(buffer) != heap->address)))
            hr = DXGI_ERROR_DEVICE_REMOVED;
        if (SUCCEEDED(hr)) {
            hr = bind(ctx, resource_slot.pDrvPrivate, buffer, texture ? VKDU_TEXTURE2D : VKDU_BUFFER, heap);
            if (SUCCEEDED(hr)) {
                auto *entry = object(resource_slot.pDrvPrivate);
                entry->native_heap = heap;
                entry->native_resource_info = resource_info;
            }
        }
        if (FAILED(hr)) {
            NativeBackendCall backend_call(ctx);
            vkdu_object_destroy(buffer);
        }
    }
    if (FAILED(hr)) {
        heap->slot = nullptr;
        if (op.live()) native_heap_cleanup(ctx, heap);
        // Retain an orphaned allocation and its VA when deallocation fails.
        // Runtime device destruction owns final kernel cleanup after retirement.
        if (!heap->allocation && !heap->resource_created) native_heap_erase(ctx, heap);
        return hr;
    }
    heap->slot = slot.pDrvPrivate;
    new (slot.pDrvPrivate) NativeHeapSlot{native_heap_magic};
    return S_OK;
}

HRESULT native_heap_map_record(Context *value, NativeHeap *heap, void **out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    NativeHeapOperation op(value);
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    auto *ctx = op.ctx;
    if (!heap || !(heap->flags & 2) || heap->pending || !heap->allocation) return E_INVALIDARG;
    if (native_heap_residency_busy(heap)) return DXGI_ERROR_WAS_STILL_DRAWING;
    // A new LockCb can rename its allocation handle. Preserve the handle used
    // by every outstanding DMA packet until its OS completion event retires.
    if (heap->submitted && !heap->locked) return DXGI_ERROR_WAS_STILL_DRAWING;
    HRESULT hr = native_heap_generation(ctx);
    if (FAILED(hr)) return hr;
    if (heap->map_count == UINT32_MAX) return E_INVALIDARG;
    if (heap->locked && heap->mapped) { ++heap->map_count; *out = heap->mapped; return S_OK; }
    if (heap->locked) return E_FAIL; // Failed null-pointer unlock: teardown owns it.
    heap->pending = true;
    D3DDDICB_LOCK args{};
    args.hAllocation = heap->allocation; args.Flags.LockEntire = 1;
    hr = ctx->kernel_callbacks.pfnLockCb(ctx->runtime_device.handle, &args);
    if (SUCCEEDED(hr)) {
        // LockCb may return a renamed handle. Always keep the returned owner.
        if (args.hAllocation) heap->allocation = args.hAllocation;
        heap->locked = true; heap->mapped = args.pData;
    }
    heap->pending = false;
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr) && (!args.hAllocation || !args.pData)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
    if (FAILED(hr)) {
        if (op.live() && heap->locked) native_heap_unlock(ctx, heap);
        return hr;
    }
    heap->map_count = 1; *out = heap->mapped;
    return S_OK;
}

HRESULT APIENTRY native_heap_map(D3D12DDI_HDEVICE h, D3D12DDI_HHEAP slot, void **out) {
    NativeHeapOperation op(context(h));
    auto *heap = op.ctx ? native_heap_find(op.ctx, slot.pDrvPrivate) : nullptr;
    return native_heap_map_record(op.ctx, heap && heap->cpu_visible ? heap : nullptr, out);
}

void APIENTRY native_heap_unmap(D3D12DDI_HDEVICE h, D3D12DDI_HHEAP slot) {
    NativeHeapOperation op(context(h));
    if (!op.live()) return;
    auto *heap = native_heap_find(op.ctx, slot.pDrvPrivate);
    if (!heap || heap->pending || !heap->map_count) { error(op.ctx, E_INVALIDARG); return; }
    if (native_heap_residency_busy(heap)) { error(op.ctx, DXGI_ERROR_WAS_STILL_DRAWING); return; }
    if (heap->map_count > 1) { --heap->map_count; return; }
    heap->pending = true;
    HRESULT hr = native_heap_unlock(op.ctx, heap);
    heap->pending = false;
    error(op.ctx, hr);
}

void APIENTRY native_heap_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HHEAP slot, D3D12DDI_HRESOURCE resource) {
    NativeHeapOperation op(context(h));
    if (!op.ctx) return;
    auto *heap = native_heap_find(op.ctx, slot.pDrvPrivate);
    if (resource.pDrvPrivate) {
        auto *obj = object(resource.pDrvPrivate);
        if (!obj || obj->context != op.ctx || !obj->native_heap || (heap && obj->native_heap != heap)) {
            error(op.ctx, E_INVALIDARG); return;
        }
        // The placed backend resource releases its imported BO before the
        // runtime heap owner is released. Separate resource destruction leaves
        // any still-live native heap slot intact.
        {
            NativeBackendCall backend_call(op.ctx);
            VioGpuD3D12BridgeUnbindObject(resource.pDrvPrivate);
        }
        heap = native_heap_find(op.ctx, slot.pDrvPrivate);
        if (!slot.pDrvPrivate) return;
    }
    if (!heap || heap->pending) { error(op.ctx, E_INVALIDARG); return; }
    // Runtime memory expires on return, even when kernel cleanup fails.
    heap->slot = nullptr;
    std::memset(slot.pDrvPrivate, 0, sizeof(NativeHeapSlot));
    if (heap->users > 1) { --heap->users; return; }
    HRESULT hr = op.live() ? native_heap_cleanup(op.ctx, heap) : DXGI_ERROR_DEVICE_REMOVED;
    if (!heap->allocation && !heap->resource_created && !heap->native_key) native_heap_erase(op.ctx, heap);
    error(op.ctx, hr);
}

void native_heap_retire(Context *ctx) {
    // Called before callback detachment. Pending callbacks cannot be interrupted
    // safely; the runtime's device destruction owns their remaining KMT handles.
    // An unlocked backend import/destructor may still be using a mapped BO.
    // Do not issue driver Unlock/Deallocate/DestroyContext while that call runs.
    // This does not keep KMT backing alive after the runtime's own device
    // teardown: cancellation is rechecked before further backend operations.
    if (ctx->native_backend_calls || ctx->native_submissions) return;
    bool pending = ctx->native_context_pending || ctx->native_submitting;
    for (auto *heap = ctx->native_heaps; heap; heap = heap->next) {
        if (heap->pending || native_heap_residency_busy(heap) || ctx->native_submitting) { pending = true; continue; }
        HRESULT hr = native_heap_cleanup(ctx, heap, true);
        if (FAILED(hr)) ctx->last_error = hr;
        pending |= heap->allocation != 0 || heap->resource_created || heap->native_key != 0;
    }
    if (!pending && ctx->native_heap_context) {
        HRESULT hr = native_heap_close_context(ctx);
        if (FAILED(hr)) ctx->last_error = hr;
    }
}

void native_heap_forget(Context *ctx) {
    // No runtime callback is legal after DestroyDevice returns. Any unreleased
    // kernel allocations/context belong to runtime device teardown, not a later
    // stale callback. User-mode residual records no longer retain runtime slots.
    native_submission_forget(ctx);
    native_runtime_queues_forget(ctx);
    while (ctx->native_heaps) {
        auto *heap = ctx->native_heaps; ctx->native_heaps = heap->next; delete heap;
    }
}

#include "native_resource_queries.inc"

void native_heap_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *table) {
    native_open_tables(table);
    table->pfnCheckResourceAllocationInfo = native_allocation_info;
    table->pfnCheckExistingResourceAllocationInfo = native_existing_allocation_info;
    table->pfnCheckResourceAllocationHandle = native_resource_allocation_handle;
    table->pfnCheckResourceVirtualAddress = native_resource_address;
    table->pfnCheckSubresourceInfo = native_subresource_info;
    table->pfnCalcPrivateHeapAndResourceSizes = native_heap_sizes;
    table->pfnCreateHeapAndResource = native_heap_create;
    table->pfnDestroyHeapAndResource = native_heap_destroy;
    table->pfnMapHeap = native_heap_map;
    table->pfnUnmapHeap = native_heap_unmap;
}

#include "runtime_memory.inc"
