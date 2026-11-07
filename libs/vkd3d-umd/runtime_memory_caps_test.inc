/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_memory_caps() {
    static_assert(sizeof(D3D12DDI_MEMORY_ARCHITECTURE_CAPS) == 12, "original R0 memory caps");
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS table{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &table;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    struct GuardedCaps {
        uint32_t prefix;
        D3D12DDI_MEMORY_ARCHITECTURE_CAPS value;
        uint32_t suffix;
    } output;
    auto canary = [&]() { std::memset(&output, 0xa5, sizeof(output)); };
    auto untouched = [&]() {
        const auto *p = reinterpret_cast<const unsigned char *>(&output);
        return std::all_of(p, p + sizeof(output), [](unsigned char c) { return c == 0xa5; });
    };
    UINT node = 0;
    D3D12DDIARG_GETCAPS caps{D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE, &node, &output.value, sizeof(output.value)};
    canary();
    const unsigned before_creates = creates, before_loads = loads, before_unloads = unloads;
    REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == S_OK);
    REQUIRE(output.prefix == 0xa5a5a5a5 && output.suffix == 0xa5a5a5a5);
    REQUIRE(output.value.UMA == TRUE && output.value.IOCoherent == TRUE && output.value.CacheCoherent == TRUE);
    REQUIRE(node == 0 && creates == before_creates && loads == before_loads + 1 && unloads == before_unloads + 1);
    std::puts("PASS MEMORY_CAPS exact R0 node0 three BOOL facts; no embedded device; adjacent canaries unchanged");

    canary();
    const unsigned bad_loads = loads, bad_queries = memory_queries, bad_wire = queue_protocol_queries;
    for (UINT size : {0u, 4u, 11u, 13u, 16u}) {
        caps.DataSize = size;
        REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == E_INVALIDARG && untouched());
    }
    caps.DataSize = sizeof(output.value);
    for (UINT invalid_node : {1u, UINT_MAX}) {
        node = invalid_node;
        REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == E_INVALIDARG && untouched());
    }
    node = 0; caps.pInfo = nullptr;
    REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == E_INVALIDARG && untouched());
    caps.pInfo = &node; caps.pData = nullptr;
    REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == E_INVALIDARG && untouched());
    caps.pData = &output.value;
    REQUIRE(table.pfnGetCaps({}, &caps) == E_INVALIDARG && untouched());
    REQUIRE(table.pfnGetCaps(open.hAdapter, nullptr) == E_INVALIDARG && untouched());
    caps.Type = D3D12DDICAPS_TYPE_3DPIPELINESUPPORT;
    REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == DXGI_ERROR_UNSUPPORTED && untouched());
    caps.Type = static_cast<D3D12DDICAPS_TYPE>(0x7fffffff);
    REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == DXGI_ERROR_UNSUPPORTED && untouched());
    caps.Type = D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE;
    REQUIRE(loads == bad_loads && memory_queries == bad_queries && queue_protocol_queries == bad_wire);
    std::puts("PASS MEMORY_CAPS bad size/node/null/type/adapter rejected before callbacks or loader");

    // Rejected shapes do not authorize a read of opaque pInfo. PAGE_NOACCESS
    // makes diagnostic payload reads fail deterministically, including when
    // the returned HRESULT and output bytes would otherwise look correct.
    void *inaccessible = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(inaccessible);
    const std::array<unsigned, 8> rejected_counts{
        loads, unloads, creates, destroys, query_calls, memory_queries, queue_protocol_queries, error_calls};
    auto rejected_without_callbacks = [&]() {
        return rejected_counts == std::array<unsigned, 8>{
            loads, unloads, creates, destroys, query_calls, memory_queries, queue_protocol_queries, error_calls};
    };
    auto inaccessible_info = caps;
    inaccessible_info.pInfo = inaccessible;
    for (UINT size : {0u, 4u, 11u, 13u, 16u}) {
        inaccessible_info.DataSize = size;
        REQUIRE(table.pfnGetCaps(open.hAdapter, &inaccessible_info) == E_INVALIDARG);
        REQUIRE(untouched() && rejected_without_callbacks());
    }
    inaccessible_info.DataSize = sizeof(output.value);
    inaccessible_info.Type = static_cast<D3D12DDICAPS_TYPE>(0x7fffffff);
    REQUIRE(table.pfnGetCaps(open.hAdapter, &inaccessible_info) == DXGI_ERROR_UNSUPPORTED);
    REQUIRE(untouched() && rejected_without_callbacks());
    // Even the outer request is unreadable here. An unregistered adapter must
    // be rejected before scalar logging snapshots or any request validation.
    auto *inaccessible_request = static_cast<const D3D12DDIARG_GETCAPS *>(inaccessible);
    REQUIRE(table.pfnGetCaps({}, inaccessible_request) == E_INVALIDARG);
    REQUIRE(untouched() && rejected_without_callbacks());
    const D3D12DDI_HADAPTER foreign_adapter{reinterpret_cast<void *>(uintptr_t(1))};
    REQUIRE(table.pfnGetCaps(foreign_adapter, inaccessible_request) == E_INVALIDARG);
    REQUIRE(untouched() && rejected_without_callbacks());
    REQUIRE(VirtualFree(inaccessible, 0, MEM_RELEASE));
    std::puts("PASS MEMORY_CAPS noaccess pInfo bad-size/unknown-type and invalid-adapter outer request: exact rejection, no payload read, callback/load or output change");

    for (uint32_t unknown_mask : {0u, 1u, 2u}) {
        memory_known_mask = unknown_mask;
        REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == DXGI_ERROR_UNSUPPORTED && untouched());
    }
    memory_known_mask = MWD_MEMORY_KNOWN_MASK;
    memory_profile_absent = true;
    REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == DXGI_ERROR_UNSUPPORTED && untouched());
    memory_profile_absent = false; memory_generation_delta = 1;
    REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == DXGI_ERROR_DEVICE_REMOVED && untouched());
    memory_generation_delta = 0;
    memory_query_callback = []() { ++generation; };
    REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == DXGI_ERROR_DEVICE_REMOVED && untouched());
    --generation; memory_query_callback = {};
    REQUIRE(table.pfnCloseAdapter(open.hAdapter) == S_OK);
    cached_coherent_wire = 0;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == DXGI_ERROR_UNSUPPORTED && untouched());
    REQUIRE(table.pfnCloseAdapter(open.hAdapter) == S_OK);
    cached_coherent_wire = 1;
    std::puts("PASS MEMORY_CAPS known-false/unknown/absent and stale facts never publish false or partial caps");

    REQUIRE(OpenAdapter12(&open) == S_OK);
    auto *request_page = static_cast<D3D12DDIARG_GETCAPS *>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto *node_page = static_cast<UINT *>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(request_page && node_page);
    *node_page = 0;
    *request_page = {D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE, node_page, &output.value, sizeof(output.value)};
    // Runtime may invalidate the request after the first callback. A successful
    // query must use the entry snapshot, not reread caller request/node storage.
    memory_query_callback = [&]() {
        if (!VirtualFree(request_page, 0, MEM_RELEASE) || !VirtualFree(node_page, 0, MEM_RELEASE))
            fixture_abort(__LINE__);
    };
    REQUIRE(table.pfnGetCaps(open.hAdapter, request_page) == S_OK);
    memory_query_callback = {};
    REQUIRE(output.value.UMA == TRUE && output.value.IOCoherent == TRUE && output.value.CacheCoherent == TRUE);
    REQUIRE(output.prefix == 0xa5a5a5a5 && output.suffix == 0xa5a5a5a5);
    REQUIRE(table.pfnCloseAdapter(open.hAdapter) == S_OK);

    REQUIRE(OpenAdapter12(&open) == S_OK); canary();
    auto *output_page = static_cast<D3D12DDI_MEMORY_ARCHITECTURE_CAPS *>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(output_page); caps.pData = output_page;
    memory_query_callback = [&]() {
        if (table.pfnCloseAdapter(open.hAdapter) != S_OK || !VirtualFree(output_page, 0, MEM_RELEASE))
            fixture_abort(__LINE__);
    };
    REQUIRE(table.pfnGetCaps(open.hAdapter, &caps) == DXGI_ERROR_DEVICE_REMOVED);
    memory_query_callback = {}; caps.pData = &output.value;
    REQUIRE(!native_adapter(open.hAdapter) && untouched());
    std::puts("PASS MEMORY_CAPS destructive reentry: request/node snapshot and retired freed output never revisited");

    REQUIRE(OpenAdapter12(&open) == S_OK);
    UINT32 count = 2; UINT64 versions[2] = {0x1234, 0x5678};
    REQUIRE(table.pfnGetSupportedVersions(open.hAdapter, &count, versions) == S_OK && !count);
    REQUIRE(versions[0] == 0x1234 && versions[1] == 0x5678 && !native_contract_complete());
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device_table;
    std::memset(&device_table, 0xa5, sizeof(device_table));
    REQUIRE(table.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
        &device_table, sizeof(device_table), 0, {}) == DXGI_ERROR_UNSUPPORTED);
    const auto *p = reinterpret_cast<const unsigned char *>(&device_table);
    REQUIRE(std::all_of(p, p + sizeof(device_table), [](unsigned char c) { return c == 0xa5; }));
    REQUIRE(table.pfnCloseAdapter(open.hAdapter) == S_OK);
    std::puts("PASS MEMORY_CAPS supported versions0, feature-level gate and public table publication remain closed");
    return 0;
}
