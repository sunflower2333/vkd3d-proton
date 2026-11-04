/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the actual parser/admission entry. Typed callback data is controlled;
 * these tests do not establish a real ICD profile or hardware coherence. */
static int test_native_adapter_facts() {
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{}; um.pfnSetErrorCb = test_error;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> memory;
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = memory.data(); create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    unsigned failures = 0;
    for (uint32_t wire : {2u, 0u}) {
        memory.fill(0xa5);
        cached_coherent_wire = wire;
        const auto before_loads = loads, before_creates = creates;
        const HRESULT hr = adapter.pfnCreateDevice(open.hAdapter, &create);
        const bool unchanged = std::all_of(memory.begin(), memory.end(), [](unsigned char byte) { return byte == 0xa5; });
        const bool refused = hr == DXGI_ERROR_UNSUPPORTED && loads == before_loads && creates == before_creates && unchanged;
        std::printf("%s ADAPTER_FACTS cached_wire=%u hr=%08x loads_delta=%u backend_delta=%u storage_unchanged=%u\n",
            refused ? "PASS" : "FAIL", wire, unsigned(hr), loads - before_loads, creates - before_creates, unsigned(unchanged));
        if (SUCCEEDED(hr)) adapter.pfnDestroyDevice(create.hDrvDevice);
        if (!refused) ++failures;
    }
    cached_coherent_wire = 1;
    if (failures) {
        REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
        return 1;
    }

    auto identity = native_adapter(open.hAdapter);
    REQUIRE(identity && identity->luid == expected_luid && identity->generation == generation);
    REQUIRE(identity->has_cached_coherent_memory.known && identity->has_cached_coherent_memory.value);
    REQUIRE(!identity->owned_allocations_only.known);
    NativeAdapterFacts facts{};
    REQUIRE(!facts.has_cached_coherent_memory.known && !facts.owned_allocations_only.known);
    REQUIRE(native_query_adapter_facts(*identity, facts) == S_OK);
    REQUIRE(facts.has_cached_coherent_memory.known && facts.has_cached_coherent_memory.value &&
        !facts.owned_allocations_only.known && facts.luid == expected_luid && facts.generation == generation);
    REQUIRE(native_query_adapter_facts(*identity, facts, true) == S_OK);
    REQUIRE(facts.owned_allocations_only.known && facts.owned_allocations_only.value);
    cached_coherent_wire = 0;
    REQUIRE(native_query_adapter_facts(*identity, facts) == S_OK);
    REQUIRE(facts.has_cached_coherent_memory.known && !facts.has_cached_coherent_memory.value &&
        !facts.owned_allocations_only.known);
    cached_coherent_wire = 1;
    // Failed responses must not replace a prior fact snapshot or turn missing
    // bytes into a known false. Literal mutations are independent of the parser.
    std::array<unsigned char, sizeof(facts)> before{};
    std::memcpy(before.data(), &facts, sizeof(facts));
    for (unsigned invalid = 0; invalid < 4; ++invalid) {
        queue_protocol_query_callback = [=](uint8_t *bytes, const D3DDDICB_QUERYADAPTERINFO *args) {
            if (invalid == 0) const_cast<D3DDDICB_QUERYADAPTERINFO *>(args)->PrivateDriverDataSize = 76;
            else if (invalid == 1) bytes[8] = 76;
            else if (invalid == 2) bytes[76] = 2;
            else std::memset(bytes + 160, 0, 32);
        };
        REQUIRE(native_query_adapter_facts(*identity, facts, true) == DXGI_ERROR_UNSUPPORTED);
        REQUIRE(!std::memcmp(before.data(), &facts, sizeof(facts)));
    }
    queue_protocol_query_callback = {};
    query_result = E_ACCESSDENIED;
    REQUIRE(native_query_adapter_facts(*identity, facts, true) == E_ACCESSDENIED);
    REQUIRE(!std::memcmp(before.data(), &facts, sizeof(facts)));
    query_result = S_OK;
    identity.reset();
    std::printf("PASS ADAPTER_FACTS parser: known_true/known_false/unknown_owned,160/192,4 malformed and callback failure leave output unchanged\n");

    // Same identity with a changed generation is rejected before backend use.
    const auto stale_loads = loads, stale_creates = creates;
    ++generation;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_DEVICE_REMOVED);
    --generation;
    REQUIRE(loads == stale_loads && creates == stale_creates);
    for (auto byte : memory) REQUIRE(byte == 0xa5);
    // Cache policy loss after backend construction must roll back publication.
    for (uint32_t wire : {0u, 2u}) {
        unsigned calls = 0;
        const auto old_creates = creates, old_destroys = destroys, old_unloads = unloads;
        queue_protocol_query_callback = [&](uint8_t *bytes, const D3DDDICB_QUERYADAPTERINFO *) {
            if (++calls == 2) std::memcpy(bytes + 76, &wire, 4);
        };
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_DEVICE_REMOVED);
        REQUIRE(calls == 2 && creates == old_creates + 1 && destroys == old_destroys + 1 && unloads == old_unloads + 1);
        for (auto byte : memory) REQUIRE(byte == 0xa5);
    }
    queue_protocol_query_callback = {};

    kt.pfnCreateContextCb = heap_context_create; kt.pfnDestroyContextCb = heap_context_destroy;
    kt.pfnEscapeCb = heap_escape;
    um.pfnCreateContextCb = queue_context_create; um.pfnDestroyContextCb = queue_context_destroy;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    REQUIRE(ctx && ctx->native_facts.luid == expected_luid && ctx->native_facts.generation == generation);
    REQUIRE(ctx->native_facts.has_cached_coherent_memory.known && ctx->native_facts.has_cached_coherent_memory.value &&
        ctx->native_facts.owned_allocations_only.known && ctx->native_facts.owned_allocations_only.value);
    REQUIRE(native_heap_generation(ctx) == S_OK); // 160's unknown owned bit is not capability loss.
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queue) == S_OK);
    Object slot{};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 request{};
    request.hDrvCommandQueue.pDrvPrivate = &slot;
    request.hRTCommandQueue.handle = reinterpret_cast<HANDLE>(uintptr_t(0x79220));
    request.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D; request.NodeMask = 1;
    const auto old_owners = heap_context_creates, old_children = runtime_context_creates, old_queues = queue_creates;
    for (uint32_t wire : {0u, 2u}) {
        cached_coherent_wire = wire;
        REQUIRE(native_heap_generation(ctx) == DXGI_ERROR_DEVICE_REMOVED);
        REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice, &request) == DXGI_ERROR_DEVICE_REMOVED);
        REQUIRE(!slot.magic && !ctx->native_runtime_queues && !ctx->native_heap_context);
        REQUIRE(heap_context_creates == old_owners && runtime_context_creates == old_children && queue_creates == old_queues);
        REQUIRE(ctx->native_facts.has_cached_coherent_memory.known && ctx->native_facts.has_cached_coherent_memory.value);
    }
    cached_coherent_wire = 1;
    REQUIRE(native_heap_generation(ctx) == S_OK);
    adapter.pfnDestroyDevice(create.hDrvDevice);
    std::printf("PASS ADAPTER_FACTS lifecycle: stale generation,2 publication rollbacks,160 owned-unknown valid,2 heap/queue cache losses,no late create\n");

    // A fresh adapter may report false honestly; that does not admit an owned
    // device whose allocation policy requires cached-coherent backing.
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    cached_coherent_wire = 0;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    identity = native_adapter(open.hAdapter);
    REQUIRE(identity->has_cached_coherent_memory.known && !identity->has_cached_coherent_memory.value);
    identity.reset();
    const auto false_loads = loads;
    memory.fill(0xa5);
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_UNSUPPORTED && loads == false_loads);
    for (auto byte : memory) REQUIRE(byte == 0xa5);
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    cached_coherent_wire = 2;
    REQUIRE(OpenAdapter12(&open) == DXGI_ERROR_UNSUPPORTED && !open.hAdapter.pDrvPrivate && loads == false_loads);
    cached_coherent_wire = 1;

    // Query reentry invalidates the device/request/output page. Both modified
    // consumers must retain Context and avoid late callbacks or slot accesses.
    for (bool heap_path : {false, true}) {
        REQUIRE(OpenAdapter12(&open) == S_OK);
        auto *page = static_cast<uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(page != nullptr);
        create.hDrvDevice.pDrvPrivate = page + 2048;
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
        ctx = context(create.hDrvDevice);
        std::weak_ptr<NativeAdapter> weak = ctx->native_adapter;
        auto *cancel_request = reinterpret_cast<D3D12DDIARG_CREATECOMMANDQUEUE_0001 *>(page);
        request.hDrvCommandQueue.pDrvPrivate = page + 1024;
        *cancel_request = request;
        const auto cancel_owners = heap_context_creates, cancel_children = runtime_context_creates;
        const auto cancel_queues = queue_creates, cancel_destroys = destroys;
        if (heap_path) ++ctx->references;
        identity_query_callback = [&]() {
            adapter.pfnDestroyDevice(create.hDrvDevice);
            if (adapter.pfnCloseAdapter(open.hAdapter) != S_OK || !VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
        };
        const auto hr = heap_path ? native_heap_generation(ctx) : table.pfnCreateCommandQueue(create.hDrvDevice, cancel_request);
        if (heap_path) release(ctx);
        REQUIRE(hr == DXGI_ERROR_DEVICE_REMOVED && !identity_query_callback && weak.expired());
        REQUIRE(heap_context_creates == cancel_owners && runtime_context_creates == cancel_children &&
            queue_creates == cancel_queues && destroys == cancel_destroys + 1);
    }
    std::printf("PASS ADAPTER_FACTS reentry: queue192 and heap160 retire/free storage; no late callback or write; no complete memory caps claimed\n");
    return 0;
}
