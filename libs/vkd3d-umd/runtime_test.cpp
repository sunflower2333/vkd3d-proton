/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* WDK lifecycle fixture with controlled Vulkan/backend peers, NOT system
 * D3D12CreateDevice or VIOGPU GPU acceptance. Uses actual production entry. */
#include "ddi.h"
#include "mesa_wddm_runtime.h"
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
#include <functional>

[[noreturn]] static void fixture_abort(int line) {
    std::fprintf(stderr, "FAIL fixture invariant at runtime_test.cpp:%d\n", line);
    std::exit(1);
}

static HRESULT backend_result = S_OK, query_result = S_OK;
static unsigned loads, unloads, creates, destroys, query_calls, error_calls;
static bool bad_loader = false, old_reply = false;
static bool reset_during_create = false;
static PFND3D12DDI_DESTROYDEVICE retire_in_error = nullptr;
static D3D12DDI_HDEVICE error_device{};
static std::function<void()> nested_error_callback;
static std::function<void(const mwd_callbacks *, void *)> backend_destructor_check;
static bool force_deferred_cleanup;
static bool force_device_command_error;
static bool force_drop_global_barrier;
static bool force_drop_indirect_count;
static unsigned signature_creates, indirect_calls;
static unsigned query_peers;
static unsigned query_execution_calls;
static uint32_t recorded_query_type, recorded_query_first, recorded_query_count;
static uint64_t recorded_query_offset;
static int recorded_query_operation;
static HRESULT query_execution_result = S_OK;
static std::function<void()> query_execution_callback;
static bool force_query_swap_fields;
static uint32_t recorded_indirect_stride, recorded_indirect_maximum;
static uint64_t recorded_argument_offset, recorded_count_offset;
static vkdu_object *recorded_arguments, *recorded_count, *recorded_signature;
static HRESULT signature_result = S_OK, indirect_result = S_OK;
static std::function<void()> backend_signature_create_callback, backend_indirect_callback;
static unsigned barrier_calls;
static HRESULT barrier_result = S_OK;
static std::vector<vkdu_resource_barrier> recorded_barriers;
static std::vector<std::pair<HANDLE, HRESULT>> command_errors;
static std::function<void()> nested_command_error, backend_command_create_callback, backend_command_destroy_callback;
static unsigned command_creates, command_destroys;
static HRESULT command_create_result = S_OK, command_close_result = S_OK;
static unsigned queue_creates, queue_destroys, queue_executes;
static HRESULT queue_create_result = S_OK, queue_execute_result = S_OK;
static std::function<void()> backend_queue_create_callback, backend_queue_destroy_callback, backend_queue_execute_callback;
static std::set<vkdu_object *> executing_objects;
static std::vector<vkdu_object *> submitted_commands;
static bool force_queue_unowned, queue_negative_active;
static bool force_early_completion, completion_negative_active;
static bool native_fence_ignore_owner, native_fence_drop_mask;
static uint64_t generation = 7;
static D3D12DDI_HRTDEVICE last_runtime{};
static const HANDLE expected_adapter = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x13570));
static const std::array<uint8_t, 8> expected_luid{1,2,3,4,5,6,7,8};
static void wait_backend_worker(const mwd_callbacks *callbacks, void *owner, bool require_live = false) {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) fixture_abort(__LINE__);
    std::thread worker([=]() {
        const auto status = callbacks->status(owner);
        uint32_t fence = 0;
        const auto completed = callbacks->completed(owner, &fence);
        if (require_live && (FAILED(status) || FAILED(completed))) {
            std::fprintf(stderr, "FAIL ordinary backend completion after premature callback retirement: status=%08x completed=%08x fence=%u\n",
                static_cast<unsigned>(status), static_cast<unsigned>(completed), fence);
            std::exit(1);
        }
        SetEvent(done);
    });
    if (WaitForSingleObject(done, 2000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "FAIL backend worker blocked by native callback lock\n");
        fixture_abort(__LINE__);
    }
    worker.join(); CloseHandle(done);
}
struct TestDevice { const mwd_callbacks *callbacks; void *owner; };
static HMODULE WINAPI test_load(LPCWSTR name, HANDLE, DWORD flags) {
    if (std::wcscmp(name, L"vulkan-1.dll") || flags != LOAD_LIBRARY_SEARCH_SYSTEM32) fixture_abort(__LINE__);
    ++loads;
    if (bad_loader) { SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    return reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x24680));
}
static FARPROC WINAPI test_symbol(HMODULE, LPCSTR name) {
    if (std::strcmp(name, "vkGetInstanceProcAddr")) fixture_abort(__LINE__);
    return reinterpret_cast<FARPROC>(static_cast<uintptr_t>(0x35790));
}
static BOOL WINAPI test_unload(HMODULE) { ++unloads; return TRUE; }
static int32_t test_create(PFN_vkGetInstanceProcAddr loader, const uint8_t luid[8],
        const mwd_callbacks *callbacks, void *owner, vkdu_device **out) {
    if (!loader || std::memcmp(luid, expected_luid.data(), 8) || !owner || !mwd_callbacks_valid(callbacks)) fixture_abort(__LINE__);
    ++creates; *out = nullptr;
    if (SUCCEEDED(backend_result)) *out = reinterpret_cast<vkdu_device *>(new TestDevice{callbacks, owner});
    if (reset_during_create) ++generation;
    return backend_result;
}
static void test_destroy(vkdu_device *device) {
    if (device) {
        auto *peer = reinterpret_cast<TestDevice *>(device);
        if (backend_destructor_check) backend_destructor_check(peer->callbacks, peer->owner);
        else wait_backend_worker(peer->callbacks, peer->owner);
        ++destroys; delete peer;
    }
}
static unsigned backend_device_removals;
static int32_t backend_removal_reason;
static int32_t test_device_remove(vkdu_device *, int32_t reason) {
    ++backend_device_removals; backend_removal_reason = reason; return reason;
}
static HRESULT APIENTRY test_query(HANDLE runtime, const D3DDDICB_QUERYADAPTERINFO *args) {
    if (runtime != expected_adapter || args->PrivateDriverDataSize != 160) fixture_abort(__LINE__);
    auto *bytes = static_cast<uint8_t *>(args->pPrivateDriverData);
    for (unsigned i = 0; i < 160; ++i) if (bytes[i]) fixture_abort(__LINE__);
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
    if (SUCCEEDED(result)) fixture_abort(__LINE__);
    ++error_calls; last_runtime = runtime;
    if (retire_in_error) retire_in_error(error_device);
    if (nested_error_callback) nested_error_callback();
}
static void APIENTRY test_command_error(D3D12DDI_HRTCOMMANDLIST runtime, HRESULT result) {
    if (!runtime.handle || SUCCEEDED(result)) fixture_abort(__LINE__);
    command_errors.emplace_back(runtime.handle, result);
    if (nested_command_error) nested_command_error();
}

static int32_t test_import_heap(vkdu_device *, void *, void *, uint64_t, int, vkdu_object **);
static int32_t test_place_buffer(vkdu_device *, vkdu_object *, uint64_t, uint64_t, uint32_t, vkdu_object **);
static int32_t test_texture_allocation(vkdu_device *, uint32_t, uint32_t, uint32_t, uint64_t *, uint64_t *);
static int32_t test_buffer_allocation(vkdu_device *, uint64_t, uint64_t *, uint64_t *);
static int32_t test_texture_import(vkdu_device *, void *, void *, uint64_t, vkdu_object **);
static int32_t test_texture_place(vkdu_device *, vkdu_object *, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, vkdu_object **);
static int32_t test_rt_allocation(vkdu_device *, uint32_t, uint32_t, uint64_t *, uint64_t *);
static int32_t test_rt_import(vkdu_device *, void *, void *, uint64_t, vkdu_object **);
static int32_t test_rt_place(vkdu_device *, vkdu_object *, uint32_t, uint32_t, uint32_t, vkdu_object **);
static void test_object_destroy(vkdu_object *);
static int test_object_retain(vkdu_object *);
static int test_object_is(vkdu_object *, vkdu_kind);
static int test_object_belongs(vkdu_device *, vkdu_object *);
static uint64_t test_buffer_address(vkdu_object *);
static uint64_t test_buffer_size(vkdu_object *);
static int32_t test_allocator_create(vkdu_device *, uint32_t, vkdu_object **);
static int32_t test_command_create(vkdu_device *, vkdu_object *, uint32_t, vkdu_object **);
static int32_t test_command_close(vkdu_object *);
static int32_t test_command_barriers(vkdu_object *, uint32_t, const vkdu_resource_barrier *);
static int32_t test_queue_create(vkdu_device *, uint32_t, vkdu_object **);
static int32_t test_queue_bind(vkdu_object *, void *, void *);
static int32_t test_queue_execute(vkdu_object *, uint32_t, vkdu_object *const *);
static int32_t test_signature_create(vkdu_device *, uint32_t, vkdu_object **);
static int32_t test_query_heap_create(vkdu_device *, uint32_t, uint32_t, vkdu_object **);
static int32_t test_query_execution(vkdu_object *, vkdu_object *, uint32_t, uint32_t, int);
static int32_t test_query_resolve(vkdu_object *, vkdu_object *, uint32_t, uint32_t, uint32_t, vkdu_object *, uint64_t);
static int32_t test_pageable_backing(vkdu_object *, uint32_t *, mwd_allocation *);
static int32_t test_execute_indirect(vkdu_object *, vkdu_object *, uint32_t, vkdu_object *, uint64_t, vkdu_object *, uint64_t);
static DWORD WINAPI test_event_wait(HANDLE event, DWORD timeout) {
    if (force_early_completion && completion_negative_active && timeout == 0) return WAIT_OBJECT_0;
    return WaitForSingleObject(event, timeout);
}

#define LoadLibraryExW test_load
#define GetProcAddress test_symbol
#define FreeLibrary test_unload
#define vkdu_device_create_shared test_create
#define vkdu_device_destroy test_destroy
#define vkdu_device_remove test_device_remove
#define vkdu_memory_heap_import test_import_heap
#define vkdu_buffer_place test_place_buffer
#define vkdu_texture2d_allocation test_texture_allocation
#define vkdu_buffer_allocation test_buffer_allocation
#define vkdu_texture_heap_import test_texture_import
#define vkdu_texture2d_place test_texture_place
#define vkdu_render_target_allocation test_rt_allocation
#define vkdu_render_target_heap_import test_rt_import
#define vkdu_render_target_place test_rt_place
#define vkdu_object_destroy test_object_destroy
#define vkdu_object_retain test_object_retain
#define vkdu_object_is test_object_is
#define vkdu_object_belongs test_object_belongs
#define vkdu_buffer_address test_buffer_address
#define vkdu_buffer_size test_buffer_size
#define vkdu_allocator_create test_allocator_create
#define vkdu_command_create test_command_create
#define vkdu_command_close test_command_close
#define vkdu_command_barriers test_command_barriers
#define vkdu_queue_create test_queue_create
#define vkdu_queue_bind_runtime test_queue_bind
#define vkdu_queue_execute test_queue_execute
#define vkdu_dispatch_signature_create test_signature_create
#define vkdu_query_heap_create test_query_heap_create
#define vkdu_command_query test_query_execution
#define vkdu_command_query_resolve test_query_resolve
#define vkdu_pageable_backing test_pageable_backing
#define vkdu_command_execute_indirect test_execute_indirect
#define WaitForSingleObject test_event_wait
#define VKDU_TEST_NATIVE_FENCE_CONTROLS
#define VKDU_TEST_NATIVE_RESIDENCY_CONTROLS
#include "ddi.cpp"
#undef WaitForSingleObject

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
    unsigned references = 1;
    uint32_t width = 0, height = 0, format = 0;
    void *runtime_route = nullptr;
};
static std::function<void()> texture_allocation_callback;
static unsigned allocation_queries;
static HRESULT allocation_query_result = S_OK;
static uint64_t texture_required_bytes = 65536;
static int32_t test_buffer_allocation(vkdu_device *device, uint64_t width, uint64_t *bytes, uint64_t *alignment) {
    auto *peer = reinterpret_cast<TestDevice *>(device);
    ++allocation_queries; wait_backend_worker(peer->callbacks, peer->owner);
    *bytes = (width + 65535) & ~uint64_t(65535); *alignment = 65536;
    if (texture_allocation_callback) texture_allocation_callback();
    return allocation_query_result;
}
static int32_t test_texture_allocation(vkdu_device *device, uint32_t width, uint32_t height,
        uint32_t format, uint64_t *bytes, uint64_t *alignment) {
    auto *peer = reinterpret_cast<TestDevice *>(device);
    ++allocation_queries; wait_backend_worker(peer->callbacks, peer->owner);
    if (!width || !height || (format != 28 && format != 41)) fixture_abort(__LINE__);
    *bytes = texture_required_bytes; *alignment = 65536;
    if (texture_allocation_callback) texture_allocation_callback();
    return allocation_query_result;
}
static int32_t test_texture_import(vkdu_device *device, void *owner, void *token, uint64_t bytes, vkdu_object **out) {
    return test_import_heap(device, owner, token, bytes, 0, out);
}
static int32_t test_texture_place(vkdu_device *device, vkdu_object *memory, uint64_t offset,
        uint32_t width, uint32_t height, uint32_t format, uint32_t state, vkdu_object **out) {
    HRESULT hr = test_place_buffer(device, memory, offset, texture_required_bytes, state, out);
    if (SUCCEEDED(hr)) {
        auto *peer = reinterpret_cast<ImportPeer *>(*out);
        peer->kind = VKDU_TEXTURE2D; peer->width = width; peer->height = height; peer->format = format;
    }
    return hr;
}
static bool import_failure, placement_failure, retire_during_import;
static unsigned rt_queries, rt_imports, rt_placements;
static int32_t test_rt_allocation(vkdu_device *device, uint32_t width, uint32_t height, uint64_t *bytes, uint64_t *alignment) {
    ++rt_queries; return test_texture_allocation(device, width, height, 28, bytes, alignment);
}
static int32_t test_rt_import(vkdu_device *device, void *owner, void *token, uint64_t bytes, vkdu_object **out) {
    ++rt_imports; return test_texture_import(device, owner, token, bytes, out);
}
static int32_t test_rt_place(vkdu_device *device, vkdu_object *memory, uint32_t width, uint32_t height, uint32_t state, vkdu_object **out) {
    ++rt_placements; return test_texture_place(device, memory, 0, width, height, 28, state, out);
}
static unsigned imports, placements;
static bool last_import_cpu_visible;
static uint32_t last_placement_state;
static void retire_import_with_map(Context *, const mwd_allocation &);
static int32_t test_import_heap(vkdu_device *device, void *owner, void *token, uint64_t size, int cpu_visible, vkdu_object **out) {
    last_import_cpu_visible = cpu_visible != 0;
    *out = nullptr; ++imports;
    wait_backend_worker(&native_runtime_callbacks, owner);
    if (import_failure) return E_OUTOFMEMORY;
    mwd_allocation allocation{};
    HRESULT hr = native_runtime_retain(owner, token, &allocation);
    if (FAILED(hr)) return hr;
    if (allocation.size != size) fixture_abort(__LINE__);
    auto backing = std::make_shared<ImportOwner>();
    backing->context = static_cast<Context *>(owner); backing->allocation = allocation;
    *out = reinterpret_cast<vkdu_object *>(new ImportPeer{backing, device, VKDU_MEMORY_HEAP, allocation.address, size});
    if (retire_during_import) retire_import_with_map(backing->context, allocation);
    return S_OK;
}
static int32_t test_place_buffer(vkdu_device *device, vkdu_object *memory, uint64_t offset, uint64_t size, uint32_t state, vkdu_object **out) {
    last_placement_state = state;
    *out = nullptr; ++placements;
    if (placement_failure) return E_INVALIDARG;
    auto *peer = reinterpret_cast<ImportPeer *>(memory);
    if (FAILED(native_runtime_status(peer->owner->context))) return DXGI_ERROR_DEVICE_REMOVED;
    if (peer->device != device || peer->kind != VKDU_MEMORY_HEAP || offset || size > peer->bytes) fixture_abort(__LINE__);
    *out = reinterpret_cast<vkdu_object *>(new ImportPeer{peer->owner, device, VKDU_BUFFER, peer->address, size});
    return S_OK;
}
static void test_object_destroy(vkdu_object *object) {
    auto *peer = reinterpret_cast<ImportPeer *>(object);
    if (peer && --peer->references) return;
    if (executing_objects.count(object)) {
        std::fputs("FAIL native queue execution released a submitted backend owner\n", stderr);
        std::exit(1);
    }
    if (peer && peer->owner) wait_backend_worker(&native_runtime_callbacks, peer->owner->context);
    if (peer && peer->kind == VKDU_COMMAND_LIST) {
        ++command_destroys;
        if (backend_command_destroy_callback) backend_command_destroy_callback();
        auto *device = reinterpret_cast<TestDevice *>(peer->device);
        wait_backend_worker(device->callbacks, device->owner);
    }
    if (peer && peer->kind == VKDU_QUEUE) {
        ++queue_destroys;
        if (backend_queue_destroy_callback) backend_queue_destroy_callback();
        auto *device = reinterpret_cast<TestDevice *>(peer->device);
        wait_backend_worker(device->callbacks, device->owner);
        if (peer->runtime_route && FAILED(device->callbacks->queue_release(device->owner, peer->runtime_route)))
            fixture_abort(__LINE__);
    }
    if (peer && peer->kind == VKDU_QUERY_HEAP) --query_peers;
    delete peer;
}
static int test_object_retain(vkdu_object *object) {
    if (!object) return 0;
    if (!queue_negative_active) ++reinterpret_cast<ImportPeer *>(object)->references;
    return 1;
}
static int32_t test_signature_create(vkdu_device *device, uint32_t stride, vkdu_object **out) {
    *out = nullptr; ++signature_creates;
    auto *peer = reinterpret_cast<TestDevice *>(device);
    wait_backend_worker(peer->callbacks, peer->owner);
    if (FAILED(signature_result)) return signature_result;
    *out = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, device, VKDU_COMMAND_SIGNATURE, 0, stride});
    recorded_indirect_stride = stride;
    if (backend_signature_create_callback) backend_signature_create_callback();
    return S_OK;
}
static int32_t test_execute_indirect(vkdu_object *command, vkdu_object *signature, uint32_t maximum,
        vkdu_object *arguments, uint64_t argument_offset, vkdu_object *count, uint64_t count_offset) {
    ++indirect_calls;
    if (!test_object_is(command, VKDU_COMMAND_LIST) || !test_object_is(signature, VKDU_COMMAND_SIGNATURE) ||
        !test_object_is(arguments, VKDU_BUFFER) || (count && !test_object_is(count, VKDU_BUFFER))) fixture_abort(__LINE__);
    recorded_indirect_maximum = maximum; recorded_signature = signature;
    recorded_arguments = arguments; recorded_count = force_drop_indirect_count ? nullptr : count;
    recorded_argument_offset = argument_offset; recorded_count_offset = count_offset;
    for (auto *owner : {command, signature, arguments, count})
        if (owner && reinterpret_cast<ImportPeer *>(owner)->references < 2) fixture_abort(__LINE__);
    auto *device = reinterpret_cast<TestDevice *>(reinterpret_cast<ImportPeer *>(command)->device);
    wait_backend_worker(device->callbacks, device->owner);
    if (backend_indirect_callback) backend_indirect_callback();
    for (auto *owner : {command, signature, arguments, count})
        if (owner && !reinterpret_cast<ImportPeer *>(owner)->references) fixture_abort(__LINE__);
    return indirect_result;
}
static int test_object_is(vkdu_object *object, vkdu_kind kind) {
    return object && reinterpret_cast<ImportPeer *>(object)->kind == kind;
}
static int test_object_belongs(vkdu_device *device, vkdu_object *object) {
    return object && reinterpret_cast<ImportPeer *>(object)->device == device;
}
static uint64_t test_buffer_address(vkdu_object *object) { return object ? reinterpret_cast<ImportPeer *>(object)->address : 0; }
static uint64_t test_buffer_size(vkdu_object *object) { return object ? reinterpret_cast<ImportPeer *>(object)->bytes : 0; }
static std::map<vkdu_object *, std::vector<void *>> pageable_tokens;
static std::function<void()> pageable_callback, query_create_callback;
static HRESULT pageable_result = S_OK, query_heap_result = S_OK;
static bool corrupt_pageable_reply;
static int32_t test_query_heap_create(vkdu_device *device, uint32_t type, uint32_t count, vkdu_object **out) {
    *out = nullptr;
    if (type > 3 || !count) return E_INVALIDARG;
    if (FAILED(query_heap_result)) return query_heap_result;
    *out = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, device, VKDU_QUERY_HEAP, type, count});
    ++query_peers;
    if (query_create_callback) query_create_callback();
    return S_OK;
}
static int32_t test_query_record(vkdu_object *command, vkdu_object *heap, uint32_t type,
        uint32_t first, uint32_t count, vkdu_object *destination, uint64_t offset, int operation) {
    ++query_execution_calls;
    if (!test_object_is(command, VKDU_COMMAND_LIST) || !test_object_is(heap, VKDU_QUERY_HEAP) ||
            (destination && !test_object_is(destination, VKDU_BUFFER))) fixture_abort(__LINE__);
    for (auto *owner : {command, heap, destination})
        if (owner && reinterpret_cast<ImportPeer *>(owner)->references < 2) fixture_abort(__LINE__);
    auto *device = reinterpret_cast<TestDevice *>(reinterpret_cast<ImportPeer *>(command)->device);
    wait_backend_worker(device->callbacks, device->owner);
    recorded_query_type = force_query_swap_fields ? first : type;
    recorded_query_first = force_query_swap_fields ? type : first;
    recorded_query_count = count; recorded_query_offset = offset; recorded_query_operation = operation;
    if (query_execution_callback) query_execution_callback();
    for (auto *owner : {command, heap, destination})
        if (owner && !reinterpret_cast<ImportPeer *>(owner)->references) fixture_abort(__LINE__);
    return query_execution_result;
}
static int32_t test_query_execution(vkdu_object *command, vkdu_object *heap, uint32_t type, uint32_t index, int begin) {
    return test_query_record(command, heap, type, index, 0, nullptr, 0, begin ? 0 : 1);
}
static int32_t test_query_resolve(vkdu_object *command, vkdu_object *heap, uint32_t type,
        uint32_t first, uint32_t count, vkdu_object *destination, uint64_t offset) {
    return test_query_record(command, heap, type, first, count, destination, offset, 2);
}
static int32_t test_pageable_backing(vkdu_object *object, uint32_t *count, mwd_allocation *out) {
    *count = 0;
    if (FAILED(pageable_result)) return pageable_result;
    auto *peer = reinterpret_cast<ImportPeer *>(object);
    if (peer->references < 2 || (peer->kind != VKDU_DESCRIPTOR_HEAP && peer->kind != VKDU_QUERY_HEAP))
        fixture_abort(__LINE__);
    auto *device = reinterpret_cast<TestDevice *>(peer->device);
    const auto tokens = pageable_tokens[object];
    if (tokens.size() > 3) fixture_abort(__LINE__);
    for (auto *token : tokens) {
        HRESULT hr = native_runtime_retain(device->owner, token, &out[*count]);
        if (FAILED(hr)) {
            while (*count) native_runtime_release(device->owner, out[--*count].token);
            return hr;
        }
        ++*count;
    }
    if (corrupt_pageable_reply && *count) ++out[0].handle;
    if (pageable_callback) pageable_callback();
    return S_OK;
}
static int32_t test_allocator_create(vkdu_device *device, uint32_t type, vkdu_object **out) {
    if (!device || type != 0 || !out) return E_INVALIDARG;
    *out = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, device, VKDU_ALLOCATOR, 0, 0});
    return S_OK;
}
static int32_t test_command_create(vkdu_device *device, vkdu_object *allocator, uint32_t type, vkdu_object **out) {
    *out = nullptr; ++command_creates;
    if (!test_object_is(allocator, VKDU_ALLOCATOR) || !test_object_belongs(device, allocator) || type != 0) return E_INVALIDARG;
    auto *peer = reinterpret_cast<TestDevice *>(device);
    wait_backend_worker(peer->callbacks, peer->owner);
    if (FAILED(command_create_result)) return command_create_result;
    *out = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, device, VKDU_COMMAND_LIST, 0, 0});
    if (backend_command_create_callback) backend_command_create_callback();
    return S_OK;
}
static int32_t test_command_close(vkdu_object *object) {
    return test_object_is(object, VKDU_COMMAND_LIST) ? command_close_result : E_INVALIDARG;
}
static int32_t test_command_barriers(vkdu_object *object, uint32_t count, const vkdu_resource_barrier *barriers) {
    if (!test_object_is(object, VKDU_COMMAND_LIST) || (count && !barriers)) fixture_abort(__LINE__);
    ++barrier_calls; recorded_barriers.clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (force_drop_global_barrier && barriers[i].type == VKDU_BARRIER_UAV && !barriers[i].resource) continue;
        recorded_barriers.push_back(barriers[i]);
    }
    return barrier_result;
}
static int32_t test_queue_create(vkdu_device *device, uint32_t type, vkdu_object **out) {
    *out = nullptr; ++queue_creates;
    if (type != 0 && type != 2 && type != 3) return E_INVALIDARG;
    auto *peer = reinterpret_cast<TestDevice *>(device);
    wait_backend_worker(peer->callbacks, peer->owner);
    if (FAILED(queue_create_result)) return queue_create_result;
    *out = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, device, VKDU_QUEUE, type, 0});
    if (backend_queue_create_callback) backend_queue_create_callback();
    return S_OK;
}
static int32_t test_queue_execute(vkdu_object *queue, uint32_t count, vkdu_object *const *commands) {
    if (!test_object_is(queue, VKDU_QUEUE) || !count || count > 64 || !commands) fixture_abort(__LINE__);
    ++queue_executes;
    submitted_commands.assign(commands, commands + count);
    auto *peer = reinterpret_cast<ImportPeer *>(queue);
    auto *device = reinterpret_cast<TestDevice *>(peer->device);
    executing_objects.insert(queue);
    for (auto *command : submitted_commands) {
        if (!test_object_is(command,VKDU_COMMAND_LIST) || !test_object_belongs(peer->device,command)) fixture_abort(__LINE__);
        executing_objects.insert(command);
    }
    wait_backend_worker(device->callbacks, device->owner);
    if (backend_queue_execute_callback) backend_queue_execute_callback();
    if (!test_object_is(queue,VKDU_QUEUE)) fixture_abort(__LINE__);
    for (auto *command : submitted_commands)
        if (!test_object_is(command,VKDU_COMMAND_LIST)) fixture_abort(__LINE__);
    executing_objects.clear();
    return queue_execute_result;
}
static int32_t test_queue_bind(vkdu_object *queue, void *owner, void *token) {
    auto *peer = reinterpret_cast<ImportPeer *>(queue);
    auto *device = reinterpret_cast<TestDevice *>(peer->device);
    if (device->owner != owner) return E_INVALIDARG;
    HRESULT hr = device->callbacks->queue_retain(owner, token);
    if (SUCCEEDED(hr)) peer->runtime_route = token;
    return hr;
}

struct KernelHeap { uint64_t address, bytes; bool locked; HANDLE resource; };
static std::map<D3DKMT_HANDLE, KernelHeap> kernel_heaps;
static std::set<HANDLE> kernel_resources;
static D3DKMT_HANDLE next_allocation = 101;
static unsigned heap_context_creates, heap_context_destroys, allocations, deallocations, locks, unlocks;
static unsigned fail_deallocate, fail_unlock;
static std::function<void()> nested_deallocate;
static unsigned fail_context_destroy;
static bool fail_context_create, invalid_context_info, retire_lock;
static bool fail_allocate, partial_allocate, partial_resource, null_map, reset_allocate, retire_allocate, rename_lock;
static bool heap_callbacks_retired;
// Same HRESULT_FROM_NT-style mapping used by the real KMT forwarding probe.
static constexpr HRESULT stale_fence_status = static_cast<HRESULT>(0xd00000a3u);
static std::array<unsigned char, 65536> mapped_heap{};
static const HANDLE expected_device = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x456a0));
static const HANDLE expected_context = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xabc50));
static HANDLE expected_render_context = expected_context;
static uintptr_t next_runtime_context = 0xddd00;
static std::map<HANDLE, HANDLE> runtime_contexts;
static unsigned runtime_context_creates, runtime_context_destroys;
static std::array<unsigned char, 65536> dma_commands[2];
static std::array<D3DDDI_ALLOCATIONLIST, 1024> allocation_lists[2];
static std::array<D3DDDI_PATCHLOCATIONLIST, 1024> patch_lists[2];
static unsigned renders;
static HRESULT render_result = S_OK;
static bool retire_render;
static bool defer_completion;
static HRESULT signal_result = S_OK;
static unsigned signals;
static std::vector<HANDLE> pending_events;
static std::function<void()> nested_render, nested_signal;
static void valid_kernel_callback(HANDLE runtime) {
    if (runtime != expected_device || heap_callbacks_retired) fixture_abort(__LINE__);
}
static HRESULT APIENTRY heap_context_create(HANDLE runtime, D3DDDICB_CREATECONTEXT *args) {
    valid_kernel_callback(runtime);
    auto *data = static_cast<NativeContextCreate *>(args->pPrivateDriverData);
    if (args->PrivateDriverDataSize != sizeof(*data) || !native_header_valid(data->header, sizeof(*data)) ||
            data->generation != generation || data->flags || data->reserved || args->NodeOrdinal || args->EngineAffinity != 1)
        fixture_abort(__LINE__);
    ++heap_context_creates; args->hContext = expected_context;
    args->pCommandBuffer = dma_commands[0].data(); args->CommandBufferSize = 65536;
    args->pAllocationList = allocation_lists[0].data(); args->AllocationListSize = 1024;
    args->pPatchLocationList = patch_lists[0].data(); args->PatchLocationListSize = 1024;
    return fail_context_create ? E_OUTOFMEMORY : S_OK;
}
static HRESULT APIENTRY heap_context_destroy(HANDLE runtime, const D3DDDICB_DESTROYCONTEXT *args) {
    valid_kernel_callback(runtime);
    if (args->hContext != expected_context || !kernel_heaps.empty() || !kernel_resources.empty()) fixture_abort(__LINE__);
    ++heap_context_destroys;
    if (fail_context_destroy) { --fail_context_destroy; return E_FAIL; }
    return S_OK;
}
static HRESULT APIENTRY queue_context_create(D3D12DDI_HRTCOMMANDQUEUE runtime, D3DDDICB_CREATECONTEXT *args) {
    auto *data = static_cast<NativeContextCreateShared *>(args->pPrivateDriverData);
    if (!runtime.handle || args->PrivateDriverDataSize != sizeof(*data) ||
            !native_header_valid(data->base.header, sizeof(*data)) || data->base.flags != 1 ||
            data->base.generation != generation || data->base.reserved || data->reserved ||
            data->allocation_context_id != 83 || args->NodeOrdinal || args->EngineAffinity != 1)
        fixture_abort(__LINE__);
    args->hContext = reinterpret_cast<HANDLE>(++next_runtime_context);
    runtime_contexts.emplace(args->hContext, runtime.handle);
    ++runtime_context_creates;
    args->pCommandBuffer = dma_commands[0].data(); args->CommandBufferSize = 65536;
    args->pAllocationList = allocation_lists[0].data(); args->AllocationListSize = 1024;
    args->pPatchLocationList = patch_lists[0].data(); args->PatchLocationListSize = 1024;
    return S_OK;
}
static HRESULT APIENTRY queue_context_destroy(D3D12DDI_HRTCOMMANDQUEUE runtime, const D3DDDICB_DESTROYCONTEXT *args) {
    auto found = runtime_contexts.find(args->hContext);
    if (found == runtime_contexts.end() || found->second != runtime.handle) fixture_abort(__LINE__);
    runtime_contexts.erase(found); ++runtime_context_destroys;
    return S_OK;
}
static HRESULT APIENTRY heap_escape(HANDLE adapter, const D3DDDICB_ESCAPE *args) {
    valid_kernel_callback(args->hDevice);
    if (args->PrivateDriverDataSize == sizeof(NativeFenceInfo)) {
        auto *fence = static_cast<NativeFenceInfo *>(args->pPrivateDriverData);
        if (adapter != expected_adapter || args->hContext != expected_context || fence->opcode != 2 ||
                fence->completed || fence->context_id) fixture_abort(__LINE__);
        // The real KMD returns STATUS_DEVICE_NOT_READY for a stale generation;
        // this well-formed query is not a corrupted callback contract.
        if (fence->expected_generation != generation) return stale_fence_status;
        fence->generation = generation; fence->context_id = 83; fence->completed = 9;
        return S_OK;
    }
    auto *info = static_cast<NativeContextInfo *>(args->pPrivateDriverData);
    if (adapter != expected_adapter || (args->hContext != expected_context && !runtime_contexts.count(args->hContext)) ||
            args->PrivateDriverDataSize != sizeof(*info) ||
            !native_header_valid(info->header, sizeof(*info)) || info->opcode != 1 || info->flags ||
            info->expected_generation != generation || info->va_start || info->va_size || info->generation ||
            info->context_id || info->queue_id) fixture_abort(__LINE__);
    info->va_start = 0x10001000; info->va_size = 0x80000; info->generation = generation;
    info->context_id = 83; info->queue_id = 1;
    if (invalid_context_info) info->va_size = UINT64_MAX;
    return S_OK;
}
static HRESULT APIENTRY heap_allocate(HANDLE runtime, D3DDDICB_ALLOCATE *args) {
    valid_kernel_callback(runtime);
    if (args->NumAllocations != 1 || !args->pAllocationInfo || args->pPrivateDriverData || args->PrivateDriverDataSize)
        fixture_abort(__LINE__);
    auto *data = static_cast<NativeAllocationInfo *>(args->pAllocationInfo->pPrivateDriverData);
    if (args->pAllocationInfo->PrivateDriverDataSize != sizeof(*data) || !native_header_valid(data->header, sizeof(*data)) ||
            data->alignment != 4096 || data->context_id != 83 || data->generation != generation ||
            (data->flags != 4 && data->flags != 6 && data->flags != 14) || data->format || data->width || data->height || data->pitch ||
            data->refresh_numerator || data->refresh_denominator || data->iova < 0x10001000 ||
            data->iova % 4096 || data->size % 4096 || data->iova + data->size > 0x10081000)
        fixture_abort(__LINE__);
    for (const auto &item : kernel_heaps) {
        const auto &heap = item.second;
        if (data->iova < heap.address + heap.bytes && heap.address < data->iova + data->size) fixture_abort(__LINE__);
    }
    ++allocations;
    if (!fail_allocate || partial_allocate) {
        args->pAllocationInfo->hAllocation = next_allocation++;
        kernel_heaps.emplace(args->pAllocationInfo->hAllocation, KernelHeap{data->iova, data->size, false, args->hResource});
        if (args->hResource) {
            if (!kernel_resources.insert(args->hResource).second) fixture_abort(__LINE__);
            args->hKMResource = 73;
        }
    }
    if (fail_allocate && partial_resource && args->hResource) {
        if (!kernel_resources.insert(args->hResource).second) fixture_abort(__LINE__);
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
        if (args->NumAllocations || args->HandleList || !kernel_resources.erase(args->hResource)) fixture_abort(__LINE__);
        for (auto found = kernel_heaps.begin(); found != kernel_heaps.end();) {
            if (found->second.resource == args->hResource) {
                if (found->second.locked) fixture_abort(__LINE__);
                found = kernel_heaps.erase(found);
            } else ++found;
        }
    } else {
        if (args->NumAllocations != 1 || !args->HandleList) fixture_abort(__LINE__);
        auto found = kernel_heaps.find(*args->HandleList);
        if (found == kernel_heaps.end() || found->second.locked || found->second.resource) fixture_abort(__LINE__);
        kernel_heaps.erase(found);
    }
    if (nested_deallocate) nested_deallocate();
    return S_OK;
}
static HRESULT APIENTRY heap_lock(HANDLE runtime, D3DDDICB_LOCK *args) {
    valid_kernel_callback(runtime);
    auto found = kernel_heaps.find(args->hAllocation);
    if (found == kernel_heaps.end() || found->second.locked || !args->Flags.LockEntire ||
            args->Flags.Discard || args->Flags.NoExistingReference || args->Flags.ReadOnly || args->NumPages || args->pPages)
        fixture_abort(__LINE__);
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
    if (args->NumAllocations != 1 || !args->phAllocations) fixture_abort(__LINE__);
    auto found = kernel_heaps.find(*args->phAllocations);
    if (found == kernel_heaps.end() || !found->second.locked) fixture_abort(__LINE__);
    ++unlocks;
    if (fail_unlock) { --fail_unlock; return E_FAIL; }
    found->second.locked = false;
    return S_OK;
}

static HRESULT APIENTRY heap_render(HANDLE runtime, D3DDDICB_RENDER *args) {
    valid_kernel_callback(runtime);
    if (args->hContext != expected_render_context || args->NumAllocations != 1 || args->NumPatchLocations != 1 ||
            args->CommandOffset || args->Flags.Value) fixture_abort(__LINE__);
    auto *header = static_cast<NativeRenderInfo *>(args->pNewCommandBuffer);
    auto *refs = reinterpret_cast<NativeRenderReference *>(static_cast<unsigned char *>(args->pNewCommandBuffer) + 64);
    if (!native_header_valid(header->header, args->CommandLength) || header->generation != generation ||
            header->stream_offset != 96 || header->references_count != 1 || refs->length != 4096 ||
            !kernel_heaps.count(args->pNewAllocationList[0].hAllocation) ||
            args->pNewPatchLocationList[0].PatchOffset != 96) fixture_abort(__LINE__);
    ++renders;
    args->pNewCommandBuffer = dma_commands[1].data(); args->NewCommandBufferSize = 65536;
    args->pNewAllocationList = allocation_lists[1].data(); args->NewAllocationListSize = 1024;
    args->pNewPatchLocationList = patch_lists[1].data(); args->NewPatchLocationListSize = 1024;
    if (nested_render) nested_render();
    if (retire_render) { native_destroy_device(error_device); heap_callbacks_retired = true; }
    return render_result;
}

static HRESULT APIENTRY heap_signal(HANDLE runtime, const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 *args) {
    valid_kernel_callback(runtime);
    if (args->hContext != expected_render_context || args->ObjectCount || args->BroadcastContextCount ||
            args->Flags.Value != 2 || !args->CpuEventHandle || !renders) fixture_abort(__LINE__);
    for (auto handle : args->ObjectHandleArray) if (handle) fixture_abort(__LINE__);
    for (auto handle : args->BroadcastContext) if (handle) fixture_abort(__LINE__);
    ++signals;
    if (nested_signal) nested_signal();
    if (FAILED(signal_result)) return signal_result;
    if (defer_completion) {
        HANDLE kernel_event = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), args->CpuEventHandle, GetCurrentProcess(),
                &kernel_event, 0, FALSE, DUPLICATE_SAME_ACCESS)) fixture_abort(__LINE__);
        pending_events.push_back(kernel_event);
    } else if (!SetEvent(args->CpuEventHandle)) fixture_abort(__LINE__);
    return S_OK;
}
static void complete_events(bool signal = true) {
    for (auto event : pending_events) {
        if (signal && !SetEvent(event)) fixture_abort(__LINE__);
        CloseHandle(event);
    }
    pending_events.clear();
}

static void retire_import_with_map(Context *ctx, const mwd_allocation &allocation) {
    void *mapped = nullptr;
    uint32_t handle = 0;
    if (FAILED(native_runtime_map(ctx, allocation.token, &mapped, &handle)) ||
            !mapped || !kernel_heaps.count(handle)) fixture_abort(__LINE__);
    // Observe mapped backing before retirement. The fake runtime does not
    // implement final KMT device teardown, so never access this pointer after
    // DestroyDevice returns or claim the runtime kept it alive for us.
    static_cast<unsigned char *>(mapped)[0] = 0x5a;
    const unsigned old_unlocks = unlocks, old_deallocations = deallocations;
    const unsigned old_context_destroys = heap_context_destroys;
    native_destroy_device(error_device);
    if (unlocks != old_unlocks || deallocations != old_deallocations ||
            heap_context_destroys != old_context_destroys ||
            !kernel_heaps.count(handle) || !kernel_heaps.at(handle).locked) fixture_abort(__LINE__);
    heap_callbacks_retired = true;
    mapped = nullptr;
    if (native_runtime_map(ctx, allocation.token, &mapped, &handle) != DXGI_ERROR_DEVICE_REMOVED || mapped)
        fixture_abort(__LINE__);
}

#include "runtime_textures_test.inc"

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
    kt.pfnSignalSynchronizationObject2Cb = heap_signal;
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
    uint32_t stale_completed = 123;
    REQUIRE(native_runtime_completed(context(create.hDrvDevice), &stale_completed) == DXGI_ERROR_DEVICE_REMOVED && !stale_completed);
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
    // R0 has no explicit UAV resource flag. Native default buffers retain
    // UAV-capable backend usage; READBACK still permits COPY_DEST only.
    resource.InitialResourceState = D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;
    before_pair = allocations;
    REQUIRE(table.pfnCalcPrivateHeapAndResourceSizes(create.hDrvDevice, &desc, &resource).Heap == 0);
    REQUIRE(paired() == DXGI_ERROR_UNSUPPORTED && allocations == before_pair);
    desc.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    sizes = table.pfnCalcPrivateHeapAndResourceSizes(create.hDrvDevice, &desc, &resource);
    REQUIRE(sizes.Heap == sizeof(NativeHeapSlot) && sizes.Resource == sizeof(Object));
    REQUIRE(paired() == S_OK && allocations == before_pair + 1 && !last_import_cpu_visible &&
        last_placement_state == D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
    REQUIRE(object(resource_handle.pDrvPrivate)->native_heap &&
        object(resource_handle.pDrvPrivate)->address == native_heap_find(ctx, ha.pDrvPrivate)->address);
    table.pfnDestroyHeapAndResource(create.hDrvDevice, ha, resource_handle);
    REQUIRE(kernel_heaps.empty() && !ctx->native_heaps);
    resource.Flags = D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS;
    before_pair = allocations;
    REQUIRE(paired() == DXGI_ERROR_UNSUPPORTED && allocations == before_pair);
    resource.Flags = D3D12DDI_RESOURCE_FLAG_0003_NONE;
    REQUIRE(test_native_textures(ctx, create.hDrvDevice, table) == 0);
    desc.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    resource.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_DEST;
    REQUIRE(paired() == S_OK);
    table.pfnDestroyHeapAndResource(create.hDrvDevice, ha, resource_handle);
    REQUIRE(kernel_heaps.empty() && !ctx->native_heaps);
    REQUIRE(paired() == S_OK);
    // Error callback retains an outer recursive lock while entering this DDI.
    nested_error_callback = [&]() { table.pfnDestroyHeapAndResource(create.hDrvDevice, ha, resource_handle); };
    error(ctx, E_FAIL);
    nested_error_callback = {};
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
    // A preceding4KiB allocation must not make the next BDA64KiB request inherit
    // page alignment. The real owner allocator selects the aligned IOVA; KMT
    // backing alignment remains4KiB and receives that exact requested address.
    mwd_allocation aligned{}, exact{};
    REQUIRE(native_runtime_allocate(ctx, 4096, 65536, 0, 6, &aligned) == S_OK);
    REQUIRE(aligned.address && !(aligned.address & 65535) && aligned.address != internal.address);
    REQUIRE(native_runtime_allocate(ctx, 4096, 65536, aligned.address + 4096, 6, &exact) == E_INVALIDARG);
    const auto aligned_address = aligned.address;
    REQUIRE(native_runtime_release(ctx, aligned.token) == S_OK);
    REQUIRE(native_runtime_allocate(ctx, 4096, 65536, aligned_address, 6, &exact) == S_OK && exact.address == aligned_address);
    REQUIRE(native_runtime_release(ctx, exact.token) == S_OK);
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
    REQUIRE(native_runtime_submit(ctx, 8, stream.data(), stream.size(), &reference, 1) == S_OK && renders == 1);
    REQUIRE(ctx->native_commands == dma_commands[1].data() && ctx->native_allocation_list == allocation_lists[1].data());
    reference.offset = imported.size;
    REQUIRE(native_runtime_submit(ctx, 9, stream.data(), stream.size(), &reference, 1) == E_INVALIDARG && renders == 1);
    reference.offset = 0; render_result = E_OUTOFMEMORY;
    ctx->native_commands = dma_commands[0].data();
    REQUIRE(native_runtime_submit(ctx, 9, stream.data(), stream.size(), &reference, 1) == E_OUTOFMEMORY);
    REQUIRE(ctx->native_commands == dma_commands[1].data()); render_result = S_OK;
    uint32_t completed = 0;
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && completed == 9);
    fail_deallocate = 1;
    REQUIRE(native_runtime_release(ctx, token) == DXGI_ERROR_WAS_STILL_DRAWING && native_token(ctx, token));
    REQUIRE(native_runtime_release(ctx, token) == S_OK && !native_token(ctx, token));
    reference.token = internal.token;
    retire_render = true; error_device = create.hDrvDevice;
    REQUIRE(native_runtime_submit(ctx, 10, stream.data(), stream.size(), &reference, 1) == DXGI_ERROR_DEVICE_REMOVED);
    REQUIRE(!context(create.hDrvDevice) && kernel_heaps.size() == 1);
    kernel_heaps.clear(); heap_callbacks_retired = retire_render = false;
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    REQUIRE(OpenAdapter12(&open) == S_OK);
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    error_device = create.hDrvDevice; retire_during_import = true;
    const unsigned placements_before_retire = placements;
    REQUIRE(paired() == DXGI_ERROR_DEVICE_REMOVED && !context(create.hDrvDevice));
    REQUIRE(placements == placements_before_retire && kernel_heaps.size() == 1);
    // Model runtime-owned final KMT teardown after the interrupted call unwinds.
    kernel_heaps.clear(); kernel_resources.clear();
    heap_callbacks_retired = retire_during_import = false;
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);

    // Ordinary last-owner teardown must keep read/cleanup callbacks legal until
    // the actual backend destructor releases its internal mapped allocation.
    REQUIRE(OpenAdapter12(&open) == S_OK);
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto *ordinary = context(create.hDrvDevice);
    mwd_allocation cleanup_allocation{};
    REQUIRE(native_runtime_allocate(ordinary, 4096, 4096, 0, 6, &cleanup_allocation) == S_OK);
    void *internal_map = nullptr; uint32_t internal_handle = 0;
    REQUIRE(native_runtime_map(ordinary, cleanup_allocation.token, &internal_map, &internal_handle) == S_OK);
    const unsigned destroys_before_cleanup = heap_context_destroys;
    bool destructor_checked = false;
    backend_destructor_check = [&](const mwd_callbacks *cb, void *owner) {
        wait_backend_worker(cb, owner, true);
        mwd_context_info info{}; mwd_allocation forbidden{};
        void *forbidden_map = nullptr; uint32_t forbidden_handle = 0;
        if (cb->context(owner, &info) != S_OK || info.context_id != 83 ||
                cb->allocate(owner, 4096, 4096, 0, 6, &forbidden) != DXGI_ERROR_DEVICE_REMOVED || forbidden.token ||
                cb->map(owner, cleanup_allocation.token, &forbidden_map, &forbidden_handle) != DXGI_ERROR_DEVICE_REMOVED || forbidden_map ||
                cb->submit(owner, 0, nullptr, 0, nullptr, 0) != DXGI_ERROR_DEVICE_REMOVED ||
                cb->unmap(owner, cleanup_allocation.token) != S_OK || cb->release(owner, cleanup_allocation.token) != S_OK ||
                !kernel_heaps.empty() || heap_context_destroys != destroys_before_cleanup) {
            std::fputs("FAIL ordinary backend cleanup ownership or new-work gate\n", stderr); std::exit(1);
        }
        destructor_checked = true;
    };
    // This negative control deliberately selects the real deferred lifetime:
    // the required ordinary callback checks must reject it after retirement.
    if (force_deferred_cleanup) ++ordinary->references;
    adapter.pfnDestroyDevice(create.hDrvDevice);
    if (force_deferred_cleanup) release(ordinary);
    backend_destructor_check = {};
    REQUIRE(destructor_checked && !context(create.hDrvDevice) &&
        heap_context_destroys == destroys_before_cleanup + 1 && kernel_heaps.empty());
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);

    // Recursive runtime retirement during that backend destructor invalidates
    // its private slot immediately. The outer call may only retain Context.
    REQUIRE(OpenAdapter12(&open) == S_OK);
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    ordinary = context(create.hDrvDevice);
    REQUIRE(native_runtime_allocate(ordinary, 4096, 4096, 0, 6, &cleanup_allocation) == S_OK);
    backend_destructor_check = [&](const mwd_callbacks *cb, void *owner) {
        wait_backend_worker(cb, owner, true);
        adapter.pfnDestroyDevice(create.hDrvDevice);
        heap_callbacks_retired = true;
        memory.fill(0xa5); // Runtime storage expires at the inner return.
        uint32_t retired_completed = 123;
        if (cb->status(owner) != DXGI_ERROR_DEVICE_REMOVED ||
                cb->completed(owner, &retired_completed) != DXGI_ERROR_DEVICE_REMOVED || retired_completed ||
                cb->release(owner, cleanup_allocation.token) != S_OK || kernel_heaps.size() != 1) {
            std::fputs("FAIL recursive backend retirement touched expired runtime\n", stderr); std::exit(1);
        }
    };
    adapter.pfnDestroyDevice(create.hDrvDevice);
    backend_destructor_check = {};
    REQUIRE(kernel_heaps.size() == 1);
    kernel_heaps.clear(); kernel_resources.clear(); heap_callbacks_retired = false;
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    std::puts("PASS ordinary backend completion/unmap/release before runtime retirement; recursive poisoned-slot retirement remains closed");
    std::printf("PASS native buffer heaps/shared runtime allocation tokens/RenderCb replacements/reset/reentrant teardown (%zu-bit); DDI versions remain unsupported\n", sizeof(void *) * 8);
    return 0;
}

static int test_native_command_errors() {
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
    um.pfnSetErrorCb = test_error; um.pfnSetCommandListErrorCb = test_command_error;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> device_memory{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = device_memory.data(); create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queue) == S_OK);
    alignas(Object) std::array<unsigned char, sizeof(Object)> allocator_memory{}, first{}, second{};
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator{};
    allocator.hDrvCommandAllocator.pDrvPrivate = allocator_memory.data();
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice, &allocator) == S_OK);
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 request{};
    request.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
    request.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; request.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    request.hDrvCommandList.pDrvPrivate = first.data(); first.fill(0xa5);
    const unsigned before_create = command_creates;
    REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &request) == E_INVALIDARG && command_creates == before_create);
    for (auto byte : first) REQUIRE(byte == 0xa5);
    const HANDLE first_runtime = reinterpret_cast<HANDLE>(uintptr_t(0x71820));
    const HANDLE second_runtime = reinterpret_cast<HANDLE>(uintptr_t(0x71930));
    request.hRTCommandList.handle = first_runtime;
    // Model a runtime missing this callback: native creation fails before the
    // backend or private command slot changes. No device-error fallback.
    ctx->runtime_callbacks.pfnSetCommandListErrorCb = nullptr;
    REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &request) == E_INVALIDARG && command_creates == before_create);
    ctx->runtime_callbacks.pfnSetCommandListErrorCb = test_command_error;
    command_create_result = E_OUTOFMEMORY;
    REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &request) == E_OUTOFMEMORY);
    for (auto byte : first) REQUIRE(byte == 0xa5);
    command_create_result = S_OK;
    REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &request) == S_OK);
    const auto first_handle = request.hDrvCommandList;
    request.hRTCommandList.handle = second_runtime; request.hDrvCommandList.pDrvPrivate = second.data();
    second.fill(0xa5);
    REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &request) == S_OK);
    const auto second_handle = request.hDrvCommandList;
    // Exercise the actual WDK ResourceBarrier table on a native-created list.
    // Opaque resources must be resolved before dereferencing, and a late bad
    // element must not record an earlier transition or UAV barrier.
    Object buffer{};
    auto *buffer_peer = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_BUFFER, 0x100000, 4096});
    REQUIRE(VioGpuD3D12BridgeBindObject(create.hDrvDevice, &buffer, buffer_peer, VKDU_BUFFER) == S_OK);
    D3D12DDIARG_RESOURCE_BARRIER_0003 batch[3]{};
    batch[0].Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
    batch[0].Transition = {{&buffer}, UINT_MAX, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS};
    batch[1].Type = batch[2].Type = D3D12DDI_RESOURCE_BARRIER_TYPE_UAV;
    batch[1].UAV.hResource = {&buffer}; // batch[2] is the distinct global barrier.
    const unsigned barrier_device_errors = error_calls;
    command_errors.clear(); recorded_barriers.clear(); barrier_calls = 0;
    commands.pfnResourceBarrier(first_handle, 3, batch);
    if (barrier_calls != 1 || recorded_barriers.size() != 3 || !command_errors.empty() ||
        recorded_barriers[0].type != VKDU_BARRIER_TRANSITION || recorded_barriers[0].resource != buffer_peer ||
        recorded_barriers[0].before != D3D12DDI_RESOURCE_STATE_COPY_DEST ||
        recorded_barriers[0].after != D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS ||
        recorded_barriers[1].type != VKDU_BARRIER_UAV || recorded_barriers[1].resource != buffer_peer ||
        recorded_barriers[2].type != VKDU_BARRIER_UAV || recorded_barriers[2].resource != nullptr) {
        std::fputs("FAIL native UAV barrier lost its resource/global ordering\n", stderr); return 1;
    }
    // Caller arrays are no longer needed after synchronous recording.
    batch[1].UAV.hResource = {};
    REQUIRE(recorded_barriers[1].resource == buffer_peer);
    batch[1].UAV.hResource = {&buffer};
    void *invalid_handles[] = {reinterpret_cast<void *>(uintptr_t(1)), first.data(), allocator_memory.data()};
    for (void *invalid : invalid_handles) {
        const unsigned prior = barrier_calls;
        batch[2].UAV.hResource = {invalid};
        commands.pfnResourceBarrier(second_handle, 3, batch);
        REQUIRE(barrier_calls == prior && command_errors.back() == std::make_pair(second_runtime, HRESULT(E_INVALIDARG)));
    }
    Context foreign_context;
    Object foreign{object_magic, &foreign_context, buffer_peer, VKDU_BUFFER};
    batch[2].UAV.hResource = {&foreign};
    const unsigned prior_invalid = barrier_calls;
    commands.pfnResourceBarrier(second_handle, 3, batch);
    REQUIRE(barrier_calls == prior_invalid && command_errors.back().second == E_INVALIDARG);
    // A valid-shaped stale resource slot is not a global barrier either.
    ctx->resources = nullptr;
    batch[2].UAV.hResource = {&buffer};
    commands.pfnResourceBarrier(second_handle, 3, batch);
    REQUIRE(barrier_calls == prior_invalid && command_errors.back().second == E_INVALIDARG);
    ctx->resources = &buffer;
    batch[2].UAV.hResource = {};
    batch[2].Flags = D3D12DDI_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    commands.pfnResourceBarrier(second_handle, 3, batch);
    REQUIRE(barrier_calls == prior_invalid && command_errors.back().second == E_NOTIMPL);
    batch[2].Flags = D3D12DDI_RESOURCE_BARRIER_FLAG_NONE;
    batch[2].Type = D3D12DDI_RESOURCE_BARRIER_TYPE_ALIASING;
    commands.pfnResourceBarrier(second_handle, 3, batch);
    REQUIRE(barrier_calls == prior_invalid && command_errors.back().second == E_NOTIMPL);
    batch[2].Type = D3D12DDI_RESOURCE_BARRIER_TYPE_UAV;
    commands.pfnResourceBarrier(first_handle, 1, nullptr);
    REQUIRE(barrier_calls == prior_invalid && command_errors.back().second == E_INVALIDARG);
    commands.pfnResourceBarrier(first_handle, 65537, batch);
    REQUIRE(barrier_calls == prior_invalid && command_errors.back().second == E_INVALIDARG);
    commands.pfnResourceBarrier(first_handle, 0, nullptr);
    REQUIRE(barrier_calls == prior_invalid + 1 && recorded_barriers.empty());
    std::array<D3D12DDIARG_RESOURCE_BARRIER_0003, 17> large_batch{};
    for (auto &barrier : large_batch) barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_UAV;
    commands.pfnResourceBarrier(first_handle, UINT(large_batch.size()), large_batch.data());
    REQUIRE(recorded_barriers.size() == large_batch.size());
    barrier_result = E_OUTOFMEMORY;
    commands.pfnResourceBarrier(first_handle, 3, batch);
    REQUIRE(command_errors.back() == std::make_pair(first_runtime, HRESULT(E_OUTOFMEMORY)));
    barrier_result = S_OK;
    REQUIRE(error_calls == barrier_device_errors && ctx->last_error == S_OK);
    VioGpuD3D12BridgeUnbindObject(&buffer);
    std::puts("PASS native UAV resource/global barriers, whole-batch rejection and command error ownership");
    // The source callback table and request are caller-owned and can change.
    um.pfnSetCommandListErrorCb = nullptr; request.hRTCommandList = {};
    const unsigned before_errors = error_calls;
    command_errors.clear();
    if (force_device_command_error) object(first.data())->runtime_command = {};
    commands.pfnResetCommandList(first_handle, nullptr);
    if (command_errors.size() != 1 || command_errors[0] != std::make_pair(first_runtime, HRESULT(E_INVALIDARG)) ||
            error_calls != before_errors || ctx->last_error != S_OK) {
        std::fputs("FAIL native command error escaped its runtime command-list owner\n", stderr);
        return 1;
    }
    commands.pfnSetComputeRootShaderResourceView(second_handle, 0, 0);
    REQUIRE(command_errors.size() == 2 && command_errors.back() == std::make_pair(second_runtime, HRESULT(E_INVALIDARG)));
    command_close_result = E_OUTOFMEMORY;
    commands.pfnCloseCommandList(first_handle);
    REQUIRE(command_errors.size() == 3 && command_errors.back() == std::make_pair(first_runtime, HRESULT(E_OUTOFMEMORY)));
    REQUIRE(error_calls == before_errors && ctx->last_error == S_OK);
    command_close_result = S_OK;
    commands.pfnCloseCommandList(second_handle);
    REQUIRE(command_errors.size() == 3); // A successful sibling operation adds no error.
    command_close_result = DXGI_ERROR_DEVICE_REMOVED;
    const unsigned before_backend_removal = backend_device_removals;
    commands.pfnCloseCommandList(second_handle);
    REQUIRE(command_errors.size() == 4 && command_errors.back() == std::make_pair(second_runtime, HRESULT(DXGI_ERROR_DEVICE_REMOVED)));
    REQUIRE(error_calls == before_errors + 1 && last_runtime.handle == expected_device && ctx->last_error == DXGI_ERROR_DEVICE_REMOVED);
    REQUIRE(backend_device_removals == before_backend_removal + 1 && backend_removal_reason == DXGI_ERROR_DEVICE_REMOVED);
    table.pfnDestroyCommandList(create.hDrvDevice, first_handle);
    table.pfnDestroyCommandList(create.hDrvDevice, second_handle);
    table.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
    adapter.pfnDestroyDevice(create.hDrvDevice);

    // Native recording error can synchronously destroy command, allocator and
    // device, then invalidate both runtime-private slots before returning.
    um.pfnSetCommandListErrorCb = test_command_error;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice, &allocator) == S_OK);
    request.hDrvCommandList = first_handle; request.hRTCommandList.handle = first_runtime;
    REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &request) == S_OK);
    ctx = context(create.hDrvDevice);
    const unsigned before_retire_errors = error_calls, before_destroy = command_destroys;
    const unsigned before_unload = unloads;
    backend_command_destroy_callback = [&]() {
        if (object(first.data()) || !ctx->runtime_device.handle) fixture_abort(__LINE__);
    };
    nested_command_error = [&]() {
        table.pfnDestroyCommandList(create.hDrvDevice, first_handle);
        first.fill(0xa5);
        table.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
        adapter.pfnDestroyDevice(create.hDrvDevice);
        device_memory.fill(0xa5);
        if (unloads != before_unload) fixture_abort(__LINE__);
    };
    barrier_result = DXGI_ERROR_DEVICE_RESET;
    D3D12DDIARG_RESOURCE_BARRIER_0003 global_barrier{};
    global_barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_UAV;
    commands.pfnResourceBarrier(first_handle, 1, &global_barrier);
    barrier_result = S_OK;
    nested_command_error = {}; backend_command_destroy_callback = {};
    REQUIRE(command_destroys == before_destroy + 1 && unloads == before_unload + 1 && error_calls == before_retire_errors);
    for (auto byte : first) REQUIRE(byte == 0xa5);
    for (auto byte : device_memory) REQUIRE(byte == 0xa5);

    // A backend create callback may retire the device and the input request.
    // The rejected late object must be destroyed, with no slot publication.
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice, &allocator) == S_OK);
    const unsigned before_cancel = command_destroys;
    backend_command_create_callback = [&]() {
        adapter.pfnDestroyDevice(create.hDrvDevice);
        device_memory.fill(0xa5);
        std::memset(&request, 0xa5, sizeof(request));
    };
    REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &request) == DXGI_ERROR_DEVICE_REMOVED);
    backend_command_create_callback = {};
    REQUIRE(command_destroys == before_cancel + 1);
    for (auto byte : first) REQUIRE(byte == 0xa5);
    VioGpuD3D12BridgeUnbindObject(allocator_memory.data()); // Drop the surviving fixture child owner.
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    command_close_result = S_OK;
    REQUIRE(!native_contract_complete());
    std::printf("PASS native command-list runtime error ownership, device-loss forwarding, rejected creation and reentrant retirement (%zu-bit); admission closed\n", sizeof(void *) * 8);
    return 0;
}

static int test_finalization_borrow() {
    // Model the interval immediately after final release wins the 1 -> 0
    // transition. No separate flag or retired runtime slot may be required.
    Context terminal;
    terminal.references = 0;
    {
        NativeHeapOperation op(&terminal);
        REQUIRE(!op.retained && terminal.references == 0);
        {
            NativeBackendCall backend_call(&terminal);
            wait_backend_worker(&native_runtime_callbacks, &terminal);
        }
        error(&terminal, E_FAIL);
        REQUIRE(terminal.references == 0 && terminal.last_error == S_OK);
    }
    REQUIRE(terminal.references == 0);
    std::puts("PASS terminal Context callback borrowing without reference resurrection");
    return 0;
}

static int test_native_queue_lifetime() {
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    kt.pfnCreateContextCb = heap_context_create; kt.pfnDestroyContextCb = heap_context_destroy;
    kt.pfnEscapeCb = heap_escape; kt.pfnAllocateCb = heap_allocate; kt.pfnDeallocateCb = heap_deallocate;
    kt.pfnLockCb = heap_lock; kt.pfnUnlockCb = heap_unlock;
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
    um.pfnCreateContextCb = queue_context_create; um.pfnDestroyContextCb = queue_context_destroy;
    um.pfnSetErrorCb = test_error; um.pfnSetCommandListErrorCb = test_command_error;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> device_memory{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = device_memory.data(); create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queue) == S_OK);
    alignas(Object) std::array<unsigned char, sizeof(Object)> queue_memory{}, allocator_memory{}, first{}, second{};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 request{};
    request.hDrvCommandQueue.pDrvPrivate = queue_memory.data();
    request.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D; request.NodeMask = 1;
    queue_memory.fill(0xa5);
    const auto before_creates = queue_creates;
    REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice,&request) == E_INVALIDARG && queue_creates == before_creates);
    for (auto byte : queue_memory) REQUIRE(byte == 0xa5);
    request.hRTCommandQueue.handle = reinterpret_cast<HANDLE>(uintptr_t(0x77110));
    for (UINT invalid : {0u, 3u, 8u, 0x80000000u}) {
        request.QueueFlags = static_cast<D3D12DDI_COMMAND_QUEUE_FLAGS>(invalid);
        REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice,&request) == E_INVALIDARG && queue_creates == before_creates);
    }
    request.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    request.NodeMask = 2;
    REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice,&request) == E_INVALIDARG && queue_creates == before_creates);
    request.NodeMask = 1;
    queue_create_result = E_OUTOFMEMORY;
    REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice,&request) == E_OUTOFMEMORY);
    for (auto byte : queue_memory) REQUIRE(byte == 0xa5);
    queue_create_result = S_OK;
    const auto original_request = request;
    backend_queue_create_callback = [&] { request = {}; };
    REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice,&request) == S_OK);
    backend_queue_create_callback = {};
    REQUIRE(object(queue_memory.data())->runtime_queue.handle == original_request.hRTCommandQueue.handle);
    request = original_request;
    const auto after_creates = queue_creates;
    REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice,&request) == E_INVALIDARG && queue_creates == after_creates);
    auto *queue_peer = object(queue_memory.data())->backend;
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator{};
    allocator.hDrvCommandAllocator.pDrvPrivate = allocator_memory.data();
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice,&allocator) == S_OK);
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 list{};
    list.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
    list.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; list.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    list.hRTCommandList.handle = reinterpret_cast<HANDLE>(uintptr_t(0x77220));
    list.hDrvCommandList.pDrvPrivate = first.data();
    REQUIRE(table.pfnCreateCommandList(create.hDrvDevice,&list) == S_OK);
    list.hRTCommandList.handle = reinterpret_cast<HANDLE>(uintptr_t(0x77330));
    list.hDrvCommandList.pDrvPrivate = second.data();
    REQUIRE(table.pfnCreateCommandList(create.hDrvDevice,&list) == S_OK);
    D3D12DDI_HCOMMANDLIST batch[2] = {{first.data()},{second.data()}};
    auto *first_peer = object(first.data())->backend;
    auto *second_peer = object(second.data())->backend;
    const unsigned before_executes = queue_executes;
    for (void *invalid : {reinterpret_cast<void *>(uintptr_t(1)), static_cast<void *>(allocator_memory.data()),
                          static_cast<void *>(queue_memory.data())}) {
        batch[1] = {invalid};
        queue.pfnExecuteCommandLists(request.hDrvCommandQueue,2,batch);
        REQUIRE(queue_executes == before_executes);
        REQUIRE(reinterpret_cast<ImportPeer *>(first_peer)->references == 1);
        REQUIRE(reinterpret_cast<ImportPeer *>(queue_peer)->references == 1);
    }
    Context foreign_context;
    Object foreign{object_magic,&foreign_context,second_peer,VKDU_COMMAND_LIST};
    batch[1] = {&foreign};
    queue.pfnExecuteCommandLists(request.hDrvCommandQueue,2,batch);
    REQUIRE(queue_executes == before_executes);
    batch[1] = {second.data()};
    queue.pfnExecuteCommandLists(request.hDrvCommandQueue,0,nullptr);
    queue.pfnExecuteCommandLists(request.hDrvCommandQueue,65,batch);
    queue.pfnExecuteCommandLists(request.hDrvCommandQueue,1,nullptr);
    REQUIRE(queue_executes == before_executes);
    queue.pfnExecuteCommandLists(request.hDrvCommandQueue,2,batch);
    REQUIRE(queue_executes == before_executes + 1 && submitted_commands == std::vector<vkdu_object *>({first_peer,second_peer}));
    std::array<D3D12DDI_HCOMMANDLIST,64> maximum{};
    for (auto &entry : maximum) entry = {first.data()};
    queue.pfnExecuteCommandLists(request.hDrvCommandQueue,64,maximum.data());
    REQUIRE(submitted_commands.size() == 64 && reinterpret_cast<ImportPeer *>(first_peer)->references == 1);
    // Queue errors belong to the live device even if queue/list slots disappear
    // inside execution; no command-list error or poisoned slot may be consulted.
    const unsigned before_errors = error_calls, before_queue_destroy = queue_destroys, before_command_destroy = command_destroys;
    const auto before_command_errors = command_errors.size();
    queue_execute_result = E_OUTOFMEMORY;
    backend_queue_execute_callback = [&] {
        batch[0] = {}; batch[1] = {}; // submitted array is already copied
        table.pfnDestroyCommandList(create.hDrvDevice,{first.data()});
        table.pfnDestroyCommandList(create.hDrvDevice,{second.data()});
        table.pfnDestroyCommandQueue(create.hDrvDevice,request.hDrvCommandQueue);
        first.fill(0xa5); second.fill(0xa5); queue_memory.fill(0xa5);
        if (command_destroys != before_command_destroy || queue_destroys != before_queue_destroy) fixture_abort(__LINE__);
    };
    queue_negative_active = force_queue_unowned;
    queue.pfnExecuteCommandLists(request.hDrvCommandQueue,2,batch);
    queue_negative_active = false; backend_queue_execute_callback = {}; queue_execute_result = S_OK;
    REQUIRE(error_calls == before_errors + 1 && last_runtime.handle == expected_device && command_errors.size() == before_command_errors);
    REQUIRE(command_destroys == before_command_destroy + 2 && queue_destroys == before_queue_destroy + 1);
    REQUIRE(!ctx->native_queue_objects && !ctx->native_command_objects);
    // Constructing a queue may synchronously retire device/request storage.
    // The unpublished backend queue is released exactly once with no slot write.
    const unsigned before_cancel = queue_destroys;
    backend_queue_create_callback = [&] {
        table.pfnDestroyCommandAllocator(create.hDrvDevice,allocator.hDrvCommandAllocator);
        adapter.pfnDestroyDevice(create.hDrvDevice);
        device_memory.fill(0xa5); queue_memory.fill(0xa5); request = {};
    };
    REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice,&request) == DXGI_ERROR_DEVICE_REMOVED);
    backend_queue_create_callback = {};
    REQUIRE(queue_destroys == before_cancel + 1);
    for (auto byte : queue_memory) REQUIRE(byte == 0xa5);
    // Retiring the entire runtime device during execution detaches error
    // callbacks, but backend queue/list ownership still lasts until return.
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter,&create) == S_OK);
    request = original_request;
    REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice,&request) == S_OK);
    REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice,&allocator) == S_OK);
    list.hDrvCommandList = {first.data()};
    REQUIRE(table.pfnCreateCommandList(create.hDrvDevice,&list) == S_OK);
    batch[0] = list.hDrvCommandList;
    const unsigned before_retirement_errors = error_calls;
    queue_execute_result = DXGI_ERROR_DEVICE_REMOVED;
    backend_queue_execute_callback = [&] {
        table.pfnDestroyCommandList(create.hDrvDevice,list.hDrvCommandList);
        table.pfnDestroyCommandQueue(create.hDrvDevice,request.hDrvCommandQueue);
        table.pfnDestroyCommandAllocator(create.hDrvDevice,allocator.hDrvCommandAllocator);
        adapter.pfnDestroyDevice(create.hDrvDevice);
        first.fill(0xa5); queue_memory.fill(0xa5); device_memory.fill(0xa5);
    };
    queue.pfnExecuteCommandLists(request.hDrvCommandQueue,1,batch);
    backend_queue_execute_callback = {}; queue_execute_result = S_OK;
    REQUIRE(error_calls == before_retirement_errors && executing_objects.empty());
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    std::puts("PASS native queue identity, whole-batch ownership, reentrant execution retirement and constructor cancellation");
    return 0;
}

static int test_native_completion() {
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    kt.pfnCreateContextCb = heap_context_create; kt.pfnDestroyContextCb = heap_context_destroy;
    kt.pfnEscapeCb = heap_escape; kt.pfnAllocateCb = heap_allocate; kt.pfnDeallocateCb = heap_deallocate;
    kt.pfnLockCb = heap_lock; kt.pfnUnlockCb = heap_unlock; kt.pfnRenderCb = heap_render;
    kt.pfnSignalSynchronizationObject2Cb = heap_signal;
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{}; um.pfnSetErrorCb = test_error;
    um.pfnCreateContextCb = queue_context_create; um.pfnDestroyContextCb = queue_context_destroy;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> memory{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = memory.data(); create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    mwd_allocation allocation{};
    std::array<unsigned char, 16> stream{};
    mwd_reference reference{};
    auto allocate = [&]() {
        HRESULT hr = native_runtime_allocate(ctx, 4096, 4096, 0, 6, &allocation);
        reference = {allocation.token, 0, 4096, 3, 0};
        return hr;
    };
    auto submit = [&]() { return native_runtime_submit(ctx, ctx->native_last_submitted + 1,
        stream.data(), stream.size(), &reference, 1); };
    uint32_t completed = 0;
    REQUIRE(allocate() == S_OK);
    const auto initial_renders = renders;
    ctx->kernel_callbacks.pfnSignalSynchronizationObject2Cb = nullptr;
    REQUIRE(submit() == DXGI_ERROR_UNSUPPORTED && renders == initial_renders);
    ctx->kernel_callbacks.pfnSignalSynchronizationObject2Cb = heap_signal;
    const auto handle = allocation.handle;
    auto *token = static_cast<NativeHeap *>(allocation.token);
    defer_completion = completion_negative_active = true;
    nested_render = [&]() {
        if (!token->submitted || token->users != 2) fixture_abort(__LINE__);
        if (native_runtime_release(ctx, token) != S_OK || !kernel_heaps.count(handle)) fixture_abort(__LINE__);
        // The caller may discard both the reference array and stream in a callback.
        reference = {}; stream.fill(0xa5);
    };
    REQUIRE(submit() == S_OK);
    nested_render = {};
    if (!kernel_heaps.count(handle) || ctx->native_submission_count != 1) {
        std::fputs("FAIL native DMA ownership retired before OS completion event\n", stderr);
        return 1;
    }
    REQUIRE(token->users == 1 && token->submitted == 1 && pending_events.size() == 1);
    void *mapping = nullptr; uint32_t mapped = 0;
    REQUIRE(native_runtime_map(ctx, token, &mapping, &mapped) == DXGI_ERROR_WAS_STILL_DRAWING && !mapping && !mapped);
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && !completed && kernel_heaps.count(handle));
    complete_events();
    // Cleanup failure retains both the accepted event and its final resource owner.
    fail_deallocate = 1;
    REQUIRE(native_runtime_completed(ctx, &completed) == DXGI_ERROR_WAS_STILL_DRAWING && !completed);
    REQUIRE(kernel_heaps.count(handle) && ctx->native_submission_count == 1);
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && !kernel_heaps.count(handle));
    REQUIRE(!ctx->native_submissions && !ctx->native_heaps);
    completion_negative_active = false;

    // At most64 outstanding packets. A completed prefix is retired in order;
    // later event registrations can conservatively include subsequent work.
    REQUIRE(allocate() == S_OK);
    token = static_cast<NativeHeap *>(allocation.token);
    const auto before_signals = signals, before_renders = renders;
    for (unsigned i = 0; i < native_submission_limit; ++i) REQUIRE(submit() == S_OK);
    REQUIRE(ctx->native_submission_count == native_submission_limit && token->users == 65 && token->submitted == 64);
    REQUIRE(signals == before_signals + 1 && renders == before_renders + 64);
    REQUIRE(submit() == DXGI_ERROR_WAS_STILL_DRAWING && renders == before_renders + 64);
    REQUIRE(native_runtime_release(ctx, token) == S_OK && kernel_heaps.count(allocation.handle));
    defer_completion = false; complete_events();
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && !ctx->native_submissions && kernel_heaps.empty());
    REQUIRE(signals == before_signals + 64);

    // Failed event registration after an accepted Render cannot drop references.
    REQUIRE(allocate() == S_OK);
    signal_result = E_ACCESSDENIED;
    REQUIRE(submit() == E_ACCESSDENIED && ctx->native_submission_count == 1);
    REQUIRE(native_runtime_release(ctx, allocation.token) == S_OK && kernel_heaps.count(allocation.handle));
    REQUIRE(native_runtime_completed(ctx, &completed) == E_ACCESSDENIED && kernel_heaps.count(allocation.handle));
    signal_result = S_OK;
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && kernel_heaps.empty());

    // Failed Render still consumes replacement buffers and retains conservative
    // ownership until the context event confirms prior work has completed.
    REQUIRE(allocate() == S_OK);
    defer_completion = true; render_result = E_OUTOFMEMORY;
    REQUIRE(submit() == E_OUTOFMEMORY && ctx->native_submission_count == 1);
    REQUIRE(ctx->native_commands == dma_commands[1].data());
    REQUIRE(native_runtime_release(ctx, allocation.token) == S_OK && kernel_heaps.count(allocation.handle));
    render_result = S_OK; defer_completion = false; complete_events();
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && kernel_heaps.empty());

    // An event callback can query completion recursively, but cannot interleave
    // a new Render into the retirement transaction or release its active owner.
    REQUIRE(allocate() == S_OK);
    nested_signal = [&]() {
        if (native_runtime_status(ctx) != S_OK || submit() != DXGI_ERROR_WAS_STILL_DRAWING ||
                native_runtime_release(ctx, allocation.token) != S_OK) fixture_abort(__LINE__);
    };
    REQUIRE(submit() == S_OK && kernel_heaps.empty());
    nested_signal = {};

    // Two genuine runtime-associated contexts submit the same allocation domain.
    // Completing the first does not retire the second or query an idle owner.
    Object first_queue{}, second_queue{};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue_args{};
    queue_args.NodeMask = 1; queue_args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    queue_args.hDrvCommandQueue.pDrvPrivate = &first_queue;
    queue_args.hRTCommandQueue.handle = reinterpret_cast<HANDLE>(uintptr_t(0x77701));
    REQUIRE(native_queue_create(ctx, &queue_args) == S_OK);
    queue_args.hDrvCommandQueue.pDrvPrivate = &second_queue;
    queue_args.hRTCommandQueue.handle = reinterpret_cast<HANDLE>(uintptr_t(0x77702));
    REQUIRE(native_queue_create(ctx, &queue_args) == S_OK);
    auto *first_route = first_queue.runtime_route;
    auto *second_route = second_queue.runtime_route;
    REQUIRE(first_route && second_route && first_route != second_route &&
        first_route->native_heap_context != second_route->native_heap_context);
    REQUIRE(native_runtime_queue_retain(ctx, &queue_args) == E_INVALIDARG);
    REQUIRE(allocate() == S_OK);
    defer_completion = true;
    expected_render_context = first_route->native_heap_context;
    const uint32_t first_serial = ctx->native_last_submitted + 1;
    REQUIRE(native_runtime_submit_queue(ctx, first_route, first_serial, stream.data(), stream.size(), &reference, 1) == S_OK);
    expected_render_context = second_route->native_heap_context;
    REQUIRE(native_runtime_submit_queue(ctx, second_route, first_serial + 1, stream.data(), stream.size(), &reference, 1) == S_OK);
    REQUIRE(first_route->submissions == 1 && second_route->submissions == 1);
    REQUIRE(native_runtime_submit_queue(ctx, first_route, first_serial, stream.data(), stream.size(), &reference, 1) == E_INVALIDARG);
    complete_events();
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && completed == first_serial);
    REQUIRE(!first_route->submissions && second_route->submissions == 1);
    complete_events();
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && completed == first_serial + 1);
    REQUIRE(!second_route->submissions);
    defer_completion = false;
    expected_render_context = expected_context;
    REQUIRE(native_runtime_release(ctx, allocation.token) == S_OK);
    const auto destroyed_queues = runtime_context_destroys;
    native_queue_destroy(ctx, &first_queue);
    native_queue_destroy(ctx, &second_queue);
    REQUIRE(runtime_context_destroys == destroyed_queues + 2);
    REQUIRE(!ctx->native_runtime_queues);

    // Signal callback retirement cancels publication. Residual allocations are
    // owned by runtime device teardown, never cleaned with detached callbacks.
    REQUIRE(allocate() == S_OK);
    ++ctx->references;
    error_device = create.hDrvDevice; defer_completion = true;
    const auto old_deallocations = deallocations, old_context_destroys = heap_context_destroys;
    nested_signal = [&]() {
        native_destroy_device(error_device); heap_callbacks_retired = true;
        memory.fill(0xa5);
    };
    REQUIRE(submit() == DXGI_ERROR_DEVICE_REMOVED);
    REQUIRE(deallocations == old_deallocations && heap_context_destroys == old_context_destroys);
    REQUIRE(ctx->native_retiring && ctx->native_submission_count == 1 && kernel_heaps.count(allocation.handle));
    nested_signal = {}; complete_events();
    REQUIRE(native_runtime_completed(ctx, &completed) == DXGI_ERROR_DEVICE_REMOVED);
    release(ctx); // Backend cleanup and event handles retire without KMT callbacks.
    kernel_heaps.clear(); kernel_resources.clear(); heap_callbacks_retired = defer_completion = false;
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    std::puts("PASS native OS completion events, ordered DMA ownership, bounded pending retirement and callback cancellation");
    return 0;
}

#include "runtime_indirect_test.inc"
#include "runtime_fences_test.inc"
#include "runtime_residency_test.inc"
#include "runtime_queries_test.inc"

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && !std::strcmp(argv[1], "--negative-control-deferred-backend")) force_deferred_cleanup = true;
    else if (argc == 2 && !std::strcmp(argv[1], "--negative-control-command-error-owner")) force_device_command_error = true;
    else if (argc == 2 && !std::strcmp(argv[1], "--negative-control-global-uav-barrier")) force_drop_global_barrier = true;
    else if (argc == 2 && !std::strcmp(argv[1], "--negative-control-queue-ownership")) force_queue_unowned = true;
    else if (argc == 2 && !std::strcmp(argv[1], "--negative-control-os-completion")) force_early_completion = true;
    else if (argc == 2 && !std::strcmp(argv[1], "--negative-control-indirect-count")) force_drop_indirect_count = true;
    else if (argc == 2 && !std::strcmp(argv[1], "--negative-control-native-fence-owner")) native_fence_ignore_owner = true;
    else if (argc == 2 && !std::strcmp(argv[1], "--negative-control-native-fence-mask")) native_fence_drop_mask = true;
    else if (argc == 2 && !std::strcmp(argv[1], "--negative-control-native-residency-pending")) native_residency_drop_pending = true;
    else if (argc == 2 && !std::strcmp(argv[1], "--negative-control-native-residency-evict")) native_residency_skip_evict = true;
    else if (argc == 2 && !std::strcmp(argv[1], "--negative-control-query-field-order")) force_query_swap_fields = true;
    else if (argc != 1) return 2;
    REQUIRE(test_finalization_borrow() == 0);
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
    REQUIRE(test_native_command_errors() == 0);
    REQUIRE(test_native_queue_lifetime() == 0);
    REQUIRE(test_native_completion() == 0);
    REQUIRE(test_native_indirect() == 0);
    REQUIRE(test_native_fences() == 0);
    REQUIRE(test_native_residency() == 0);
    REQUIRE(test_native_queries() == 0);
    std::printf("PASS native OpenAdapter12 WDK identity/negotiation/private memory/callback/lifetime/error cleanup (%zu-bit); no system-runtime or GPU acceptance\n", sizeof(void *) * 8);
    return 0;
}
