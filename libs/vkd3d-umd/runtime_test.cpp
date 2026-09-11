/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* WDK lifecycle fixture with controlled Vulkan/backend peers, NOT system
 * D3D12CreateDevice or VIOGPU GPU acceptance. Uses actual production entry. */
#include "ddi.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <thread>

static HRESULT backend_result = S_OK, query_result = S_OK;
static unsigned loads, unloads, creates, destroys, query_calls, error_calls;
static bool bad_loader = false, old_reply = false;
static bool reset_during_create = false;
static PFND3D12DDI_DESTROYDEVICE retire_in_error = nullptr;
static D3D12DDI_HDEVICE error_device{};
static uint64_t generation = 7;
static D3D12DDI_HRTDEVICE last_runtime{};
static const HANDLE expected_adapter = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x13570));
static const std::array<uint8_t, 8> expected_luid{1,2,3,4,5,6,7,8};
static void wait_backend_worker(const mwd_callbacks *callbacks, void *owner) {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) std::abort();
    std::thread worker([=]() {
        callbacks->status(owner);
        uint32_t fence = 0;
        callbacks->completed(owner, &fence);
        SetEvent(done);
    });
    if (WaitForSingleObject(done, 2000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "FAIL backend worker blocked by native callback lock\n");
        std::abort();
    }
    worker.join(); CloseHandle(done);
}
struct TestDevice { const mwd_callbacks *callbacks; void *owner; };
static HMODULE WINAPI test_load(LPCWSTR name, HANDLE, DWORD flags) {
    if (std::wcscmp(name, L"vulkan-1.dll") || flags != LOAD_LIBRARY_SEARCH_SYSTEM32) std::abort();
    ++loads;
    if (bad_loader) { SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    return reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x24680));
}
static FARPROC WINAPI test_symbol(HMODULE, LPCSTR name) {
    if (std::strcmp(name, "vkGetInstanceProcAddr")) std::abort();
    return reinterpret_cast<FARPROC>(static_cast<uintptr_t>(0x35790));
}
static BOOL WINAPI test_unload(HMODULE) { ++unloads; return TRUE; }
static int32_t test_create(PFN_vkGetInstanceProcAddr loader, const uint8_t luid[8],
        const mwd_callbacks *callbacks, void *owner, vkdu_device **out) {
    if (!loader || std::memcmp(luid, expected_luid.data(), 8) || !owner || !mwd_callbacks_valid(callbacks)) std::abort();
    ++creates; *out = nullptr;
    if (SUCCEEDED(backend_result)) *out = reinterpret_cast<vkdu_device *>(new TestDevice{callbacks, owner});
    if (reset_during_create) ++generation;
    return backend_result;
}
static void test_destroy(vkdu_device *device) {
    if (device) {
        auto *peer = reinterpret_cast<TestDevice *>(device);
        wait_backend_worker(peer->callbacks, peer->owner);
        ++destroys; delete peer;
    }
}
static HRESULT APIENTRY test_query(HANDLE runtime, const D3DDDICB_QUERYADAPTERINFO *args) {
    if (runtime != expected_adapter || args->PrivateDriverDataSize != 160) std::abort();
    auto *bytes = static_cast<uint8_t *>(args->pPrivateDriverData);
    for (unsigned i = 0; i < 160; ++i) if (bytes[i]) std::abort();
    ++query_calls;
    if (FAILED(query_result)) return query_result;
    auto put = [&](size_t offset, uint32_t value) { std::memcpy(bytes + offset, &value, 4); };
    put(0, 0x504d5644); put(8, 128);
    std::memcpy(bytes + 24, &generation, 8);
    if (!old_reply) {
        put(128, 0x44494c56); put(132, 1); put(136, 32); put(140, 1); put(152, 1);
        std::memcpy(bytes + 144, expected_luid.data(), 8);
    }
    return S_OK;
}
static void APIENTRY test_error(D3D10DDI_HRTDEVICE runtime, HRESULT result) {
    if (SUCCEEDED(result)) std::abort();
    ++error_calls; last_runtime = runtime;
    if (retire_in_error) retire_in_error(error_device);
}

static int32_t test_import_heap(vkdu_device *, void *, void *, uint64_t, int, vkdu_object **);
static int32_t test_place_buffer(vkdu_device *, vkdu_object *, uint64_t, uint64_t, uint32_t, vkdu_object **);
static void test_object_destroy(vkdu_object *);
static int test_object_is(vkdu_object *, vkdu_kind);
static int test_object_belongs(vkdu_device *, vkdu_object *);
static uint64_t test_buffer_address(vkdu_object *);
static uint64_t test_buffer_size(vkdu_object *);

#define LoadLibraryExW test_load
#define GetProcAddress test_symbol
#define FreeLibrary test_unload
#define vkdu_device_create_shared test_create
#define vkdu_device_destroy test_destroy
#define vkdu_memory_heap_import test_import_heap
#define vkdu_buffer_place test_place_buffer
#define vkdu_object_destroy test_object_destroy
#define vkdu_object_is test_object_is
#define vkdu_object_belongs test_object_belongs
#define vkdu_buffer_address test_buffer_address
#define vkdu_buffer_size test_buffer_size
#include "ddi.cpp"

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

struct ImportOwner {
    Context *context;
    mwd_allocation allocation;
    ~ImportOwner() { native_runtime_release(context, allocation.token); }
};
struct ImportPeer {
    std::shared_ptr<ImportOwner> owner;
    vkdu_device *device;
    vkdu_kind kind;
    uint64_t address, bytes;
};
static bool import_failure, placement_failure;
static unsigned imports, placements;
static int32_t test_import_heap(vkdu_device *device, void *owner, void *token, uint64_t size, int, vkdu_object **out) {
    *out = nullptr; ++imports;
    wait_backend_worker(&native_runtime_callbacks, owner);
    if (import_failure) return E_OUTOFMEMORY;
    mwd_allocation allocation{};
    HRESULT hr = native_runtime_retain(owner, token, &allocation);
    if (FAILED(hr)) return hr;
    if (allocation.size != size) std::abort();
    auto backing = std::make_shared<ImportOwner>();
    backing->context = static_cast<Context *>(owner); backing->allocation = allocation;
    *out = reinterpret_cast<vkdu_object *>(new ImportPeer{backing, device, VKDU_MEMORY_HEAP, allocation.address, size});
    return S_OK;
}
static int32_t test_place_buffer(vkdu_device *device, vkdu_object *memory, uint64_t offset, uint64_t size, uint32_t, vkdu_object **out) {
    *out = nullptr; ++placements;
    if (placement_failure) return E_INVALIDARG;
    auto *peer = reinterpret_cast<ImportPeer *>(memory);
    if (peer->device != device || peer->kind != VKDU_MEMORY_HEAP || offset || size > peer->bytes) std::abort();
    *out = reinterpret_cast<vkdu_object *>(new ImportPeer{peer->owner, device, VKDU_BUFFER, peer->address, size});
    return S_OK;
}
static void test_object_destroy(vkdu_object *object) {
    auto *peer = reinterpret_cast<ImportPeer *>(object);
    if (peer) wait_backend_worker(&native_runtime_callbacks, peer->owner->context);
    delete peer;
}
static int test_object_is(vkdu_object *object, vkdu_kind kind) {
    return object && reinterpret_cast<ImportPeer *>(object)->kind == kind;
}
static int test_object_belongs(vkdu_device *device, vkdu_object *object) {
    return object && reinterpret_cast<ImportPeer *>(object)->device == device;
}
static uint64_t test_buffer_address(vkdu_object *object) { return object ? reinterpret_cast<ImportPeer *>(object)->address : 0; }
static uint64_t test_buffer_size(vkdu_object *object) { return object ? reinterpret_cast<ImportPeer *>(object)->bytes : 0; }

struct KernelHeap { uint64_t address, bytes; bool locked; HANDLE resource; };
static std::map<D3DKMT_HANDLE, KernelHeap> kernel_heaps;
static std::set<HANDLE> kernel_resources;
static D3DKMT_HANDLE next_allocation = 101;
static unsigned heap_context_creates, heap_context_destroys, allocations, deallocations, locks, unlocks;
static unsigned fail_deallocate, fail_unlock;
static unsigned fail_context_destroy;
static bool fail_context_create, invalid_context_info, retire_lock;
static bool fail_allocate, partial_allocate, partial_resource, null_map, reset_allocate, retire_allocate, rename_lock;
static bool heap_callbacks_retired;
static std::array<unsigned char, 65536> mapped_heap{};
static const HANDLE expected_device = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x456a0));
static const HANDLE expected_context = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xabc50));
static std::array<unsigned char, 65536> commands[2];
static std::array<D3DDDI_ALLOCATIONLIST, 1024> allocation_lists[2];
static std::array<D3DDDI_PATCHLOCATIONLIST, 1024> patch_lists[2];
static unsigned renders;
static HRESULT render_result = S_OK;
static bool retire_render;
static void valid_kernel_callback(HANDLE runtime) {
    if (runtime != expected_device || heap_callbacks_retired) std::abort();
}
static HRESULT APIENTRY heap_context_create(HANDLE runtime, D3DDDICB_CREATECONTEXT *args) {
    valid_kernel_callback(runtime);
    auto *data = static_cast<NativeContextCreate *>(args->pPrivateDriverData);
    if (args->PrivateDriverDataSize != sizeof(*data) || !native_header_valid(data->header, sizeof(*data)) ||
            data->generation != generation || data->flags || data->reserved || args->NodeOrdinal || args->EngineAffinity != 1)
        std::abort();
    ++heap_context_creates; args->hContext = expected_context;
    args->pCommandBuffer = commands[0].data(); args->CommandBufferSize = 65536;
    args->pAllocationList = allocation_lists[0].data(); args->AllocationListSize = 1024;
    args->pPatchLocationList = patch_lists[0].data(); args->PatchLocationListSize = 1024;
    return fail_context_create ? E_OUTOFMEMORY : S_OK;
}
static HRESULT APIENTRY heap_context_destroy(HANDLE runtime, const D3DDDICB_DESTROYCONTEXT *args) {
    valid_kernel_callback(runtime);
    if (args->hContext != expected_context || !kernel_heaps.empty() || !kernel_resources.empty()) std::abort();
    ++heap_context_destroys;
    if (fail_context_destroy) { --fail_context_destroy; return E_FAIL; }
    return S_OK;
}
static HRESULT APIENTRY heap_escape(HANDLE adapter, const D3DDDICB_ESCAPE *args) {
    valid_kernel_callback(args->hDevice);
    if (args->PrivateDriverDataSize == sizeof(NativeFenceInfo)) {
        auto *fence = static_cast<NativeFenceInfo *>(args->pPrivateDriverData);
        if (adapter != expected_adapter || args->hContext != expected_context || fence->opcode != 2 ||
                fence->expected_generation != generation || fence->completed || fence->context_id) std::abort();
        fence->generation = generation; fence->context_id = 83; fence->completed = 9;
        return S_OK;
    }
    auto *info = static_cast<NativeContextInfo *>(args->pPrivateDriverData);
    if (adapter != expected_adapter || args->hContext != expected_context || args->PrivateDriverDataSize != sizeof(*info) ||
            !native_header_valid(info->header, sizeof(*info)) || info->opcode != 1 || info->flags ||
            info->expected_generation != generation || info->va_start || info->va_size || info->generation ||
            info->context_id || info->queue_id) std::abort();
    info->va_start = 0x10001000; info->va_size = 0x80000; info->generation = generation;
    info->context_id = 83; info->queue_id = 1;
    if (invalid_context_info) info->va_size = UINT64_MAX;
    return S_OK;
}
static HRESULT APIENTRY heap_allocate(HANDLE runtime, D3DDDICB_ALLOCATE *args) {
    valid_kernel_callback(runtime);
    if (args->NumAllocations != 1 || !args->pAllocationInfo || args->pPrivateDriverData || args->PrivateDriverDataSize)
        std::abort();
    auto *data = static_cast<NativeAllocationInfo *>(args->pAllocationInfo->pPrivateDriverData);
    if (args->pAllocationInfo->PrivateDriverDataSize != sizeof(*data) || !native_header_valid(data->header, sizeof(*data)) ||
            data->alignment != 4096 || data->context_id != 83 || data->generation != generation ||
            (data->flags != 4 && data->flags != 6 && data->flags != 14) || data->format || data->width || data->height || data->pitch ||
            data->refresh_numerator || data->refresh_denominator || data->iova < 0x10001000 ||
            data->iova % 4096 || data->size % 4096 || data->iova + data->size > 0x10081000)
        std::abort();
    for (const auto &item : kernel_heaps) {
        const auto &heap = item.second;
        if (data->iova < heap.address + heap.bytes && heap.address < data->iova + data->size) std::abort();
    }
    ++allocations;
    if (!fail_allocate || partial_allocate) {
        args->pAllocationInfo->hAllocation = next_allocation++;
        kernel_heaps.emplace(args->pAllocationInfo->hAllocation, KernelHeap{data->iova, data->size, false, args->hResource});
        if (args->hResource) {
            if (!kernel_resources.insert(args->hResource).second) std::abort();
            args->hKMResource = 73;
        }
    }
    if (fail_allocate && partial_resource && args->hResource) {
        if (!kernel_resources.insert(args->hResource).second) std::abort();
        args->hKMResource = 73;
    }
    if (reset_allocate) ++generation;
    if (retire_allocate) {
        native_destroy_device(error_device);
        heap_callbacks_retired = true;
    }
    return fail_allocate ? E_OUTOFMEMORY : S_OK;
}
static HRESULT APIENTRY heap_deallocate(HANDLE runtime, const D3DDDICB_DEALLOCATE *args) {
    valid_kernel_callback(runtime);
    ++deallocations;
    if (fail_deallocate) { --fail_deallocate; return DXGI_ERROR_WAS_STILL_DRAWING; }
    if (args->hResource) {
        if (args->NumAllocations || args->HandleList || !kernel_resources.erase(args->hResource)) std::abort();
        for (auto found = kernel_heaps.begin(); found != kernel_heaps.end();) {
            if (found->second.resource == args->hResource) {
                if (found->second.locked) std::abort();
                found = kernel_heaps.erase(found);
            } else ++found;
        }
    } else {
        if (args->NumAllocations != 1 || !args->HandleList) std::abort();
        auto found = kernel_heaps.find(*args->HandleList);
        if (found == kernel_heaps.end() || found->second.locked || found->second.resource) std::abort();
        kernel_heaps.erase(found);
    }
    return S_OK;
}
static HRESULT APIENTRY heap_lock(HANDLE runtime, D3DDDICB_LOCK *args) {
    valid_kernel_callback(runtime);
    auto found = kernel_heaps.find(args->hAllocation);
    if (found == kernel_heaps.end() || found->second.locked || !args->Flags.LockEntire ||
            args->Flags.Discard || args->Flags.NoExistingReference || args->Flags.ReadOnly || args->NumPages || args->pPages)
        std::abort();
    found->second.locked = true;
    if (rename_lock) {
        args->hAllocation = next_allocation++;
        kernel_heaps.emplace(args->hAllocation, found->second); kernel_heaps.erase(found);
    }
    ++locks; args->pData = null_map ? nullptr : mapped_heap.data();
    if (retire_lock) { native_destroy_device(error_device); heap_callbacks_retired = true; }
    return S_OK;
}
static HRESULT APIENTRY heap_unlock(HANDLE runtime, const D3DDDICB_UNLOCK *args) {
    valid_kernel_callback(runtime);
    if (args->NumAllocations != 1 || !args->phAllocations) std::abort();
    auto found = kernel_heaps.find(*args->phAllocations);
    if (found == kernel_heaps.end() || !found->second.locked) std::abort();
    ++unlocks;
    if (fail_unlock) { --fail_unlock; return E_FAIL; }
    found->second.locked = false;
    return S_OK;
}

static HRESULT APIENTRY heap_render(HANDLE runtime, D3DDDICB_RENDER *args) {
    valid_kernel_callback(runtime);
    if (args->hContext != expected_context || args->NumAllocations != 1 || args->NumPatchLocations != 1 ||
            args->CommandOffset || args->Flags.Value) std::abort();
    auto *header = static_cast<NativeRenderInfo *>(args->pNewCommandBuffer);
    auto *refs = reinterpret_cast<NativeRenderReference *>(static_cast<unsigned char *>(args->pNewCommandBuffer) + 64);
    if (!native_header_valid(header->header, args->CommandLength) || header->generation != generation ||
            header->stream_offset != 96 || header->references_count != 1 || refs->length != 4096 ||
            !kernel_heaps.count(args->pNewAllocationList[0].hAllocation) ||
            args->pNewPatchLocationList[0].PatchOffset != 96) std::abort();
    ++renders;
    args->pNewCommandBuffer = commands[1].data(); args->NewCommandBufferSize = 65536;
    args->pNewAllocationList = allocation_lists[1].data(); args->NewAllocationListSize = 1024;
    args->pNewPatchLocationList = patch_lists[1].data(); args->NewPatchLocationListSize = 1024;
    if (retire_render) { native_destroy_device(error_device); heap_callbacks_retired = true; }
    return render_result;
}

static int test_native_heaps() {
    D3DDDI_ADAPTERCALLBACKS adapter_callbacks{};
    adapter_callbacks.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &adapter_callbacks; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    kt.pfnCreateContextCb = heap_context_create; kt.pfnDestroyContextCb = heap_context_destroy;
    kt.pfnEscapeCb = heap_escape; kt.pfnAllocateCb = heap_allocate; kt.pfnDeallocateCb = heap_deallocate;
    kt.pfnLockCb = heap_lock; kt.pfnUnlockCb = heap_unlock; kt.pfnRenderCb = heap_render;
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{}; um.pfnSetErrorCb = test_error;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> memory{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = memory.data(); create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queue) == S_OK);
    D3D12DDIARG_CREATEHEAP_0001 desc{};
    desc.ByteSize = 65536; desc.Alignment = 65536; desc.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    desc.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK; desc.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
    desc.CreationNodeMask = desc.VisibleNodeMask = 1;
    D3D12DDI_HRTRESOURCE runtime_resource{}; runtime_resource.handle = reinterpret_cast<HANDLE>(uintptr_t(0xdef60));
    alignas(NativeHeapSlot) std::array<unsigned char, sizeof(NativeHeapSlot) + 16> a, b;
    a.fill(0xa5); b.fill(0xa5);
    D3D12DDI_HHEAP ha{}, hb{}; ha.pDrvPrivate = a.data(); hb.pDrvPrivate = b.data();
    auto allocate = [&](D3D12DDI_HHEAP heap) {
        auto rt = runtime_resource;
        if (heap.pDrvPrivate != ha.pDrvPrivate) rt.handle = nullptr;
        return table.pfnCreateHeapAndResource(create.hDrvDevice, &desc, heap, rt, nullptr, nullptr, {});
    };
    auto destroy = [&](D3D12DDI_HHEAP heap) { table.pfnDestroyHeapAndResource(create.hDrvDevice, heap, {}); };
    REQUIRE(table.pfnCalcPrivateHeapAndResourceSizes(create.hDrvDevice, &desc, nullptr).Heap == sizeof(NativeHeapSlot));
    D3D12DDIARG_CREATERESOURCE_0003 resource{};
    REQUIRE(table.pfnCalcPrivateHeapAndResourceSizes(create.hDrvDevice, &desc, &resource).Heap == 0);
    REQUIRE(table.pfnCreateHeapAndResource(create.hDrvDevice, &desc, ha, {}, &resource, nullptr, {}) == DXGI_ERROR_UNSUPPORTED);
    desc.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    REQUIRE(allocate(ha) == DXGI_ERROR_UNSUPPORTED && !allocations);
    desc.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    REQUIRE(allocate(ha) == S_OK && heap_context_creates == 1 && allocations == 1);
    REQUIRE(kernel_heaps.begin()->second.address == 0x10010000 && kernel_heaps.begin()->second.resource == runtime_resource.handle);
    for (size_t i = sizeof(NativeHeapSlot); i < a.size(); ++i) REQUIRE(a[i] == 0xa5);
    REQUIRE(allocate(ha) == E_INVALIDARG && allocations == 1);
    REQUIRE(table.pfnCreateHeapAndResource(create.hDrvDevice, &desc, hb, runtime_resource, nullptr, nullptr, {}) == E_INVALIDARG);
    REQUIRE(allocate(hb) == S_OK && allocations == 2 && heap_context_creates == 1);
    REQUIRE(kernel_heaps.rbegin()->second.address == 0x10020000);
    // The arena starts at a non-64K boundary, has no room for this large heap,
    // and cannot be exhausted by a request that never reached AllocateCb.
    alignas(NativeHeapSlot) std::array<unsigned char, sizeof(NativeHeapSlot)> extra{};
    D3D12DDI_HHEAP hc{}; hc.pDrvPrivate = extra.data();
    desc.ByteSize = 0x70000;
    REQUIRE(allocate(hc) == E_OUTOFMEMORY && allocations == 2);
    desc.ByteSize = UINT64_MAX;
    REQUIRE(allocate(hc) == E_INVALIDARG && allocations == 2);
    desc.ByteSize = 65536;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> other_memory{};
    auto other_create = create; other_create.hDrvDevice.pDrvPrivate = other_memory.data();
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &other_create) == S_OK);
    void *foreign_map = reinterpret_cast<void *>(uintptr_t(1));
    REQUIRE(table.pfnMapHeap(other_create.hDrvDevice, ha, &foreign_map) == E_INVALIDARG && !foreign_map);
    adapter.pfnDestroyDevice(other_create.hDrvDevice);
    // Nested maps use one kernel lock. LockCb may rename the allocation handle.
    rename_lock = true;
    void *mapping = nullptr, *again = nullptr;
    REQUIRE(table.pfnMapHeap(create.hDrvDevice, ha, &mapping) == S_OK && mapping == mapped_heap.data());
    REQUIRE(table.pfnMapHeap(create.hDrvDevice, ha, &again) == S_OK && again == mapping && locks == 1);
    table.pfnUnmapHeap(create.hDrvDevice, ha); REQUIRE(unlocks == 0);
    table.pfnUnmapHeap(create.hDrvDevice, ha); REQUIRE(unlocks == 1);
    rename_lock = false;
    destroy(ha); destroy(hb);
    REQUIRE(kernel_heaps.empty() && kernel_resources.empty() && !ctx->native_heaps);
    fail_allocate = partial_resource = true;
    REQUIRE(allocate(ha) == E_OUTOFMEMORY && kernel_heaps.empty() && kernel_resources.empty() && !ctx->native_heaps);
    fail_allocate = partial_resource = false;
    // Failed allocation must not publish a runtime slot; even a failed cleanup
    // keeps its GPUVA quarantined until device retirement retries deallocation.
    a.fill(0xa5);
    fail_allocate = partial_allocate = true; fail_deallocate = 1;
    REQUIRE(allocate(ha) == E_OUTOFMEMORY && kernel_heaps.size() == 1);
    for (auto byte : a) REQUIRE(byte == 0xa5);
    fail_allocate = partial_allocate = false;
    REQUIRE(allocate(hb) == S_OK && kernel_heaps.rbegin()->second.address == 0x10020000);
    destroy(hb);
    REQUIRE(kernel_heaps.size() == 1);
    adapter.pfnDestroyDevice(create.hDrvDevice);
    REQUIRE(kernel_heaps.empty() && kernel_resources.empty() && heap_context_destroys == 1);
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);

    REQUIRE(OpenAdapter12(&open) == S_OK);
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    fail_context_create = true; fail_context_destroy = 1;
    const unsigned create_count = heap_context_creates;
    REQUIRE(allocate(ha) == E_OUTOFMEMORY && heap_context_creates == create_count + 1);
    REQUIRE(context(create.hDrvDevice)->native_heap_context && !context(create.hDrvDevice)->native_context_id);
    fail_context_create = false; invalid_context_info = true;
    REQUIRE(allocate(ha) == E_FAIL && !context(create.hDrvDevice)->native_heap_context);
    invalid_context_info = false;
    REQUIRE(allocate(ha) == S_OK);
    null_map = true; fail_unlock = 1;
    mapping = reinterpret_cast<void *>(uintptr_t(1));
    REQUIRE(table.pfnMapHeap(create.hDrvDevice, ha, &mapping) == E_FAIL && !mapping);
    REQUIRE(kernel_heaps.begin()->second.locked);
    null_map = false;
    destroy(ha);
    REQUIRE(kernel_heaps.empty());
    // Reset between AllocateCb and publication rolls back using the exact
    // returned allocation owner, and leaves runtime storage untouched.
    a.fill(0xa5); reset_allocate = true;
    REQUIRE(allocate(ha) == DXGI_ERROR_DEVICE_REMOVED && kernel_heaps.empty());
    for (auto byte : a) REQUIRE(byte == 0xa5);
    reset_allocate = false;
    adapter.pfnDestroyDevice(create.hDrvDevice);
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);

    REQUIRE(OpenAdapter12(&open) == S_OK);
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    error_device = create.hDrvDevice; retire_allocate = true;
    const unsigned before = destroys;
    REQUIRE(allocate(ha) == DXGI_ERROR_DEVICE_REMOVED);
    REQUIRE(!context(create.hDrvDevice) && destroys == before + 1 && kernel_heaps.size() == 1);
    for (auto byte : a) REQUIRE(byte == 0xa5);
    // The runtime owns remaining kernel objects after its device teardown.
    // No stale callback is allowed to clean them from a retired Context.
    kernel_heaps.clear(); kernel_resources.clear(); heap_callbacks_retired = retire_allocate = false;
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    REQUIRE(OpenAdapter12(&open) == S_OK);
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    REQUIRE(allocate(ha) == S_OK);
    retire_lock = true; error_device = create.hDrvDevice;
    mapping = reinterpret_cast<void *>(uintptr_t(1));
    REQUIRE(table.pfnMapHeap(create.hDrvDevice, ha, &mapping) == DXGI_ERROR_DEVICE_REMOVED && !mapping);
    REQUIRE(!context(create.hDrvDevice) && kernel_heaps.size() == 1 && kernel_heaps.begin()->second.locked);
    kernel_heaps.clear(); kernel_resources.clear(); heap_callbacks_retired = retire_lock = false;
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    REQUIRE(!native_contract_complete());
    // One real allocation token survives destruction of its public heap slot.
    REQUIRE(OpenAdapter12(&open) == S_OK);
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    ctx = context(create.hDrvDevice);
    resource.ResourceType = D3D12DDI_RT_BUFFER; resource.Width = 4096;
    resource.Height = resource.DepthOrArraySize = resource.MipLevels = resource.SampleDesc.Count = 1;
    resource.Layout = D3D12DDI_TL_ROW_MAJOR;
    resource.InitialResourceState = static_cast<D3D12DDI_RESOURCE_STATES>(0x400);
    alignas(Object) std::array<unsigned char, sizeof(Object) + 8> resource_storage;
    resource_storage.fill(0xa5);
    D3D12DDI_HRESOURCE resource_handle{}; resource_handle.pDrvPrivate = resource_storage.data();
    auto paired = [&]() {
        return table.pfnCreateHeapAndResource(create.hDrvDevice, &desc, ha, {}, &resource, nullptr, resource_handle);
    };
    auto sizes = table.pfnCalcPrivateHeapAndResourceSizes(create.hDrvDevice, &desc, &resource);
    REQUIRE(sizes.Heap == sizeof(NativeHeapSlot) && sizes.Resource == sizeof(Object));
    unsigned before_pair = allocations;
    REQUIRE(paired() == S_OK && allocations == before_pair + 1 && imports == 1 && placements == 1);
    auto *paired_heap = native_heap_find(ctx, ha.pDrvPrivate);
    REQUIRE(paired_heap && object(resource_handle.pDrvPrivate)->address == paired_heap->address && paired_heap->users == 2);
    for (size_t i = sizeof(Object); i < resource_storage.size(); ++i) REQUIRE(resource_storage[i] == 0xa5);
    // Destroying the heap slot cannot destroy the resource's imported backing.
    destroy(ha);
    REQUIRE(native_token(ctx, paired_heap) && paired_heap->users == 1 && kernel_heaps.size() == 1);
    table.pfnDestroyHeapAndResource(create.hDrvDevice, {}, resource_handle);
    REQUIRE(kernel_heaps.empty() && !ctx->native_heaps);
    REQUIRE(paired() == S_OK);
    table.pfnDestroyHeapAndResource(create.hDrvDevice, ha, resource_handle);
    REQUIRE(kernel_heaps.empty() && !ctx->native_heaps);
    // Both import failure and placement failure unwind their actual owners.
    resource_storage.fill(0xa5); a.fill(0xa5); import_failure = true;
    REQUIRE(paired() == E_OUTOFMEMORY && kernel_heaps.empty() && !ctx->native_heaps);
    import_failure = false; placement_failure = true;
    REQUIRE(paired() == E_INVALIDARG && kernel_heaps.empty() && !ctx->native_heaps);
    placement_failure = false;
    for (auto byte : resource_storage) REQUIRE(byte == 0xa5);
    for (auto byte : a) REQUIRE(byte == 0xa5);
    resource.ReuseBufferGPUVA.BaseAddress.UMD.Offset = 65536;
    before_pair = allocations;
    REQUIRE(paired() == DXGI_ERROR_UNSUPPORTED && allocations == before_pair);
    resource.ReuseBufferGPUVA = {};
    resource.Width = desc.ByteSize + 1;
    REQUIRE(paired() == DXGI_ERROR_UNSUPPORTED && allocations == before_pair);
    resource.Width = 4096;
    REQUIRE(allocate(hb) == S_OK);
    auto *token = native_heap_find(ctx, hb.pDrvPrivate);
    const unsigned allocation_count = allocations;
    mwd_allocation imported{}, internal{};
    mwd_context_info shared{};
    REQUIRE(native_runtime_context(ctx, &shared) == S_OK && shared.context_id == 83 && shared.queue_id == 1);
    REQUIRE(native_runtime_retain(ctx, token, &imported) == S_OK && allocations == allocation_count);
    REQUIRE(imported.handle == token->allocation && imported.address == token->address);
    REQUIRE(native_runtime_allocate(ctx, 4096, 4096, imported.address, 6, &internal) == E_OUTOFMEMORY);
    REQUIRE(native_runtime_allocate(ctx, 4096, 4096, 0, 6, &internal) == S_OK);
    REQUIRE(internal.address + internal.size <= imported.address || imported.address + imported.size <= internal.address);
    REQUIRE(native_runtime_retain(ctx, reinterpret_cast<void *>(uintptr_t(0x123)), &internal) == E_INVALIDARG);
    // Re-obtain the internal token whose output was deliberately cleared by the invalid retain.
    for (auto *p = ctx->native_heaps; p; p = p->next) if (p != token) native_export(ctx, p, &internal);
    destroy(hb);
    REQUIRE(native_token(ctx, token) && kernel_heaps.count(imported.handle));
    unsigned map_count = locks, unmap_count = unlocks;
    uint32_t mapped_handle = 0;
    REQUIRE(native_runtime_map(ctx, token, &mapping, &mapped_handle) == S_OK && mapped_handle == imported.handle);
    REQUIRE(native_runtime_map(ctx, token, &again, &mapped_handle) == S_OK && locks == map_count + 1);
    REQUIRE(native_runtime_unmap(ctx, token) == S_OK && unlocks == unmap_count);
    REQUIRE(native_runtime_unmap(ctx, token) == S_OK && unlocks == unmap_count + 1);
    std::array<unsigned char, 16> stream{};
    mwd_reference reference{token, 0, 4096, 3, 0};
    REQUIRE(native_runtime_submit(ctx, stream.data(), stream.size(), &reference, 1) == S_OK && renders == 1);
    REQUIRE(ctx->native_commands == commands[1].data() && ctx->native_allocation_list == allocation_lists[1].data());
    reference.offset = imported.size;
    REQUIRE(native_runtime_submit(ctx, stream.data(), stream.size(), &reference, 1) == E_INVALIDARG && renders == 1);
    reference.offset = 0; render_result = E_OUTOFMEMORY;
    ctx->native_commands = commands[0].data();
    REQUIRE(native_runtime_submit(ctx, stream.data(), stream.size(), &reference, 1) == E_OUTOFMEMORY);
    REQUIRE(ctx->native_commands == commands[1].data()); render_result = S_OK;
    uint32_t completed = 0;
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && completed == 9);
    fail_deallocate = 1;
    REQUIRE(native_runtime_release(ctx, token) == DXGI_ERROR_WAS_STILL_DRAWING && native_token(ctx, token));
    REQUIRE(native_runtime_release(ctx, token) == S_OK && !native_token(ctx, token));
    reference.token = internal.token;
    retire_render = true; error_device = create.hDrvDevice;
    REQUIRE(native_runtime_submit(ctx, stream.data(), stream.size(), &reference, 1) == DXGI_ERROR_DEVICE_REMOVED);
    REQUIRE(!context(create.hDrvDevice) && kernel_heaps.size() == 1);
    kernel_heaps.clear(); heap_callbacks_retired = retire_render = false;
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    std::printf("PASS native buffer heaps/shared runtime allocation tokens/RenderCb replacements/reset/reentrant teardown (%zu-bit); DDI versions remain unsupported\n", sizeof(void *) * 8);
    return 0;
}

int main() {
    REQUIRE(OpenAdapter12(nullptr) == E_INVALIDARG);
    D3DDDI_ADAPTERCALLBACKS callbacks{};
    callbacks.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS funcs{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter;
    open.pAdapterCallbacks = &callbacks; open.pAdapterFuncs = &funcs;
    old_reply = true;
    REQUIRE(OpenAdapter12(&open) == DXGI_ERROR_UNSUPPORTED && !open.hAdapter.pDrvPrivate);
    old_reply = false; query_result = E_ACCESSDENIED;
    REQUIRE(OpenAdapter12(&open) == E_ACCESSDENIED && !open.hAdapter.pDrvPrivate);
    query_result = S_OK;
    REQUIRE(OpenAdapter12(&open) == S_OK && open.hAdapter.pDrvPrivate && !loads);
    callbacks.pfnQueryAdapterInfoCb = nullptr; // Must retain the original callback.
    UINT count = 123;
    std::array<UINT64, 2> versions{0x1234, 0x5678};
    REQUIRE(funcs.pfnGetSupportedVersions(open.hAdapter, &count, versions.data()) == S_OK && count == 0);
    REQUIRE(versions[0] == 0x1234 && versions[1] == 0x5678);
    REQUIRE(funcs.pfnGetOptionalDDITables(open.hAdapter, &count, nullptr) == S_OK && count == 0);
    D3D12DDI_3DPIPELINELEVEL level = D3D12DDI_3DPIPELINELEVEL_12_2;
    D3D12DDIARG_GETCAPS caps{D3D12DDICAPS_TYPE_3DPIPELINESUPPORT, nullptr, &level, sizeof(level)};
    REQUIRE(funcs.pfnGetCaps(open.hAdapter, &caps) == DXGI_ERROR_UNSUPPORTED);
    REQUIRE(level == D3D12DDI_3DPIPELINELEVEL_12_2);
    std::array<unsigned char, sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0003)> table;
    table.fill(0xa5);
    REQUIRE(funcs.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
        table.data(), table.size(), 0, {}) == DXGI_ERROR_UNSUPPORTED);
    for (auto byte : table) REQUIRE(byte == 0xa5);
    REQUIRE(funcs.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
        table.data(), table.size() - 1, 0, {}) == E_INVALIDARG);
    D3D12DDIARG_CALCPRIVATEDEVICESIZE size{D3D12DDI_INTERFACE_VERSION_R0, D3D12DDI_BUILD_VERSION << 16, D3D12DDI_CREATE_DEVICE_FLAG_NONE};
    REQUIRE(funcs.pfnCalcPrivateDeviceSize(open.hAdapter, &size) == sizeof(NativeDevice));
    ++size.Interface;
    REQUIRE(funcs.pfnCalcPrivateDeviceSize(open.hAdapter, &size) == 0);
    --size.Interface;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice) + 16> storage;
    storage.fill(0xa5);
    D3DDDI_DEVICECALLBACKS kt{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
    um.pfnSetErrorCb = test_error;
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hRTDevice.handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x456a0));
    create.Interface = size.Interface; create.Version = size.Version;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    create.hDrvDevice.pDrvPrivate = storage.data();
    ++generation;
    REQUIRE(funcs.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_DEVICE_REMOVED && !loads);
    --generation; bad_loader = true;
    REQUIRE(funcs.pfnCreateDevice(open.hAdapter, &create) == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
    bad_loader = false; backend_result = E_OUTOFMEMORY;
    REQUIRE(funcs.pfnCreateDevice(open.hAdapter, &create) == E_OUTOFMEMORY && unloads == 1 && destroys == 0);
    for (auto byte : storage) REQUIRE(byte == 0xa5);
    backend_result = S_OK;
    REQUIRE(funcs.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    for (size_t i = sizeof(NativeDevice); i < storage.size(); ++i) REQUIRE(storage[i] == 0xa5);
    auto *ctx = context(create.hDrvDevice);
    REQUIRE(ctx && ctx->backend && ctx->runtime_device.handle == create.hRTDevice.handle);
    um.pfnSetErrorCb = nullptr;
    error(ctx, E_INVALIDARG);
    REQUIRE(error_calls == 1 && last_runtime.handle == create.hRTDevice.handle);
    std::weak_ptr<NativeAdapter> retained = ctx->native_adapter;
    REQUIRE(funcs.pfnCloseAdapter(open.hAdapter) == S_OK && !retained.expired());
    REQUIRE(funcs.pfnCloseAdapter(open.hAdapter) == E_INVALIDARG);
    // Model an existing object's Context reference after runtime slot retirement.
    ++ctx->references;
    funcs.pfnDestroyDevice(create.hDrvDevice);
    REQUIRE(!context(create.hDrvDevice) && destroys == 0 && unloads == 1);
    error(ctx, E_INVALIDARG);
    REQUIRE(error_calls == 1 && !ctx->runtime_device.handle && !ctx->kernel_callbacks.pfnAllocateCb);
    release(ctx);
    REQUIRE(destroys == 1 && unloads == 2 && retained.expired());
    funcs.pfnDestroyDevice(create.hDrvDevice); // Runtime memory is never deleted.
    for (size_t i = sizeof(NativeDevice); i < storage.size(); ++i) REQUIRE(storage[i] == 0xa5);

    // A reset during backend creation must clean up without publishing a slot.
    loads = unloads = creates = destroys = 0;
    callbacks.pfnQueryAdapterInfoCb = test_query;
    um.pfnSetErrorCb = test_error;
    storage.fill(0xa5);
    REQUIRE(OpenAdapter12(&open) == S_OK);
    reset_during_create = true;
    REQUIRE(funcs.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_DEVICE_REMOVED);
    REQUIRE(creates == 1 && destroys == 1 && unloads == 1);
    for (auto byte : storage) REQUIRE(byte == 0xa5);
    reset_during_create = false;
    REQUIRE(funcs.pfnCloseAdapter(open.hAdapter) == S_OK);
    REQUIRE(OpenAdapter12(&open) == S_OK);
    REQUIRE(funcs.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    ctx = context(create.hDrvDevice);
    retained = ctx->native_adapter;
    REQUIRE(funcs.pfnCloseAdapter(open.hAdapter) == S_OK);
    // The runtime can tear down synchronously from SetErrorCb. This must not
    // deadlock, unlock freed mutex storage, or unload before callback return.
    retire_in_error = funcs.pfnDestroyDevice;
    error_device = create.hDrvDevice;
    error(ctx, E_INVALIDARG);
    REQUIRE(!context(create.hDrvDevice) && retained.expired());
    REQUIRE(destroys == 2 && unloads == 2);
    retire_in_error = nullptr;
    REQUIRE(test_native_heaps() == 0);
    std::printf("PASS native OpenAdapter12 WDK identity/negotiation/private memory/callback/lifetime/error cleanup (%zu-bit); no system-runtime or GPU acceptance\n", sizeof(void *) * 8);
    return 0;
}
