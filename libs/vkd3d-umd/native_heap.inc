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
static_assert(sizeof(NativeAllocationInfo) == 80 && offsetof(NativeAllocationInfo, context_id) == 76, "v0 allocation ABI");
constexpr uint64_t native_heap_alignment = 65536;
struct NativeHeapSlot { uint32_t magic; };
constexpr uint32_t native_heap_magic = 0x564b484d;
struct NativeHeap {
    NativeHeap *next = nullptr;
    void *slot = nullptr;
    D3DKMT_HANDLE allocation = 0;
    HANDLE runtime_resource = nullptr;
    uint64_t address = 0, bytes = 0;
    void *mapped = nullptr;
    uint32_t map_count = 0;
    uint32_t users = 1, flags = 4;
    uint64_t alignment = native_heap_alignment;
    bool cpu_visible = false, locked = false, pending = false, resource_created = false;
};

// One retained context and recursive lock cover every callback. A synchronous
// DestroyDevice may retire callbacks while the call is on the stack. Check
// native_retiring after each callback and never use retired callback handles.
struct NativeHeapOperation {
    Context *ctx;
    bool retained;
    explicit NativeHeapOperation(Context *value) : ctx(value) {
        retained = ctx && !ctx->finalizing;
        if (ctx) { if (retained) ++ctx->references; ctx->error_mutex.lock(); }
    }
    ~NativeHeapOperation() {
        if (ctx) { ctx->error_mutex.unlock(); if (retained) release(ctx); }
    }
    bool live() const { return ctx && !ctx->native_retiring && ctx->runtime_device.handle && ctx->native_adapter; }
};

// Vulkan can wait for queue workers which enter the runtime callback lock.
// Keep the operation's Context reference, but never that lock across a call
// into Vulkan or a backend destructor.
struct NativeBackendCall {
    Context *ctx;
    explicit NativeBackendCall(Context *value) : ctx(value) { ctx->error_mutex.unlock(); }
    ~NativeBackendCall() { ctx->error_mutex.lock(); }
};

NativeAbiHeader native_header(uint32_t size) { return {0x504d5644, 0, size, 0}; }
bool native_header_valid(const NativeAbiHeader &h, uint32_t size) {
    return h.magic == 0x504d5644 && !h.version && h.size == size && !h.reserved;
}

HRESULT native_heap_generation(Context *ctx) {
    if (ctx->native_retiring || !ctx->runtime_device.handle) return DXGI_ERROR_DEVICE_REMOVED;
    std::array<uint8_t, 8> luid{};
    uint64_t generation = 0;
    HRESULT hr = native_query_identity(*ctx->native_adapter, luid, generation);
    if (ctx->native_retiring) return DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(hr)) return hr;
    return luid == ctx->native_adapter->luid && generation == ctx->native_adapter->generation
        ? S_OK : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT native_heap_desc(const D3D12DDIARG_CREATEHEAP_0001 *heap,
        const D3D12DDIARG_CREATERESOURCE_0003 *resource) {
    if (!heap || !heap->ByteSize || heap->ByteSize % native_heap_alignment ||
            heap->ByteSize > (UINT32_MAX & ~uint64_t(4095)) ||
            (heap->Alignment && heap->Alignment != native_heap_alignment) ||
            heap->CreationNodeMask != 1 || heap->VisibleNodeMask != 1) return E_INVALIDARG;
    if (heap->Flags != D3D12DDI_HEAP_FLAG_BUFFERS || heap->MemoryPool != D3D12DDI_MEMORY_POOL_L0 ||
            (heap->CPUPageProperty != D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
             heap->CPUPageProperty != D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK)) return DXGI_ERROR_UNSUPPORTED;
    if (resource) {
        D3D12DDIARG_BUFFER_PLACEMENT no_reuse{};
        if (resource->ResourceType != D3D12DDI_RT_BUFFER || !resource->Width || resource->Width > heap->ByteSize ||
                resource->Height != 1 || resource->DepthOrArraySize != 1 || resource->MipLevels != 1 ||
                resource->Format != DXGI_FORMAT_UNKNOWN || resource->SampleDesc.Count != 1 || resource->SampleDesc.Quality ||
                resource->Layout != D3D12DDI_TL_ROW_MAJOR || resource->Flags || resource->pRowMajorLayout ||
                std::memcmp(&resource->ReuseBufferGPUVA, &no_reuse, sizeof(no_reuse))) return DXGI_ERROR_UNSUPPORTED;
        const uint32_t state = static_cast<uint32_t>(resource->InitialResourceState);
        if (state != 0 && state != 0x400 && state != 0x800) return DXGI_ERROR_UNSUPPORTED;
        if (heap->CPUPageProperty == D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK && state != 0x400)
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

HRESULT native_heap_cleanup(Context *ctx, NativeHeap *heap, bool retirement = false) {
    if (heap->pending) return DXGI_ERROR_WAS_STILL_DRAWING;
    heap->pending = true;
    HRESULT hr = native_heap_unlock(ctx, heap);
    // During normal operation DestroyDevice may have run inside UnlockCb.
    if (SUCCEEDED(hr) && (retirement || !ctx->native_retiring) && (heap->allocation || heap->resource_created)) {
        D3DDDICB_DEALLOCATE args{};
        // This slice owns exactly one heap per runtime resource. Releasing only
        // its allocation leaks the kernel resource (see DeallocateCb contract).
        if (heap->resource_created) args.hResource = heap->runtime_resource;
        else { args.NumAllocations = 1; args.HandleList = &heap->allocation; }
        hr = ctx->kernel_callbacks.pfnDeallocateCb(ctx->runtime_device.handle, &args);
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

D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY native_heap_sizes(D3D12DDI_HDEVICE h,
        const D3D12DDIARG_CREATEHEAP_0001 *heap, const D3D12DDIARG_CREATERESOURCE_0003 *resource) {
    NativeHeapOperation op(context(h));
    D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes{};
    if (op.live() && SUCCEEDED(native_heap_desc(heap, resource))) {
        sizes.Heap = sizeof(NativeHeapSlot);
        if (resource) sizes.Resource = sizeof(Object);
    }
    return sizes;
}

HRESULT APIENTRY native_heap_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001 *args,
        D3D12DDI_HHEAP slot, D3D12DDI_HRTRESOURCE runtime_resource,
        const D3D12DDIARG_CREATERESOURCE_0003 *resource, const D3D12DDI_CLEAR_VALUES *clear,
        D3D12DDI_HRESOURCE resource_slot) {
    NativeHeapOperation op(context(h));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    HRESULT hr = native_heap_desc(args, resource);
    if (FAILED(hr)) return hr;
    auto *ctx = op.ctx;
    if (!slot.pDrvPrivate || clear || (resource != nullptr) != (resource_slot.pDrvPrivate != nullptr) ||
            native_heap_find(ctx, slot.pDrvPrivate)) return E_INVALIDARG;
    if (runtime_resource.handle)
        for (auto *existing = ctx->native_heaps; existing; existing = existing->next)
            if (existing->runtime_resource == runtime_resource.handle) return E_INVALIDARG;
    if (FAILED(hr = native_heap_generation(ctx)) || FAILED(hr = native_heap_open_context(ctx))) return hr;
    auto *heap = new (std::nothrow) NativeHeap;
    if (!heap) return E_OUTOFMEMORY;
    heap->bytes = args->ByteSize;
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
            hr = vkdu_memory_heap_import(ctx->backend, ctx, heap, heap->bytes, heap->cpu_visible, &memory);
            if (SUCCEEDED(hr)) hr = vkdu_buffer_place(ctx->backend, memory, 0, resource->Width,
                                                       static_cast<uint32_t>(resource->InitialResourceState), &buffer);
            vkdu_object_destroy(memory);
        }
        if (SUCCEEDED(hr)) hr = native_heap_generation(ctx);
        if (SUCCEEDED(hr) && (!op.live() || vkdu_buffer_address(buffer) != heap->address))
            hr = DXGI_ERROR_DEVICE_REMOVED;
        if (SUCCEEDED(hr)) {
            hr = bind(ctx, resource_slot.pDrvPrivate, buffer, VKDU_BUFFER);
            if (SUCCEEDED(hr)) object(resource_slot.pDrvPrivate)->native_heap = heap;
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
    if (!heap->allocation && !heap->resource_created) native_heap_erase(op.ctx, heap);
    error(op.ctx, hr);
}

void native_heap_retire(Context *ctx) {
    // Called before callback detachment. Pending callbacks cannot be interrupted
    // safely; the runtime's device destruction owns their remaining KMT handles.
    bool pending = ctx->native_context_pending || ctx->native_submitting;
    for (auto *heap = ctx->native_heaps; heap; heap = heap->next) {
        if (heap->pending || ctx->native_submitting) { pending = true; continue; }
        HRESULT hr = native_heap_cleanup(ctx, heap, true);
        if (FAILED(hr)) ctx->last_error = hr;
        pending |= heap->allocation != 0 || heap->resource_created;
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
    while (ctx->native_heaps) {
        auto *heap = ctx->native_heaps; ctx->native_heaps = heap->next; delete heap;
    }
}

void native_heap_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *table) {
    table->pfnCalcPrivateHeapAndResourceSizes = native_heap_sizes;
    table->pfnCreateHeapAndResource = native_heap_create;
    table->pfnDestroyHeapAndResource = native_heap_destroy;
    table->pfnMapHeap = native_heap_map;
    table->pfnUnmapHeap = native_heap_unmap;
}

#include "runtime_memory.inc"
