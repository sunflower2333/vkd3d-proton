/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ddi.h"
#include <atomic>
#include <cstring>
#include <new>
#include <memory>
#include <mutex>
#include <limits>
#include "mesa_wddm_runtime.h"

namespace {
constexpr uint32_t context_magic = 0x564b4455, object_magic = 0x564b4f42;
constexpr uint32_t native_device_magic = 0x564b4e44;
struct NativeAdapter;
struct NativeHeap;
struct NativeSubmission;
struct NativeRuntimeQueue;
struct Context;
struct Object;
void native_heap_forget(Context *);
void native_heap_retire(Context *);
void native_submission_forget(Context *);
void native_runtime_queues_forget(Context *);
HRESULT native_submission_reap(Context *);
void native_heap_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *);
void native_graphics_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *, D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *);
HRESULT native_graphics_pipeline(Context *, const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *);
HRESULT native_command_create(Context *, const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *);
void native_command_destroy(Context *, Object *);
HRESULT native_queue_create(Context *, const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *);
void native_queue_destroy(Context *, void *);
void native_queue_execute(Object *, UINT, const D3D12DDI_HCOMMANDLIST *);
Object *native_submission_find(Object *, void *);
SIZE_T APIENTRY indirect_signature_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *);
HRESULT APIENTRY indirect_signature_create(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *, D3D12DDI_HCOMMANDSIGNATURE);
void APIENTRY indirect_signature_destroy(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDSIGNATURE);
void APIENTRY execute_indirect(D3D12DDI_HCOMMANDLIST, D3D12DDI_HCOMMANDSIGNATURE, UINT,
        D3D12DDIARG_BUFFER_PLACEMENT, D3D12DDIARG_BUFFER_PLACEMENT);
void APIENTRY create_sampler(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_SAMPLER *, D3D12DDI_CPU_DESCRIPTOR_HANDLE);
HRESULT native_root_backend(Context *, void *, const vkdu_root_parameter *, UINT, UINT,
        const vkdu_static_sampler *, UINT);
void native_texture_srv(Context *, const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 *, D3D12DDI_CPU_DESCRIPTOR_HANDLE);
void APIENTRY copy_texture(D3D12DDI_HCOMMANDLIST, const D3D12DDIARG_BUFFER_PLACEMENT *, D3D12DDIARG_PLACED_RESOURCE,
        UINT, UINT, UINT, const D3D12DDIARG_BUFFER_PLACEMENT *, D3D12DDIARG_PLACED_RESOURCE, const D3D12DDI_BOX *);
// Runtime owns this slot; child objects hold references to the separate Context.
struct NativeDevice { uint32_t magic; Context *context; };
// Track recursion only while the underlying mutex is owned. A call back into
// Vulkan must release every nesting level: the runtime may synchronously
// re-enter resource destruction from an outer error/kernel callback.
class NativeCallbackMutex {
    std::recursive_mutex mutex;
    unsigned depth = 0;
public:
    void lock() { mutex.lock(); ++depth; }
    void unlock() { --depth; mutex.unlock(); }
    unsigned suspend() {
        const unsigned held = depth;
        for (unsigned i = 0; i < held; ++i) unlock();
        return held;
    }
    void resume(unsigned held) { for (unsigned i = 0; i < held; ++i) lock(); }
};
struct NativeContextBuffers {
    HANDLE native_heap_context = nullptr;
    void *native_commands = nullptr;
    D3DDDI_ALLOCATIONLIST *native_allocation_list = nullptr;
    D3DDDI_PATCHLOCATIONLIST *native_patch_list = nullptr;
    uint32_t native_command_capacity = 0, native_allocation_capacity = 0, native_patch_capacity = 0;
};
struct Context : NativeContextBuffers {
    uint32_t magic = context_magic;
    std::atomic_uint references{1};
    vkdu_device *backend = nullptr;
    VKDU_REPORT_ERROR report = nullptr;
    void *report_context = nullptr;
    std::atomic<HRESULT> last_error{S_OK};
    SRWLOCK resources_lock = SRWLOCK_INIT;
    Object *resources = nullptr;
    Object *descriptor_heaps = nullptr;
    Object *native_queue_objects = nullptr;
    NativeRuntimeQueue *native_runtime_queues = nullptr;
    Object *native_command_objects = nullptr;
    Object *indirect_signatures = nullptr; // protected by error_mutex
    Object *graphics_objects = nullptr;
    std::shared_ptr<NativeAdapter> native_adapter;
    D3D12DDI_HRTDEVICE runtime_device{};
    D3DDDI_DEVICECALLBACKS kernel_callbacks{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 runtime_callbacks{};
    HMODULE vulkan_module = nullptr;
    NativeCallbackMutex error_mutex;
    NativeHeap *native_heaps = nullptr;
    uint64_t native_va_start = 0, native_va_size = 0;
    uint32_t native_context_id = 0;
    uint32_t native_queue_id = 0;
    bool native_submitting = false;
    unsigned native_backend_calls = 0;
    bool native_destroying = false;
    bool native_retiring = false;
    bool native_context_pending = false;
    NativeSubmission *native_submissions = nullptr;
    unsigned native_submission_count = 0;
    uint32_t native_os_completed = 0;
    uint32_t native_last_submitted = 0;
    bool native_reaping = false;
};
struct Object {
    uint32_t magic;
    Context *context;
    vkdu_object *backend;
    vkdu_kind kind;
    uint32_t *shader;
    uint32_t shader_words;
    Object *next;
    uint64_t address, bytes;
    uint32_t descriptor_flags = 0;
    uint32_t descriptor_type = UINT32_MAX;
    NativeHeap *native_heap = nullptr;
    D3D12DDI_HRTCOMMANDLIST runtime_command{};
    D3D12DDI_HRTCOMMANDQUEUE runtime_queue{};
    NativeRuntimeQueue *runtime_route = nullptr;
    uint32_t native_graphics_tag = 0;
};
Context *context(D3D12DDI_HDEVICE handle) {
    if (handle.pDrvPrivate) {
        uint32_t magic;
        std::memcpy(&magic, handle.pDrvPrivate, sizeof(magic));
        if (magic == native_device_magic)
            return static_cast<NativeDevice *>(handle.pDrvPrivate)->context;
    }
    auto *value = static_cast<Context *>(handle.pDrvPrivate);
    return value && value->magic == context_magic ? value : nullptr;
}
Object *object(void *memory) {
    auto *value = static_cast<Object *>(memory);
    return value && value->magic == object_magic ? value : nullptr;
}
vkdu_object *backend(void *memory, vkdu_kind kind) {
    auto *value = object(memory);
    return value && value->kind == kind ? value->backend : nullptr;
}
// The backend may enter callbacks while its final destructor drains workers.
// Such callbacks borrow the still-owned metadata but must not resurrect a zero
// reference count. A separate finalizing flag has a check/increment race.
bool retain_live(Context *value) {
    if (!value) return false;
    unsigned count = value->references.load();
    while (count && count != (std::numeric_limits<unsigned>::max)()) {
        if (value->references.compare_exchange_weak(count, count + 1)) return true;
    }
    return false;
}
void release(Context *value) {
    if (value && --value->references == 0) {
        vkdu_device_destroy(value->backend);
        native_heap_forget(value);
        value->magic = 0;
        // Backend destruction can call Vulkan; unload only after that finishes.
        if (value->vulkan_module) FreeLibrary(value->vulkan_module);
        delete value;
    }
}
void error(Context *value, HRESULT result) {
    if (!value || SUCCEEDED(result)) return;
    // A runtime error callback can synchronously retire the native device.
    // Retain Context until its recursive callback lock has been released.
    if (!retain_live(value)) return;
    {
        std::lock_guard<NativeCallbackMutex> lock(value->error_mutex);
        value->last_error = result;
        if (value->backend && value->native_adapter &&
                (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
                 result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR))
            vkdu_device_remove(value->backend, result);
        if (value->report) value->report(value->report_context, result);
    }
    release(value);
}
void error(Object *value, HRESULT result) {
    if (!value || SUCCEEDED(result)) return;
    auto *ctx = value->context;
    const auto runtime = value->runtime_command;
    if (value->kind != VKDU_COMMAND_LIST || !runtime.handle) { error(ctx, result); return; }
    // Runtime owns the command-list slot and may release/overwrite it inside
    // this callback. Copy its opaque handle first, and retain only Context.
    if (!retain_live(ctx)) return;
    {
        std::lock_guard<NativeCallbackMutex> lock(ctx->error_mutex);
        if (!ctx->native_retiring && ctx->runtime_device.handle && ctx->runtime_callbacks.pfnSetCommandListErrorCb) {
            const bool removed = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
                result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
            if (removed) ctx->last_error = result;
            ctx->runtime_callbacks.pfnSetCommandListErrorCb(runtime, result);
            // The callback can retire the device as well as the command list.
            // error(Context) consults the retained, possibly detached report.
            if (removed) error(ctx, result);
        }
    }
    release(ctx);
}
HRESULT bind(Context *ctx, void *memory, vkdu_object *value, vkdu_kind kind) {
    if (!ctx || !memory || !vkdu_object_is(value, kind) || !vkdu_object_belongs(ctx->backend, value) || object(memory)) return E_INVALIDARG;
    auto *entry = new (memory) Object{object_magic, ctx, value, kind, nullptr, 0, nullptr, 0, 0};
    if (kind == VKDU_BUFFER) {
        entry->address = vkdu_buffer_address(value); entry->bytes = vkdu_buffer_size(value);
        if (!entry->address || !entry->bytes || entry->bytes > UINT64_MAX - entry->address) {
            std::memset(entry, 0, sizeof(*entry)); return E_INVALIDARG;
        }
        AcquireSRWLockExclusive(&ctx->resources_lock);
        for (auto *p = ctx->resources; p; p = p->next) {
            if (entry->address < p->address + p->bytes && p->address < entry->address + entry->bytes) {
                ReleaseSRWLockExclusive(&ctx->resources_lock); std::memset(entry, 0, sizeof(*entry)); return E_INVALIDARG;
            }
        }
        entry->next = ctx->resources; ctx->resources = entry;
        ReleaseSRWLockExclusive(&ctx->resources_lock);
    } else if (kind == VKDU_TEXTURE2D) {
        AcquireSRWLockExclusive(&ctx->resources_lock);
        entry->next = ctx->resources; ctx->resources = entry;
        ReleaseSRWLockExclusive(&ctx->resources_lock);
    } else if (kind == VKDU_DESCRIPTOR_HEAP) {
        AcquireSRWLockExclusive(&ctx->resources_lock);
        entry->next = ctx->descriptor_heaps; ctx->descriptor_heaps = entry;
        ReleaseSRWLockExclusive(&ctx->resources_lock);
    }
    ++ctx->references;
    return S_OK;
}

HRESULT finish(Context *ctx, void *memory, vkdu_object *value, vkdu_kind kind, HRESULT result) {
    if (FAILED(result)) return result;
    result = bind(ctx, memory, value, kind);
    if (FAILED(result)) vkdu_object_destroy(value);
    return result;
}
uint32_t command_type(D3D12DDI_COMMAND_QUEUE_FLAGS flags) {
    if (flags == D3D12DDI_COMMAND_QUEUE_FLAG_3D) return 0;
    if (flags == D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE) return 2;
    if (flags == D3D12DDI_COMMAND_QUEUE_FLAG_COPY) return 3;
    return UINT32_MAX;
}
bool belongs(Context *ctx, void *memory) { auto *o = object(memory); return o && o->context == ctx; }

SIZE_T APIENTRY heap_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *) { return sizeof(Object); }
HRESULT APIENTRY heap_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args, D3D12DDI_HDESCRIPTORHEAP heap) {
    auto *ctx = context(h); vkdu_object *value = nullptr;
    if (!ctx || !args || args->NodeMask > 1 || (static_cast<UINT>(args->Flags) & ~3u)) return E_INVALIDARG;
    // DDI SHADER_VISIBLE is bit 2; the embedded API uses bit 1. Map explicitly.
    HRESULT hr = vkdu_heap_create(ctx->backend, args->Type, args->NumDescriptors,
            (args->Flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0, &value);
    hr = finish(ctx, heap.pDrvPrivate, value, VKDU_DESCRIPTOR_HEAP, hr);
    if (SUCCEEDED(hr)) {
        object(heap.pDrvPrivate)->descriptor_flags = args->Flags;
        object(heap.pDrvPrivate)->descriptor_type = args->Type;
    }
    return hr;
}
void APIENTRY heap_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HDESCRIPTORHEAP heap) {
    if (belongs(context(h), heap.pDrvPrivate) && backend(heap.pDrvPrivate, VKDU_DESCRIPTOR_HEAP))
        VioGpuD3D12BridgeUnbindObject(heap.pDrvPrivate);
    else error(context(h), E_INVALIDARG);
}
UINT APIENTRY descriptor_size(D3D12DDI_HDEVICE h, D3D12DDI_DESCRIPTOR_HEAP_TYPE type) {
    auto *ctx = context(h);
    return ctx ? vkdu_descriptor_size(ctx->backend, type) : 0;
}
D3D12DDI_CPU_DESCRIPTOR_HANDLE APIENTRY heap_cpu(D3D12DDI_HDEVICE h, D3D12DDI_HDESCRIPTORHEAP heap) {
    if (!belongs(context(h), heap.pDrvPrivate) || !(object(heap.pDrvPrivate)->descriptor_flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE)) return {};
    return {static_cast<SIZE_T>(vkdu_heap_start(backend(heap.pDrvPrivate, VKDU_DESCRIPTOR_HEAP), 0))};
}
D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY heap_gpu(D3D12DDI_HDEVICE h, D3D12DDI_HDESCRIPTORHEAP heap) {
    if (!belongs(context(h), heap.pDrvPrivate)) return {};
    return {vkdu_heap_start(backend(heap.pDrvPrivate, VKDU_DESCRIPTOR_HEAP), 1)};
}
// Caller holds resources_lock until the backend operation is recorded.
Object *find_heap(Context *ctx, uint64_t address, bool gpu, uint32_t *index) {
    for (auto *p = ctx->descriptor_heaps; p; p = p->next) {
        if (!gpu && !(p->descriptor_flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE)) continue;
        if (vkdu_heap_resolve(p->backend, address, gpu, index)) return p;
    }
    return nullptr;
}
// Caller holds resources_lock; never dereference an unregistered native handle.
Object *find_buffer(Context *ctx, void *handle) {
    for (auto *p = ctx->resources; p; p = p->next)
        if (p == handle && p->kind == VKDU_BUFFER) return p;
    return nullptr;
}
Object *find_resource(Context *ctx, void *handle) {
    for (auto *p = ctx->resources; p; p = p->next)
        if (p == handle) return p;
    return nullptr;
}
void APIENTRY create_uav(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 *args, D3D12DDI_CPU_DESCRIPTOR_HANDLE destination) {
    auto *ctx = context(h);
    if (!ctx) return;
    if (!args) { error(ctx, E_INVALIDARG); return; }
    if (args->ResourceDimension != D3D12DDI_RD_BUFFER) { error(ctx, E_NOTIMPL); return; }
    uint32_t index = 0; HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *heap = find_heap(ctx, destination.ptr, false, &index);
    auto *resource = find_buffer(ctx, args->hDrvResource.pDrvPrivate);
    auto *counter = find_buffer(ctx, args->Buffer.hDrvCounterResource.pDrvPrivate);
    if (heap && resource && (!args->Buffer.hDrvCounterResource.pDrvPrivate || counter))
        hr = vkdu_buffer_uav(heap->backend, index, resource->backend, args->Format, args->Buffer.FirstElement,
                args->Buffer.NumElements, args->Buffer.StructureByteStride, args->Buffer.Flags,
                counter ? counter->backend : nullptr, args->Buffer.CounterOffsetInBytes);
    ReleaseSRWLockShared(&ctx->resources_lock);
    error(ctx, hr);
}
void APIENTRY create_srv(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 *args, D3D12DDI_CPU_DESCRIPTOR_HANDLE destination) {
    auto *ctx = context(h);
    if (!ctx) return;
    if (!args) { error(ctx, E_INVALIDARG); return; }
    if (args->ResourceDimension == D3D12DDI_RD_TEXTURE2D) { native_texture_srv(ctx, args, destination); return; }
    if (args->ResourceDimension != D3D12DDI_RD_BUFFER) { error(ctx, E_NOTIMPL); return; }
    uint32_t index = 0; HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *heap = find_heap(ctx, destination.ptr, false, &index);
    // Resolve only live owned resources. An unknown non-null handle must not
    // become a valid null descriptor or require dereferencing foreign memory.
    auto *resource = find_buffer(ctx, args->hDrvResource.pDrvPrivate);
    if (heap && (!args->hDrvResource.pDrvPrivate || resource))
        hr = vkdu_buffer_srv(heap->backend, index, resource ? resource->backend : nullptr,
                args->Format, args->Buffer.FirstElement, args->Buffer.NumElements,
                args->Buffer.StructureByteStride, args->Buffer.Flags, args->Shader4ComponentMapping);
    ReleaseSRWLockShared(&ctx->resources_lock);
    error(ctx, hr);
}
void APIENTRY create_cbv(D3D12DDI_HDEVICE h, const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC *args, D3D12DDI_CPU_DESCRIPTOR_HANDLE destination) {
    auto *ctx = context(h);
    if (!ctx) return;
    if (!args) { error(ctx, E_INVALIDARG); return; }
    uint32_t index = 0; HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *heap = find_heap(ctx, destination.ptr, false, &index);
    if (heap) {
        if (!args->BufferLocation) hr = vkdu_buffer_cbv(heap->backend, index, nullptr, 0, args->SizeInBytes);
        else for (auto *p = ctx->resources; p; p = p->next) {
            if (args->BufferLocation >= p->address && args->BufferLocation - p->address < p->bytes) {
                hr = vkdu_buffer_cbv(heap->backend, index, p->backend, args->BufferLocation - p->address, args->SizeInBytes);
                break;
            }
        }
    }
    ReleaseSRWLockShared(&ctx->resources_lock);
    error(ctx, hr);
}
void APIENTRY copy_descriptors_simple(D3D12DDI_HDEVICE h, UINT count, D3D12DDI_CPU_DESCRIPTOR_HANDLE destination,
        D3D12DDI_CPU_DESCRIPTOR_HANDLE source, D3D12DDI_DESCRIPTOR_HEAP_TYPE type) {
    auto *ctx = context(h);
    if (!ctx) return;
    if (!count) return;
    uint32_t dst_index = 0, src_index = 0; HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *dst = find_heap(ctx, destination.ptr, false, &dst_index);
    auto *src = find_heap(ctx, source.ptr, false, &src_index);
    if (dst && src && dst->descriptor_type == static_cast<uint32_t>(type) && src->descriptor_type == static_cast<uint32_t>(type))
        hr = vkdu_descriptor_copy(dst->backend, dst_index, src->backend, src_index, count);
    ReleaseSRWLockShared(&ctx->resources_lock);
    error(ctx, hr);
}
// The shared resources lock keeps every resolved heap alive through the copy.
bool resolve_descriptor_spans(Context *ctx, UINT count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *starts,
        const UINT *sizes, D3D12DDI_DESCRIPTOR_HEAP_TYPE type, vkdu_descriptor_span *spans) {
    for (UINT i = 0; i < count; ++i) {
        spans[i] = {nullptr, 0, sizes ? sizes[i] : 1};
        if (!spans[i].count) continue;
        auto *heap = find_heap(ctx, starts[i].ptr, false, &spans[i].first);
        if (!heap || heap->descriptor_type != static_cast<uint32_t>(type)) return false;
        spans[i].heap = heap->backend;
    }
    return true;
}
void APIENTRY copy_descriptors(D3D12DDI_HDEVICE h, UINT dst_count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *dst_starts,
        const UINT *dst_sizes, UINT src_count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *src_starts,
        const UINT *src_sizes, D3D12DDI_DESCRIPTOR_HEAP_TYPE type) {
    auto *ctx = context(h);
    if (!ctx) return;
    if ((dst_count && !dst_starts) || (src_count && !src_starts) || static_cast<UINT>(type) > 3) {
        error(ctx, E_INVALIDARG); return;
    }
    if (static_cast<uint64_t>(dst_count) > SIZE_MAX / sizeof(vkdu_descriptor_span) ||
            static_cast<uint64_t>(src_count) > SIZE_MAX / sizeof(vkdu_descriptor_span)) {
        error(ctx, E_OUTOFMEMORY); return;
    }
    std::unique_ptr<vkdu_descriptor_span[]> dst(dst_count ? new (std::nothrow) vkdu_descriptor_span[dst_count] : nullptr);
    std::unique_ptr<vkdu_descriptor_span[]> src(src_count ? new (std::nothrow) vkdu_descriptor_span[src_count] : nullptr);
    if ((dst_count && !dst) || (src_count && !src)) { error(ctx, E_OUTOFMEMORY); return; }
    HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&ctx->resources_lock);
    if (resolve_descriptor_spans(ctx, dst_count, dst_starts, dst_sizes, type, dst.get()) &&
            resolve_descriptor_spans(ctx, src_count, src_starts, src_sizes, type, src.get()))
        hr = vkdu_descriptor_copy_ranges(ctx->backend, type, dst_count, dst.get(), src_count, src.get());
    ReleaseSRWLockShared(&ctx->resources_lock);
    error(ctx, hr);
}
void APIENTRY set_heaps(D3D12DDI_HCOMMANDLIST c, UINT count, D3D12DDI_HDESCRIPTORHEAP *heaps) {
    auto *cmd = object(c.pDrvPrivate); vkdu_object *native[2] = {};
    if (!cmd) return;
    if (count > 2 || (count && !heaps)) { error(cmd, E_INVALIDARG); return; }
    for (UINT i = 0; i < count; ++i) native[i] = backend(heaps[i].pDrvPrivate, VKDU_DESCRIPTOR_HEAP);
    error(cmd, vkdu_command_heaps(cmd->backend, count, native));
}
void APIENTRY set_table(D3D12DDI_HCOMMANDLIST c, UINT index, D3D12DDI_GPU_DESCRIPTOR_HANDLE address) {
    auto *cmd = object(c.pDrvPrivate);
    if (!cmd) return;
    auto *ctx = cmd->context; uint32_t first = 0; HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *heap = find_heap(ctx, address.ptr, true, &first);
    if (heap) hr = vkdu_command_table(cmd->backend, index, heap->backend, first);
    ReleaseSRWLockShared(&ctx->resources_lock);
    error(cmd, hr);
}

SIZE_T APIENTRY queue_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *) { return sizeof(Object); }
HRESULT APIENTRY queue_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args) {
    auto *ctx = context(h); vkdu_object *value = nullptr;
    if (!ctx || !args || args->NodeMask > 1) return E_INVALIDARG;
    if (ctx->native_adapter) return native_queue_create(ctx, args);
    HRESULT hr = vkdu_queue_create(ctx->backend, command_type(args->QueueFlags), &value);
    return finish(ctx, args->hDrvCommandQueue.pDrvPrivate, value, VKDU_QUEUE, hr);
}
void APIENTRY queue_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDQUEUE q) {
    auto *ctx = context(h);
    if (ctx && ctx->native_adapter) native_queue_destroy(ctx, q.pDrvPrivate);
    else if (belongs(ctx, q.pDrvPrivate) && backend(q.pDrvPrivate, VKDU_QUEUE)) VioGpuD3D12BridgeUnbindObject(q.pDrvPrivate);
    else error(ctx, E_INVALIDARG);
}
SIZE_T APIENTRY allocator_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATECOMMANDALLOCATOR *) { return sizeof(Object); }
HRESULT APIENTRY allocator_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATECOMMANDALLOCATOR *args) {
    auto *ctx = context(h); vkdu_object *value = nullptr;
    if (!ctx || !args || args->Type != D3D12DDI_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    HRESULT hr = vkdu_allocator_create(ctx->backend, command_type(args->QueueFlags), &value);
    return finish(ctx, args->hDrvCommandAllocator.pDrvPrivate, value, VKDU_ALLOCATOR, hr);
}
void APIENTRY allocator_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDALLOCATOR a) {
    if (belongs(context(h), a.pDrvPrivate)) VioGpuD3D12BridgeUnbindObject(a.pDrvPrivate);
    else error(context(h), E_INVALIDARG);
}
void APIENTRY allocator_reset(D3D12DDI_HCOMMANDALLOCATOR a) {
    error(object(a.pDrvPrivate), vkdu_allocator_reset(backend(a.pDrvPrivate, VKDU_ALLOCATOR)));
}
SIZE_T APIENTRY command_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *) { return sizeof(Object); }
HRESULT APIENTRY command_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args) {
    auto *ctx = context(h); vkdu_object *value = nullptr;
    if (!ctx || !args || args->Type != D3D12DDI_COMMAND_LIST_TYPE_DIRECT || args->NodeMask > 1 || args->CommandListFlags ||
        !belongs(ctx, args->hDrvCommandAllocator.pDrvPrivate)) return E_INVALIDARG;
    if (ctx->native_adapter) return native_command_create(ctx, args);
    HRESULT hr = vkdu_command_create(ctx->backend, backend(args->hDrvCommandAllocator.pDrvPrivate, VKDU_ALLOCATOR), command_type(args->QueueFlags), &value);
    return finish(ctx, args->hDrvCommandList.pDrvPrivate, value, VKDU_COMMAND_LIST, hr);
}
void APIENTRY command_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDLIST c) {
    auto *ctx = context(h);
    if (ctx && ctx->native_adapter) {
        // Native handles are looked up under the callback lock in the helper.
        native_command_destroy(ctx, static_cast<Object *>(c.pDrvPrivate));
        return;
    }
    auto *cmd = object(c.pDrvPrivate);
    if (!cmd || cmd->context != ctx || cmd->kind != VKDU_COMMAND_LIST) { error(ctx, E_INVALIDARG); return; }
    VioGpuD3D12BridgeUnbindObject(c.pDrvPrivate);
}
void APIENTRY command_close(D3D12DDI_HCOMMANDLIST c) {
    error(object(c.pDrvPrivate), vkdu_command_close(backend(c.pDrvPrivate, VKDU_COMMAND_LIST)));
}
void APIENTRY command_reset(D3D12DDI_HCOMMANDLIST c, const D3D12DDIARG_RESETCOMMANDLIST *args) {
    auto *cmd = object(c.pDrvPrivate);
    if (!cmd) return;
    if (!args || args->Slot || args->CommandListFlags || !belongs(cmd->context, args->hDrvCommandAllocator.pDrvPrivate)) { error(cmd, E_INVALIDARG); return; }
    error(cmd, vkdu_command_reset(cmd->backend, backend(args->hDrvCommandAllocator.pDrvPrivate, VKDU_ALLOCATOR)));
}
void APIENTRY copy(D3D12DDI_HCOMMANDLIST c, D3D12DDIARG_BUFFER_PLACEMENT dst, D3D12DDIARG_BUFFER_PLACEMENT src, UINT64 bytes) {
    auto *cmd = object(c.pDrvPrivate);
    error(cmd, vkdu_command_copy(backend(c.pDrvPrivate, VKDU_COMMAND_LIST),
            backend(dst.BaseAddress.UMD.hResource.pDrvPrivate, VKDU_BUFFER), dst.BaseAddress.UMD.Offset,
            backend(src.BaseAddress.UMD.hResource.pDrvPrivate, VKDU_BUFFER), src.BaseAddress.UMD.Offset, bytes));
}
void APIENTRY barriers(D3D12DDI_HCOMMANDLIST c, UINT count, const D3D12DDIARG_RESOURCE_BARRIER_0003 *args) {
    auto *cmd = object(c.pDrvPrivate);
    if (!cmd) return;
    if (cmd->kind != VKDU_COMMAND_LIST || (count && !args) || count > 65536) { error(cmd, E_INVALIDARG); return; }
    vkdu_resource_barrier stack[16]{};
    std::unique_ptr<vkdu_resource_barrier[]> allocated;
    auto *translated = stack;
    if (count > 16) {
        allocated.reset(new (std::nothrow) vkdu_resource_barrier[count]{});
        if (!allocated) { error(cmd, E_OUTOFMEMORY); return; }
        translated = allocated.get();
    }
    auto *ctx = cmd->context;
    HRESULT hr = S_OK;
    AcquireSRWLockShared(&ctx->resources_lock);
    // Resolve opaque runtime resource handles in the owned live registry,
    // never by dereferencing arbitrary memory. Keep the resources stable until
    // the backend has consumed the complete translated batch synchronously.
    for (UINT i = 0; i < count; ++i) {
        if (args[i].Flags) { hr = E_NOTIMPL; break; }
        void *handle;
        if (args[i].Type == D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION) {
            if (args[i].Transition.Subresource != UINT_MAX) { hr = E_NOTIMPL; break; }
            handle = args[i].Transition.hResource.pDrvPrivate;
            if (!handle) { hr = E_INVALIDARG; break; }
            translated[i].type = VKDU_BARRIER_TRANSITION;
            translated[i].before = args[i].Transition.StateBefore;
            translated[i].after = args[i].Transition.StateAfter;
        } else if (args[i].Type == D3D12DDI_RESOURCE_BARRIER_TYPE_UAV) {
            handle = args[i].UAV.hResource.pDrvPrivate;
            translated[i].type = VKDU_BARRIER_UAV;
        } else { hr = E_NOTIMPL; break; }
        auto *resource = find_resource(ctx, handle);
        if (handle && !resource) { hr = E_INVALIDARG; break; }
        translated[i].resource = resource ? resource->backend : nullptr;
    }
    if (SUCCEEDED(hr)) hr = vkdu_command_barriers(cmd->backend, count, translated);
    ReleaseSRWLockShared(&ctx->resources_lock);
    // The runtime error callback may destroy this command/device and poison
    // its private slot. No command or caller-array access follows reporting.
    error(cmd, hr);
}
void APIENTRY dispatch(D3D12DDI_HCOMMANDLIST c, UINT x, UINT y, UINT z) {
    error(object(c.pDrvPrivate), vkdu_command_dispatch(backend(c.pDrvPrivate, VKDU_COMMAND_LIST), x, y, z));
}
void APIENTRY root_set(D3D12DDI_HCOMMANDLIST c, D3D12DDI_HROOTSIGNATURE root) {
    error(object(c.pDrvPrivate), vkdu_command_root(backend(c.pDrvPrivate, VKDU_COMMAND_LIST), backend(root.pDrvPrivate, VKDU_ROOT)));
}
void APIENTRY pipeline_set(D3D12DDI_HCOMMANDLIST c, D3D12DDI_HPIPELINESTATE pipeline) {
    error(object(c.pDrvPrivate), vkdu_command_pipeline(backend(c.pDrvPrivate, VKDU_COMMAND_LIST), backend(pipeline.pDrvPrivate, VKDU_PIPELINE)));
}
void APIENTRY root_uav(D3D12DDI_HCOMMANDLIST c, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS address) {
    auto *cmd = object(c.pDrvPrivate);
    if (!cmd) return;
    auto *ctx = cmd->context; HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&ctx->resources_lock);
    for (auto *p = ctx->resources; p; p = p->next) {
        if (address >= p->address && address - p->address < p->bytes) {
            hr = vkdu_command_uav(cmd->backend, index, p->backend, address - p->address); break;
        }
    }
    ReleaseSRWLockShared(&ctx->resources_lock);
    error(cmd, hr);
}
void APIENTRY root_cbv(D3D12DDI_HCOMMANDLIST c, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS address) {
    auto *cmd = object(c.pDrvPrivate);
    if (!cmd) return;
    if (!address) { error(cmd, E_INVALIDARG); return; }
    auto *ctx = cmd->context; HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&ctx->resources_lock);
    for (auto *p = ctx->resources; p; p = p->next) {
        if (address >= p->address && address - p->address < p->bytes) {
            hr = vkdu_command_cbv(cmd->backend, index, p->backend, address - p->address); break;
        }
    }
    ReleaseSRWLockShared(&ctx->resources_lock);
    error(cmd, hr);
}
void APIENTRY root_srv(D3D12DDI_HCOMMANDLIST c, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS address) {
    auto *cmd = object(c.pDrvPrivate);
    if (!cmd) return;
    if (!address) { error(cmd, E_INVALIDARG); return; }
    auto *ctx = cmd->context; HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&ctx->resources_lock);
    for (auto *p = ctx->resources; p; p = p->next) {
        if (address >= p->address && address - p->address < p->bytes) {
            hr = vkdu_command_srv(cmd->backend, index, p->backend, address - p->address); break;
        }
    }
    ReleaseSRWLockShared(&ctx->resources_lock);
    error(cmd, hr);
}
void APIENTRY execute(D3D12DDI_HCOMMANDQUEUE q, UINT count, const D3D12DDI_HCOMMANDLIST *commands) {
    auto *queue = object(q.pDrvPrivate); vkdu_object *native[64];
    if (!queue) return;
    if (queue->context->native_adapter) { native_queue_execute(queue, count, commands); return; }
    if (!commands || !count || count > 64) { error(queue, E_INVALIDARG); return; }
    for (UINT i = 0; i < count; ++i) native[i] = backend(commands[i].pDrvPrivate, VKDU_COMMAND_LIST);
    error(queue, vkdu_queue_execute(queue->backend, count, native));
}
void APIENTRY root_constant(D3D12DDI_HCOMMANDLIST c, UINT index, UINT value, UINT offset) {
    auto *cmd = object(c.pDrvPrivate);
    if (!cmd) return;
    error(cmd, vkdu_command_constants(backend(c.pDrvPrivate, VKDU_COMMAND_LIST), index, offset, 1, &value));
}
void APIENTRY root_constants(D3D12DDI_HCOMMANDLIST c, UINT index, UINT count, const VOID *values, UINT offset) {
    auto *cmd = object(c.pDrvPrivate);
    if (!cmd) return;
    error(cmd, vkdu_command_constants(backend(c.pDrvPrivate, VKDU_COMMAND_LIST), index, offset, count,
            static_cast<const uint32_t *>(values)));
}
SIZE_T APIENTRY root_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 *) { return sizeof(Object); }
HRESULT APIENTRY root_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 *args, D3D12DDI_HROOTSIGNATURE root) {
    auto *ctx = context(h); vkdu_root_parameter parameters[64]{};
    std::unique_ptr<vkdu_descriptor_range[]> ranges; UINT used = 0;
    std::unique_ptr<vkdu_static_sampler[]> samplers;
    if (!ctx || !args || !args->pRootSignature || args->NodeMask > 1) return E_INVALIDARG;
    const auto &desc = *args->pRootSignature;
    if (desc.NumParameters > 64 || (desc.NumParameters && !desc.pRootParameters) ||
            desc.NumStaticSamplers > 2032 || (desc.NumStaticSamplers && !desc.pStaticSamplers)) return E_INVALIDARG;
    if (desc.NumStaticSamplers) {
        samplers.reset(new (std::nothrow) vkdu_static_sampler[desc.NumStaticSamplers]{});
        if (!samplers) return E_OUTOFMEMORY;
        for (UINT i = 0; i < desc.NumStaticSamplers; ++i) {
            const auto &s = desc.pStaticSamplers[i];
            auto &out = samplers[i];
            out.desc.filter = s.Filter; out.desc.address_u = s.AddressU;
            out.desc.address_v = s.AddressV; out.desc.address_w = s.AddressW;
            out.desc.mip_bias = s.MipLODBias; out.desc.max_anisotropy = s.MaxAnisotropy;
            out.desc.comparison = s.ComparisonFunc; out.desc.min_lod = s.MinLOD; out.desc.max_lod = s.MaxLOD;
            out.border_color = s.BorderColor; out.shader_register = s.ShaderRegister;
            out.register_space = s.RegisterSpace; out.visibility = s.ShaderVisibility;
        }
    }
    for (UINT i = 0; i < desc.NumParameters; ++i) {
        if (desc.pRootParameters[i].ParameterType == D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            ranges.reset(new (std::nothrow) vkdu_descriptor_range[4096]);
            if (!ranges) return E_OUTOFMEMORY;
            break;
        }
    }
    for (UINT i = 0; i < desc.NumParameters; ++i) {
        const auto &p = desc.pRootParameters[i];
        parameters[i].type = p.ParameterType; parameters[i].visibility = p.ShaderVisibility;
        if (p.ParameterType == D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            if (!p.DescriptorTable.pDescriptorRanges || !p.DescriptorTable.NumDescriptorRanges ||
                p.DescriptorTable.NumDescriptorRanges > 4096 - used) return E_INVALIDARG;
            parameters[i].ranges = ranges.get() + used; parameters[i].range_count = p.DescriptorTable.NumDescriptorRanges;
            for (UINT j = 0; j < p.DescriptorTable.NumDescriptorRanges; ++j) {
                const auto &r = p.DescriptorTable.pDescriptorRanges[j];
                ranges[used++] = {static_cast<uint32_t>(r.RangeType), r.NumDescriptors, r.BaseShaderRegister,
                    r.RegisterSpace, r.OffsetInDescriptorsFromTableStart};
            }
        } else if (p.ParameterType == D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            parameters[i].shader_register = p.Constants.ShaderRegister; parameters[i].register_space = p.Constants.RegisterSpace;
            parameters[i].constant_count = p.Constants.Num32BitValues;
        } else {
            parameters[i].shader_register = p.Descriptor.ShaderRegister; parameters[i].register_space = p.Descriptor.RegisterSpace;
        }
    }
    return native_root_backend(ctx, root.pDrvPrivate, parameters, desc.NumParameters, desc.Flags,
            samplers.get(), desc.NumStaticSamplers);
}
void APIENTRY root_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HROOTSIGNATURE root) {
    if (belongs(context(h), root.pDrvPrivate)) VioGpuD3D12BridgeUnbindObject(root.pDrvPrivate);
    else error(context(h), E_INVALIDARG);
}
SIZE_T APIENTRY shader_size(D3D12DDI_HDEVICE, const UINT *, D3D12DDI_HROOTSIGNATURE, const D3D12DDIARG_STAGE_IO_SIGNATURES *) { return sizeof(Object); }
void APIENTRY shader_create(D3D12DDI_HDEVICE h, const UINT *tokens, D3D12DDI_HROOTSIGNATURE root, D3D12DDI_HSHADER shader, D3D12DDI_CREATE_SHADER_FLAGS flags) {
    auto *ctx = context(h);
    if (!ctx) return;
    if (!tokens || !shader.pDrvPrivate || object(shader.pDrvPrivate) || flags || !belongs(ctx, root.pDrvPrivate) ||
        tokens[1] < 2 || tokens[1] > 1024 * 1024 || (tokens[0] >> 16) != 5) { error(ctx, E_INVALIDARG); return; }
    auto *copy = new (std::nothrow) uint32_t[tokens[1]];
    if (!copy) { error(ctx, E_OUTOFMEMORY); return; }
    std::memcpy(copy, tokens, tokens[1] * sizeof(uint32_t));
    new (shader.pDrvPrivate) Object{object_magic, ctx, nullptr, VKDU_PIPELINE, copy, tokens[1], nullptr, 0, 0};
    ++ctx->references;
}
void APIENTRY shader_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HSHADER shader) {
    if (belongs(context(h), shader.pDrvPrivate)) VioGpuD3D12BridgeUnbindObject(shader.pDrvPrivate);
    else error(context(h), E_INVALIDARG);
}
SIZE_T APIENTRY pipeline_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *) { return sizeof(Object); }
HRESULT APIENTRY pipeline_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args) {
    auto *ctx = context(h); vkdu_object *value = nullptr;
    if (!ctx || !args || args->NodeMask > 1 || !belongs(ctx, args->hRootSignature.pDrvPrivate)) return E_INVALIDARG;
    if (args->hVertexShader.pDrvPrivate || args->hPixelShader.pDrvPrivate || args->hDomainShader.pDrvPrivate ||
        args->hHullShader.pDrvPrivate || args->hGeometryShader.pDrvPrivate) return native_graphics_pipeline(ctx, args);
    if (!belongs(ctx, args->hComputeShader.pDrvPrivate)) return E_INVALIDARG;
    auto *shader = object(args->hComputeShader.pDrvPrivate);
    if (!shader->shader) return E_INVALIDARG;
    HRESULT hr = vkdu_pipeline_create_tokens(ctx->backend, backend(args->hRootSignature.pDrvPrivate, VKDU_ROOT), shader->shader, shader->shader_words, &value);
    return finish(ctx, args->hDrvPipelineState.pDrvPrivate, value, VKDU_PIPELINE, hr);
}
void APIENTRY pipeline_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HPIPELINESTATE pipeline) {
    if (belongs(context(h), pipeline.pDrvPrivate)) VioGpuD3D12BridgeUnbindObject(pipeline.pDrvPrivate);
    else error(context(h), E_INVALIDARG);
}
}

extern "C" HRESULT APIENTRY VioGpuD3D12BridgeCreate(PFN_vkGetInstanceProcAddr loader, const vkdu_adapter *adapter,
        VKDU_REPORT_ERROR report, void *report_context, D3D12DDI_HDEVICE *out) {
    if (!out || !report) return E_INVALIDARG;
    out->pDrvPrivate = nullptr;
    auto *ctx = new (std::nothrow) Context;
    if (!ctx) return E_OUTOFMEMORY;
    HRESULT hr = vkdu_device_create(loader, adapter, &ctx->backend);
    if (FAILED(hr)) { delete ctx; return hr; }
    ctx->report = report; ctx->report_context = report_context; out->pDrvPrivate = ctx;
    return S_OK;
}
extern "C" void APIENTRY VioGpuD3D12BridgeDestroy(D3D12DDI_HDEVICE h) { release(context(h)); }
extern "C" HRESULT APIENTRY VioGpuD3D12BridgeStatus(D3D12DDI_HDEVICE h) {
    auto *ctx = context(h);
    if (!ctx) return E_INVALIDARG;
    HRESULT hr = ctx->last_error.load();
    return FAILED(hr) ? hr : vkdu_device_status(ctx->backend);
}
extern "C" SIZE_T APIENTRY VioGpuD3D12BridgeObjectSize(void) { return sizeof(Object); }
extern "C" HRESULT APIENTRY VioGpuD3D12BridgeBindObject(D3D12DDI_HDEVICE h, void *memory, vkdu_object *value, vkdu_kind kind) {
    return bind(context(h), memory, value, kind);
}
extern "C" void APIENTRY VioGpuD3D12BridgeUnbindObject(void *memory) {
    auto *value = object(memory);
    if (!value) return;
    auto *ctx = value->context;
    if (value->kind == VKDU_BUFFER || value->kind == VKDU_TEXTURE2D || value->kind == VKDU_DESCRIPTOR_HEAP) {
        AcquireSRWLockExclusive(&ctx->resources_lock);
        Object **p = value->kind == VKDU_DESCRIPTOR_HEAP ? &ctx->descriptor_heaps : &ctx->resources;
        while (*p && *p != value) p = &(*p)->next;
        if (*p) *p = value->next;
        ReleaseSRWLockExclusive(&ctx->resources_lock);
    }
    if (value->native_graphics_tag) {
        std::lock_guard<NativeCallbackMutex> lock(ctx->error_mutex);
        auto **link = &ctx->graphics_objects;
        while (*link && *link != value) link = &(*link)->next;
        if (*link) *link = value->next;
    }
    vkdu_object_destroy(value->backend); delete[] value->shader;
    std::memset(value, 0, sizeof(*value)); release(ctx);
}
extern "C" HRESULT APIENTRY VioGpuD3D12BridgeGetTables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *device,
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *commands, D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue) {
    if (!device || !commands || !queue) return E_INVALIDARG;
    *device = {}; *commands = {}; *queue = {};
    native_heap_tables(device);
    // Assignment to actual WDK fields compile-checks ABI, including x86 stdcall.
    device->pfnCalcPrivateCommandQueueSize = queue_size; device->pfnCreateCommandQueue = queue_create; device->pfnDestroyCommandQueue = queue_destroy;
    device->pfnCalcPrivateCommandAllocatorSize = allocator_size; device->pfnCreateCommandAllocator = allocator_create;
    device->pfnDestroyCommandAllocator = allocator_destroy; device->pfnResetCommandAllocator = allocator_reset;
    device->pfnCalcPrivateCommandListSize = command_size; device->pfnCreateCommandList = command_create; device->pfnDestroyCommandList = command_destroy;
    device->pfnCalcPrivateCommandSignatureSize = indirect_signature_size;
    device->pfnCreateCommandSignature = indirect_signature_create; device->pfnDestroyCommandSignature = indirect_signature_destroy;
    device->pfnCalcPrivateRootSignatureSize = root_size; device->pfnCreateRootSignature = root_create; device->pfnDestroyRootSignature = root_destroy;
    device->pfnCalcPrivateShaderSize = shader_size; device->pfnCreateComputeShader = shader_create; device->pfnDestroyShader = shader_destroy;
    device->pfnCalcPrivatePipelineStateSize = pipeline_size; device->pfnCreatePipelineState = pipeline_create; device->pfnDestroyPipelineState = pipeline_destroy;
    device->pfnCalcPrivateDescriptorHeapSize = heap_size; device->pfnCreateDescriptorHeap = heap_create; device->pfnDestroyDescriptorHeap = heap_destroy;
    device->pfnGetDescriptorSizeInBytes = descriptor_size; device->pfnGetCPUDescriptorHandleForHeapStart = heap_cpu;
    device->pfnGetGPUDescriptorHandleForHeapStart = heap_gpu; device->pfnCreateUnorderedAccessView = create_uav;
    device->pfnCreateConstantBufferView = create_cbv;
    device->pfnCreateShaderResourceView = create_srv;
    device->pfnCreateSampler = create_sampler;
    device->pfnCopyDescriptorsSimple = copy_descriptors_simple;
    device->pfnCopyDescriptors = copy_descriptors;
    commands->pfnCloseCommandList = command_close; commands->pfnResetCommandList = command_reset;
    commands->pfnCopyBufferRegion = copy; commands->pfnResourceBarrier = barriers; commands->pfnDispatch = dispatch;
    commands->pfnCopyTextureRegion = copy_texture;
    commands->pfnExecuteIndirect = execute_indirect;
    commands->pfnSetComputeRootSignature = root_set; commands->pfnSetPipelineState = pipeline_set;
    commands->pfnSetComputeRootUnorderedAccessView = root_uav;
    commands->pfnSetComputeRootConstantBufferView = root_cbv;
    commands->pfnSetComputeRootShaderResourceView = root_srv;
    commands->pfnSetComputeRoot32BitConstant = root_constant;
    commands->pfnSetComputeRoot32BitConstants = root_constants;
    commands->pfnSetDescriptorHeaps = set_heaps; commands->pfnSetComputeRootDescriptorTable = set_table;
    queue->pfnExecuteCommandLists = execute;
    native_graphics_tables(device, commands);
    // Native heaps/import cover buffers and bounded non-RT/DS textures. OS monitored fences,
    // residency, WDDM2 GPUVA, graphics and Present remain absent; no admission.
    return S_OK;
}

#include "runtime.inc"
