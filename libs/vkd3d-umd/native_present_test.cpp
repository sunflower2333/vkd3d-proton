/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exact production functions; controlled Vulkan/object peers, no GPU proof. */
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(c, message) do { if (!(c)) { std::fprintf(stderr, "FAIL native Present: %s\n", message); std::exit(1); } } while (0)
using HRESULT = int32_t;
constexpr HRESULT S_OK = 0, E_INVALIDARG = -1, E_OUTOFMEMORY = -2, DXGI_ERROR_UNSUPPORTED = -3;
constexpr unsigned D3D12_COMMAND_LIST_TYPE_DIRECT = 0, VKD3D_RESOURCE_GENERAL_LAYOUT = 1, D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3;
#define FAILED(hr) ((hr) < 0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct IUnknown { unsigned references = 1; };
struct d3d12_device { bool wddm_native_present = true; HRESULT status = 0; struct { PFN_vkCmdPipelineBarrier2 vkCmdPipelineBarrier2; } vk_procs{}; };
struct d3d12_resource : IUnknown {
    d3d12_device *device;
    VkDeviceMemory wddm_native_memory = (VkDeviceMemory)(uintptr_t)1;
    unsigned flags = VKD3D_RESOURCE_GENERAL_LAYOUT;
    VkImageLayout common_layout = VK_IMAGE_LAYOUT_GENERAL;
    struct { unsigned Dimension = 3, DepthOrArraySize = 1, MipLevels = 1; struct { unsigned Count = 1; } SampleDesc; } desc;
    struct { VkImage vk_image; } res;
};
struct d3d12_command_allocator { IUnknown **wddm_recording_owners = nullptr; size_t wddm_recording_owners_size = 0, wddm_recording_owners_count = 0; };
struct d3d12_command_list {
    bool is_recording = true;
    unsigned type = 0;
    d3d12_device *device;
    d3d12_command_allocator *allocator;
    struct { VkCommandBuffer vk_command_buffer = (VkCommandBuffer)(uintptr_t)0x77; } cmd;
};
using ID3D12GraphicsCommandList = d3d12_command_list;
using d3d12_command_list_iface = d3d12_command_list;
using ID3D12Resource = d3d12_resource;
static bool fail_reserve;
static unsigned tracked, decayed, dispatches;
static bool vkd3d_array_reserve(void **out, size_t *capacity, size_t count, size_t stride) {
    if (fail_reserve) return false;
    if (count > *capacity) { *out = std::realloc(*out, count * stride); *capacity = count; }
    return true;
}
[[maybe_unused]] static void IUnknown_AddRef(IUnknown *object) { ++object->references; }
static d3d12_command_list *impl_from_ID3D12GraphicsCommandList(d3d12_command_list_iface *value) { return value; }
static d3d12_resource *impl_from_ID3D12Resource(ID3D12Resource *value) { return value; }
static HRESULT d3d12_device_removed_reason(d3d12_device *device) { return device->status; }
static HRESULT hresult_from_vk_result(VkResult result) { return result == VK_SUCCESS ? S_OK : -5; }
static void d3d12_command_list_track_resource_usage(d3d12_command_list *, d3d12_resource *resource, bool transition) {
    CHECK(resource->references > 1 && transition, "retained image before tracking"); ++tracked;
}
static void d3d12_command_list_decay_tracked_state(d3d12_command_list *) { ++decayed; }
// PRODUCTION_FUNCTIONS
static unsigned expected_count;
static VkImage expected_images[2];
static void VKAPI_CALL record(VkCommandBuffer command, const VkDependencyInfo *dependency) {
    CHECK(command == (VkCommandBuffer)(uintptr_t)0x77 && dependency->sType == VK_STRUCTURE_TYPE_DEPENDENCY_INFO &&
        !dependency->pNext && !dependency->dependencyFlags && !dependency->memoryBarrierCount &&
        !dependency->bufferMemoryBarrierCount && dependency->imageMemoryBarrierCount == expected_count, "exact dependency");
    for (unsigned i = 0; i < expected_count; ++i) {
        const auto &barrier = dependency->pImageMemoryBarriers[i];
        CHECK(barrier.image == expected_images[i] && barrier.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 &&
            !barrier.pNext && barrier.oldLayout == VK_IMAGE_LAYOUT_GENERAL && barrier.newLayout == VK_IMAGE_LAYOUT_GENERAL &&
            barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
            barrier.srcStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT && barrier.dstStageMask == barrier.srcStageMask &&
            barrier.srcAccessMask == (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT) &&
            barrier.dstAccessMask == barrier.srcAccessMask && barrier.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
            !barrier.subresourceRange.baseMipLevel && !barrier.subresourceRange.baseArrayLayer &&
            barrier.subresourceRange.levelCount == 1 && barrier.subresourceRange.layerCount == 1, "exact native barrier");
    }
    ++dispatches;
}
int main() {
    d3d12_device device{}, other{}; device.vk_procs.vkCmdPipelineBarrier2 = record;
    d3d12_command_allocator allocator{};
    d3d12_command_list list{}; list.device = &device; list.allocator = &allocator;
    d3d12_resource source{}, destination{}; source.device = destination.device = &device;
    source.res.vk_image = expected_images[0] = (VkImage)(uintptr_t)0x101;
    destination.res.vk_image = expected_images[1] = (VkImage)(uintptr_t)0x202;
    expected_count = 2;
    CHECK(vkd3d_wddm_command_prepare_present(&list, &source, &destination) == S_OK, "successful record");
    CHECK(source.references == 2 && destination.references == 2 && allocator.wddm_recording_owners_count == 2,
        "allocator owns both images");
    CHECK(dispatches == 1 && tracked == 2 && decayed == 1, "actual barrier dispatch");
    expected_count = 1;
    CHECK(vkd3d_wddm_command_prepare_present(&list, &source, nullptr) == S_OK &&
        source.references == 2 && allocator.wddm_recording_owners_count == 2, "deduplicate retained owner");
    const auto calls = dispatches;
    list.is_recording = false;
    CHECK(vkd3d_wddm_command_prepare_present(&list, &source, nullptr) == E_INVALIDARG, "closed command rejected");
    list.is_recording = true; list.type = 3;
    CHECK(vkd3d_wddm_command_prepare_present(&list, &source, nullptr) == E_INVALIDARG, "copy queue rejected");
    list.type = 0; device.wddm_native_present = false;
    CHECK(vkd3d_wddm_command_prepare_present(&list, &source, nullptr) == DXGI_ERROR_UNSUPPORTED, "old ICD rejected");
    device.wddm_native_present = true; device.status = -19;
    CHECK(vkd3d_wddm_command_prepare_present(&list, &source, nullptr) == -19, "device loss preserved");
    device.status = 0; source.device = &other;
    CHECK(vkd3d_wddm_command_prepare_present(&list, &source, nullptr) == E_INVALIDARG, "foreign image rejected");
    source.device = &device; source.wddm_native_memory = VK_NULL_HANDLE;
    CHECK(vkd3d_wddm_command_prepare_present(&list, &source, nullptr) == E_INVALIDARG, "ordinary image rejected");
    source.wddm_native_memory = (VkDeviceMemory)(uintptr_t)1; source.common_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkd3d_wddm_command_prepare_present(&list, &source, nullptr) == E_INVALIDARG, "unpreserved image rejected");
    source.common_layout = VK_IMAGE_LAYOUT_GENERAL; source.desc.MipLevels = 2;
    CHECK(vkd3d_wddm_command_prepare_present(&list, &source, nullptr) == E_INVALIDARG, "mip chain rejected");
    source.desc.MipLevels = 1; fail_reserve = true;
    CHECK(vkd3d_wddm_command_prepare_present(&list, &source, nullptr) == E_OUTOFMEMORY && dispatches == calls,
        "allocation failure has no partial GPU work");
    for (size_t i = 0; i < allocator.wddm_recording_owners_count; ++i) --allocator.wddm_recording_owners[i]->references;
    std::free(allocator.wddm_recording_owners);
    CHECK(source.references == 1 && destination.references == 1, "allocator reset releases retained owners");
    std::puts("PASS production native Present barriers, exact GENERAL dependency, allocator owner deduplication, rejection and failure rollback");
}
