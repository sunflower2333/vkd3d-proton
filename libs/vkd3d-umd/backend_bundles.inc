/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Bundle commands borrow engine COM pointers. Keep wrappers while recording,
 * then transfer COM owners to the primary's existing completion-retained set.
 * List/allocator operations retain the API's external synchronization rule. */
enum {
    VKDU_BUNDLE_GRAPHICS_ROOT = 1, VKDU_BUNDLE_COMPUTE_ROOT = 2,
    VKDU_BUNDLE_PIPELINE = 4, VKDU_BUNDLE_TOPOLOGY = 8,
    VKDU_BUNDLE_INDEX = 16, VKDU_BUNDLE_HEAPS = 32,
};
struct vkdu_bundle_owner { vkdu_object *object; struct vkdu_bundle_owner *next; };
struct vkdu_bundle_requirement {
    uint32_t graphics, index, type, extent, heap_type, inherited;
    uint64_t heap_address;
    struct vkdu_bundle_requirement *next;
};
static int vkdu_bundle_inherits_root(vkdu_object *command, int graphics)
{
    return RECORDING(command) && command->command_type == 1 &&
        !(command->bundle_changed & (graphics ? VKDU_BUNDLE_GRAPHICS_ROOT : VKDU_BUNDLE_COMPUTE_ROOT));
}
static HRESULT vkdu_bundle_hold(vkdu_object *command, vkdu_object *object)
{
    struct vkdu_bundle_owner *entry;
    if (!object) return S_OK;
    for (entry = command->bundle_owners; entry; entry = entry->next)
        if (entry->object == object) return S_OK;
    if (!(entry = malloc(sizeof(*entry)))) return E_OUTOFMEMORY;
    if (!vkdu_object_retain(object)) { free(entry); return E_INVALIDARG; }
    entry->object = object; entry->next = command->bundle_owners;
    command->bundle_owners = entry;
    if (object->kind == VKDU_COMMAND_LIST && object->command_type == 1) {
        ++object->bundle_users; ++object->command_allocator->bundle_users;
    }
    return S_OK;
}
static HRESULT vkdu_bundle_hold_if_recording(vkdu_object *command, vkdu_object *object)
{ return command->command_type == 1 ? vkdu_bundle_hold(command, object) : S_OK; }
static void vkdu_bundle_drop_owners(vkdu_object *command)
{
    struct vkdu_bundle_owner *entry;
    struct vkdu_bundle_requirement *requirement;
    while ((entry = command->bundle_owners)) {
        command->bundle_owners = entry->next;
        if (entry->object->kind == VKDU_COMMAND_LIST && entry->object->command_type == 1) {
            --entry->object->bundle_users; --entry->object->command_allocator->bundle_users;
        }
        vkdu_object_destroy(entry->object); free(entry);
    }
    while ((requirement = command->bundle_requirements)) {
        command->bundle_requirements = requirement->next; free(requirement);
    }
    command->bundle_changed = command->bundle_needs_root = command->bundle_needs_targets =
        command->bundle_inherited_indices = command->bundle_has_raster = 0;
    command->bundle_raster_samples = command->bundle_raster_quality = 0;
}
/* Keep the bundle's raster contract immutable. Validate and publish the tuple
 * before the draw reaches the engine, so a mismatch cannot leave a recorded
 * GPU draw without a matching ExecuteBundle validation entry. */
static HRESULT vkdu_bundle_record_raster(vkdu_object *command)
{
    if (command->command_type != 1 || (command->graphics_pipeline & 2)) return S_OK;
    if (!command->pipeline_samples) return E_INVALIDARG;
    if (command->bundle_has_raster)
        return command->bundle_raster_samples == command->pipeline_samples &&
            command->bundle_raster_quality == command->pipeline_quality ? S_OK : E_INVALIDARG;
    command->bundle_has_raster = 1;
    command->bundle_raster_samples = command->pipeline_samples;
    command->bundle_raster_quality = command->pipeline_quality;
    command->bundle_needs_targets = 1;
    return S_OK;
}
static HRESULT vkdu_bundle_require(vkdu_object *command, uint32_t graphics, uint32_t index,
        uint32_t type, uint32_t extent, uint32_t heap_type, uint64_t address, uint32_t inherited)
{
    struct vkdu_bundle_requirement *entry;
    if (!(entry = malloc(sizeof(*entry)))) return E_OUTOFMEMORY;
    *entry = (struct vkdu_bundle_requirement){graphics, index, type, extent, heap_type, inherited,
        address, command->bundle_requirements};
    command->bundle_requirements = entry;
    return S_OK;
}
static HRESULT vkdu_bundle_validate(vkdu_object *command, vkdu_object *bundle)
{
    struct vkdu_bundle_requirement *entry;
    if (bundle->bundle_needs_root && !command->graphics_root) return E_INVALIDARG;
    if (bundle->bundle_needs_targets && (!command->viewport_count || !command->scissor_count || !command->render_target ||
            !bundle->bundle_has_raster
#if !defined(VKDU_ENABLE_TEST_DEVICE) || !defined(VKDU_TEST_BUNDLE_DROP_SAMPLE_VALIDATION)
            || command->attachment_samples != bundle->bundle_raster_samples ||
            command->attachment_quality != bundle->bundle_raster_quality
#endif
            )) return E_INVALIDARG;
    if (bundle->bundle_inherited_indices > command->index_count) return E_INVALIDARG;
    if ((bundle->bundle_changed & VKDU_BUNDLE_HEAPS) &&
            memcmp(bundle->bound_heaps, command->bound_heaps, sizeof(command->bound_heaps))) return E_INVALIDARG;
    for (entry = bundle->bundle_requirements; entry; entry = entry->next) {
        const struct vkdu_root_slot *slots = entry->graphics ? command->graphics_slots : command->slots;
        uint32_t count = entry->graphics ? command->graphics_slot_count : command->slot_count;
        if (entry->heap_address && command->bound_heaps[entry->heap_type] != entry->heap_address) return E_INVALIDARG;
        if (!entry->inherited) continue;
        if (entry->index >= count || slots[entry->index].type != entry->type) return E_INVALIDARG;
        if (entry->type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS && slots[entry->index].extent < entry->extent)
            return E_INVALIDARG;
        if (entry->type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
                (slots[entry->index].heap_type != entry->heap_type ||
                 (!slots[entry->index].unbounded && slots[entry->index].extent > entry->extent))) return E_INVALIDARG;
    }
    return S_OK;
}
int32_t vkdu_command_execute_bundle(vkdu_object *command, vkdu_object *bundle)
{
    struct vkdu_bundle_owner *entry;
    IUnknown *object;
    HRESULT hr;
    if (!RECORDING(command) || command->command_type != 0 || !VALID(bundle, VKDU_COMMAND_LIST) ||
            bundle->command_type != 1 || !bundle->closed || !vkdu_same_device(command, bundle) ||
            !bundle->command_allocator || bundle->bundle_generation != bundle->command_allocator->allocator_generation)
        return E_INVALIDARG;
    if (FAILED(hr = vkdu_bundle_validate(command, bundle))) return hr;
    if (FAILED(hr = vkdu_bundle_hold(command, bundle))) return hr;
    /* Every pointer replayed by the engine survives parent list destruction and
     * native submission retirement, independently of the runtime's slots. */
    object = bundle->object;
    if (FAILED(hr = vkd3d_wddm_command_retain_objects(OBJ(ID3D12GraphicsCommandList, command), 1, &object))) return hr;
    object = bundle->command_allocator->object;
    if (FAILED(hr = vkd3d_wddm_command_retain_objects(OBJ(ID3D12GraphicsCommandList, command), 1, &object))) return hr;
    for (entry = bundle->bundle_owners; entry; entry = entry->next) {
        object = entry->object->object;
        if (FAILED(hr = vkd3d_wddm_command_retain_objects(OBJ(ID3D12GraphicsCommandList, command), 1, &object))) return hr;
    }
#if !defined(VKDU_ENABLE_TEST_DEVICE) || !defined(VKDU_TEST_BUNDLE_DROP_EXECUTE)
    ID3D12GraphicsCommandList_ExecuteBundle(OBJ(ID3D12GraphicsCommandList, command), OBJ(ID3D12GraphicsCommandList, bundle));
#endif
    if (bundle->bundle_changed & VKDU_BUNDLE_GRAPHICS_ROOT) {
        command->graphics_root = bundle->graphics_root; command->graphics_slot_count = bundle->graphics_slot_count;
        memcpy(command->graphics_slots, bundle->graphics_slots, sizeof(command->graphics_slots));
    }
    if (bundle->bundle_changed & VKDU_BUNDLE_COMPUTE_ROOT) {
        command->slot_count = bundle->slot_count; memcpy(command->slots, bundle->slots, sizeof(command->slots));
    }
#if !defined(VKDU_ENABLE_TEST_DEVICE) || !defined(VKDU_TEST_BUNDLE_DROP_STATE)
    if (bundle->bundle_changed & VKDU_BUNDLE_PIPELINE) {
        command->graphics_pipeline = bundle->graphics_pipeline;
        command->pipeline_samples = bundle->pipeline_samples;
        command->pipeline_quality = bundle->pipeline_quality;
    }
    if (bundle->bundle_changed & VKDU_BUNDLE_TOPOLOGY) command->topology = bundle->topology;
    if (bundle->bundle_changed & VKDU_BUNDLE_INDEX) command->index_count = bundle->index_count;
#endif
    return ID3D12Device_GetDeviceRemovedReason(command->owner);
}
