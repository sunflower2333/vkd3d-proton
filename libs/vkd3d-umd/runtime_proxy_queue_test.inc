/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* CPU callback model for the explicitly injected native-surface provider.
 * This never creates an OS/GPU context or synchronization object. */
struct ProxyQueueFixture {
    HANDLE runtime = nullptr;
    D3DKMT_HANDLE fences[2]{};
    unsigned created = 0, destroyed = 0, phase = 0;
    uint64_t value = 0;
    bool context_destroyed = false;
};
static bool proxy_queue_fixture;
static std::map<HANDLE, ProxyQueueFixture> proxy_contexts;
static std::map<D3DKMT_HANDLE, HANDLE> proxy_fence_contexts;
static HANDLE proxy_creating_context, proxy_submitting_context;
static D3DKMT_HANDLE next_proxy_fence = 0x71000;
static unsigned proxy_sync_creates, proxy_sync_destroys, proxy_gpu_signals, proxy_gpu_waits;
static unsigned fail_proxy_sync_create;

static HRESULT APIENTRY proxy_sync_create(HANDLE runtime, D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 *args) {
    valid_kernel_callback(runtime);
    D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 expected{};
    expected.Info.Type = D3DDDI_MONITORED_FENCE;
    if (!proxy_queue_fixture || !proxy_creating_context || std::memcmp(args, &expected, sizeof(expected)))
        fixture_abort(__LINE__);
    auto &owner = proxy_contexts.at(proxy_creating_context);
    if (owner.context_destroyed || owner.created >= 2) fixture_abort(__LINE__);
    if (fail_proxy_sync_create && !--fail_proxy_sync_create) return E_OUTOFMEMORY;
    args->hSyncObject = ++next_proxy_fence;
    owner.fences[owner.created++] = args->hSyncObject;
    if (!proxy_fence_contexts.emplace(args->hSyncObject, proxy_creating_context).second) fixture_abort(__LINE__);
    ++proxy_sync_creates;
    if (owner.created == 2) proxy_creating_context = nullptr;
    return S_OK;
}
static HRESULT APIENTRY proxy_sync_destroy(HANDLE runtime, const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT *args) {
    valid_kernel_callback(runtime);
    auto fence = proxy_fence_contexts.find(args->hSyncObject);
    if (!proxy_queue_fixture || fence == proxy_fence_contexts.end()) fixture_abort(__LINE__);
    auto context = proxy_contexts.find(fence->second);
    auto &owner = context->second;
    D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT expected{}; expected.hSyncObject = args->hSyncObject;
    if (std::memcmp(args, &expected, sizeof(expected)) || !owner.context_destroyed || owner.phase ||
            owner.destroyed >= owner.created || owner.fences[owner.destroyed] != args->hSyncObject)
        fixture_abort(__LINE__);
    proxy_fence_contexts.erase(fence); ++proxy_sync_destroys;
    if (++owner.destroyed == owner.created) proxy_contexts.erase(context);
    return S_OK;
}
static HRESULT APIENTRY proxy_gpu_signal(HANDLE runtime, const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU *args) {
    valid_kernel_callback(runtime);
    if (!proxy_queue_fixture || args->ObjectCount != 1 || !args->ObjectHandleArray || !args->MonitoredFenceValueArray)
        fixture_abort(__LINE__);
    auto fence = proxy_fence_contexts.find(*args->ObjectHandleArray);
    if (fence == proxy_fence_contexts.end() || fence->second != proxy_submitting_context) fixture_abort(__LINE__);
    auto &owner = proxy_contexts.at(fence->second);
    const bool before = *args->ObjectHandleArray == owner.fences[0];
    D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU expected{};
    expected.hContext = before ? fence->second : expected_context;
    expected.ObjectCount = 1; expected.ObjectHandleArray = args->ObjectHandleArray;
    expected.MonitoredFenceValueArray = args->MonitoredFenceValueArray;
    if (std::memcmp(args, &expected, sizeof(expected)) || owner.context_destroyed || owner.created != 2 ||
            owner.phase != (before ? 0u : 3u) || *args->MonitoredFenceValueArray != owner.value + 1)
        fixture_abort(__LINE__);
    owner.value = *args->MonitoredFenceValueArray; owner.phase = before ? 1 : 4;
    ++proxy_gpu_signals;
    return S_OK;
}
static HRESULT APIENTRY proxy_gpu_wait(HANDLE runtime, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *args) {
    valid_kernel_callback(runtime);
    if (!proxy_queue_fixture || args->ObjectCount != 1 || !args->ObjectHandleArray || !args->MonitoredFenceValueArray)
        fixture_abort(__LINE__);
    auto fence = proxy_fence_contexts.find(*args->ObjectHandleArray);
    if (fence == proxy_fence_contexts.end() || fence->second != proxy_submitting_context) fixture_abort(__LINE__);
    auto &owner = proxy_contexts.at(fence->second);
    const bool before = *args->ObjectHandleArray == owner.fences[0];
    D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU expected{};
    expected.hContext = before ? expected_context : fence->second;
    expected.ObjectCount = 1; expected.ObjectHandleArray = args->ObjectHandleArray;
    expected.MonitoredFenceValueArray = args->MonitoredFenceValueArray;
    if (std::memcmp(args, &expected, sizeof(expected)) || owner.context_destroyed ||
            owner.phase != (before ? 1u : 4u) || *args->MonitoredFenceValueArray != owner.value)
        fixture_abort(__LINE__);
    owner.phase = before ? 2 : 0; ++proxy_gpu_waits;
    return S_OK;
}
static void proxy_fixture_render(HANDLE context) {
    if (!proxy_submitting_context) return;
    auto &owner = proxy_contexts.at(proxy_submitting_context);
    if (context != expected_context || owner.phase != 2 || owner.context_destroyed) fixture_abort(__LINE__);
    owner.phase = 3;
}

struct GuardedProxyQueue {
    uint64_t before = 0x1122334455667788ull;
    Object object{};
    uint64_t after = 0x8877665544332211ull;
    bool intact() const { return before == 0x1122334455667788ull && after == 0x8877665544332211ull; }
};
