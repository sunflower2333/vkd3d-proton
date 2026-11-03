/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "vkd3d.h"
#include "vkd3d_wddm.h"
#include "vkd3d_atomic.h"
#include "vkd3d_threads.h"
#include "vkd3d_shader.h"
#include "backend.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#ifndef _WIN32
#include <time.h>
#endif

struct vkdu_device { ID3D12Device *object; };
struct vkdu_root_slot { uint32_t type, extent, heap_type, unbounded; };
struct vkdu_query_scope { IUnknown *heap; uint32_t type, index; };
/* Descriptors do not own resources. A generation ID distinguishes stale views
 * and placed aliases even when their object address or GPU address is reused. */
struct vkdu_uav_record {
    uint64_t resource_id, first;
    uint32_t format, count, stride, flags;
};
struct vkdu_attachment_lifetime {
    uint64_t references, generation;
    vkdu_object *live;
};
struct vkdu_attachment_record {
    struct vkdu_attachment_lifetime *lifetime;
    uint32_t width, height, layers, format, mip, first_layer, sample_count, sample_quality;
};
static uint64_t vkdu_object_serial;
struct vkdu_bundle_owner;
struct vkdu_bundle_requirement;
struct vkdu_object {
    uint32_t references;
    IUnknown *object;
    ID3D12Device *owner;
    enum vkdu_kind kind;
    uint64_t identity;
    uint64_t bytes;
    uint64_t placement_heap_id, placement_offset;
    uint32_t heap_type, command_type;
    int closed;
    vkdu_object *command_allocator;
    uint64_t allocator_generation, bundle_generation;
    uint32_t bundle_recordings, bundle_users, bundle_changed;
    uint32_t bundle_needs_root, bundle_needs_targets, bundle_inherited_indices;
    /* Bundles use one immutable raster sample tuple for this private slice.
     * A later raster PSO with a different tuple is rejected before any GPU
     * command is emitted; stream-output-only PSOs do not set this tuple. */
    uint32_t bundle_has_raster, bundle_raster_samples, bundle_raster_quality;
    struct vkdu_bundle_owner *bundle_owners;
    struct vkdu_bundle_requirement *bundle_requirements;
    int predicate_enabled;
    uint32_t resource_flags, descriptor_count, descriptor_stride;
    int shader_visible;
    /* Copy root layout at bind time: never retain a borrowed private wrapper. */
    struct vkdu_root_slot slots[64];
    uint32_t slot_count;
    struct vkdu_root_slot graphics_slots[64];
    uint32_t graphics_slot_count;
    uint64_t bound_heaps[2];
    uint32_t width, height, format;
    int texture_heap;
    uint32_t graphics_root, graphics_pipeline, topology, viewport_count, scissor_count, render_target, index_count;
    uint32_t pipeline_samples, pipeline_quality, attachment_samples, attachment_quality;
    uint32_t query_type, query_count, query_scope_count, query_scope_capacity;
    struct vkdu_query_scope *query_scopes;
    struct vkdu_uav_record *uav_records;
    struct vkdu_attachment_lifetime *attachment_lifetime;
    struct vkdu_attachment_record *attachment_records;
#ifdef VKDU_ENABLE_TEST_DEVICE
    void (*test_retire_hook)(void *);
    void *test_retire_context;
#endif
};

#define OBJ(type, o) ((type *)(o)->object)
#define VALID(o, k) ((o) && (o)->object && (o)->kind == (k))
#define RECORDING(o) (VALID(o, VKDU_COMMAND_LIST) && !(o)->closed)
#include "backend_attachment_records.inc"
#include "backend_bundles.inc"

static int32_t wrap(vkdu_device *device, enum vkdu_kind kind, HRESULT hr, IUnknown *object, vkdu_object **out)
{
    vkdu_object *value;
    if (FAILED(hr)) return hr;
    if (!(value = calloc(1, sizeof(*value)))) { IUnknown_Release(object); return E_OUTOFMEMORY; }
    value->object = object;
    value->references = 1;
    value->owner = device->object;
    ID3D12Device_AddRef(value->owner);
    value->kind = kind;
    value->identity = vkd3d_atomic_uint64_increment(&vkdu_object_serial, vkd3d_memory_order_relaxed);
    if (kind == VKDU_TEXTURE2D) {
        if (!(value->attachment_lifetime = calloc(1, sizeof(*value->attachment_lifetime)))) {
            IUnknown_Release(object); ID3D12Device_Release(value->owner); free(value); return E_OUTOFMEMORY;
        }
        value->attachment_lifetime->references = 1;
        value->attachment_lifetime->generation = value->identity;
        value->attachment_lifetime->live = value;
    }
    *out = value;
    return S_OK;
}

#ifdef _WIN32
static void vkdu_trace(const char *format, ...)
{
    char value[8] = {0}, line[512];
    va_list args;
    int n;
    DWORD len = GetEnvironmentVariableA("VIOGPU_D3D12_EXPERIMENTAL", value, sizeof(value));
    if (!len || len >= sizeof(value) || value[0] != '1') return;
    va_start(args, format);
    n = vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (n < 0) return;
    strcat(line, "\n");
    OutputDebugStringA("[viogpud3d12:backend] ");
    OutputDebugStringA(line);
}
#else
static void vkdu_trace(const char *format, ...) { (void)format; }
#endif
#include "backend_memory_profile.inc"

static int32_t select_physical_device(PFN_vkGetInstanceProcAddr loader, const struct vkdu_adapter *adapter,
        int test_cpu, int runtime_identity, struct vkd3d_instance **out_instance, VkPhysicalDevice *out_physical)
{
    struct vkd3d_instance_create_info info = {0};
    PFN_vkEnumeratePhysicalDevices enumerate;
    PFN_vkGetPhysicalDeviceProperties2 properties;
    struct vkd3d_instance *instance = NULL;
    VkPhysicalDevice *devices = NULL, selected = VK_NULL_HANDLE;
    VkInstance vk_instance;
    uint32_t count = 0, capacity, i;
    HRESULT hr;
    if (!loader || (!test_cpu && !adapter)) return E_INVALIDARG;
    info.pfn_vkGetInstanceProcAddr = loader;
    if (FAILED(hr = vkd3d_create_instance(&info, &instance))) return hr;
    vk_instance = vkd3d_instance_get_vk_instance(instance);
    enumerate = (PFN_vkEnumeratePhysicalDevices)loader(vk_instance, "vkEnumeratePhysicalDevices");
    properties = (PFN_vkGetPhysicalDeviceProperties2)loader(vk_instance, "vkGetPhysicalDeviceProperties2");
    hr = DXGI_ERROR_NOT_FOUND;
    if (!enumerate || !properties || enumerate(vk_instance, &count, NULL) != VK_SUCCESS || !count) goto done;
    capacity = count;
    if (!(devices = calloc(count, sizeof(*devices)))) { hr = E_OUTOFMEMORY; goto done; }
    if (enumerate(vk_instance, &count, devices) != VK_SUCCESS || count > capacity) { hr = E_FAIL; goto done; }
    for (i = 0; i < count; ++i) {
        VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id; id.pNext = &driver;
        properties(devices[i], &props);
        if (test_cpu ? props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU :
            (id.deviceLUIDValid && !memcmp(id.deviceLUID, adapter->luid, 8) &&
             (runtime_identity ? id.deviceNodeMask == 1 :
              (props.properties.vendorID == adapter->vendor_id && props.properties.deviceID == adapter->device_id)) &&
             driver.driverID == VK_DRIVER_ID_MESA_TURNIP)) {
            if (selected) { hr = E_INVALIDARG; goto done; }
            selected = devices[i];
        }
    }
    if (selected) {
        *out_instance = instance; instance = NULL; *out_physical = selected; hr = S_OK;
    }
done:
    free(devices);
    if (instance) vkd3d_instance_decref(instance);
    return hr;
}

int32_t vkdu_query_adapter_memory_facts(PFN_vkGetInstanceProcAddr loader,
        const struct vkdu_memory_expectation *expected, struct vkdu_memory_facts *out)
{
    struct vkd3d_instance *instance = NULL;
    struct vkdu_adapter adapter = {0};
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    HRESULT hr;
    if (!out) return E_POINTER;
    if (!expected) return E_INVALIDARG;
    memcpy(adapter.luid, expected->luid, 8);
    if (FAILED(hr = select_physical_device(loader, &adapter, 0, 1, &instance, &selected))) return hr;
    hr = vkdu_memory_facts_for_device(loader, vkd3d_instance_get_vk_instance(instance), selected, expected, out);
    vkd3d_instance_decref(instance);
    return hr;
}

static int32_t create_device(PFN_vkGetInstanceProcAddr loader, const struct vkdu_adapter *adapter, int test_cpu,
        int runtime_identity, const struct mwd_device_create_info *runtime,
        const struct vkdu_memory_expectation *expected, struct vkdu_memory_facts *facts, vkdu_device **out)
{
    struct vkd3d_device_create_info device_info = {0};
    struct vkd3d_instance *instance = NULL;
    struct vkdu_memory_facts current = {0};
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    vkdu_device *device = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (FAILED(hr = select_physical_device(loader, adapter, test_cpu, runtime_identity, &instance, &selected))) {
        vkdu_trace("create_device: select_physical_device hr=%08x", (unsigned)hr);
        return hr;
    }
    if (expected) {
        hr = vkdu_memory_facts_for_device(loader, vkd3d_instance_get_vk_instance(instance), selected, expected, &current);
        if (FAILED(hr)) goto done;
        if (!vkdu_memory_facts_qualified(&current)) { hr = DXGI_ERROR_UNSUPPORTED; goto done; }
    }
    if (!(device = calloc(1, sizeof(*device)))) { hr = E_OUTOFMEMORY; goto done; }
    device_info.instance = instance;
    device_info.vk_physical_device = selected;
    device_info.minimum_feature_level = D3D_FEATURE_LEVEL_11_0;
    device_info.independent = true;
    if (adapter) memcpy(&device_info.adapter_luid, adapter->luid, 8);
    hr = runtime ? vkd3d_create_device_wddm(&device_info, runtime, &IID_ID3D12Device, (void **)&device->object) :
                   vkd3d_create_device(&device_info, &IID_ID3D12Device, (void **)&device->object);
    vkdu_trace("create_device: vkd3d_create_device%s hr=%08x", runtime ? "_wddm" : "", (unsigned)hr);
    if (SUCCEEDED(hr) && vkd3d_get_vk_physical_device(device->object) != selected) {
        ID3D12Device_Release(device->object); device->object = NULL; hr = DXGI_ERROR_NOT_FOUND;
    }
    if (SUCCEEDED(hr)) {
        if (facts) *facts = current;
        *out = device; device = NULL;
    }
done:
    free(device);
    vkd3d_instance_decref(instance);
    return hr;
}

int32_t vkdu_device_create(PFN_vkGetInstanceProcAddr loader, const struct vkdu_adapter *adapter, vkdu_device **out)
{ return create_device(loader, adapter, 0, 0, NULL, NULL, NULL, out); }
int32_t vkdu_device_create_runtime(PFN_vkGetInstanceProcAddr loader, const uint8_t luid[8], vkdu_device **out)
{
    struct vkdu_adapter adapter = {0};
    static const uint8_t zero_luid[8] = {0};
    if (out) *out = NULL;
    if (!luid || !memcmp(luid, zero_luid, sizeof(zero_luid))) return E_INVALIDARG;
    memcpy(adapter.luid, luid, sizeof(adapter.luid));
    return create_device(loader, &adapter, 0, 1, NULL, NULL, NULL, out);
}
static int32_t create_shared(PFN_vkGetInstanceProcAddr loader, const uint8_t luid[8],
        const struct mwd_callbacks *callbacks, void *owner, int native_surfaces,
        const struct vkdu_memory_expectation *expected, struct vkdu_memory_facts *facts, vkdu_device **out)
{
    struct vkdu_adapter adapter = {0};
    struct mwd_device_create_info runtime = {MWD_STYPE_DEVICE, NULL, callbacks, owner};
    struct mwd_native_surface_device_info native = {MWD_STYPE_NATIVE_SURFACE_DEVICE_V2, NULL,
        owner, MWD_SUPPORT_NATIVE_SURFACE | MWD_SUPPORT_NATIVE_PRESENT, 0};
    static const uint8_t zero[8] = {0};
    if (out) *out = NULL;
    if (!luid || !memcmp(luid, zero, 8) || !owner || !mwd_callbacks_valid(callbacks)) return E_INVALIDARG;
    memcpy(adapter.luid, luid, 8);
    if (native_surfaces) runtime.pNext = &native;
    return create_device(loader, &adapter, 0, 1, &runtime, expected, facts, out);
}
int32_t vkdu_device_create_shared(PFN_vkGetInstanceProcAddr loader, const struct vkdu_memory_expectation *expected,
        const struct mwd_callbacks *callbacks, void *owner, struct vkdu_memory_facts *facts, vkdu_device **out)
{
    if (!expected || !facts) { if (out) *out = NULL; return E_INVALIDARG; }
    return create_shared(loader, expected->luid, callbacks, owner, 0, expected, facts, out);
}
int32_t vkdu_device_create_shared_native_facts(PFN_vkGetInstanceProcAddr loader, const struct vkdu_memory_expectation *expected,
        const struct mwd_callbacks *callbacks, void *owner, struct vkdu_memory_facts *facts, vkdu_device **out)
{
    if (!expected || !facts) { if (out) *out = NULL; return E_INVALIDARG; }
    return create_shared(loader, expected->luid, callbacks, owner, 1, expected, facts, out);
}
int32_t vkdu_device_create_shared_native(PFN_vkGetInstanceProcAddr loader, const uint8_t luid[8],
        const struct mwd_callbacks *callbacks, void *owner, vkdu_device **out)
{ return create_shared(loader, luid, callbacks, owner, 1, NULL, NULL, out); }

int32_t vkdu_native_image_import(vkdu_device *device, void *owner, void *token,
        uint32_t width, uint32_t height, uint32_t format, uint32_t stride,
        int render_target, int preserve_contents, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc = {0};
    ID3D12Resource *resource = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || (render_target != 0 && render_target != 1)) return E_INVALIDARG;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Flags = render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    hr = vkd3d_create_native_image_wddm(device->object, &desc, owner, token, stride, preserve_contents, &resource);
    hr = wrap(device, VKDU_TEXTURE2D, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr))
    {
        (*out)->width = width; (*out)->height = height; (*out)->format = format;
        (*out)->resource_flags = desc.Flags;
    }
    return hr;
}
#ifdef VKDU_ENABLE_TEST_DEVICE
int32_t vkdu_test_device_create(PFN_vkGetInstanceProcAddr loader, vkdu_device **out)
{ return create_device(loader, NULL, 1, 0, NULL, NULL, NULL, out); }
static void __stdcall test_destroyed(void *counter)
{ vkd3d_atomic_uint32_increment((uint32_t *)counter, vkd3d_memory_order_release); }
int32_t vkdu_test_destroy_counter(vkdu_object *object, uint32_t *counter)
{
    ID3DDestructionNotifier *notifier = NULL;
    UINT id;
    HRESULT hr;
    if (!object || !counter) return E_INVALIDARG;
    hr = IUnknown_QueryInterface(object->object, &IID_ID3DDestructionNotifier, (void **)&notifier);
    if (FAILED(hr)) return hr;
    hr = ID3DDestructionNotifier_RegisterDestructionCallback(notifier, test_destroyed, counter, &id);
    ID3DDestructionNotifier_Release(notifier);
    return hr;
}
uint32_t vkdu_test_counter_load(uint32_t *counter)
{ return vkd3d_atomic_uint32_load_explicit(counter, vkd3d_memory_order_acquire); }
void *vkdu_test_command_iface(vkdu_object *command)
{ return VALID(command, VKDU_COMMAND_LIST) ? command->object : NULL; }
#endif
void vkdu_device_destroy(vkdu_device *device)
{ if (device) { ID3D12Device_Release(device->object); free(device); } }
int32_t vkdu_device_status(vkdu_device *device)
{ return device ? ID3D12Device_GetDeviceRemovedReason(device->object) : E_INVALIDARG; }
int32_t vkdu_device_remove(vkdu_device *device, int32_t reason)
{
    return device ? vkd3d_wddm_device_lost(device->object, reason) : E_INVALIDARG;
}
int vkdu_object_retain(vkdu_object *object)
{
    uint32_t count, observed;
    if (!object) return 0;
    count = vkd3d_atomic_uint32_load_explicit(&object->references, vkd3d_memory_order_acquire);
    while (count && count < UINT32_MAX) {
        observed = vkd3d_atomic_uint32_compare_exchange(&object->references, count, count + 1,
                vkd3d_memory_order_acq_rel, vkd3d_memory_order_acquire);
        if (observed == count) return 1;
        count = observed;
    }
    return 0;
}
void vkdu_object_destroy(vkdu_object *object)
{
    if (object && !vkd3d_atomic_uint32_decrement(&object->references, vkd3d_memory_order_acq_rel)) {
#ifdef VKDU_ENABLE_TEST_DEVICE
        if (object->test_retire_hook) object->test_retire_hook(object->test_retire_context);
#endif
        vkdu_attachment_retire(object);
        IUnknown_Release(object->object);
        if (object->kind == VKDU_COMMAND_LIST) {
            if (object->command_type == 1 && !object->closed && object->command_allocator)
                --object->command_allocator->bundle_recordings;
            vkdu_bundle_drop_owners(object);
            vkdu_object_destroy(object->command_allocator);
        }
        ID3D12Device_Release(object->owner);
        free(object->query_scopes); free(object->uav_records); free(object);
    }
}
#ifdef VKDU_ENABLE_TEST_DEVICE
int32_t vkdu_test_retire_hook(vkdu_object *object, void (*hook)(void *), void *context)
{
    if (!VALID(object, VKDU_TEXTURE2D)) return E_INVALIDARG;
    object->test_retire_hook = hook; object->test_retire_context = context;
    return S_OK;
}
#endif
int vkdu_object_is(vkdu_object *object, enum vkdu_kind kind) { return VALID(object, kind); }
int vkdu_same_device(vkdu_object *a, vkdu_object *b) { return a && b && a->owner == b->owner; }
/* Conservatively reject aliases represented by distinct placed resources.
 * One resource's distinct subresources retain their existing legal copy path.
 * Subtraction keeps the range test independent of integer end overflow. */
static int vkdu_placed_overlap(vkdu_object *a, vkdu_object *b)
{
    if (a->object == b->object || !a->placement_heap_id || a->placement_heap_id != b->placement_heap_id) return 0;
    return a->placement_offset <= b->placement_offset ?
        b->placement_offset - a->placement_offset < a->bytes :
        a->placement_offset - b->placement_offset < b->bytes;
}
int vkdu_object_belongs(vkdu_device *device, vkdu_object *object) { return device && object && device->object == object->owner; }

static D3D12_HEAP_DESC vkdu_memory_heap_desc(uint64_t bytes, int cpu_visible)
{
    D3D12_HEAP_DESC desc = {0};
    desc.SizeInBytes = bytes; desc.Alignment = 65536;
    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    desc.Properties.CreationNodeMask = desc.Properties.VisibleNodeMask = 1;
    desc.Properties.Type = cpu_visible ? D3D12_HEAP_TYPE_CUSTOM : D3D12_HEAP_TYPE_DEFAULT;
    if (cpu_visible) {
        /* The native KMD maps CPU-visible allocations as cached pages. The DDI
         * supplies WRITE_BACK, not an API READBACK heap type; the same heap can
         * back a coherent UMA upload buffer. Preserve those actual properties. */
        desc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
#ifdef VKDU_TEST_CACHED_HEAP_AS_READBACK
        desc.Properties.Type = D3D12_HEAP_TYPE_READBACK;
        desc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
#endif
    }
    return desc;
}

int32_t vkdu_memory_heap_import(vkdu_device *device, void *owner, void *token, uint64_t bytes,
        int cpu_visible, vkdu_object **out)
{
    D3D12_HEAP_DESC desc;
    ID3D12Heap *heap = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !owner || !token || !bytes || bytes % 65536) return E_INVALIDARG;
    desc = vkdu_memory_heap_desc(bytes, cpu_visible);
    hr = vkd3d_create_heap_wddm(device->object, &desc, owner, token, &heap);
    hr = wrap(device, VKDU_MEMORY_HEAP, hr, (IUnknown *)heap, out);
    if (SUCCEEDED(hr)) { (*out)->bytes = bytes; (*out)->heap_type = desc.Properties.Type; }
    return hr;
}

#ifdef VKDU_ENABLE_TEST_DEVICE
/* Same production heap description and placement on CPU Vulkan. This factory
 * uses local VkDeviceMemory, so it cannot prove native KMD allocation/import. */
int32_t vkdu_test_memory_heap_create(vkdu_device *device, uint64_t bytes, int cpu_visible, vkdu_object **out)
{
    D3D12_HEAP_DESC desc;
    ID3D12Heap *heap = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !bytes || bytes % 65536) return E_INVALIDARG;
    desc = vkdu_memory_heap_desc(bytes, cpu_visible);
    hr = ID3D12Device_CreateHeap(device->object, &desc, &IID_ID3D12Heap, (void **)&heap);
    hr = wrap(device, VKDU_MEMORY_HEAP, hr, (IUnknown *)heap, out);
    if (SUCCEEDED(hr)) { (*out)->bytes = bytes; (*out)->heap_type = desc.Properties.Type; }
    return hr;
}
#endif

int32_t vkdu_buffer_place(vkdu_device *device, vkdu_object *heap, uint64_t offset,
        uint64_t bytes, uint32_t state, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc = {0};
    ID3D12Resource *resource = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !VALID(heap, VKDU_MEMORY_HEAP) || heap->owner != device->object || !bytes ||
            offset % 65536 || offset > heap->bytes || bytes > heap->bytes - offset ||
            (!vkdu_buffer_read_state(state) &&
             state != D3D12_RESOURCE_STATE_COPY_DEST && state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
             state != D3D12_RESOURCE_STATE_STREAM_OUT) ||
            (heap->heap_type == D3D12_HEAP_TYPE_READBACK && state != D3D12_RESOURCE_STATE_COPY_DEST) ||
            (heap->heap_type != D3D12_HEAP_TYPE_DEFAULT && state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) return E_INVALIDARG;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    /* R0 has no unordered-access resource flag. Its supported default buffers
     * must remain usable by the native UAV/view/command DDIs. Do not confuse
     * later DDI flag 0x80 with embedded ALLOW_UNORDERED_ACCESS (0x4). */
    desc.Flags = heap->heap_type == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : 0;
    hr = ID3D12Device_CreatePlacedResource(device->object, OBJ(ID3D12Heap, heap), offset, &desc, state, NULL,
                                            &IID_ID3D12Resource, (void **)&resource);
    hr = wrap(device, VKDU_BUFFER, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr)) {
        (*out)->bytes = bytes; (*out)->heap_type = heap->heap_type;
        (*out)->resource_flags = desc.Flags;
    }
    return hr;
}

int32_t vkdu_buffer_create(vkdu_device *device, uint64_t bytes, uint32_t heap_type, uint32_t flags, uint32_t state, vkdu_object **out)
{
    D3D12_HEAP_PROPERTIES heap = {0};
    D3D12_RESOURCE_DESC desc = {0};
    ID3D12Resource *resource = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !bytes || heap_type < 1 || heap_type > 3) return E_INVALIDARG;
    heap.Type = heap_type; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
    hr = ID3D12Device_CreateCommittedResource(device->object, &heap, D3D12_HEAP_FLAG_NONE, &desc, state, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    hr = wrap(device, VKDU_BUFFER, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr)) { (*out)->bytes = bytes; (*out)->heap_type = heap_type; (*out)->resource_flags = flags; }
    return hr;
}
int32_t vkdu_buffer_map(vkdu_object *buffer, uint64_t begin, uint64_t end, void **out)
{
    D3D12_RANGE range;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!VALID(buffer, VKDU_BUFFER) || buffer->heap_type == 1 || begin > end || end > buffer->bytes || end > SIZE_MAX) return E_INVALIDARG;
    range.Begin = (size_t)begin; range.End = (size_t)end;
    return ID3D12Resource_Map(OBJ(ID3D12Resource, buffer), 0, &range, out);
}
int32_t vkdu_buffer_unmap(vkdu_object *buffer, uint64_t begin, uint64_t end)
{
    D3D12_RANGE range;
    if (!VALID(buffer, VKDU_BUFFER) || buffer->heap_type == 1 || begin > end || end > buffer->bytes || end > SIZE_MAX) return E_INVALIDARG;
    range.Begin = (size_t)begin; range.End = (size_t)end;
    ID3D12Resource_Unmap(OBJ(ID3D12Resource, buffer), 0, &range);
    return S_OK;
}
uint64_t vkdu_buffer_address(vkdu_object *buffer)
{ return VALID(buffer, VKDU_BUFFER) ? ID3D12Resource_GetGPUVirtualAddress(OBJ(ID3D12Resource, buffer)) : 0; }
uint64_t vkdu_buffer_size(vkdu_object *buffer) { return VALID(buffer, VKDU_BUFFER) ? buffer->bytes : 0; }
uint32_t vkdu_descriptor_size(vkdu_device *device, uint32_t type)
{
    return device && type <= D3D12_DESCRIPTOR_HEAP_TYPE_DSV
            ? ID3D12Device_GetDescriptorHandleIncrementSize(device->object, type) : 0;
}
int32_t vkdu_heap_create(vkdu_device *device, uint32_t type, uint32_t count, int shader_visible, vkdu_object **out)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {0}; ID3D12DescriptorHeap *heap = NULL; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !count || type > D3D12_DESCRIPTOR_HEAP_TYPE_DSV ||
        (shader_visible != 0 && shader_visible != 1) || (shader_visible && type > D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER))
        return E_INVALIDARG;
    desc.Type = type; desc.NumDescriptors = count; desc.NodeMask = 1;
    desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = ID3D12Device_CreateDescriptorHeap(device->object, &desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    hr = wrap(device, VKDU_DESCRIPTOR_HEAP, hr, (IUnknown *)heap, out);
    if (SUCCEEDED(hr)) {
        (*out)->heap_type = type; (*out)->descriptor_count = count; (*out)->shader_visible = shader_visible;
        (*out)->descriptor_stride = vkdu_descriptor_size(device, type);
        if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
                !((*out)->uav_records = calloc(count, sizeof(*(*out)->uav_records)))) {
            vkdu_object_destroy(*out); *out = NULL; return E_OUTOFMEMORY;
        }
        if (type >= D3D12_DESCRIPTOR_HEAP_TYPE_RTV &&
                !((*out)->attachment_records = calloc(count, sizeof(*(*out)->attachment_records)))) {
            vkdu_object_destroy(*out); *out = NULL; return E_OUTOFMEMORY;
        }
    }
    return hr;
}
uint64_t vkdu_heap_start(vkdu_object *heap, int gpu)
{
    if (!VALID(heap, VKDU_DESCRIPTOR_HEAP)) return 0;
    if (gpu) return heap->shader_visible ? ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(OBJ(ID3D12DescriptorHeap, heap)).ptr : 0;
    return ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(OBJ(ID3D12DescriptorHeap, heap)).ptr;
}
int32_t vkdu_query_heap_create(vkdu_device *device, uint32_t type, uint32_t count, vkdu_object **out)
{
    D3D12_QUERY_HEAP_DESC desc = {0}; ID3D12QueryHeap *heap = NULL; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !count || type > D3D12_QUERY_HEAP_TYPE_SO_STATISTICS) return E_INVALIDARG;
    desc.Type = type; desc.Count = count; desc.NodeMask = 1;
    hr = ID3D12Device_CreateQueryHeap(device->object, &desc, &IID_ID3D12QueryHeap, (void **)&heap);
    hr = wrap(device, VKDU_QUERY_HEAP, hr, (IUnknown *)heap, out);
    if (SUCCEEDED(hr)) { (*out)->query_type = type; (*out)->query_count = count; }
    return hr;
}
int32_t vkdu_pageable_backing(vkdu_object *object, uint32_t *count, struct mwd_allocation *allocations)
{
    if (!count || !allocations) return E_POINTER;
    *count = 0;
    if (VALID(object, VKDU_DESCRIPTOR_HEAP))
        return vkd3d_wddm_descriptor_backing(OBJ(ID3D12DescriptorHeap, object), count, allocations);
    if (VALID(object, VKDU_QUERY_HEAP))
        return vkd3d_wddm_query_backing(OBJ(ID3D12QueryHeap, object), count, allocations);
    return E_INVALIDARG;
}
int vkdu_heap_resolve(vkdu_object *heap, uint64_t handle, int gpu, uint32_t *index)
{
    uint64_t start = vkdu_heap_start(heap, gpu), offset;
    if (!start || handle < start || !index || !heap->descriptor_stride) return 0;
    offset = handle - start;
    if (offset % heap->descriptor_stride || offset / heap->descriptor_stride >= heap->descriptor_count) return 0;
    *index = (uint32_t)(offset / heap->descriptor_stride);
    return 1;
}

#include "backend_samplers.inc"
#include "backend_textures.inc"
#include "backend_resource_copy.inc"
#include "backend_resolve.inc"
int32_t vkdu_buffer_uav(vkdu_object *heap, uint32_t index, vkdu_object *buffer,
        uint32_t format, uint64_t first, uint32_t count, uint32_t stride, uint32_t flags,
        vkdu_object *counter, uint64_t counter_offset)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {0}; D3D12_CPU_DESCRIPTOR_HANDLE destination;
    uint32_t element_size;
    if (!VALID(heap, VKDU_DESCRIPTOR_HEAP) || heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        index >= heap->descriptor_count || !VALID(buffer, VKDU_BUFFER) || !vkdu_same_device(heap, buffer) ||
        !(buffer->resource_flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !count || flags > D3D12_BUFFER_UAV_FLAG_RAW)
        return E_INVALIDARG;
    if (flags == D3D12_BUFFER_UAV_FLAG_RAW) {
        if (format != DXGI_FORMAT_R32_TYPELESS || stride || counter) return E_INVALIDARG;
        element_size = 4;
    } else if (stride) {
        if (format != DXGI_FORMAT_UNKNOWN || (stride & 3) || stride > 2048) return E_INVALIDARG;
        element_size = stride;
    } else {
        if (counter) return E_INVALIDARG;
        switch (format) {
            case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R32_FLOAT: element_size = 4; break;
            default: return E_NOTIMPL;
        }
    }
    if (first > buffer->bytes / element_size || count > buffer->bytes / element_size - first) return E_INVALIDARG;
    if (counter) {
        if (!VALID(counter, VKDU_BUFFER) || !vkdu_same_device(heap, counter) ||
            !(counter->resource_flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || (counter_offset & 4095) ||
            counter_offset > counter->bytes || 4 > counter->bytes - counter_offset) return E_INVALIDARG;
    } else if (counter_offset) return E_INVALIDARG;
    desc.Format = format; desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = first; desc.Buffer.NumElements = count; desc.Buffer.StructureByteStride = stride;
    desc.Buffer.CounterOffsetInBytes = counter_offset; desc.Buffer.Flags = flags;
    destination.ptr = (SIZE_T)(vkdu_heap_start(heap, 0) + (uint64_t)index * heap->descriptor_stride);
    ID3D12Device_CreateUnorderedAccessView(heap->owner, OBJ(ID3D12Resource, buffer),
            counter ? OBJ(ID3D12Resource, counter) : NULL, &desc, destination);
    heap->uav_records[index] = (struct vkdu_uav_record){buffer->identity, first, format, count, stride, flags};
    return ID3D12Device_GetDeviceRemovedReason(heap->owner);
}
int32_t vkdu_buffer_cbv(vkdu_object *heap, uint32_t index, vkdu_object *buffer, uint64_t offset, uint32_t bytes)
{
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE destination;
    if (!VALID(heap, VKDU_DESCRIPTOR_HEAP) || heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        index >= heap->descriptor_count || (offset & 255) || (bytes & 255) || bytes > 65536) return E_INVALIDARG;
    if (buffer) {
        if (!VALID(buffer, VKDU_BUFFER) || !vkdu_same_device(heap, buffer) || !bytes ||
            offset > buffer->bytes || bytes > buffer->bytes - offset) return E_INVALIDARG;
        desc.BufferLocation = vkdu_buffer_address(buffer) + offset;
        if (!desc.BufferLocation || (desc.BufferLocation & 255)) return E_INVALIDARG;
    } else if (offset) return E_INVALIDARG;
    /* A zero BufferLocation creates a real null descriptor. Never resolve it
     * to an arbitrary allocation or pass an absent description to vkd3d. */
    desc.SizeInBytes = bytes;
    destination.ptr = (SIZE_T)(vkdu_heap_start(heap, 0) + (uint64_t)index * heap->descriptor_stride);
    ID3D12Device_CreateConstantBufferView(heap->owner, &desc, destination);
    memset(&heap->uav_records[index], 0, sizeof(*heap->uav_records));
    return ID3D12Device_GetDeviceRemovedReason(heap->owner);
}
int32_t vkdu_buffer_srv(vkdu_object *heap, uint32_t index, vkdu_object *buffer,
        uint32_t format, uint64_t first, uint32_t count, uint32_t stride, uint32_t flags, uint32_t mapping)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE destination;
    uint32_t element_size, component;
    if (!VALID(heap, VKDU_DESCRIPTOR_HEAP) || heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        index >= heap->descriptor_count || !count || flags > D3D12_BUFFER_SRV_FLAG_RAW ||
        (mapping & ~0x1fffu) || !(mapping & D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES))
        return E_INVALIDARG;
    for (component = 0; component < 4; ++component)
        if (D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(component, mapping) > D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1)
            return E_INVALIDARG;
    /* The embedded engine applies component mappings to textures only.
     * Do not silently accept a buffer swizzle that it would discard. */
    if (mapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) return E_NOTIMPL;
    if (flags == D3D12_BUFFER_SRV_FLAG_RAW) {
        if (format != DXGI_FORMAT_R32_TYPELESS || stride) return E_INVALIDARG;
        element_size = 4;
    } else if (stride) {
        if (format != DXGI_FORMAT_UNKNOWN || (stride & 3) || stride > 2048) return E_INVALIDARG;
        element_size = stride;
    } else {
        switch (format) {
            case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R32_FLOAT: element_size = 4; break;
            default: return E_NOTIMPL;
        }
    }
    if (first > UINT64_MAX / element_size || count > UINT64_MAX / element_size - first) return E_INVALIDARG;
    if (buffer && (!VALID(buffer, VKDU_BUFFER) || !vkdu_same_device(heap, buffer) ||
        (buffer->resource_flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        first > buffer->bytes / element_size || count > buffer->bytes / element_size - first)) return E_INVALIDARG;
    /* A null resource plus a valid description is a real null table SRV.
     * Invalid non-null resources must not be silently converted to null. */
    desc.Format = format; desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = mapping;
    desc.Buffer.FirstElement = first; desc.Buffer.NumElements = count;
    desc.Buffer.StructureByteStride = stride; desc.Buffer.Flags = flags;
    destination.ptr = (SIZE_T)(vkdu_heap_start(heap, 0) + (uint64_t)index * heap->descriptor_stride);
    ID3D12Device_CreateShaderResourceView(heap->owner, buffer ? OBJ(ID3D12Resource, buffer) : NULL, &desc, destination);
    memset(&heap->uav_records[index], 0, sizeof(*heap->uav_records));
    return ID3D12Device_GetDeviceRemovedReason(heap->owner);
}
int32_t vkdu_descriptor_copy(vkdu_object *dst, uint32_t dst_index, vkdu_object *src, uint32_t src_index, uint32_t count)
{
    D3D12_CPU_DESCRIPTOR_HANDLE destination, source; HRESULT status;
    if (!VALID(dst, VKDU_DESCRIPTOR_HEAP) || !VALID(src, VKDU_DESCRIPTOR_HEAP) || !vkdu_same_device(dst, src) ||
        dst->heap_type != src->heap_type || src->shader_visible || dst_index > dst->descriptor_count ||
        count > dst->descriptor_count - dst_index || src_index > src->descriptor_count || count > src->descriptor_count - src_index ||
        (dst == src && count && dst_index < (uint64_t)src_index + count && src_index < (uint64_t)dst_index + count))
        return E_INVALIDARG;
    if (!count) return S_OK;
    destination.ptr = (SIZE_T)(vkdu_heap_start(dst, 0) + (uint64_t)dst_index * dst->descriptor_stride);
    source.ptr = (SIZE_T)(vkdu_heap_start(src, 0) + (uint64_t)src_index * src->descriptor_stride);
    ID3D12Device_CopyDescriptorsSimple(dst->owner, count, destination, source, dst->heap_type);
    if (dst->uav_records) memcpy(&dst->uav_records[dst_index], &src->uav_records[src_index],
            count * sizeof(*dst->uav_records));
    status = ID3D12Device_GetDeviceRemovedReason(dst->owner);
    vkdu_attachment_copy(dst, dst_index, src, src_index, count, SUCCEEDED(status));
    return status;
}

static int valid_descriptor_span(vkdu_device *device, uint32_t type,
        const struct vkdu_descriptor_span *span, int source)
{
    vkdu_object *heap = span->heap;
    if (!span->count) return 1;
    return VALID(heap, VKDU_DESCRIPTOR_HEAP) && heap->owner == device->object &&
            heap->heap_type == type && (!source || !heap->shader_visible) &&
            span->first < heap->descriptor_count && span->count <= heap->descriptor_count - span->first;
}

int32_t vkdu_descriptor_copy_ranges(vkdu_device *device, uint32_t type,
        uint32_t dst_count, const struct vkdu_descriptor_span *dst,
        uint32_t src_count, const struct vkdu_descriptor_span *src)
{
    uint64_t dst_total = 0, src_total = 0;
    uint32_t d, s, di = 0, si = 0;
    if (!device || type > D3D12_DESCRIPTOR_HEAP_TYPE_DSV ||
            (dst_count && !dst) || (src_count && !src)) return E_INVALIDARG;
    /* Validate the entire operation before writing the first descriptor. A
     * malformed late range must not partially overwrite the destination. */
    for (d = 0; d < dst_count; ++d) {
        if (!valid_descriptor_span(device, type, &dst[d], 0)) return E_INVALIDARG;
        dst_total += dst[d].count;
    }
    for (s = 0; s < src_count; ++s) {
        if (!valid_descriptor_span(device, type, &src[s], 1)) return E_INVALIDARG;
        src_total += src[s].count;
    }
    if (dst_total != src_total) return E_INVALIDARG;
    for (d = 0; d < dst_count; ++d)
        for (s = 0; s < src_count; ++s)
            if (dst[d].count && src[s].count && dst[d].heap == src[s].heap &&
                    dst[d].first < (uint64_t)src[s].first + src[s].count &&
                    src[s].first < (uint64_t)dst[d].first + dst[d].count) return E_INVALIDARG;
    /* Walk both flattened descriptor streams while retaining their independent
     * heap/range boundaries. Repeated source ranges are legal. */
    for (d = 0, s = 0; d < dst_count && s < src_count;) {
        D3D12_CPU_DESCRIPTOR_HANDLE destination, source;
        uint32_t count;
        if (di == dst[d].count) { ++d; di = 0; continue; }
        if (si == src[s].count) { ++s; si = 0; continue; }
        count = dst[d].count - di;
        if (count > src[s].count - si) count = src[s].count - si;
        destination.ptr = (SIZE_T)(vkdu_heap_start(dst[d].heap, 0) +
                ((uint64_t)dst[d].first + di) * dst[d].heap->descriptor_stride);
        source.ptr = (SIZE_T)(vkdu_heap_start(src[s].heap, 0) +
                ((uint64_t)src[s].first + si) * src[s].heap->descriptor_stride);
        ID3D12Device_CopyDescriptorsSimple(device->object, count, destination, source, type);
        if (dst[d].heap->uav_records) memcpy(&dst[d].heap->uav_records[dst[d].first + di],
                &src[s].heap->uav_records[src[s].first + si], count * sizeof(*dst[d].heap->uav_records));
        if (dst[d].heap->attachment_records)
            vkdu_attachment_copy(dst[d].heap, dst[d].first + di, src[s].heap, src[s].first + si, count,
                    SUCCEEDED(ID3D12Device_GetDeviceRemovedReason(device->object)));
        di += count; si += count;
    }
    return dst_total ? ID3D12Device_GetDeviceRemovedReason(device->object) : S_OK;
}
int32_t vkdu_command_heaps(vkdu_object *command, uint32_t count, vkdu_object *const *heaps)
{
    ID3D12DescriptorHeap *native[2]; uint64_t addresses[2] = {0}; uint32_t i;
    if (!RECORDING(command) || command->command_type == 3 || count > 2 || (count && !heaps)) return E_INVALIDARG;
    for (i = 0; i < count; ++i) {
        if (!VALID(heaps[i], VKDU_DESCRIPTOR_HEAP) || !heaps[i]->shader_visible ||
            !vkdu_same_device(command, heaps[i]) || heaps[i]->heap_type > 1 || addresses[heaps[i]->heap_type]) return E_INVALIDARG;
        addresses[heaps[i]->heap_type] = vkdu_heap_start(heaps[i], 1);
        native[i] = OBJ(ID3D12DescriptorHeap, heaps[i]);
    }
    /* Every heap bind in a bundle must match the executing primary. Two
     * different pairs can never satisfy that rule for a single execution. */
    if (command->command_type == 1 && (command->bundle_changed & VKDU_BUNDLE_HEAPS) &&
            memcmp(command->bound_heaps, addresses, sizeof(addresses))) return E_INVALIDARG;
    for (i = 0; i < count; ++i)
        if (FAILED(vkdu_bundle_hold_if_recording(command, heaps[i]))) return E_OUTOFMEMORY;
    command->bundle_changed |= VKDU_BUNDLE_HEAPS;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(OBJ(ID3D12GraphicsCommandList, command), count, native);
    memcpy(command->bound_heaps, addresses, sizeof(addresses));
    return S_OK;
}
static const struct vkdu_root_slot *vkdu_command_slot(vkdu_object *command, uint32_t index, int graphics)
{
    if (!RECORDING(command) || (graphics ? command->command_type > 1 : command->command_type == 3)) return NULL;
    if (index >= (graphics ? command->graphics_slot_count : command->slot_count)) return NULL;
    return graphics ? &command->graphics_slots[index] : &command->slots[index];
}
static int32_t vkdu_command_table_mode(vkdu_object *command, uint32_t index, vkdu_object *heap, uint32_t first, int graphics)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    const struct vkdu_root_slot *slot = vkdu_command_slot(command, index, graphics);
    int inherited = vkdu_bundle_inherits_root(command, graphics);
    if (index >= 64 || (!inherited && (!slot || slot->type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)) ||
        !VALID(heap, VKDU_DESCRIPTOR_HEAP) || !vkdu_same_device(command, heap) || !heap->shader_visible ||
        heap->heap_type > 1 || first >= heap->descriptor_count ||
        (!inherited && (heap->heap_type != slot->heap_type ||
         (!slot->unbounded && slot->extent > heap->descriptor_count - first))) ||
        (command->command_type != 1 && command->bound_heaps[heap->heap_type] != vkdu_heap_start(heap, 1))) return E_INVALIDARG;
    if (FAILED(vkdu_bundle_hold_if_recording(command, heap))) return E_OUTOFMEMORY;
    if (command->command_type == 1 && FAILED(vkdu_bundle_require(command, graphics, index,
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, heap->descriptor_count - first, heap->heap_type,
            vkdu_heap_start(heap, 1), inherited))) return E_OUTOFMEMORY;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_BUNDLE_TABLE_DROP_OFFSET)
    if (command->command_type == 1) first = 0;
#endif
    handle.ptr = vkdu_heap_start(heap, 1) + (uint64_t)first * heap->descriptor_stride;
    if (graphics) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(OBJ(ID3D12GraphicsCommandList, command), index, handle);
    else ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(OBJ(ID3D12GraphicsCommandList, command), index, handle);
    return S_OK;
}
int32_t vkdu_command_table(vkdu_object *command, uint32_t index, vkdu_object *heap, uint32_t first)
{ return vkdu_command_table_mode(command, index, heap, first, 0); }
int32_t vkdu_command_graphics_table(vkdu_object *command, uint32_t index, vkdu_object *heap, uint32_t first)
{ return vkdu_command_table_mode(command, index, heap, first, 1); }
int32_t vkdu_queue_create(vkdu_device *device, uint32_t type, vkdu_object **out)
{
    D3D12_COMMAND_QUEUE_DESC desc = {0}; ID3D12CommandQueue *queue = NULL; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || (type != 0 && type != 2 && type != 3)) return E_INVALIDARG;
    desc.Type = type;
    hr = ID3D12Device_CreateCommandQueue(device->object, &desc, &IID_ID3D12CommandQueue, (void **)&queue);
    hr = wrap(device, VKDU_QUEUE, hr, (IUnknown *)queue, out);
    if (SUCCEEDED(hr)) (*out)->command_type = type;
    return hr;
}
int32_t vkdu_queue_clock(vkdu_object *queue, uint64_t *gpu, uint64_t *cpu)
{
    if (!VALID(queue, VKDU_QUEUE) || !gpu || !cpu) return E_INVALIDARG;
    return ID3D12CommandQueue_GetClockCalibration(OBJ(ID3D12CommandQueue, queue), gpu, cpu);
}
int32_t vkdu_queue_bind_runtime(vkdu_object *queue, void *owner, void *token)
{
    if (!VALID(queue, VKDU_QUEUE)) return E_INVALIDARG;
    return vkd3d_wddm_queue_bind(OBJ(ID3D12CommandQueue, queue), owner, token);
}
int32_t vkdu_queue_drain_enqueue(vkdu_object *queue)
{
    if (!VALID(queue, VKDU_QUEUE)) return E_INVALIDARG;
    return vkd3d_wddm_queue_drain_enqueue(OBJ(ID3D12CommandQueue, queue));
}
int32_t vkdu_allocator_create(vkdu_device *device, uint32_t type, vkdu_object **out)
{
    ID3D12CommandAllocator *allocator = NULL; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || type > 3) return E_INVALIDARG;
    hr = ID3D12Device_CreateCommandAllocator(device->object, type, &IID_ID3D12CommandAllocator, (void **)&allocator);
    hr = wrap(device, VKDU_ALLOCATOR, hr, (IUnknown *)allocator, out);
    if (SUCCEEDED(hr)) (*out)->command_type = type;
    return hr;
}
int32_t vkdu_allocator_reset(vkdu_object *allocator)
{
    HRESULT hr;
    if (!VALID(allocator, VKDU_ALLOCATOR) || allocator->bundle_recordings || allocator->bundle_users)
        return E_INVALIDARG;
    /* The runtime signals application fences on its own queue context, so an
     * application can legally reset after its fence completes but before the
     * embedded fence worker has observed the same GPU completion. Give that
     * worker a bounded chance to retire, or the reset is skipped and the
     * allocator keeps every command buffer and upload chunk. */
    {
        ID3D12CommandAllocator *native = OBJ(ID3D12CommandAllocator, allocator);
        if (vkd3d_wddm_allocator_pending(native))
        {
            LARGE_INTEGER freq, start, now;
            unsigned int spins = 0;
            QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start);
            do {
                Sleep(spins++ < 16 ? 0 : 1);
                QueryPerformanceCounter(&now);
            } while (vkd3d_wddm_allocator_pending(native) && now.QuadPart - start.QuadPart < freq.QuadPart / 4);
            vkdu_trace("allocator reset waited %.3f ms pending=%u",
                1000.0 * (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart,
                vkd3d_wddm_allocator_pending(native));
        }
    }
    hr = ID3D12CommandAllocator_Reset(OBJ(ID3D12CommandAllocator, allocator));
    if (SUCCEEDED(hr)) ++allocator->allocator_generation;
    return hr;
}
int32_t vkdu_command_create(vkdu_device *device, vkdu_object *allocator, uint32_t type, vkdu_object **out)
{
    ID3D12GraphicsCommandList *command = NULL; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !VALID(allocator, VKDU_ALLOCATOR) || allocator->owner != device->object || allocator->command_type != type) return E_INVALIDARG;
    hr = ID3D12Device_CreateCommandList(device->object, 0, type, OBJ(ID3D12CommandAllocator, allocator), NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&command);
    hr = wrap(device, VKDU_COMMAND_LIST, hr, (IUnknown *)command, out);
    if (SUCCEEDED(hr)) {
        (*out)->command_type = type;
        vkdu_object_retain(allocator); (*out)->command_allocator = allocator;
        (*out)->bundle_generation = allocator->allocator_generation;
        if (type == 1) ++allocator->bundle_recordings;
    }
    return hr;
}
int32_t vkdu_command_native_present(vkdu_object *command, vkdu_object *queue,
        vkdu_object *source, vkdu_object *destination)
{
    if (!RECORDING(command) || command->command_type || !VALID(queue, VKDU_QUEUE) || queue->command_type ||
            queue->owner != command->owner || !VALID(source, VKDU_TEXTURE2D) || source->owner != command->owner ||
            (destination && (!VALID(destination, VKDU_TEXTURE2D) || destination->owner != command->owner ||
                destination == source))) return E_INVALIDARG;
    return vkd3d_wddm_command_prepare_present(OBJ(ID3D12GraphicsCommandList, command),
        OBJ(ID3D12Resource, source), destination ? OBJ(ID3D12Resource, destination) : NULL);
}

int32_t vkdu_command_close(vkdu_object *command)
{
    HRESULT hr;
    if (!RECORDING(command) || command->query_scope_count) return E_INVALIDARG;
    hr = ID3D12GraphicsCommandList_Close(OBJ(ID3D12GraphicsCommandList, command));
    if (SUCCEEDED(hr)) {
        command->closed = 1;
        if (command->command_type == 1) --command->command_allocator->bundle_recordings;
    }
    return hr;
}
int32_t vkdu_command_reset(vkdu_object *command, vkdu_object *allocator)
{
    HRESULT hr;
    if (!VALID(command, VKDU_COMMAND_LIST) || !command->closed || command->bundle_users || !VALID(allocator, VKDU_ALLOCATOR) ||
        !vkdu_same_device(command, allocator) || command->command_type != allocator->command_type) return E_INVALIDARG;
    hr = ID3D12GraphicsCommandList_Reset(OBJ(ID3D12GraphicsCommandList, command), OBJ(ID3D12CommandAllocator, allocator), NULL);
    if (SUCCEEDED(hr)) {
        vkdu_bundle_drop_owners(command);
        vkdu_object_retain(allocator); vkdu_object_destroy(command->command_allocator);
        command->command_allocator = allocator; command->bundle_generation = allocator->allocator_generation;
        if (command->command_type == 1) ++allocator->bundle_recordings;
        command->closed = 0; command->slot_count = command->graphics_slot_count = 0;
        memset(command->bound_heaps, 0, sizeof(command->bound_heaps));
        command->graphics_root = command->graphics_pipeline = command->topology = 0;
        command->pipeline_samples = command->pipeline_quality = command->attachment_samples = command->attachment_quality = 0;
        command->viewport_count = command->scissor_count = command->render_target = command->index_count = 0;
        command->query_scope_count = 0;
        command->predicate_enabled = 0;
    }
    return hr;
}
int32_t vkdu_command_clear_root_arguments(vkdu_object *command)
{
    if (!RECORDING(command)) return E_INVALIDARG;
    if (command->command_type == 1) return vkd3d_wddm_bundle_clear_root_arguments(OBJ(ID3D12GraphicsCommandList, command));
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_ROOT_CLEAR_DROP)
    return S_OK;
#else
    return vkd3d_wddm_command_clear_root_arguments(OBJ(ID3D12GraphicsCommandList, command));
#endif
}
#include "backend_queries.inc"
#include "backend_uav_clear.inc"
#include "backend_predication.inc"

int32_t vkdu_command_copy(vkdu_object *command, vkdu_object *dst, uint64_t dst_offset, vkdu_object *src, uint64_t src_offset, uint64_t bytes)
{
    if (!RECORDING(command) || command->command_type == 1 || !VALID(dst, VKDU_BUFFER) || !VALID(src, VKDU_BUFFER) ||
        !vkdu_same_device(command, dst) || !vkdu_same_device(command, src) || !bytes ||
        dst_offset > dst->bytes || bytes > dst->bytes - dst_offset ||
        src_offset > src->bytes || bytes > src->bytes - src_offset) return E_INVALIDARG;
    ID3D12GraphicsCommandList_CopyBufferRegion(OBJ(ID3D12GraphicsCommandList, command), OBJ(ID3D12Resource, dst), dst_offset,
            OBJ(ID3D12Resource, src), src_offset, bytes);
    return S_OK;
}
int32_t vkdu_command_transition(vkdu_object *command, vkdu_object *resource, uint32_t before, uint32_t after)
{
    const struct vkdu_resource_barrier barrier = {VKDU_BARRIER_TRANSITION, resource, before, after};
    return vkdu_command_barriers(command, 1, &barrier);
}
int32_t vkdu_command_barriers(vkdu_object *command, uint32_t count, const struct vkdu_resource_barrier *barriers)
{
    D3D12_RESOURCE_BARRIER stack[16], *native = stack;
    uint32_t i;
    HRESULT hr;
    if (!RECORDING(command) || command->command_type == 1 || count > 65536 || (count && !barriers)) return E_INVALIDARG;
    /* Validate all resources, command types and UAV flags before issuing even
     * the first barrier. A rejected late element cannot alter recording. */
    for (i = 0; i < count; ++i) {
        vkdu_object *resource = barriers[i].resource;
        if (resource && ((!VALID(resource, VKDU_BUFFER) && !VALID(resource, VKDU_TEXTURE2D)) ||
                !vkdu_same_device(command, resource))) return E_INVALIDARG;
        if (barriers[i].type == VKDU_BARRIER_TRANSITION) {
            if (!resource || barriers[i].alias_after) return E_INVALIDARG;
            if (barriers[i].subresource_plus_one) {
                D3D12_RESOURCE_DESC desc = ID3D12Resource_GetDesc(OBJ(ID3D12Resource, resource));
                uint64_t subresources = (uint64_t)desc.MipLevels * desc.DepthOrArraySize;
                if (vkdu_stencil_format(desc.Format)) subresources *= 2;
                if (!subresources || subresources > UINT32_MAX || barriers[i].subresource_plus_one > subresources)
                    return E_INVALIDARG;
            }
        } else if (barriers[i].type == VKDU_BARRIER_UAV) {
            if (command->command_type == D3D12_COMMAND_LIST_TYPE_COPY ||
                barriers[i].before || barriers[i].after || barriers[i].alias_after || barriers[i].subresource_plus_one ||
                (resource && !(resource->resource_flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) return E_INVALIDARG;
        } else if (barriers[i].type == VKDU_BARRIER_ALIASING) {
            vkdu_object *after = barriers[i].alias_after;
            if (barriers[i].before || barriers[i].after || barriers[i].subresource_plus_one ||
                    (after && ((!VALID(after, VKDU_BUFFER) && !VALID(after, VKDU_TEXTURE2D)) ||
                     !vkdu_same_device(command, after)))) return E_INVALIDARG;
            /* Image activation/initialization has a separate contract. Keep
             * it closed until the native texture alias path is implemented. */
            if ((resource && !VALID(resource, VKDU_BUFFER)) || (after && !VALID(after, VKDU_BUFFER))) return E_NOTIMPL;
        } else return E_INVALIDARG;
    }
    if (!count) return S_OK;
    if (count > sizeof(stack) / sizeof(stack[0]) && !(native = calloc(count, sizeof(*native)))) return E_OUTOFMEMORY;
    for (i = 0; i < count; ++i) {
        memset(&native[i], 0, sizeof(native[i]));
        if (barriers[i].type == VKDU_BARRIER_TRANSITION) {
            native[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            native[i].Transition.pResource = OBJ(ID3D12Resource, barriers[i].resource);
            native[i].Transition.Subresource = barriers[i].subresource_plus_one ?
                barriers[i].subresource_plus_one - 1 : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            native[i].Transition.StateBefore = barriers[i].before;
            native[i].Transition.StateAfter = barriers[i].after;
        } else if (barriers[i].type == VKDU_BARRIER_UAV) {
            native[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            native[i].UAV.pResource = barriers[i].resource ? OBJ(ID3D12Resource, barriers[i].resource) : NULL;
        } else {
            native[i].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            native[i].Aliasing.pResourceBefore = barriers[i].resource ? OBJ(ID3D12Resource, barriers[i].resource) : NULL;
            native[i].Aliasing.pResourceAfter = barriers[i].alias_after ? OBJ(ID3D12Resource, barriers[i].alias_after) : NULL;
        }
    }
    /* Retain barrier-only references until legal allocator reset/completion.
     * Validation/allocation precedes retention and all retention precedes GPU
     * commands. An OOM may leave harmless extra owners until reset, never a
     * partially recorded batch or an unretained alias endpoint. */
    for (i = 0; i < count; ++i) {
        ID3D12Resource *owners[2] = {
            barriers[i].resource ? OBJ(ID3D12Resource, barriers[i].resource) : NULL,
            barriers[i].alias_after ? OBJ(ID3D12Resource, barriers[i].alias_after) : NULL};
#if !defined(VKDU_ENABLE_TEST_DEVICE) || !defined(VKDU_TEST_ALIAS_DROP_OWNER)
        hr = vkd3d_wddm_command_retain_resources(OBJ(ID3D12GraphicsCommandList, command), 2, owners);
#else
        hr = S_OK; (void)owners;
#endif
        if (FAILED(hr)) { if (native != stack) free(native); return hr; }
    }
    ID3D12GraphicsCommandList_ResourceBarrier(OBJ(ID3D12GraphicsCommandList, command), count, native);
    if (native != stack) free(native);
    return S_OK;
}
int32_t vkdu_root_create(vkdu_device *device, const struct vkdu_root_parameter *parameters, uint32_t count, uint32_t flags, vkdu_object **out)
{
    return vkdu_root_create_samplers(device, parameters, count, flags, NULL, 0, out);
}
int32_t vkdu_root_create_samplers(vkdu_device *device, const struct vkdu_root_parameter *parameters,
        uint32_t count, uint32_t flags, const struct vkdu_static_sampler *samplers,
        uint32_t sampler_count, vkdu_object **out)
{
    D3D12_ROOT_PARAMETER native[64] = {{0}}; D3D12_ROOT_SIGNATURE_DESC desc = {0};
    ID3DBlob *blob = NULL, *error = NULL; ID3D12RootSignature *root = NULL; HRESULT hr; uint32_t i, j, total = 0, used = 0, cost = 0;
    D3D12_DESCRIPTOR_RANGE *ranges = NULL;
    D3D12_STATIC_SAMPLER_DESC *static_samplers = NULL;
    struct vkdu_root_slot slots[64] = {{0}};
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || count > 64 || (count && !parameters) || sampler_count > 2032 ||
            (sampler_count && !samplers)) return E_INVALIDARG;
    for (i = 0; i < count; ++i) {
        uint32_t words;
        if (parameters[i].type > 4 || parameters[i].visibility > 5) return E_INVALIDARG;
        words = parameters[i].type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ? 1 :
                parameters[i].type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ? parameters[i].constant_count : 2;
        if (!words || words > 64 - cost) return E_INVALIDARG;
        cost += words;
        if (parameters[i].type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
        if (!parameters[i].range_count || !parameters[i].ranges || parameters[i].range_count > 4096 - total) return E_INVALIDARG;
        total += parameters[i].range_count;
    }
    if (total && !(ranges = calloc(total, sizeof(*ranges)))) return E_OUTOFMEMORY;
    for (i = 0; i < count; ++i) {
        slots[i].type = parameters[i].type;
        native[i].ParameterType = parameters[i].type; native[i].ShaderVisibility = parameters[i].visibility;
        if (parameters[i].type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            uint64_t append = 0, extent = 0;
            native[i].DescriptorTable.NumDescriptorRanges = parameters[i].range_count;
            native[i].DescriptorTable.pDescriptorRanges = ranges + used;
            slots[i].heap_type = parameters[i].ranges[0].type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            for (j = 0; j < parameters[i].range_count; ++j) {
                const struct vkdu_descriptor_range *r = &parameters[i].ranges[j];
                uint64_t offset = r->offset == UINT32_MAX ? append : r->offset;
                if (r->type > 3 || !r->count || (r->type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) != slots[i].heap_type ||
                    offset >= UINT32_MAX || (r->count != UINT32_MAX && r->count > UINT32_MAX - offset)) {
                    free(ranges); return E_INVALIDARG;
                }
                ranges[used].RangeType = r->type; ranges[used].NumDescriptors = r->count;
                ranges[used].BaseShaderRegister = r->shader_register; ranges[used].RegisterSpace = r->register_space;
                ranges[used++].OffsetInDescriptorsFromTableStart = r->offset;
                append = r->count == UINT32_MAX ? UINT32_MAX : offset + r->count;
                if (r->count == UINT32_MAX) slots[i].unbounded = 1;
                if (append > extent) extent = append;
            }
            slots[i].extent = (uint32_t)extent;
        } else if (parameters[i].type == 1) {
            slots[i].extent = parameters[i].constant_count;
            native[i].Constants.ShaderRegister = parameters[i].shader_register;
            native[i].Constants.RegisterSpace = parameters[i].register_space;
            native[i].Constants.Num32BitValues = parameters[i].constant_count;
        } else {
            native[i].Descriptor.ShaderRegister = parameters[i].shader_register;
            native[i].Descriptor.RegisterSpace = parameters[i].register_space;
        }
    }
    if (sampler_count) {
        if (!(static_samplers = calloc(sampler_count, sizeof(*static_samplers)))) {
            free(ranges); return E_OUTOFMEMORY;
        }
        hr = vkdu_static_samplers_translate(parameters, count, samplers, sampler_count, static_samplers);
        if (FAILED(hr)) { free(static_samplers); free(ranges); return hr; }
    }
    desc.NumParameters = count; desc.pParameters = native; desc.Flags = flags;
    desc.NumStaticSamplers = sampler_count; desc.pStaticSamplers = static_samplers;
    hr = vkd3d_serialize_root_signature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    free(static_samplers);
    free(ranges);
    if (error) ID3D10Blob_Release(error);
    if (FAILED(hr)) return hr;
    hr = ID3D12Device_CreateRootSignature(device->object, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&root);
    ID3D10Blob_Release(blob);
    hr = wrap(device, VKDU_ROOT, hr, (IUnknown *)root, out);
    if (SUCCEEDED(hr)) { (*out)->slot_count = count; memcpy((*out)->slots, slots, sizeof(slots)); }
    return hr;
}
int32_t vkdu_pipeline_create(vkdu_device *device, vkdu_object *root, const void *code, size_t size, vkdu_object **out)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {0}; ID3D12PipelineState *pipeline = NULL; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !VALID(root, VKDU_ROOT) || root->owner != device->object || !code || !size) return E_INVALIDARG;
    desc.pRootSignature = OBJ(ID3D12RootSignature, root); desc.CS.pShaderBytecode = code; desc.CS.BytecodeLength = size;
    hr = ID3D12Device_CreateComputePipelineState(device->object, &desc, &IID_ID3D12PipelineState, (void **)&pipeline);
    return wrap(device, VKDU_PIPELINE, hr, (IUnknown *)pipeline, out);
}
int32_t vkdu_command_root(vkdu_object *command, vkdu_object *root)
{
    if (!RECORDING(command) || (command->command_type != 0 && command->command_type != 1 && command->command_type != 2) ||
            (root && (!VALID(root, VKDU_ROOT) || !vkdu_same_device(command, root)))) return E_INVALIDARG;
    if (FAILED(vkdu_bundle_hold_if_recording(command, root))) return E_OUTOFMEMORY;
    command->bundle_changed |= VKDU_BUNDLE_COMPUTE_ROOT;
    /* NULL is the API/DDI unbind operation. The embedded implementation clears
     * its signature/static sampler and invalidates state on the next bind. */
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_COMPUTE_UNBIND_DROP)
    if (root)
#endif
    ID3D12GraphicsCommandList_SetComputeRootSignature(OBJ(ID3D12GraphicsCommandList, command),
            root ? OBJ(ID3D12RootSignature, root) : NULL);
    command->slot_count = root ? root->slot_count : 0;
    if (root) memcpy(command->slots, root->slots, sizeof(root->slots));
    else memset(command->slots, 0, sizeof(command->slots));
    return S_OK;
}
/* Native SM5 compute DDIs receive SHEX tokens, not an application DXBC blob. */
void vkd3d_compute_dxbc_checksum(const void *dxbc, size_t size, uint32_t checksum[4]);
int32_t vkdu_pipeline_create_tokens(vkdu_device *device, vkdu_object *root, const uint32_t *tokens, uint32_t words, vkdu_object **out)
{
    uint32_t *container;
    size_t bytes;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!tokens || words < 2 || words > 1024 * 1024 || tokens[1] != words || (tokens[0] >> 16) != 5) return E_INVALIDARG;
    bytes = (size_t)(11 + words) * 4;
    if (!(container = calloc(1, bytes))) return E_OUTOFMEMORY;
    container[0] = 0x43425844; container[5] = 1; container[6] = (uint32_t)bytes;
    container[7] = 1; container[8] = 36; container[9] = 0x58454853; container[10] = words * 4;
    memcpy(container + 11, tokens, words * 4);
    vkd3d_compute_dxbc_checksum(container, bytes, container + 1);
    hr = vkdu_pipeline_create(device, root, container, bytes, out);
    free(container);
    return hr;
}
int32_t vkdu_command_pipeline(vkdu_object *command, vkdu_object *pipeline)
{
    if (!RECORDING(command) || !VALID(pipeline, VKDU_PIPELINE) || !vkdu_same_device(command, pipeline) ||
            (command->command_type == 1 && !pipeline->graphics_pipeline)) return E_INVALIDARG;
    if (FAILED(vkdu_bundle_hold_if_recording(command, pipeline))) return E_OUTOFMEMORY;
    command->bundle_changed |= VKDU_BUNDLE_PIPELINE;
    ID3D12GraphicsCommandList_SetPipelineState(OBJ(ID3D12GraphicsCommandList, command), OBJ(ID3D12PipelineState, pipeline));
    command->graphics_pipeline = pipeline->graphics_pipeline;
    command->pipeline_samples = pipeline->pipeline_samples; command->pipeline_quality = pipeline->pipeline_quality;
    return S_OK;
}
#include "backend_graphics.inc"
#include "backend_depth.inc"
#include "backend_format.inc"
static int32_t vkdu_command_root_buffer(vkdu_object *command, uint32_t index, vkdu_object *buffer,
        uint64_t offset, uint32_t type, int graphics)
{
    uint64_t address;
    const uint64_t mask = type == D3D12_ROOT_PARAMETER_TYPE_CBV ? 255 : 3;
    const struct vkdu_root_slot *slot = vkdu_command_slot(command, index, graphics);
    int inherited = vkdu_bundle_inherits_root(command, graphics);
    if (index >= 64 || (!inherited && (!slot || slot->type != type)) || (offset & mask) ||
        !VALID(buffer, VKDU_BUFFER) || !vkdu_same_device(command, buffer) || offset >= buffer->bytes ||
        (type == D3D12_ROOT_PARAMETER_TYPE_SRV && (buffer->resource_flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) ||
        (type == D3D12_ROOT_PARAMETER_TYPE_UAV && !(buffer->resource_flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
        return E_INVALIDARG;
    /* Root descriptors are raw GPU addresses, with no descriptor bounds or
     * null-read guarantee. Only forward addresses in a live owned buffer. */
    address = vkdu_buffer_address(buffer);
    if (!address || offset > UINT64_MAX - address || ((address + offset) & mask)) return E_INVALIDARG;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_GRAPHICS_ROOT_DROP_OFFSET)
    if (graphics) offset = 0;
#endif
    if (FAILED(vkdu_bundle_hold_if_recording(command, buffer))) return E_OUTOFMEMORY;
    if (inherited && FAILED(vkdu_bundle_require(command, graphics, index, type, 0, 0, 0, 1))) return E_OUTOFMEMORY;
    address += offset;
    if (type == D3D12_ROOT_PARAMETER_TYPE_CBV) {
        if (graphics) ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(OBJ(ID3D12GraphicsCommandList, command), index, address);
        else ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(OBJ(ID3D12GraphicsCommandList, command), index, address);
    } else if (type == D3D12_ROOT_PARAMETER_TYPE_SRV) {
        if (graphics) ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(OBJ(ID3D12GraphicsCommandList, command), index, address);
        else ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(OBJ(ID3D12GraphicsCommandList, command), index, address);
    } else {
        if (graphics) ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(OBJ(ID3D12GraphicsCommandList, command), index, address);
        else ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(OBJ(ID3D12GraphicsCommandList, command), index, address);
    }
    return S_OK;
}
int32_t vkdu_command_uav(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset)
{ return vkdu_command_root_buffer(command, index, buffer, offset, D3D12_ROOT_PARAMETER_TYPE_UAV, 0); }
int32_t vkdu_command_cbv(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset)
{ return vkdu_command_root_buffer(command, index, buffer, offset, D3D12_ROOT_PARAMETER_TYPE_CBV, 0); }
int32_t vkdu_command_srv(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset)
{ return vkdu_command_root_buffer(command, index, buffer, offset, D3D12_ROOT_PARAMETER_TYPE_SRV, 0); }
int32_t vkdu_command_graphics_uav(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset)
{ return vkdu_command_root_buffer(command, index, buffer, offset, D3D12_ROOT_PARAMETER_TYPE_UAV, 1); }
int32_t vkdu_command_graphics_cbv(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset)
{ return vkdu_command_root_buffer(command, index, buffer, offset, D3D12_ROOT_PARAMETER_TYPE_CBV, 1); }
int32_t vkdu_command_graphics_srv(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset)
{ return vkdu_command_root_buffer(command, index, buffer, offset, D3D12_ROOT_PARAMETER_TYPE_SRV, 1); }
int32_t vkdu_command_dispatch(vkdu_object *command, uint32_t x, uint32_t y, uint32_t z)
{
    if (!RECORDING(command) || command->command_type == 1 || command->command_type == 3 || x > 65535 || y > 65535 || z > 65535) return E_INVALIDARG;
    ID3D12GraphicsCommandList_Dispatch(OBJ(ID3D12GraphicsCommandList, command), x, y, z); return S_OK;
}
int32_t vkdu_dispatch_signature_create(vkdu_device *device, uint32_t stride, vkdu_object **out)
{
    D3D12_INDIRECT_ARGUMENT_DESC argument = {0};
    D3D12_COMMAND_SIGNATURE_DESC desc = {0};
    ID3D12CommandSignature *signature = NULL;
    HRESULT hr;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!device || !device->object || stride < sizeof(D3D12_DISPATCH_ARGUMENTS) || (stride & 3)) return E_INVALIDARG;
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    desc.ByteStride = stride; desc.NumArgumentDescs = 1; desc.pArgumentDescs = &argument;
    hr = ID3D12Device_CreateCommandSignature(device->object, &desc, NULL, &IID_ID3D12CommandSignature, (void **)&signature);
    hr = wrap(device, VKDU_COMMAND_SIGNATURE, hr, (IUnknown *)signature, out);
    if (SUCCEEDED(hr)) (*out)->bytes = stride;
    return hr;
}
int32_t vkdu_command_execute_indirect(vkdu_object *command, vkdu_object *signature, uint32_t maximum,
        vkdu_object *arguments, uint64_t argument_offset, vkdu_object *count, uint64_t count_offset)
{
    if (!RECORDING(command) || command->command_type == 1 || command->command_type == D3D12_COMMAND_LIST_TYPE_COPY ||
            !VALID(signature, VKDU_COMMAND_SIGNATURE) || !vkdu_same_device(command, signature) ||
            !VALID(arguments, VKDU_BUFFER) || !vkdu_same_device(command, arguments) ||
            (argument_offset & 3) || argument_offset > arguments->bytes ||
            (uint64_t)maximum * signature->bytes > arguments->bytes - argument_offset ||
            (count && (!VALID(count, VKDU_BUFFER) || !vkdu_same_device(command, count) ||
                (count_offset & 3) || count_offset > count->bytes || sizeof(uint32_t) > count->bytes - count_offset)) ||
            (!count && count_offset)) return E_INVALIDARG;
    /* Validation precedes recording. Count is clamped on GPU by the embedded
     * engine; CPU emulation would break GPU-produced arguments and ordering. */
    if (maximum) ID3D12GraphicsCommandList_ExecuteIndirect(OBJ(ID3D12GraphicsCommandList, command),
            OBJ(ID3D12CommandSignature, signature), maximum, OBJ(ID3D12Resource, arguments), argument_offset,
            count ? OBJ(ID3D12Resource, count) : NULL, count_offset);
    return S_OK;
}
static int32_t vkdu_command_constants_mode(vkdu_object *command, uint32_t index, uint32_t offset,
        uint32_t count, const uint32_t *values, int graphics)
{
    const struct vkdu_root_slot *slot = vkdu_command_slot(command, index, graphics);
    int inherited = vkdu_bundle_inherits_root(command, graphics);
    if (index >= 64 || count > 64 || offset > 64 - count ||
        (!inherited && (!slot || slot->type != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ||
         offset > slot->extent || count > slot->extent - offset)) || offset > UINT32_MAX - count ||
        (count && !values)) return E_INVALIDARG;
    if (inherited && FAILED(vkdu_bundle_require(command, graphics, index,
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, offset + count, 0, 0, 1))) return E_OUTOFMEMORY;
    /* The backend records an immediate copy. No caller pointer survives this
     * call, and validation completes before any root constants can change. */
    if (count) {
#ifdef VKDU_TEST_TESS_DROP_HULL_CONSTANTS
        uint32_t replacement[64], i;
        if (graphics && (command->graphics_pipeline & 4)) {
            for (i = 0; i < count && i < 64; ++i) replacement[i] = 0x3f800000;
            values = replacement;
        }
#endif
        if (graphics) ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(
                OBJ(ID3D12GraphicsCommandList, command), index, count, values, offset);
        else ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(
                OBJ(ID3D12GraphicsCommandList, command), index, count, values, offset);
    }
    return S_OK;
}
int32_t vkdu_command_constants(vkdu_object *command, uint32_t index, uint32_t offset, uint32_t count, const uint32_t *values)
{ return vkdu_command_constants_mode(command, index, offset, count, values, 0); }
int32_t vkdu_command_graphics_constants(vkdu_object *command, uint32_t index, uint32_t offset, uint32_t count, const uint32_t *values)
{ return vkdu_command_constants_mode(command, index, offset, count, values, 1); }
int32_t vkdu_queue_execute(vkdu_object *queue, uint32_t count, vkdu_object *const *commands)
{
    ID3D12CommandList *native[64]; uint32_t i;
    HRESULT hr;
    if (!VALID(queue, VKDU_QUEUE) || !count || count > 64 || !commands) return E_INVALIDARG;
    for (i = 0; i < count; ++i) {
        if (!VALID(commands[i], VKDU_COMMAND_LIST) || !commands[i]->closed || !vkdu_same_device(queue, commands[i]) ||
            commands[i]->command_type != queue->command_type) return E_INVALIDARG;
        native[i] = OBJ(ID3D12CommandList, commands[i]);
    }
    ID3D12CommandQueue_ExecuteCommandLists(OBJ(ID3D12CommandQueue, queue), count, native);
    if (FAILED(hr = vkdu_queue_drain_enqueue(queue))) return hr;
    return ID3D12Device_GetDeviceRemovedReason(queue->owner);
}
int32_t vkdu_fence_create(vkdu_device *device, uint64_t initial, vkdu_object **out)
{
    ID3D12Fence *fence = NULL; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || initial == UINT64_MAX) return E_INVALIDARG;
    hr = ID3D12Device_CreateFence(device->object, initial, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    return wrap(device, VKDU_FENCE, hr, (IUnknown *)fence, out);
}
int32_t vkdu_queue_signal(vkdu_object *queue, vkdu_object *fence, uint64_t value)
{
    HRESULT hr;
    if (!VALID(queue, VKDU_QUEUE) || !VALID(fence, VKDU_FENCE) || !vkdu_same_device(queue, fence) || value == UINT64_MAX) return E_INVALIDARG;
    if (FAILED(hr = ID3D12Device_GetDeviceRemovedReason(queue->owner))) return hr;
    return ID3D12CommandQueue_Signal(OBJ(ID3D12CommandQueue, queue), OBJ(ID3D12Fence, fence), value);
}
int32_t vkdu_queue_wait(vkdu_object *queue, vkdu_object *fence, uint64_t value)
{
    HRESULT hr;
    if (!VALID(queue, VKDU_QUEUE) || !VALID(fence, VKDU_FENCE) || !vkdu_same_device(queue, fence) || value == UINT64_MAX) return E_INVALIDARG;
    if (FAILED(hr = ID3D12Device_GetDeviceRemovedReason(queue->owner))) return hr;
#ifdef VKDU_TEST_FENCE_DROP_WAIT
    return S_OK; /* Semantic negative-control binary only. */
#endif
    return ID3D12CommandQueue_Wait(OBJ(ID3D12CommandQueue, queue), OBJ(ID3D12Fence, fence), value);
}
int32_t vkdu_fence_completed(vkdu_object *fence, uint64_t *value)
{
    HRESULT hr;
    if (!VALID(fence, VKDU_FENCE) || !value) return E_INVALIDARG;
    *value = ID3D12Fence_GetCompletedValue(OBJ(ID3D12Fence, fence));
    if (*value != UINT64_MAX) return S_OK;
    hr = ID3D12Device_GetDeviceRemovedReason(fence->owner);
    return FAILED(hr) ? hr : DXGI_ERROR_DEVICE_REMOVED;
}
int32_t vkdu_fence_signal_cpu(vkdu_object *fence, uint64_t value)
{
    HRESULT hr;
    if (!VALID(fence, VKDU_FENCE) || value == UINT64_MAX) return E_INVALIDARG;
    if (FAILED(hr = ID3D12Device_GetDeviceRemovedReason(fence->owner))) return hr;
    return ID3D12Fence_Signal(OBJ(ID3D12Fence, fence), value);
}
int32_t vkdu_fence_event_create(vkdu_object *fence, uint64_t value, vkdu_fence_event **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!VALID(fence, VKDU_FENCE)) return E_INVALIDARG;
    return vkd3d_wddm_fence_event_create(OBJ(ID3D12Fence, fence), value, out);
}
int32_t vkdu_fence_event_wait(vkdu_fence_event *event, uint32_t timeout_ms)
{ return vkd3d_wddm_fence_event_wait(event, timeout_ms); }
void vkdu_fence_event_cancel(vkdu_fence_event *event)
{ vkd3d_wddm_fence_event_cancel(event); }
void vkdu_fence_event_destroy(vkdu_fence_event *event)
{ vkd3d_wddm_fence_event_destroy(event); }
int32_t vkdu_fence_wait(vkdu_object *fence, uint64_t value, uint32_t timeout_ms)
{
    vkdu_fence_event *event = NULL;
    HRESULT hr = vkdu_fence_event_create(fence, value, &event);
    if (FAILED(hr)) return hr;
    hr = vkdu_fence_event_wait(event, timeout_ms);
    vkdu_fence_event_destroy(event);
    return hr;
}
