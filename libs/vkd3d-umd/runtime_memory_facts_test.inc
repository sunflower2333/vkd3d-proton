/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_memory_facts() {
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS table{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &table;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    vkdu_memory_facts facts{};
    unsigned before_creates = creates, before_loads = loads, before_unloads = unloads, before_wire = queue_protocol_queries;
    REQUIRE(native_adapter_memory_facts(open.hAdapter, &facts) == S_OK && vkdu_memory_facts_qualified(&facts));
    REQUIRE(creates == before_creates && loads == before_loads + 1 && unloads == before_unloads + 1 && queue_protocol_queries == before_wire + 2);
    REQUIRE(!facts.gpu_va_bits_known && !facts.raw.pNext);
    auto canary = [&]() { std::memset(&facts, 0xa5, sizeof(facts)); };
    auto untouched = [&]() { const auto *p = reinterpret_cast<const unsigned char *>(&facts);
        return std::all_of(p, p + sizeof(facts), [](unsigned char c) { return c == 0xa5; }); };
    canary(); memory_profile_absent = true;
    REQUIRE(native_adapter_memory_facts(open.hAdapter, &facts) == DXGI_ERROR_UNSUPPORTED && untouched());
    memory_profile_absent = false; memory_generation_delta = 1;
    REQUIRE(native_adapter_memory_facts(open.hAdapter, &facts) == DXGI_ERROR_DEVICE_REMOVED && untouched());
    memory_generation_delta = 0;
    memory_query_callback = []() { ++generation; };
    REQUIRE(native_adapter_memory_facts(open.hAdapter, &facts) == DXGI_ERROR_DEVICE_REMOVED && untouched());
    --generation; memory_query_callback = {};
    memory_query_callback = [&]() { if (table.pfnCloseAdapter(open.hAdapter) != S_OK) fixture_abort(__LINE__); };
    before_wire = queue_protocol_queries;
    REQUIRE(native_adapter_memory_facts(open.hAdapter, &facts) == DXGI_ERROR_DEVICE_REMOVED && untouched());
    REQUIRE(queue_protocol_queries == before_wire + 1); // no callback after registration retired
    memory_query_callback = {};
    REQUIRE(!native_adapter(open.hAdapter));

    cached_coherent_wire = 0;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    REQUIRE(native_adapter_memory_facts(open.hAdapter, &facts) == S_OK);
    REQUIRE(facts.raw.known_mask == 3 && facts.raw.value_mask == 1 && !facts.uma.known && !facts.io_coherent.known && !facts.cache_coherent.known);
    REQUIRE(table.pfnCloseAdapter(open.hAdapter) == S_OK);
    cached_coherent_wire = 1;

    // Close from the first wire callback: no load, late caller write or second query.
    REQUIRE(OpenAdapter12(&open) == S_OK); canary(); before_loads = loads;
    identity_query_callback = [&]() { if (table.pfnCloseAdapter(open.hAdapter) != S_OK) fixture_abort(__LINE__); };
    REQUIRE(native_adapter_memory_facts(open.hAdapter, &facts) == DXGI_ERROR_DEVICE_REMOVED && untouched());
    REQUIRE(loads == before_loads); identity_query_callback = {};

    D3DDDI_DEVICECALLBACKS kt{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{}; um.pfnSetErrorCb = test_error;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> slot; slot.fill(0xa5);
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = slot.data(); create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    before_creates = creates; memory_profile_absent = true;
    REQUIRE(table.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_UNSUPPORTED && creates == before_creates);
    memory_profile_absent = false; memory_generation_delta = 1;
    REQUIRE(table.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_DEVICE_REMOVED && creates == before_creates);
    memory_generation_delta = 0;
    const auto before_destroys = destroys; before_unloads = unloads;
    memory_create_callback = [&]() { if (table.pfnCloseAdapter(open.hAdapter) != S_OK) fixture_abort(__LINE__); };
    before_wire = queue_protocol_queries;
    REQUIRE(table.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_DEVICE_REMOVED);
    memory_create_callback = {};
    REQUIRE(creates == before_creates + 1 && destroys == before_destroys + 1 && unloads == before_unloads + 1);
    REQUIRE(queue_protocol_queries == before_wire + 1);
    for (auto c : slot) REQUIRE(c == 0xa5);
    REQUIRE(OpenAdapter12(&open) == S_OK);
    REQUIRE(table.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    REQUIRE(ctx && vkdu_memory_facts_qualified(&ctx->native_memory_facts) && !ctx->native_memory_facts.gpu_va_bits_known);
    table.pfnDestroyDevice(create.hDrvDevice);
    REQUIRE(table.pfnCloseAdapter(open.hAdapter) == S_OK);
    std::puts("PASS MEMORY_FACTS native: 192-before/after, metadata-only, raw false/unknown, stale/absent, CloseAdapter reentry, create rollback/no late slot writes");
    return 0;
}
