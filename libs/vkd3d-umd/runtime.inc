/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Included by ddi.cpp: one Context and one set of production DDI functions. */
#include <array>
#include <mutex>
#include <unordered_map>

namespace {
struct NativeAdapter {
    D3D12DDI_HRTADAPTER runtime{};
    D3DDDI_ADAPTERCALLBACKS callbacks{};
    std::array<uint8_t, 8> luid{};
    uint64_t generation = 0;
};
std::mutex native_adapters_lock;
std::unordered_map<void *, std::shared_ptr<NativeAdapter>> native_adapters;

std::shared_ptr<NativeAdapter> native_adapter(D3D12DDI_HADAPTER handle) {
    std::lock_guard<std::mutex> lock(native_adapters_lock);
    auto found = native_adapters.find(handle.pDrvPrivate);
    return found == native_adapters.end() ? nullptr : found->second;
}

HRESULT native_query_identity(const NativeAdapter &adapter, std::array<uint8_t, 8> &luid, uint64_t &generation) {
    // VIOGPU v0 128-byte prefix + independently versioned VLID trailer.
    // Output-only callback: never encode a request in its output buffer.
    std::array<uint8_t, 160> reply{};
    D3DDDICB_QUERYADAPTERINFO args{};
    args.pPrivateDriverData = reply.data();
    args.PrivateDriverDataSize = static_cast<UINT>(reply.size());
    HRESULT hr = adapter.callbacks.pfnQueryAdapterInfoCb(adapter.runtime.handle, &args);
    if (FAILED(hr)) return hr;
    if (args.PrivateDriverDataSize != reply.size()) return DXGI_ERROR_UNSUPPORTED;
    auto u32 = [&](size_t i) {
        return uint32_t(reply[i]) | (uint32_t(reply[i+1]) << 8) |
            (uint32_t(reply[i+2]) << 16) | (uint32_t(reply[i+3]) << 24);
    };
    if (u32(0) != 0x504d5644 || u32(4) || u32(8) != 128 || u32(12) ||
            u32(112) || u32(116) || u32(120) || u32(124) ||
            u32(128) != 0x44494c56 || u32(132) != 1 || u32(136) != 32 ||
            u32(140) != 1 || u32(152) != 1 || u32(156)) return DXGI_ERROR_UNSUPPORTED;
    generation = uint64_t(u32(24)) | (uint64_t(u32(28)) << 32);
    std::memcpy(luid.data(), reply.data() + 144, luid.size());
    if (!generation || luid == std::array<uint8_t, 8>{}) return DXGI_ERROR_UNSUPPORTED;
    return S_OK;
}

bool native_version(UINT interface_version, UINT runtime_version, D3D12DDI_CREATE_DEVICE_FLAGS flags) {
    // The only actual implemented table layouts. Never reinterpret newer tables.
    return interface_version == D3D12DDI_INTERFACE_VERSION_R0 &&
        (runtime_version >> 16) >= D3D12DDI_BUILD_VERSION &&
        !(static_cast<UINT>(flags) & ~static_cast<UINT>(D3D12DDI_CREATE_DEVICE_FLAG_DISABLE_IMPLICIT_MGPU));
}

bool native_contract_complete() {
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    VioGpuD3D12BridgeGetTables(&device, &commands, &queue);
    // These mandatory native contracts are still absent. This is a prerequisite
    // gate, NOT automatic feature-level certification if these become non-null.
    // Full FL11_0 graphics/format/residency/Present validation must replace this
    // gate as one coherent change before any version is advertised.
    const bool prerequisites = device.pfnCreateHeapAndResource && device.pfnCreateFence &&
        device.pfnMakeResident && queue.pfnSignalFence && queue.pfnWaitForFence &&
        commands.pfnDrawInstanced && commands.pfnDrawIndexedInstanced;
    return prerequisites && false; // No complete native feature-level contract yet.
}

HRESULT APIENTRY native_versions(D3D12DDI_HADAPTER handle, UINT32 *count, UINT64 *versions) {
    if (!native_adapter(handle) || !count) return E_INVALIDARG;
    const UINT32 capacity = *count;
    *count = native_contract_complete() ? 1u : 0u;
    if (!versions || !*count) return S_OK;
    if (capacity < *count) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    versions[0] = D3D12DDI_SUPPORTED_0003;
    return S_OK;
}

HRESULT APIENTRY native_optional_tables(D3D12DDI_HADAPTER handle, UINT32 *count, D3D12DDI_TABLE_REQUEST *) {
    if (!native_adapter(handle) || !count) return E_INVALIDARG;
    *count = 0;
    return S_OK;
}

HRESULT APIENTRY native_caps(D3D12DDI_HADAPTER handle, const D3D12DDIARG_GETCAPS *args) {
    if (!native_adapter(handle) || !args || !args->pData || !args->DataSize) return E_INVALIDARG;
    // The embedded API feature level is not the native driver's feature level.
    // Bounded compute/graphics callbacks do not satisfy the complete FL11_0 contract.
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT APIENTRY native_fill_table(D3D12DDI_HADAPTER handle, D3D12DDI_TABLE_TYPE type,
        void *table, SIZE_T bytes, UINT index, D3D12DDI_HRTTABLE) {
    if (!native_adapter(handle) || !table) return E_INVALIDARG;
    if (index) return E_INVALIDARG;
    SIZE_T expected = 0;
    switch (type) {
        case D3D12DDI_TABLE_TYPE_DEVICE_CORE: expected = sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0003); break;
        case D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D: expected = sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003); break;
        case D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D: expected = sizeof(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001); break;
        default: return DXGI_ERROR_UNSUPPORTED;
    }
    if (bytes != expected) return E_INVALIDARG;
    // Leave caller memory untouched on failure, including tables with a newer
    // layout. Never let the runtime call a partially initialized callback table.
    if (!native_contract_complete()) return DXGI_ERROR_UNSUPPORTED;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    VioGpuD3D12BridgeGetTables(&device, &commands, &queue);
    if (type == D3D12DDI_TABLE_TYPE_DEVICE_CORE) std::memcpy(table, &device, bytes);
    else if (type == D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D) std::memcpy(table, &commands, bytes);
    else std::memcpy(table, &queue, bytes);
    return S_OK;
}

SIZE_T APIENTRY native_device_size(D3D12DDI_HADAPTER handle, const D3D12DDIARG_CALCPRIVATEDEVICESIZE *args) {
    return native_adapter(handle) && args && native_version(args->Interface, args->Version, args->Flags)
        ? sizeof(NativeDevice) : 0;
}

void APIENTRY native_report(void *opaque, HRESULT hr) {
    auto *ctx = static_cast<Context *>(opaque);
    if (ctx->runtime_device.handle && ctx->runtime_callbacks.pfnSetErrorCb)
        ctx->runtime_callbacks.pfnSetErrorCb(ctx->runtime_device, hr);
}

HRESULT native_load_vulkan(HMODULE *module, PFN_vkGetInstanceProcAddr *loader) {
    *module = nullptr; *loader = nullptr;
    // Match the active process ABI through the normal system Vulkan loader.
    // Do not search the application's working directory or mutate its DLL path.
    HMODULE loaded = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loaded) return HRESULT_FROM_WIN32(GetLastError());
    auto entry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loaded, "vkGetInstanceProcAddr"));
    if (!entry) { FreeLibrary(loaded); return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND); }
    *module = loaded; *loader = entry;
    return S_OK;
}

#include "native_heap.inc"
#include "native_samplers.inc"
#include "native_textures.inc"
#include "native_graphics.inc"
#include "native_fences.inc"
#include "native_residency.inc"

// error_mutex owns these lists. Compare opaque slots before dereferencing them.
Object *native_submission_find(Object *head, void *slot) {
    for (auto *entry = head; entry; entry = entry->next)
        if (entry == slot) return entry;
    return nullptr;
}
void native_submission_remove(Object *&head, Object *entry) {
    Object **link = &head;
    while (*link && *link != entry) link = &(*link)->next;
    if (*link) *link = entry->next;
}

#include "native_indirect.inc"

HRESULT native_queue_create(Context *ctx, const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args) {
    NativeHeapOperation op(ctx);
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    // R0 has no priority field. Never reinterpret a later queue revision.
    const auto request = *args;
    const auto type = command_type(request.QueueFlags);
    if (!request.hDrvCommandQueue.pDrvPrivate || !request.hRTCommandQueue.handle || type == UINT32_MAX ||
        native_submission_find(ctx->native_queue_objects, request.hDrvCommandQueue.pDrvPrivate) ||
        native_submission_find(ctx->native_command_objects, request.hDrvCommandQueue.pDrvPrivate)) return E_INVALIDARG;
    vkdu_object *value = nullptr;
    NativeRuntimeQueue *route = nullptr;
    HRESULT hr = native_runtime_queue_open(ctx, request.hRTCommandQueue, &route);
    if (FAILED(hr)) return hr;
    {
        NativeBackendCall call(ctx);
        hr = vkdu_queue_create(ctx->backend, type, &value);
        if (SUCCEEDED(hr)) hr = vkdu_queue_bind_runtime(value, ctx, route);
    }
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) hr = bind(ctx, request.hDrvCommandQueue.pDrvPrivate, value, VKDU_QUEUE);
    if (FAILED(hr)) {
        {
            NativeBackendCall call(ctx);
            vkdu_object_destroy(value);
        }
        native_runtime_queue_close(ctx, route);
        native_runtime_queue_release_record(ctx, route);
        return hr;
    }
    auto *queue = object(request.hDrvCommandQueue.pDrvPrivate);
    queue->runtime_queue = request.hRTCommandQueue;
    queue->runtime_route = route;
    queue->next = ctx->native_queue_objects;
    ctx->native_queue_objects = queue;
    return S_OK;
}

void native_queue_destroy(Context *ctx, void *slot) {
    NativeHeapOperation op(ctx, true);
    if (!op.retained) return;
    auto *queue = native_submission_find(ctx->native_queue_objects, slot);
    if (!queue) { error(ctx, E_INVALIDARG); return; }
    auto *value = queue->backend;
    auto *route = queue->runtime_route;
    native_submission_remove(ctx->native_queue_objects, queue);
    // The runtime may release this memory immediately, including reentrantly.
    std::memset(queue, 0, sizeof(*queue));
    {
        NativeBackendCall call(ctx);
        // All translation/Render callbacks must finish while hRTCommandQueue
        // is still valid. The embedded queue destructor drains remaining GPU work.
        vkdu_object_destroy(value);
    }
    HRESULT hr = op.live() ? native_runtime_queue_drain(ctx, route) : DXGI_ERROR_DEVICE_REMOVED;
    const HRESULT close_hr = native_runtime_queue_close(ctx, route);
    if (SUCCEEDED(hr)) hr = close_hr;
    native_runtime_queue_release_record(ctx, route);
    if (op.live() && FAILED(hr)) error(ctx, hr);
    release(ctx);
}

void native_queue_execute(Object *queue, UINT count, const D3D12DDI_HCOMMANDLIST *commands) {
    auto *ctx = queue->context;
    NativeHeapOperation op(ctx);
    if (!op.live()) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    if (!native_submission_find(ctx->native_queue_objects, queue) || !commands || !count || count > 64) {
        error(ctx, E_INVALIDARG); return;
    }
    vkdu_object *native[64]{};
    auto *queue_backend = queue->backend;
    if (!vkdu_object_retain(queue_backend)) { error(ctx, E_OUTOFMEMORY); return; }
    UINT retained = 0;
    HRESULT hr = S_OK;
    for (; retained < count; ++retained) {
        auto *command = native_submission_find(ctx->native_command_objects, commands[retained].pDrvPrivate);
        if (!command || command->kind != VKDU_COMMAND_LIST || !vkdu_object_retain(command->backend)) {
            hr = E_INVALIDARG; break;
        }
        native[retained] = command->backend;
    }
    {
        // Copies and independent backend references outlive every runtime slot.
        // A backend callback may destroy queue/list/device and poison all slots.
        NativeBackendCall call(ctx);
        if (SUCCEEDED(hr)) hr = vkdu_queue_execute(queue_backend, count, native);
        for (UINT index = 0; index < retained; ++index) vkdu_object_destroy(native[index]);
        vkdu_object_destroy(queue_backend);
    }
    // Never report through the queue slot after backend execution/cleanup.
    // error(Context) respects device callback detachment during retirement.
    error(ctx, hr);
}

HRESULT native_command_create(Context *ctx, const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args) {
    NativeHeapOperation op(ctx);
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (!args->hDrvCommandList.pDrvPrivate || !args->hRTCommandList.handle ||
            !ctx->runtime_callbacks.pfnSetCommandListErrorCb) return E_INVALIDARG;
    // Copy all runtime-owned input before a backend callback can retire it.
    void *slot = args->hDrvCommandList.pDrvPrivate;
    const auto runtime = args->hRTCommandList;
    auto *allocator = backend(args->hDrvCommandAllocator.pDrvPrivate, VKDU_ALLOCATOR);
    const auto type = command_type(args->QueueFlags);
    vkdu_object *value = nullptr;
    HRESULT hr;
    {
        NativeBackendCall call(ctx);
        hr = vkdu_command_create(ctx->backend, allocator, type, &value);
    }
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) hr = bind(ctx, slot, value, VKDU_COMMAND_LIST);
    if (FAILED(hr)) {
        NativeBackendCall call(ctx);
        vkdu_object_destroy(value);
        return hr;
    }
    object(slot)->runtime_command = runtime;
    object(slot)->next = ctx->native_command_objects;
    ctx->native_command_objects = object(slot);
    return S_OK;
}

void native_command_destroy(Context *ctx, Object *cmd) {
    NativeHeapOperation op(ctx, true);
    if (!op.retained) return;
    cmd = native_submission_find(ctx->native_command_objects, cmd);
    if (!cmd) { error(ctx, E_INVALIDARG); return; }
    auto *value = cmd->backend;
    native_submission_remove(ctx->native_command_objects, cmd);
    // The runtime can destroy this slot recursively from its error callback.
    // Invalidate it before any backend callback and never revisit it afterward.
    std::memset(cmd, 0, sizeof(*cmd));
    {
        NativeBackendCall call(ctx);
        vkdu_object_destroy(value);
    }
    release(ctx); // Drop the command-list owner; op still retains Context.
}

HRESULT APIENTRY native_create_device(D3D12DDI_HADAPTER handle, const D3D12DDIARG_CREATEDEVICE_0003 *args) {
    auto adapter = native_adapter(handle);
    if (!adapter || !args || !args->hDrvDevice.pDrvPrivate || !args->hRTDevice.handle ||
            !args->pKTCallbacks || !args->p12UMCallbacks || !args->p12UMCallbacks->pfnSetErrorCb)
        return E_INVALIDARG;
    if (!native_version(args->Interface, args->Version, args->Flags)) return DXGI_ERROR_UNSUPPORTED;
    // Never inspect uninitialized runtime-owned storage before constructing it.
    std::array<uint8_t, 8> current_luid{}; uint64_t current_generation = 0;
    HRESULT hr = native_query_identity(*adapter, current_luid, current_generation);
    if (FAILED(hr)) return hr;
    if (current_luid != adapter->luid || current_generation != adapter->generation) return DXGI_ERROR_DEVICE_REMOVED;
    auto *ctx = new (std::nothrow) Context;
    if (!ctx) return E_OUTOFMEMORY;
    PFN_vkGetInstanceProcAddr loader = nullptr;
    hr = native_load_vulkan(&ctx->vulkan_module, &loader);
    ctx->native_adapter = adapter;
    ctx->runtime_device = args->hRTDevice;
    ctx->kernel_callbacks = *args->pKTCallbacks;
    ctx->runtime_callbacks = *args->p12UMCallbacks;
    if (SUCCEEDED(hr)) hr = vkdu_device_create_shared(loader, adapter->luid.data(), &native_runtime_callbacks, ctx, &ctx->backend);
    // Backend creation may race a KMD reset; revalidate before publishing state.
    if (SUCCEEDED(hr)) hr = native_query_identity(*adapter, current_luid, current_generation);
    if (SUCCEEDED(hr) && (current_luid != adapter->luid || current_generation != adapter->generation))
        hr = DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(hr)) {
        ctx->native_retiring = true;
        native_heap_retire(ctx);
        ctx->runtime_device = {}; ctx->kernel_callbacks = {};
        release(ctx); return hr;
    }
    ctx->native_adapter = std::move(adapter);
    ctx->runtime_device = args->hRTDevice;
    ctx->kernel_callbacks = *args->pKTCallbacks;
    ctx->runtime_callbacks = *args->p12UMCallbacks;
    ctx->report = native_report;
    ctx->report_context = ctx;
    new (args->hDrvDevice.pDrvPrivate) NativeDevice{native_device_magic, ctx};
    return S_OK;
}

void APIENTRY native_destroy_device(D3D12DDI_HDEVICE handle) {
    if (!handle.pDrvPrivate) return;
    auto *slot = static_cast<NativeDevice *>(handle.pDrvPrivate);
    if (slot->magic != native_device_magic) return;
    NativeHeapOperation op(slot->context);
    auto *ctx = op.ctx;
    if (!op.retained || ctx->native_retiring) return;
    const bool first_destroy = !ctx->native_destroying;
    ctx->native_destroying = true;
    // The ordinary last owner drains Vulkan while its completion/unmap/release
    // callbacks are still legal. New allocation/map/submit work is closed.
    // This operation retains a separate reference across a possible recursive
    // DestroyDevice, which may invalidate the runtime slot before we return.
    if (first_destroy && ctx->references.load() == 2 && !ctx->native_backend_calls &&
            !ctx->native_context_pending && !ctx->native_submitting) {
        auto *backend = ctx->backend;
        ctx->backend = nullptr;
        {
            NativeBackendCall backend_call(ctx);
            vkdu_device_destroy(backend);
        }
    }
    // A recursive destroy has already detached the slot/callbacks and dropped
    // its owner reference. Do not touch that expired runtime storage again.
    if (ctx->native_retiring) return;
    // Runtime releases this storage immediately after return. Child objects may
    // keep Context alive, but must never call a retired runtime handle/callback.
    ctx->native_retiring = true;
    native_fences_retire(ctx);
    native_heap_retire(ctx);
    ctx->runtime_device = {};
    ctx->runtime_callbacks = {};
    ctx->kernel_callbacks = {};
    ctx->report = nullptr; ctx->report_context = nullptr;
    slot->context = nullptr; slot->magic = 0;
    release(ctx);
}

HRESULT APIENTRY native_close_adapter(D3D12DDI_HADAPTER handle) {
    std::lock_guard<std::mutex> lock(native_adapters_lock);
    return native_adapters.erase(handle.pDrvPrivate) ? S_OK : E_INVALIDARG;
}
}

extern "C" HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER *args) {
    if (!args) return E_INVALIDARG;
    args->hAdapter = {};
    if (!args->pAdapterFuncs) return E_INVALIDARG;
    *args->pAdapterFuncs = {};
    if (!args->hRTAdapter.handle || !args->pAdapterCallbacks || !args->pAdapterCallbacks->pfnQueryAdapterInfoCb)
        return E_INVALIDARG;
    try {
        auto adapter = std::make_shared<NativeAdapter>();
        adapter->runtime = args->hRTAdapter;
        adapter->callbacks = *args->pAdapterCallbacks;
        HRESULT hr = native_query_identity(*adapter, adapter->luid, adapter->generation);
        if (FAILED(hr)) return hr;
        {
            std::lock_guard<std::mutex> lock(native_adapters_lock);
            native_adapters.emplace(adapter.get(), adapter);
        }
        D3D12DDI_ADAPTERFUNCS table{};
        table.pfnCalcPrivateDeviceSize = native_device_size;
        table.pfnCreateDevice = native_create_device;
        table.pfnCloseAdapter = native_close_adapter;
        table.pfnGetSupportedVersions = native_versions;
        table.pfnGetCaps = native_caps;
        table.pfnGetOptionalDDITables = native_optional_tables;
        table.pfnFillDDITable = native_fill_table;
        table.pfnDestroyDevice = native_destroy_device;
        *args->pAdapterFuncs = table;
        args->hAdapter.pDrvPrivate = adapter.get();
        return S_OK;
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
