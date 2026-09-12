/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Private Turnip callbacks. Tokens are looked up, never dereferenced before
 * validating ownership. Runtime slots and opaque runtime handles do not cross
 * the Vulkan boundary. All callbacks serialize with native device retirement. */
#pragma pack(push, 4)
struct NativeFenceInfo {
    NativeAbiHeader header;
    uint32_t opcode, flags;
    uint64_t expected_generation, completed, generation;
    uint32_t context_id, reserved;
};
struct NativeRenderInfo {
    NativeAbiHeader header;
    uint32_t opcode, flags;
    uint64_t generation;
    uint32_t references_offset, references_count, stream_offset, stream_size, reserved[4];
};
struct NativeRenderReference {
    uint32_t index, flags;
    uint64_t offset, length;
    uint32_t patch_offset, reserved;
};
#pragma pack(pop)
static_assert(sizeof(NativeFenceInfo) == 56 && sizeof(NativeRenderInfo) == 64 &&
    sizeof(NativeRenderReference) == 32, "v0 submit ABI");

#include "native_completion.inc"

NativeHeap *native_token(Context *ctx, void *token) {
    if (ctx && token)
        for (auto *heap = ctx->native_heaps; heap; heap = heap->next)
            if (heap == token) return heap;
    return nullptr;
}
void native_export(Context *ctx, NativeHeap *heap, mwd_allocation *out) {
    *out = {heap, heap->address, heap->bytes, ctx->native_adapter->generation, heap->allocation, heap->flags};
}
int32_t MWD_CALL native_runtime_status(void *owner) {
    NativeHeapOperation op(static_cast<Context *>(owner), true);
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    HRESULT hr = native_submission_reap(op.ctx);
    return FAILED(hr) ? hr : native_heap_generation(op.ctx);
}
int32_t MWD_CALL native_runtime_context(void *owner, mwd_context_info *out) {
    if (!out) return E_POINTER;
    *out = {};
    NativeHeapOperation op(static_cast<Context *>(owner), true);
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    HRESULT hr = native_heap_generation(op.ctx);
    if (SUCCEEDED(hr) && op.ctx->native_destroying && !op.ctx->native_heap_context)
        return DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) hr = native_heap_open_context(op.ctx);
    if (FAILED(hr)) return hr;
    if (!op.ctx->kernel_callbacks.pfnRenderCb) return DXGI_ERROR_UNSUPPORTED;
    std::memcpy(out->luid, op.ctx->native_adapter->luid.data(), sizeof(out->luid));
    out->generation = op.ctx->native_adapter->generation;
    out->va_start = op.ctx->native_va_start; out->va_size = op.ctx->native_va_size;
    out->context_id = op.ctx->native_context_id; out->queue_id = op.ctx->native_queue_id;
    return S_OK;
}
int32_t MWD_CALL native_runtime_allocate(void *owner, uint64_t size, uint64_t alignment,
        uint64_t requested, uint32_t flags, mwd_allocation *out) {
    if (!out) return E_POINTER;
    *out = {};
    NativeHeapOperation op(static_cast<Context *>(owner));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (!size || size > (UINT32_MAX & ~uint64_t(4095)) || alignment < 4096 ||
            (alignment & (alignment - 1)) || (requested & (alignment - 1)) ||
            (flags & ~14u) || !(flags & 4u)) return E_INVALIDARG;
    HRESULT hr = native_heap_generation(op.ctx);
    if (SUCCEEDED(hr)) hr = native_heap_open_context(op.ctx);
    if (FAILED(hr)) return hr;
    auto *heap = new (std::nothrow) NativeHeap;
    if (!heap) return E_OUTOFMEMORY;
    heap->bytes = (size + 4095) & ~uint64_t(4095);
    heap->alignment = alignment; heap->flags = flags; heap->cpu_visible = (flags & 2) != 0;
    if (FAILED(hr = native_heap_reserve(op.ctx, heap, requested))) { delete heap; return hr; }
    heap->pending = true;
    NativeAllocationInfo info{};
    info.header = native_header(sizeof(info)); info.size = heap->bytes; info.alignment = 4096;
    info.iova = heap->address; info.flags = flags; info.context_id = op.ctx->native_context_id;
    info.generation = op.ctx->native_adapter->generation;
    D3DDDI_ALLOCATIONINFO allocation{};
    allocation.pPrivateDriverData = &info; allocation.PrivateDriverDataSize = sizeof(info);
    D3DDDICB_ALLOCATE args{};
    args.NumAllocations = 1; args.pAllocationInfo = &allocation;
    hr = op.ctx->kernel_callbacks.pfnAllocateCb(op.ctx->runtime_device.handle, &args);
    heap->allocation = allocation.hAllocation; heap->pending = false;
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr) && !heap->allocation) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = native_heap_generation(op.ctx);
    if (FAILED(hr)) {
        if (op.live()) native_heap_cleanup(op.ctx, heap);
        if (!heap->allocation) native_heap_erase(op.ctx, heap);
        return hr;
    }
    native_export(op.ctx, heap, out);
    return S_OK;
}
int32_t MWD_CALL native_runtime_retain(void *owner, void *token, mwd_allocation *out) {
    if (!out) return E_POINTER;
    *out = {};
    NativeHeapOperation op(static_cast<Context *>(owner));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    auto *heap = native_token(op.ctx, token);
    if (!heap || !heap->allocation || heap->pending || heap->users == UINT32_MAX) return E_INVALIDARG;
    HRESULT hr = native_heap_generation(op.ctx);
    if (FAILED(hr)) return hr;
    ++heap->users; native_export(op.ctx, heap, out);
    return S_OK;
}
int32_t MWD_CALL native_runtime_release(void *owner, void *token) {
    NativeHeapOperation op(static_cast<Context *>(owner), true);
    auto *heap = native_token(op.ctx, token);
    if (!heap || heap->pending || !heap->users) return E_INVALIDARG;
    if (heap->users > 1) { --heap->users; return S_OK; }
    // After retirement kernel teardown owns any residual handles. The borrowed
    // callback owner stays alive until VkDevice destruction has dropped tokens.
    HRESULT hr = op.live() ? native_heap_cleanup(op.ctx, heap) : S_OK;
    if (SUCCEEDED(hr)) native_heap_erase(op.ctx, heap);
    return hr;
}
int32_t MWD_CALL native_runtime_map(void *owner, void *token, void **out, uint32_t *handle) {
    if (!out || !handle) return E_POINTER;
    *out = nullptr; *handle = 0;
    NativeHeapOperation op(static_cast<Context *>(owner));
    auto *heap = native_token(op.ctx, token);
    HRESULT hr = native_heap_map_record(op.ctx, heap, out);
    if (SUCCEEDED(hr)) *handle = heap->allocation;
    return hr;
}
int32_t MWD_CALL native_runtime_unmap(void *owner, void *token) {
    NativeHeapOperation op(static_cast<Context *>(owner), true);
    auto *heap = native_token(op.ctx, token);
    if (!heap || heap->pending) return E_INVALIDARG;
    if (!op.live()) return S_OK; // Kernel lock is retired with the device.
    if (!heap->map_count) return E_INVALIDARG;
    if (heap->map_count > 1) { --heap->map_count; return S_OK; }
    heap->pending = true;
    HRESULT hr = native_heap_unlock(op.ctx, heap);
    heap->pending = false;
    return hr;
}
int32_t MWD_CALL native_runtime_completed(void *owner, uint32_t *out) {
    if (!out) return E_POINTER;
    *out = 0;
    NativeHeapOperation op(static_cast<Context *>(owner), true);
    if (!op.live() || !op.ctx->native_context_id) return DXGI_ERROR_DEVICE_REMOVED;
    auto *ctx = op.ctx;
    HRESULT completed_hr = native_submission_reap(ctx);
    if (FAILED(completed_hr)) return completed_hr;
    NativeFenceInfo info{};
    info.header = native_header(sizeof(info)); info.opcode = 2;
    info.expected_generation = ctx->native_adapter->generation;
    D3DDDICB_ESCAPE args{};
    args.hDevice = ctx->runtime_device.handle; args.hContext = ctx->native_heap_context;
    args.pPrivateDriverData = &info; args.PrivateDriverDataSize = sizeof(info);
    HRESULT hr = ctx->kernel_callbacks.pfnEscapeCb(ctx->native_adapter->runtime.handle, &args);
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(hr)) return hr;
    if (!native_header_valid(info.header, sizeof(info)) || info.opcode != 2 || info.flags || info.reserved ||
            info.expected_generation != ctx->native_adapter->generation || info.generation != info.expected_generation ||
            info.context_id != ctx->native_context_id || info.completed > UINT32_MAX) return E_FAIL;
    // Never expose private completion ahead of OS-ordered backing retirement.
    if (!ctx->native_submissions) ctx->native_os_completed = static_cast<uint32_t>(info.completed);
    *out = ctx->native_os_completed;
    return native_heap_generation(ctx);
}
int32_t MWD_CALL native_runtime_submit(void *owner, const void *stream, uint32_t size,
        const mwd_reference *refs, uint32_t count) {
    NativeHeapOperation op(static_cast<Context *>(owner));
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    auto *ctx = op.ctx;
    if (!stream || size < 8 || size > 65536 || !refs || !count || count > 1024 ||
            ctx->native_submitting || !ctx->kernel_callbacks.pfnRenderCb) return E_INVALIDARG;
    if (ctx->native_reaping) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (!ctx->kernel_callbacks.pfnSignalSynchronizationObject2Cb) return DXGI_ERROR_UNSUPPORTED;
    // Runtime/kernel callbacks may reenter and release the caller's storage.
    // Copy the complete command and reference request before the first callback.
    std::unique_ptr<uint8_t[]> owned_stream(new (std::nothrow) uint8_t[size]);
    std::unique_ptr<mwd_reference[]> owned_refs(new (std::nothrow) mwd_reference[count]);
    if (!owned_stream || !owned_refs) return E_OUTOFMEMORY;
    std::memcpy(owned_stream.get(), stream, size);
    std::memcpy(owned_refs.get(), refs, sizeof(mwd_reference) * count);
    stream = owned_stream.get(); refs = owned_refs.get();
    HRESULT hr = native_submission_reap(ctx);
    if (FAILED(hr)) return hr;
    if (ctx->native_submission_count == native_submission_limit) return DXGI_ERROR_WAS_STILL_DRAWING;
    hr = native_heap_generation(ctx);
    if (FAILED(hr)) return hr;
    const uint64_t offset = sizeof(NativeRenderInfo) + uint64_t(count) * sizeof(NativeRenderReference);
    const uint64_t bytes = offset + size;
    if (bytes > 65536 || bytes > ctx->native_command_capacity || count > ctx->native_allocation_capacity ||
            count > ctx->native_patch_capacity || !ctx->native_commands || !ctx->native_allocation_list ||
            !ctx->native_patch_list) return E_INVALIDARG;
    for (uint32_t i = 0; i < count; ++i) {
        auto *heap = native_token(ctx, refs[i].token);
        if (!heap || heap->pending || !heap->allocation || heap->users == UINT32_MAX ||
                !refs[i].flags || (refs[i].flags & ~3u) ||
                ((heap->flags & 8u) && (refs[i].flags & 2u)) || refs[i].offset > UINT32_MAX ||
                refs[i].offset > heap->bytes || !refs[i].length || refs[i].length > heap->bytes - refs[i].offset ||
                refs[i].patch_offset % 4 || refs[i].patch_offset > size - 8) return E_INVALIDARG;
        for (uint32_t j = 0; j < i; ++j) {
            const uint64_t a = refs[i].patch_offset, b = refs[j].patch_offset;
            if (refs[i].token == refs[j].token || (a < b + 8 && b < a + 8)) return E_INVALIDARG;
        }
    }
    std::unique_ptr<NativeSubmission> completion(new (std::nothrow) NativeSubmission);
    if (!completion) return E_OUTOFMEMORY;
    completion->heaps.reset(new (std::nothrow) NativeHeap *[count]);
    if (!completion->heaps) return E_OUTOFMEMORY;
    completion->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completion->event) return HRESULT_FROM_WIN32(GetLastError());
    completion->count = count;
    auto *packet = static_cast<uint8_t *>(ctx->native_commands);
    std::memset(packet, 0, static_cast<size_t>(bytes));
    auto *header = reinterpret_cast<NativeRenderInfo *>(packet);
    header->header = native_header(static_cast<uint32_t>(bytes)); header->opcode = 1;
    header->generation = ctx->native_adapter->generation;
    header->references_offset = sizeof(*header); header->references_count = count;
    header->stream_offset = static_cast<uint32_t>(offset); header->stream_size = size;
    std::memcpy(packet + offset, stream, size);
    auto *wire = reinterpret_cast<NativeRenderReference *>(packet + sizeof(*header));
    for (uint32_t i = 0; i < count; ++i) {
        wire[i] = {i, refs[i].flags, refs[i].offset, refs[i].length, refs[i].patch_offset, 0};
        ctx->native_allocation_list[i] = {};
        ctx->native_allocation_list[i].hAllocation = native_token(ctx, refs[i].token)->allocation;
        ctx->native_allocation_list[i].WriteOperation = (refs[i].flags & 2u) != 0;
        ctx->native_patch_list[i] = {};
        ctx->native_patch_list[i].AllocationIndex = i;
        ctx->native_patch_list[i].AllocationOffset = static_cast<UINT>(refs[i].offset);
        ctx->native_patch_list[i].PatchOffset = static_cast<UINT>(offset + refs[i].patch_offset);
    }
    D3DDDICB_RENDER args{};
    args.hContext = ctx->native_heap_context; args.CommandLength = static_cast<UINT>(bytes);
    args.NumAllocations = args.NumPatchLocations = count;
    args.pNewCommandBuffer = ctx->native_commands; args.NewCommandBufferSize = ctx->native_command_capacity;
    args.pNewAllocationList = ctx->native_allocation_list; args.NewAllocationListSize = ctx->native_allocation_capacity;
    args.pNewPatchLocationList = ctx->native_patch_list; args.NewPatchLocationListSize = ctx->native_patch_capacity;
    for (uint32_t i = 0; i < count; ++i) {
        auto *heap = native_token(ctx, refs[i].token);
        ++heap->users; ++heap->submitted;
        completion->heaps[i] = heap;
    }
    auto **tail = &ctx->native_submissions;
    while (*tail) tail = &(*tail)->next;
    auto *submitted = completion.release();
    *tail = submitted; ++ctx->native_submission_count;
    ctx->native_submitting = true;
    hr = ctx->kernel_callbacks.pfnRenderCb(ctx->runtime_device.handle, &args);
    ctx->native_submitting = false;
    submitted->pending = false;
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    // All replacement buffers are consumed even on callback failure.
    ctx->native_commands = args.pNewCommandBuffer; ctx->native_command_capacity = args.NewCommandBufferSize;
    ctx->native_allocation_list = args.pNewAllocationList; ctx->native_allocation_capacity = args.NewAllocationListSize;
    ctx->native_patch_list = args.pNewPatchLocationList; ctx->native_patch_capacity = args.NewPatchLocationListSize;
    if (!ctx->native_commands || !ctx->native_allocation_list || !ctx->native_patch_list ||
            ctx->native_command_capacity < sizeof(NativeRenderInfo) || ctx->native_command_capacity > 65536 ||
            !ctx->native_allocation_capacity || ctx->native_allocation_capacity > 1024 ||
            !ctx->native_patch_capacity || ctx->native_patch_capacity > 1024) hr = E_FAIL;
    const HRESULT completion_hr = native_submission_reap(ctx);
    if (FAILED(hr)) return hr;
    return FAILED(completion_hr) ? completion_hr : native_heap_generation(ctx);
}
static const mwd_callbacks native_runtime_callbacks = {
    MWD_RUNTIME_MAGIC, MWD_RUNTIME_ABI_VERSION, sizeof(mwd_callbacks), 0,
    native_runtime_context, native_runtime_allocate, native_runtime_retain, native_runtime_release,
    native_runtime_map, native_runtime_unmap, native_runtime_submit, native_runtime_completed, native_runtime_status
};
