/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual native entrypoints, with controlled runtime/backend callback peers.
 * This is local typed contract evidence, not a system D3D12 or GPU workload. */
static int test_native_node_metadata() {
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    kt.pfnCreateContextCb = heap_context_create; kt.pfnDestroyContextCb = heap_context_destroy;
    kt.pfnEscapeCb = heap_escape; kt.pfnAllocateCb = heap_allocate; kt.pfnDeallocateCb = heap_deallocate;
    kt.pfnDeallocate2Cb = heap_deallocate2; kt.pfnLockCb = heap_lock; kt.pfnUnlockCb = heap_unlock;
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
    um.pfnCreateContextCb = queue_context_create; um.pfnDestroyContextCb = queue_context_destroy;
    um.pfnSetErrorCb = test_error;
    void *memory = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    REQUIRE(memory);
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> other_memory{}, empty_memory{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = memory; create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto other = create;
    other.hDrvDevice.pDrvPrivate = other_memory.data();
    other.hRTDevice.handle = reinterpret_cast<HANDLE>(uintptr_t(0x78910));
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &other) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    auto *other_ctx = context(other.hDrvDevice);
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queue) == S_OK);
    PFND3D12DDI_GETIMPLICITPHYSICALADAPTERMASK mask = table.pfnGetImplicitPhysicalAdapterMask;
    PFND3D12DDI_QUERY_NODE_MAP node_map = table.pfnQueryNodeMap;
    REQUIRE(mask && node_map && !native_contract_complete());
    UINT32 versions = 1; UINT64 version = 0xfeedfacecafebeefull;
    REQUIRE(native_versions(open.hAdapter, &versions, &version) == S_OK && !versions &&
        version == 0xfeedfacecafebeefull && !ctx->native_surfaces && !other_ctx->native_surfaces);
    struct GuardedMap { UINT before, value, adjacent, after; } output;
    const GuardedMap poison{0xa5a5a5a5u, 0xa5a5a5a5u, 0xa5a5a5a5u, 0xa5a5a5a5u};
    auto untouched = [&]() { return !std::memcmp(&output, &poison, sizeof(output)); };
    const auto original_errors = error_calls, original_queries = query_calls, original_loads = loads;
    const auto original_allocations = allocations, original_renders = renders, original_signals = signals;
    for (auto device : {create.hDrvDevice, other.hDrvDevice}) {
        auto *owner = context(device);
        const auto references = owner->references.load();
        output = poison;
        REQUIRE(mask(device) == 1);
        node_map(device, 1, &output.value);
        REQUIRE(output.value == 0 && output.before == poison.before &&
            output.adjacent == poison.adjacent && output.after == poison.after);
        REQUIRE(owner->references == references);
    }
    REQUIRE(error_calls == original_errors);
    std::puts("PASS NODE_METADATA typed live native devices: physical mask1, node ordinal0, one UINT output, no reference leak");

    // Unsupported count/null shapes are a defensive policy, not an optional
    // size-query convention inferred from the sparse Microsoft callback page.
    for (auto device : {create.hDrvDevice, other.hDrvDevice}) {
        auto *owner = context(device);
        const auto references = owner->references.load();
        const auto runtime = owner->runtime_device;
        for (UINT count : {0u, 2u, UINT_MAX}) {
            output = poison;
            const auto before = error_calls;
            node_map(device, count, &output.value);
            REQUIRE(untouched() && error_calls == before + 1 && owner->last_error == E_INVALIDARG &&
                last_runtime.handle == runtime.handle && owner->references == references);
        }
        for (UINT count : {0u, 1u, 2u}) {
            const auto before = error_calls;
            node_map(device, count, nullptr);
            REQUIRE(error_calls == before + 1 && owner->last_error == E_INVALIDARG &&
                last_runtime.handle == runtime.handle && owner->references == references);
        }
        // An ordinary argument error must not poison the next valid query.
        output = poison;
        REQUIRE(mask(device) == 1);
        node_map(device, 1, &output.value);
        REQUIRE(output.value == 0 && output.adjacent == poison.adjacent);
    }
    REQUIRE(query_calls == original_queries && loads == original_loads &&
        allocations == original_allocations && renders == original_renders && signals == original_signals);
    std::puts("PASS NODE_METADATA defensive count/null rejection: no partial writes, exact runtime error owner, no KMD/loader work");

    const auto before_invalid = error_calls;
    Context bridge; // Readable bridge-only Context has no native runtime owner.
    for (D3D12DDI_HDEVICE invalid : {D3D12DDI_HDEVICE{},
            D3D12DDI_HDEVICE{empty_memory.data()}, D3D12DDI_HDEVICE{&bridge}}) {
        output = poison;
        REQUIRE(mask(invalid) == 0);
        node_map(invalid, 1, &output.value);
        REQUIRE(untouched());
    }
    REQUIRE(error_calls == before_invalid && bridge.references == 1);
    adapter.pfnDestroyDevice(other.hDrvDevice);
    output = poison;
    REQUIRE(mask(other.hDrvDevice) == 0);
    node_map(other.hDrvDevice, 1, &output.value);
    REQUIRE(untouched() && error_calls == before_invalid);
    std::puts("PASS NODE_METADATA null, readable unowned and torn-down runtime slots publish no metadata or callback");

    for (HRESULT fatal : {DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET,
            DXGI_ERROR_DEVICE_HUNG, DXGI_ERROR_DRIVER_INTERNAL_ERROR}) {
        ctx->last_error = fatal;
        const auto before = error_calls, references = ctx->references.load();
        output = poison;
        REQUIRE(mask(create.hDrvDevice) == 0);
        node_map(create.hDrvDevice, 1, &output.value);
        REQUIRE(untouched() && error_calls == before + 2 && ctx->last_error == fatal &&
            last_runtime.handle == expected_device && ctx->references == references);
        REQUIRE(backend_removal_reason == fatal);
    }
    ctx->last_error = S_OK;
    std::puts("PASS NODE_METADATA all four fatal device states preserve output and owning error status");

    // The error callback may reenter both DDIs and then release both caller
    // allocations. The outer query must retain only Context and never touch
    // runtime device/map storage after reporting the invalid shape.
    auto *page = static_cast<UINT *>(VirtualAlloc(nullptr, 4096,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(page); page[0] = poison.value; page[1] = poison.adjacent;
    ++ctx->references;
    const auto before_reentry = error_calls;
    nested_error_callback = [&]() {
        if (last_runtime.handle != expected_device || page[0] != poison.value || page[1] != poison.adjacent)
            fixture_abort(__LINE__);
        UINT nested = 0xeeeeeeeeu;
        if (mask(create.hDrvDevice) != 1) fixture_abort(__LINE__);
        node_map(create.hDrvDevice, 1, &nested);
        if (nested != 0) fixture_abort(__LINE__);
        adapter.pfnDestroyDevice(create.hDrvDevice);
        // Still-readable retired slot and retained Context during this callback.
        if (mask(create.hDrvDevice) != 0) fixture_abort(__LINE__);
        node_map(create.hDrvDevice, 1, &nested);
        if (nested != 0) fixture_abort(__LINE__);
        if (!VirtualFree(page, 0, MEM_RELEASE) || !VirtualFree(memory, 0, MEM_RELEASE))
            fixture_abort(__LINE__);
    };
    node_map(create.hDrvDevice, 2, page);
    nested_error_callback = {};
    REQUIRE(error_calls == before_reentry + 1 && ctx->native_retiring &&
        !ctx->runtime_device.handle && !ctx->report && ctx->references == 1);
    release(ctx);
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK && !native_contract_complete());
    std::puts("PASS NODE_METADATA reentrant query/destroy/unmap retains Context and never revisits expired runtime storage");
    std::puts("PASS NODE_METADATA closed versions0/native_contract_complete=false/native_surfaces=false; local typed peers only");
    return 0;
}
