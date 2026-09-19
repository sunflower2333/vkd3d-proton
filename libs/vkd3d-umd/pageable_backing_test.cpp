/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include "mesa_wddm_pageable.h"
using HRESULT = int32_t;
#define FAILED(hr) ((hr) < 0)
constexpr HRESULT S_OK = 0, E_INVALIDARG = (HRESULT)0x80070057u;
constexpr HRESULT DXGI_ERROR_UNSUPPORTED = (HRESULT)0x887a0004u;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct d3d12_device {
    mwd_callbacks wddm_runtime_callbacks;
    void *wddm_runtime_owner;
    void *vk_device;
    mwd_pageable_acquire_fn wddm_pageable_acquire;
};
struct allocation { uint64_t vk_memory; };
struct d3d12_descriptor_heap {
    struct { allocation device_allocation; } descriptor_buffer;
    allocation device_allocation;
    uint64_t vk_descriptor_pool;
    d3d12_device *device;
};
struct d3d12_query_heap { allocation device_allocation; uint64_t vk_query_pool; d3d12_device *device; };
using ID3D12DescriptorHeap = d3d12_descriptor_heap;
using ID3D12QueryHeap = d3d12_query_heap;
static d3d12_descriptor_heap *impl_from_ID3D12DescriptorHeap(ID3D12DescriptorHeap *heap) { return heap; }
static d3d12_query_heap *impl_from_ID3D12QueryHeap(ID3D12QueryHeap *heap) { return heap; }
// PRODUCTION_FUNCTIONS
static std::map<void *, unsigned> refs;
static std::vector<std::pair<uint32_t, uint64_t>> calls;
static std::map<uint64_t, void *> tokens;
static unsigned fail_at;
static bool corrupt_generation, fail_status;
static void *expected_owner, *expected_device;
static void die(const char *message) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
static int32_t MWD_CALL context_info(void *owner, mwd_context_info *out) {
    if (owner != expected_owner) die("collector context ownership");
    *out = {}; out->generation = 7; return 0;
}
static int32_t MWD_CALL release(void *owner, void *token) {
    if (owner != expected_owner || !refs[token]) die("collector released unowned backing");
    --refs[token]; return 0;
}
static int32_t MWD_CALL status(void *owner) {
    if (owner != expected_owner) die("collector status ownership");
    return fail_status ? (int32_t)0x887a0005u : 0;
}
static int32_t MWD_CALL acquire(void *device, void *owner, uint64_t generation,
        uint32_t type, uint64_t object, mwd_pageable_backing *out) {
    if (owner != expected_owner || device != expected_device || generation != 7) die("provider invocation ownership");
    calls.emplace_back(type, object);
    if (fail_at && calls.size() == fail_at) return (int32_t)0x8007000eu;
    *out = {};
    auto *token = tokens.at(object);
    if (!token) out->flags = MWD_PAGEABLE_NO_BACKING;
    else {
        ++refs[token];
        out->allocation = {token, 0x100000, 4096, corrupt_generation ? 8u : 7u, 22, 6};
    }
    return 0;
}
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    int owner, token_a, token_b, token_c;
    d3d12_device device{}; expected_device = &device; expected_owner = &owner;
    device.wddm_runtime_owner = &owner; device.vk_device = &device; device.wddm_pageable_acquire = acquire;
    auto &cb = device.wddm_runtime_callbacks;
    cb.magic = MWD_RUNTIME_MAGIC; cb.version = MWD_RUNTIME_ABI_VERSION; cb.size = sizeof(cb);
    cb.context = context_info; cb.release = release; cb.status = status;
    cb.allocate = [](void *, uint64_t, uint64_t, uint64_t, uint32_t, mwd_allocation *) -> int32_t { return 0; };
    cb.retain = [](void *, void *, mwd_allocation *) -> int32_t { return 0; };
    cb.map = [](void *, void *, void **, uint32_t *) -> int32_t { return 0; };
    cb.unmap = [](void *, void *) -> int32_t { return 0; };
    cb.submit = [](void *, uint32_t, const void *, uint32_t, const mwd_reference *, uint32_t) -> int32_t { return 0; };
    cb.completed = [](void *, uint32_t *) -> int32_t { return 0; };
    cb.queue_retain = cb.queue_release = [](void *, void *) -> int32_t { return 0; };
    cb.submit_queue = [](void *, void *, uint32_t, const void *, uint32_t, const mwd_reference *, uint32_t) -> int32_t { return 0; };
    tokens = {{10, &token_a}, {11, &token_b}, {12, &token_c}, {20, &token_a}, {21, &token_c}};
    d3d12_descriptor_heap descriptor{{{10}}, {11}, 12, &device};
    d3d12_query_heap query{{20}, 21, &device};
    mwd_allocation allocations[3]{}; uint32_t count = 99;
    CHECK(vkd3d_wddm_descriptor_backing(&descriptor, &count, allocations) == S_OK);
    if (count != 3 || calls != std::vector<std::pair<uint32_t, uint64_t>>{{1,10},{1,11},{2,12}})
        die("actual descriptor backing omitted pool or auxiliary memory");
    for (uint32_t i = 0; i < count; ++i) release(&owner, allocations[i].token);
    calls.clear();
    CHECK(vkd3d_wddm_query_backing(&query, &count, allocations) == S_OK);
    if (count != 2 || calls != std::vector<std::pair<uint32_t, uint64_t>>{{1,20},{3,21}})
        die("actual query backing omitted pool or result memory");
    for (uint32_t i = 0; i < count; ++i) release(&owner, allocations[i].token);
    calls.clear(); tokens[11] = &token_a;
    CHECK(vkd3d_wddm_descriptor_backing(&descriptor, &count, allocations) == S_OK);
    if (count != 2 || refs[&token_a] != 1 || refs[&token_c] != 1)
        die("pageable collector retained duplicate backing");
    for (uint32_t i = 0; i < count; ++i) release(&owner, allocations[i].token);
    for (unsigned mode : {0u, 1u, 2u, 3u}) {
        calls.clear();
        fail_at = mode == 0 ? 2 : 0; corrupt_generation = mode == 1; fail_status = mode == 2;
        device.wddm_pageable_acquire = mode == 3 ? nullptr : acquire;
        std::memset(allocations, 0xa5, sizeof(allocations));
        mwd_allocation untouched[3]; std::memcpy(untouched, allocations, sizeof(allocations));
        CHECK(FAILED(vkd3d_wddm_descriptor_backing(&descriptor, &count, allocations)) && !count);
        CHECK(!std::memcmp(untouched, allocations, sizeof(allocations)));
        for (const auto &entry : refs) CHECK(!entry.second);
    }
    fail_at = 0; corrupt_generation = fail_status = false; device.wddm_pageable_acquire = acquire;
    tokens[10] = tokens[11] = tokens[12] = nullptr;
    CHECK(vkd3d_wddm_descriptor_backing(&descriptor, &count, allocations) == S_OK && !count);
    descriptor.descriptor_buffer.device_allocation.vk_memory = 0;
    descriptor.device_allocation.vk_memory = descriptor.vk_descriptor_pool = 0;
    device.wddm_pageable_acquire = nullptr;
    CHECK(vkd3d_wddm_descriptor_backing(&descriptor, &count, allocations) == S_OK && !count);
    std::puts("PASS actual descriptor/query backing enumeration, deduplication, absent provider, zero backing and ownership rollback");
    return 0;
}
