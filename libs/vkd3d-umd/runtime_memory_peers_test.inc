/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Controlled loader metadata for lifetime tests. Classification is the actual
 * compiled backend consumer; the paired ICD producer is tested separately. */
static bool memory_profile_absent;
static uint32_t memory_known_mask = MWD_MEMORY_KNOWN_MASK;
static uint64_t memory_generation_delta;
static unsigned memory_queries;
static std::function<void()> memory_query_callback, memory_create_callback;
static void VKAPI_CALL test_memory_properties(VkPhysicalDevice, VkPhysicalDeviceProperties2 *props) {
    auto *id = static_cast<VkPhysicalDeviceIDProperties *>(props->pNext);
    auto *driver = static_cast<VkPhysicalDeviceDriverProperties *>(id->pNext);
    auto *support = static_cast<mwd_support *>(driver->pNext);
    id->deviceLUIDValid = VK_TRUE; id->deviceNodeMask = 1;
    std::memcpy(id->deviceLUID, expected_luid.data(), 8);
    driver->driverID = VK_DRIVER_ID_MESA_TURNIP;
    support->magic = MWD_RUNTIME_MAGIC; support->version = MWD_RUNTIME_ABI_VERSION;
    support->size = sizeof(mwd_callbacks); support->flags = 1;
}
static void VKAPI_CALL test_memory_reply(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2 *props) {
    ++memory_queries;
    if (memory_profile_absent) return;
    auto *raw = static_cast<mwd_memory_profile_v1 *>(props->pNext);
    raw->magic = MWD_MEMORY_PROFILE_MAGIC; raw->version = 1; raw->size = sizeof(*raw);
    raw->profile = MWD_MEMORY_PROFILE_OWNED_RUNTIME2;
    std::memcpy(raw->luid, expected_luid.data(), 8); raw->reset_generation = generation + memory_generation_delta;
    raw->known_mask = memory_known_mask;
    raw->value_mask = (1 | (cached_coherent_wire ? 2 : 0)) & memory_known_mask;
    props->memoryProperties.memoryHeapCount = 1; props->memoryProperties.memoryHeaps[0] = {0x40000000, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    props->memoryProperties.memoryTypeCount = 1;
    props->memoryProperties.memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0};
}
static PFN_vkVoidFunction VKAPI_CALL test_memory_loader(VkInstance, const char *name) {
    if (!std::strcmp(name, "vkGetPhysicalDeviceProperties2")) return reinterpret_cast<PFN_vkVoidFunction>(test_memory_properties);
    if (!std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties2")) return reinterpret_cast<PFN_vkVoidFunction>(test_memory_reply);
    return nullptr;
}
static int32_t test_memory_query(PFN_vkGetInstanceProcAddr loader,
        const vkdu_memory_expectation *expected, vkdu_memory_facts *out) {
    if (!loader) fixture_abort(__LINE__);
    const auto hr = vkdu_memory_facts_for_device(test_memory_loader, VK_NULL_HANDLE,
        reinterpret_cast<VkPhysicalDevice>(uintptr_t(0x86420)), expected, out);
    if (memory_query_callback) { auto callback = memory_query_callback; callback(); }
    return hr;
}
