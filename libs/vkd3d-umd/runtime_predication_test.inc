/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_predication() {
    struct Inputs { Object command{}, resource{}; };
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{}; D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{}; D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
    um.pfnSetErrorCb = test_error; um.pfnSetCommandListErrorCb = test_command_error;
    NativeDevice storage{}; D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = &storage; create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queues) == S_OK);
    for (unsigned mode = 0; mode < 4; ++mode) {
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
        auto *ctx = context(create.hDrvDevice); REQUIRE(retain_live(ctx));
        Object allocator_slot{}; D3D12DDIARG_CREATECOMMANDALLOCATOR allocator{};
        allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_slot;
        allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice, &allocator) == S_OK);
        auto *p = static_cast<Inputs *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(p); new (p) Inputs{};
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 request{};
        request.hDrvCommandList.pDrvPrivate = &p->command; request.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
        request.hRTCommandList.handle = reinterpret_cast<HANDLE>(uintptr_t(0xbca180 + mode));
        request.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; request.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &request) == S_OK);
        auto *resource = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_BUFFER, 0x40000, 4096});
        REQUIRE(bind(ctx, &p->resource, resource, VKDU_BUFFER) == S_OK);
        const auto calls = predication_calls, device_errors = error_calls;
        const auto errors = command_errors.size(); bool retired = false;
        auto retire = [&]() {
            if (!TryAcquireSRWLockExclusive(&ctx->resources_lock)) fixture_abort(__LINE__);
            ReleaseSRWLockExclusive(&ctx->resources_lock);
            if (mode && mode != 3) wait_backend_worker(&native_runtime_callbacks, ctx);
            else if (ctx->native_backend_calls || reinterpret_cast<ImportPeer *>(p->resource.backend)->references != 1)
                fixture_abort(__LINE__);
            VioGpuD3D12BridgeUnbindObject(&p->resource);
            table.pfnDestroyCommandList(create.hDrvDevice, request.hDrvCommandList);
            table.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
            adapter.pfnDestroyDevice(create.hDrvDevice);
            std::memset(&storage, 0xa5, sizeof(storage));
            if (!VirtualFree(p, 0, MEM_RELEASE)) fixture_abort(__LINE__);
            retired = true;
        };
        predication_result = mode == 1 ? S_OK : E_OUTOFMEMORY;
        if (!mode || mode == 3) nested_command_error = retire; else predication_callback = retire;
        if (mode == 3) commands.pfnSOSetTargets(request.hDrvCommandList, 4, 1, nullptr);
        else commands.pfnSetPredication(request.hDrvCommandList, {&p->resource}, 0x123456780ull, D3D12DDI_PREDICATION_OP_NOT_EQUAL_ZERO);
        nested_command_error = {}; predication_callback = {}; predication_result = S_OK;
        REQUIRE(retired && predication_calls == calls + (mode != 3) && error_calls == device_errors &&
            command_errors.size() == errors + (mode == 0 || mode == 3) && !ctx->native_command_objects && !ctx->resources);
        if (!mode) REQUIRE(command_errors.back() == std::make_pair(request.hRTCommandList.handle, HRESULT(E_OUTOFMEMORY)));
        if (mode == 3) REQUIRE(command_errors.back() == std::make_pair(request.hRTCommandList.handle, HRESULT(E_INVALIDARG)));
        release(ctx);
    }
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    puts("PASS actual native predication and SO early error: callbacks retire resource/command/allocator/device and VirtualFree slots, exact error owner, unlocked backend, no late callbacks (controlled peers)");
    return 0;
}
