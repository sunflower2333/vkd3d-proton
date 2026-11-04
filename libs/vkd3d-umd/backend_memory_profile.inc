/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Included by backend.c. No embedded-device feature query or heap0 heuristic. */
int32_t vkdu_memory_facts_for_device(PFN_vkGetInstanceProcAddr loader, VkInstance instance,
        VkPhysicalDevice physical, const struct vkdu_memory_expectation *expected, struct vkdu_memory_facts *out)
{
    PFN_vkGetPhysicalDeviceProperties2 properties;
    PFN_vkGetPhysicalDeviceMemoryProperties2 memory;
    struct mwd_support support = {MWD_STYPE_SUPPORT_V2};
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceMemoryProperties2 mem = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    struct mwd_memory_profile_v1 raw = {MWD_STYPE_MEMORY_PROFILE_V1};
    struct vkdu_memory_facts result = {0};
    static const uint8_t zero_luid[8];
    const VkMemoryPropertyFlags witness = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    uint32_t i;
    if (!out) return E_POINTER;
    if (!loader || !physical || !expected || !expected->generation ||
            !memcmp(expected->luid, zero_luid, 8) || expected->cached_coherent.known > 1 ||
            expected->cached_coherent.value > expected->cached_coherent.known ||
            expected->owned_allocations_only.known > 1 ||
            expected->owned_allocations_only.value > expected->owned_allocations_only.known) return E_INVALIDARG;
    properties = (PFN_vkGetPhysicalDeviceProperties2)loader(instance, "vkGetPhysicalDeviceProperties2");
    memory = (PFN_vkGetPhysicalDeviceMemoryProperties2)loader(instance, "vkGetPhysicalDeviceMemoryProperties2");
    if (!properties || !memory) return DXGI_ERROR_UNSUPPORTED;
    props.pNext = &id; id.pNext = &driver; driver.pNext = &support;
    properties(physical, &props);
    if (props.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 || props.pNext != &id ||
            id.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES || id.pNext != &driver ||
            driver.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES || driver.pNext != &support ||
            support.sType != MWD_STYPE_SUPPORT_V2 || support.pNext || !id.deviceLUIDValid ||
            id.deviceNodeMask != 1 || driver.driverID != VK_DRIVER_ID_MESA_TURNIP ||
            support.magic != MWD_RUNTIME_MAGIC || support.version != MWD_RUNTIME_ABI_VERSION ||
            support.size != sizeof(struct mwd_callbacks) || support.flags != 1) return DXGI_ERROR_UNSUPPORTED;
    if (memcmp(id.deviceLUID, expected->luid, 8)) return DXGI_ERROR_DEVICE_REMOVED;
    mem.pNext = &raw;
    memory(physical, &mem);
    if (mem.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 || mem.pNext != &raw ||
            raw.sType != MWD_STYPE_MEMORY_PROFILE_V1 || raw.pNext || raw.magic != MWD_MEMORY_PROFILE_MAGIC ||
            raw.version != MWD_MEMORY_PROFILE_VERSION || raw.size != sizeof(raw) ||
            raw.profile != MWD_MEMORY_PROFILE_OWNED_RUNTIME2 || raw.reserved[0] || raw.reserved[1] ||
            (raw.known_mask & ~MWD_MEMORY_KNOWN_MASK) || (raw.value_mask & ~raw.known_mask) ||
            !raw.reset_generation || !memcmp(raw.luid, zero_luid, 8)) return DXGI_ERROR_UNSUPPORTED;
    if (memcmp(raw.luid, expected->luid, 8) || raw.reset_generation != expected->generation ||
            (expected->cached_coherent.known && (raw.known_mask & MWD_MEMORY_CACHED_COHERENT_AVAILABLE) &&
             !!(raw.value_mask & MWD_MEMORY_CACHED_COHERENT_AVAILABLE) != expected->cached_coherent.value))
        return DXGI_ERROR_DEVICE_REMOVED;
    if (!mem.memoryProperties.memoryTypeCount || mem.memoryProperties.memoryTypeCount > VK_MAX_MEMORY_TYPES ||
            !mem.memoryProperties.memoryHeapCount || mem.memoryProperties.memoryHeapCount > VK_MAX_MEMORY_HEAPS)
        return DXGI_ERROR_UNSUPPORTED;
    for (i = 0; i < mem.memoryProperties.memoryTypeCount; ++i) {
        const VkMemoryType *type = &mem.memoryProperties.memoryTypes[i];
        if (type->heapIndex >= mem.memoryProperties.memoryHeapCount ||
                !mem.memoryProperties.memoryHeaps[type->heapIndex].size) return DXGI_ERROR_UNSUPPORTED;
        if ((type->propertyFlags & witness) == witness)
            result.coherent_cached_types |= UINT32_C(1) << i;
    }
    result.raw = raw; /* pNext is NULL: no pointer into the expired query stack. */
    result.properties = mem.memoryProperties;
    if (raw.known_mask == MWD_MEMORY_KNOWN_MASK && raw.value_mask == MWD_MEMORY_KNOWN_MASK &&
            expected->cached_coherent.known && expected->cached_coherent.value &&
            expected->owned_allocations_only.known && expected->owned_allocations_only.value &&
            result.coherent_cached_types) {
        /* Conditional source-contract inference for the owned L0 guest-PFN,
         * MmCached/CACHED_COHERENT path. Not a physical snooping measurement,
         * not a promise for AHB/foreign imports or per-resource typeBits. */
        result.uma.known = result.uma.value = 1;
        result.io_coherent.known = result.io_coherent.value = 1;
        result.cache_coherent.known = result.cache_coherent.value = 1;
    }
    *out = result;
    return S_OK;
}
