/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_root_clear() {
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
    um.pfnSetErrorCb = test_error; um.pfnSetCommandListErrorCb = test_command_error;
    NativeDevice storage{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = &storage; create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queues) == S_OK && commands.pfnClearRootArguments);
    for (unsigned mode = 0; mode < 3; ++mode) {
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
        auto *ctx = context(create.hDrvDevice); REQUIRE(retain_live(ctx));
        Object allocator_slot{};
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator{};
        allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_slot;
        allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice, &allocator) == S_OK);
        auto *page = static_cast<Object *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(page); *page = {};
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 request{};
        request.hDrvCommandList.pDrvPrivate = page; request.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
        request.hRTCommandList.handle = reinterpret_cast<HANDLE>(uintptr_t(0xc1ea00 + mode));
        request.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; request.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &request) == S_OK);
        root_clear_result = S_OK;
        const auto start_calls = root_clear_calls, device_errors = error_calls;
        const auto errors = command_errors.size();
        commands.pfnClearRootArguments(request.hDrvCommandList);
        REQUIRE(root_clear_calls == start_calls + 1 && command_errors.size() == errors);
        bool retired = false;
        auto retire = [&]() {
            if (!TryAcquireSRWLockExclusive(&ctx->resources_lock)) fixture_abort(__LINE__);
            ReleaseSRWLockExclusive(&ctx->resources_lock);
            if (native_runtime_status(ctx) != S_OK) fixture_abort(__LINE__);
            // Backend callbacks run with the callback mutex suspended. The
            // runtime error callback is intentionally serialized, and its
            // reentrant destroy must release that lock around backend work.
            if (mode) wait_backend_worker(&native_runtime_callbacks, ctx);
            else if (ctx->native_backend_calls) fixture_abort(__LINE__);
            table.pfnDestroyCommandList(create.hDrvDevice, request.hDrvCommandList);
            if (mode != 1) {
                table.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
                adapter.pfnDestroyDevice(create.hDrvDevice);
                std::memset(&storage, 0xa5, sizeof(storage));
            }
            if (!VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
            retired = true;
        };
        if (!mode) { root_clear_result = E_OUTOFMEMORY; nested_command_error = retire; }
        else root_clear_callback = retire;
        commands.pfnClearRootArguments(request.hDrvCommandList);
        nested_command_error = {}; root_clear_callback = {}; root_clear_result = S_OK;
        REQUIRE(retired && root_clear_calls == start_calls + 2 && error_calls == device_errors &&
            command_errors.size() == errors + (mode == 0) && !ctx->native_command_objects && !ctx->resources);
        if (!mode) REQUIRE(command_errors.back() == std::make_pair(request.hRTCommandList.handle, HRESULT(E_OUTOFMEMORY)));
        if (mode == 1) {
            table.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
            adapter.pfnDestroyDevice(create.hDrvDevice);
        }
        release(ctx);
    }
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    puts("PASS actual native ClearRootArguments: copied runtime error identity, unlocked reentry, backend command/device retirement and VirtualFree, ordered error-callback teardown, no late callbacks (controlled peers)");
    return 0;
}
