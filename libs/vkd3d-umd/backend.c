/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "vkd3d.h"
#include "backend.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <time.h>
#endif

struct vkdu_device { ID3D12Device *object; };
struct vkdu_root_slot { uint32_t type, extent, heap_type, unbounded; };
struct vkdu_object {
    IUnknown *object;
    ID3D12Device *owner;
    enum vkdu_kind kind;
    uint64_t bytes;
    uint32_t heap_type, command_type;
    int closed;
    uint32_t resource_flags, descriptor_count, descriptor_stride;
    int shader_visible;
    /* Copy root layout at bind time: never retain a borrowed private wrapper. */
    struct vkdu_root_slot slots[64];
    uint32_t slot_count;
    uint64_t bound_heaps[2];
};

#define OBJ(type, o) ((type *)(o)->object)
#define VALID(o, k) ((o) && (o)->object && (o)->kind == (k))
#define RECORDING(o) (VALID(o, VKDU_COMMAND_LIST) && !(o)->closed)

static int32_t wrap(vkdu_device *device, enum vkdu_kind kind, HRESULT hr, IUnknown *object, vkdu_object **out)
{
    vkdu_object *value;
    if (FAILED(hr)) return hr;
    if (!(value = calloc(1, sizeof(*value)))) { IUnknown_Release(object); return E_OUTOFMEMORY; }
    value->object = object;
    value->owner = device->object;
    ID3D12Device_AddRef(value->owner);
    value->kind = kind;
    *out = value;
    return S_OK;
}

static int32_t create_device(PFN_vkGetInstanceProcAddr loader, const struct vkdu_adapter *adapter, int test_cpu, vkdu_device **out)
{
    struct vkd3d_instance_create_info instance_info = {0};
    struct vkd3d_device_create_info device_info = {0};
    PFN_vkEnumeratePhysicalDevices enumerate;
    PFN_vkGetPhysicalDeviceProperties2 properties;
    struct vkd3d_instance *instance = NULL;
    VkPhysicalDevice *devices = NULL, selected = VK_NULL_HANDLE;
    VkInstance vk_instance;
    uint32_t count = 0, i;
    vkdu_device *device = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!loader || (!test_cpu && !adapter)) return E_INVALIDARG;
    instance_info.pfn_vkGetInstanceProcAddr = loader;
    if (FAILED(hr = vkd3d_create_instance(&instance_info, &instance))) return hr;
    vk_instance = vkd3d_instance_get_vk_instance(instance);
    enumerate = (PFN_vkEnumeratePhysicalDevices)loader(vk_instance, "vkEnumeratePhysicalDevices");
    properties = (PFN_vkGetPhysicalDeviceProperties2)loader(vk_instance, "vkGetPhysicalDeviceProperties2");
    hr = DXGI_ERROR_NOT_FOUND;
    if (!enumerate || !properties || enumerate(vk_instance, &count, NULL) != VK_SUCCESS || !count) goto done;
    if (!(devices = calloc(count, sizeof(*devices)))) { hr = E_OUTOFMEMORY; goto done; }
    if (enumerate(vk_instance, &count, devices) != VK_SUCCESS) { hr = E_FAIL; goto done; }
    for (i = 0; i < count; ++i) {
        VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id; id.pNext = &driver;
        properties(devices[i], &props);
        if (test_cpu ? props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU :
            (id.deviceLUIDValid && !memcmp(id.deviceLUID, adapter->luid, 8) &&
             props.properties.vendorID == adapter->vendor_id && props.properties.deviceID == adapter->device_id &&
             driver.driverID == VK_DRIVER_ID_MESA_TURNIP)) {
            if (selected) { hr = E_INVALIDARG; goto done; } /* Never select ambiguously. */
            selected = devices[i];
        }
    }
    if (!selected) goto done;
    if (!(device = calloc(1, sizeof(*device)))) { hr = E_OUTOFMEMORY; goto done; }
    device_info.instance = instance;
    device_info.vk_physical_device = selected;
    device_info.minimum_feature_level = D3D_FEATURE_LEVEL_11_0;
    device_info.independent = true;
    if (adapter) memcpy(&device_info.adapter_luid, adapter->luid, 8);
    hr = vkd3d_create_device(&device_info, &IID_ID3D12Device, (void **)&device->object);
    if (SUCCEEDED(hr) && vkd3d_get_vk_physical_device(device->object) != selected) {
        ID3D12Device_Release(device->object); device->object = NULL; hr = DXGI_ERROR_NOT_FOUND;
    }
    if (SUCCEEDED(hr)) { *out = device; device = NULL; }
done:
    free(device); free(devices);
    vkd3d_instance_decref(instance);
    return hr;
}

int32_t vkdu_device_create(PFN_vkGetInstanceProcAddr loader, const struct vkdu_adapter *adapter, vkdu_device **out)
{ return create_device(loader, adapter, 0, out); }
#ifdef VKDU_ENABLE_TEST_DEVICE
int32_t vkdu_test_device_create(PFN_vkGetInstanceProcAddr loader, vkdu_device **out)
{ return create_device(loader, NULL, 1, out); }
#endif
void vkdu_device_destroy(vkdu_device *device)
{ if (device) { ID3D12Device_Release(device->object); free(device); } }
int32_t vkdu_device_status(vkdu_device *device)
{ return device ? ID3D12Device_GetDeviceRemovedReason(device->object) : E_INVALIDARG; }
void vkdu_object_destroy(vkdu_object *object)
{ if (object) { IUnknown_Release(object->object); ID3D12Device_Release(object->owner); free(object); } }
int vkdu_object_is(vkdu_object *object, enum vkdu_kind kind) { return VALID(object, kind); }
int vkdu_same_device(vkdu_object *a, vkdu_object *b) { return a && b && a->owner == b->owner; }
int vkdu_object_belongs(vkdu_device *device, vkdu_object *object) { return device && object && device->object == object->owner; }

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
    }
    return hr;
}
uint64_t vkdu_heap_start(vkdu_object *heap, int gpu)
{
    if (!VALID(heap, VKDU_DESCRIPTOR_HEAP)) return 0;
    if (gpu) return heap->shader_visible ? ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(OBJ(ID3D12DescriptorHeap, heap)).ptr : 0;
    return ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(OBJ(ID3D12DescriptorHeap, heap)).ptr;
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
    return ID3D12Device_GetDeviceRemovedReason(heap->owner);
}
int32_t vkdu_descriptor_copy(vkdu_object *dst, uint32_t dst_index, vkdu_object *src, uint32_t src_index, uint32_t count)
{
    D3D12_CPU_DESCRIPTOR_HANDLE destination, source;
    if (!VALID(dst, VKDU_DESCRIPTOR_HEAP) || !VALID(src, VKDU_DESCRIPTOR_HEAP) || !vkdu_same_device(dst, src) ||
        dst->heap_type != src->heap_type || src->shader_visible || dst_index > dst->descriptor_count ||
        count > dst->descriptor_count - dst_index || src_index > src->descriptor_count || count > src->descriptor_count - src_index ||
        (dst == src && count && dst_index < (uint64_t)src_index + count && src_index < (uint64_t)dst_index + count))
        return E_INVALIDARG;
    if (!count) return S_OK;
    destination.ptr = (SIZE_T)(vkdu_heap_start(dst, 0) + (uint64_t)dst_index * dst->descriptor_stride);
    source.ptr = (SIZE_T)(vkdu_heap_start(src, 0) + (uint64_t)src_index * src->descriptor_stride);
    ID3D12Device_CopyDescriptorsSimple(dst->owner, count, destination, source, dst->heap_type);
    return ID3D12Device_GetDeviceRemovedReason(dst->owner);
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
    ID3D12GraphicsCommandList_SetDescriptorHeaps(OBJ(ID3D12GraphicsCommandList, command), count, native);
    memcpy(command->bound_heaps, addresses, sizeof(addresses));
    return S_OK;
}
int32_t vkdu_command_table(vkdu_object *command, uint32_t index, vkdu_object *heap, uint32_t first)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    if (!RECORDING(command) || command->command_type == 3 || index >= command->slot_count ||
        command->slots[index].type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
        !VALID(heap, VKDU_DESCRIPTOR_HEAP) || !vkdu_same_device(command, heap) || !heap->shader_visible ||
        heap->heap_type > 1 || heap->heap_type != command->slots[index].heap_type || first >= heap->descriptor_count ||
        (!command->slots[index].unbounded && command->slots[index].extent > heap->descriptor_count - first) ||
        command->bound_heaps[heap->heap_type] != vkdu_heap_start(heap, 1)) return E_INVALIDARG;
    handle.ptr = vkdu_heap_start(heap, 1) + (uint64_t)first * heap->descriptor_stride;
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(OBJ(ID3D12GraphicsCommandList, command), index, handle);
    return S_OK;
}
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
int32_t vkdu_allocator_create(vkdu_device *device, uint32_t type, vkdu_object **out)
{
    ID3D12CommandAllocator *allocator = NULL; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || (type != 0 && type != 2 && type != 3)) return E_INVALIDARG;
    hr = ID3D12Device_CreateCommandAllocator(device->object, type, &IID_ID3D12CommandAllocator, (void **)&allocator);
    hr = wrap(device, VKDU_ALLOCATOR, hr, (IUnknown *)allocator, out);
    if (SUCCEEDED(hr)) (*out)->command_type = type;
    return hr;
}
int32_t vkdu_allocator_reset(vkdu_object *allocator)
{ return VALID(allocator, VKDU_ALLOCATOR) ? ID3D12CommandAllocator_Reset(OBJ(ID3D12CommandAllocator, allocator)) : E_INVALIDARG; }
int32_t vkdu_command_create(vkdu_device *device, vkdu_object *allocator, uint32_t type, vkdu_object **out)
{
    ID3D12GraphicsCommandList *command = NULL; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !VALID(allocator, VKDU_ALLOCATOR) || allocator->owner != device->object || allocator->command_type != type) return E_INVALIDARG;
    hr = ID3D12Device_CreateCommandList(device->object, 0, type, OBJ(ID3D12CommandAllocator, allocator), NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&command);
    hr = wrap(device, VKDU_COMMAND_LIST, hr, (IUnknown *)command, out);
    if (SUCCEEDED(hr)) (*out)->command_type = type;
    return hr;
}
int32_t vkdu_command_close(vkdu_object *command)
{
    HRESULT hr;
    if (!RECORDING(command)) return E_INVALIDARG;
    hr = ID3D12GraphicsCommandList_Close(OBJ(ID3D12GraphicsCommandList, command));
    if (SUCCEEDED(hr)) command->closed = 1;
    return hr;
}
int32_t vkdu_command_reset(vkdu_object *command, vkdu_object *allocator)
{
    HRESULT hr;
    if (!VALID(command, VKDU_COMMAND_LIST) || !command->closed || !VALID(allocator, VKDU_ALLOCATOR) ||
        !vkdu_same_device(command, allocator) || command->command_type != allocator->command_type) return E_INVALIDARG;
    hr = ID3D12GraphicsCommandList_Reset(OBJ(ID3D12GraphicsCommandList, command), OBJ(ID3D12CommandAllocator, allocator), NULL);
    if (SUCCEEDED(hr)) {
        command->closed = 0; command->slot_count = 0;
        memset(command->bound_heaps, 0, sizeof(command->bound_heaps));
    }
    return hr;
}
int32_t vkdu_command_copy(vkdu_object *command, vkdu_object *dst, uint64_t dst_offset, vkdu_object *src, uint64_t src_offset, uint64_t bytes)
{
    if (!RECORDING(command) || !VALID(dst, VKDU_BUFFER) || !VALID(src, VKDU_BUFFER) ||
        !vkdu_same_device(command, dst) || !vkdu_same_device(command, src) || !bytes ||
        dst_offset > dst->bytes || bytes > dst->bytes - dst_offset ||
        src_offset > src->bytes || bytes > src->bytes - src_offset) return E_INVALIDARG;
    ID3D12GraphicsCommandList_CopyBufferRegion(OBJ(ID3D12GraphicsCommandList, command), OBJ(ID3D12Resource, dst), dst_offset,
            OBJ(ID3D12Resource, src), src_offset, bytes);
    return S_OK;
}
int32_t vkdu_command_transition(vkdu_object *command, vkdu_object *resource, uint32_t before, uint32_t after)
{
    D3D12_RESOURCE_BARRIER barrier = {0};
    if (!RECORDING(command) || !VALID(resource, VKDU_BUFFER) || !vkdu_same_device(command, resource)) return E_INVALIDARG;
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = OBJ(ID3D12Resource, resource);
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(OBJ(ID3D12GraphicsCommandList, command), 1, &barrier);
    return S_OK;
}
int32_t vkdu_root_create(vkdu_device *device, const struct vkdu_root_parameter *parameters, uint32_t count, uint32_t flags, vkdu_object **out)
{
    D3D12_ROOT_PARAMETER native[64] = {{0}}; D3D12_ROOT_SIGNATURE_DESC desc = {0};
    ID3DBlob *blob = NULL, *error = NULL; ID3D12RootSignature *root = NULL; HRESULT hr; uint32_t i, j, total = 0, used = 0;
    D3D12_DESCRIPTOR_RANGE *ranges = NULL;
    struct vkdu_root_slot slots[64] = {{0}};
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || count > 64 || (count && !parameters)) return E_INVALIDARG;
    for (i = 0; i < count; ++i) {
        if (parameters[i].type > 4) return E_INVALIDARG;
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
            native[i].Constants.ShaderRegister = parameters[i].shader_register;
            native[i].Constants.RegisterSpace = parameters[i].register_space;
            native[i].Constants.Num32BitValues = parameters[i].constant_count;
        } else {
            native[i].Descriptor.ShaderRegister = parameters[i].shader_register;
            native[i].Descriptor.RegisterSpace = parameters[i].register_space;
        }
    }
    desc.NumParameters = count; desc.pParameters = native; desc.Flags = flags;
    hr = vkd3d_serialize_root_signature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
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
    if (!RECORDING(command) || !VALID(root, VKDU_ROOT) || !vkdu_same_device(command, root)) return E_INVALIDARG;
    ID3D12GraphicsCommandList_SetComputeRootSignature(OBJ(ID3D12GraphicsCommandList, command), OBJ(ID3D12RootSignature, root));
    command->slot_count = root->slot_count; memcpy(command->slots, root->slots, sizeof(root->slots)); return S_OK;
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
    if (!RECORDING(command) || !VALID(pipeline, VKDU_PIPELINE) || !vkdu_same_device(command, pipeline)) return E_INVALIDARG;
    ID3D12GraphicsCommandList_SetPipelineState(OBJ(ID3D12GraphicsCommandList, command), OBJ(ID3D12PipelineState, pipeline)); return S_OK;
}
int32_t vkdu_command_uav(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset)
{
    if (!RECORDING(command) || index >= command->slot_count || command->slots[index].type != D3D12_ROOT_PARAMETER_TYPE_UAV ||
        !VALID(buffer, VKDU_BUFFER) || !vkdu_same_device(command, buffer) || offset >= buffer->bytes || (offset & 3)) return E_INVALIDARG;
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(OBJ(ID3D12GraphicsCommandList, command), index, vkdu_buffer_address(buffer) + offset); return S_OK;
}
int32_t vkdu_command_dispatch(vkdu_object *command, uint32_t x, uint32_t y, uint32_t z)
{
    if (!RECORDING(command) || command->command_type == 3 || x > 65535 || y > 65535 || z > 65535) return E_INVALIDARG;
    ID3D12GraphicsCommandList_Dispatch(OBJ(ID3D12GraphicsCommandList, command), x, y, z); return S_OK;
}
int32_t vkdu_queue_execute(vkdu_object *queue, uint32_t count, vkdu_object *const *commands)
{
    ID3D12CommandList *native[64]; uint32_t i;
    if (!VALID(queue, VKDU_QUEUE) || !count || count > 64 || !commands) return E_INVALIDARG;
    for (i = 0; i < count; ++i) {
        if (!VALID(commands[i], VKDU_COMMAND_LIST) || !commands[i]->closed || !vkdu_same_device(queue, commands[i]) ||
            commands[i]->command_type != queue->command_type) return E_INVALIDARG;
        native[i] = OBJ(ID3D12CommandList, commands[i]);
    }
    ID3D12CommandQueue_ExecuteCommandLists(OBJ(ID3D12CommandQueue, queue), count, native);
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
    if (!VALID(queue, VKDU_QUEUE) || !VALID(fence, VKDU_FENCE) || !vkdu_same_device(queue, fence) || value == UINT64_MAX) return E_INVALIDARG;
    return ID3D12CommandQueue_Signal(OBJ(ID3D12CommandQueue, queue), OBJ(ID3D12Fence, fence), value);
}
int32_t vkdu_queue_wait(vkdu_object *queue, vkdu_object *fence, uint64_t value)
{
    if (!VALID(queue, VKDU_QUEUE) || !VALID(fence, VKDU_FENCE) || !vkdu_same_device(queue, fence) || value == UINT64_MAX) return E_INVALIDARG;
    return ID3D12CommandQueue_Wait(OBJ(ID3D12CommandQueue, queue), OBJ(ID3D12Fence, fence), value);
}
int32_t vkdu_fence_completed(vkdu_object *fence, uint64_t *value)
{
    if (!VALID(fence, VKDU_FENCE) || !value) return E_INVALIDARG;
    *value = ID3D12Fence_GetCompletedValue(OBJ(ID3D12Fence, fence));
    return *value == UINT64_MAX ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
}
static uint64_t milliseconds(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
#endif
}
int32_t vkdu_fence_wait(vkdu_object *fence, uint64_t value, uint32_t timeout_ms)
{
    uint64_t completed, start = milliseconds(); HRESULT hr;
    if (!VALID(fence, VKDU_FENCE) || value == UINT64_MAX) return E_INVALIDARG;
    do {
        if (FAILED(hr = vkdu_fence_completed(fence, &completed))) return hr;
        if (completed >= value) return S_OK;
        if (milliseconds() - start >= timeout_ms) return (int32_t)0x887a000a; /* WAS_STILL_DRAWING */
#ifdef _WIN32
        Sleep(1);
#else
        { struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL); }
#endif
    } while (1);
}
