/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_alias_barriers() {
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
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queues) == S_OK);
    for (unsigned cancel = 0; cancel < 2; ++cancel) {
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
        auto *ctx = context(create.hDrvDevice); REQUIRE(retain_live(ctx));
        Object allocator_slot{}, texture{};
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator{};
        allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_slot;
        allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice, &allocator) == S_OK);
        struct Page { Object command, a, b; D3D12DDIARG_RESOURCE_BARRIER_0003 barriers[4]; };
        static_assert(sizeof(Page) < 4096, "one fixture page");
        auto *page = static_cast<Page *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(page); *page = {};
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 request{};
        request.hDrvCommandList.pDrvPrivate = &page->command; request.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
        request.hRTCommandList.handle = reinterpret_cast<HANDLE>(uintptr_t(0xa11500));
        request.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; request.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &request) == S_OK);
        auto *a = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_BUFFER, 0x200000, 4096});
        auto *b = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_BUFFER, 0x210000, 4096});
        auto *t = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_TEXTURE2D, 0, 0});
        REQUIRE(bind(ctx, &page->a, a, VKDU_BUFFER) == S_OK);
        REQUIRE(bind(ctx, &page->b, b, VKDU_BUFFER) == S_OK);
        REQUIRE(bind(ctx, &texture, t, VKDU_TEXTURE2D) == S_OK);
        for (unsigned i = 0; i < 4; ++i) {
            page->barriers[i].Type = D3D12DDI_RESOURCE_BARRIER_TYPE_ALIASING;
            page->barriers[i].Aliasing.hResourceBefore.pDrvPrivate = i & 1 ? nullptr : &page->a;
            page->barriers[i].Aliasing.hResourceAfter.pDrvPrivate = i & 2 ? nullptr : &page->b;
        }
        barrier_result = S_OK;
        commands.pfnResourceBarrier(request.hDrvCommandList, 4, page->barriers);
        REQUIRE(recorded_barriers.size() == 4);
        for (unsigned i = 0; i < 4; ++i) REQUIRE(recorded_barriers[i].type == VKDU_BARRIER_ALIASING &&
            recorded_barriers[i].resource == (i & 1 ? nullptr : a) &&
            recorded_barriers[i].alias_after == (i & 2 ? nullptr : b));
        const auto calls = barrier_calls;
        page->barriers[3].Aliasing.hResourceAfter.pDrvPrivate = &texture;
        commands.pfnResourceBarrier(request.hDrvCommandList, 4, page->barriers);
        REQUIRE(barrier_calls == calls && command_errors.back().second == E_NOTIMPL);
        page->barriers[3].Aliasing.hResourceAfter.pDrvPrivate = reinterpret_cast<void *>(uintptr_t(1));
        commands.pfnResourceBarrier(request.hDrvCommandList, 4, page->barriers);
        REQUIRE(barrier_calls == calls && command_errors.back().second == E_INVALIDARG);
        page->barriers[3].Aliasing.hResourceAfter = {};
        VioGpuD3D12BridgeUnbindObject(&texture);
        const auto errors = command_errors.size();
        const auto device_errors = error_calls;
        bool callback_seen = false;
        backend_barriers_callback = [&]() {
            // Failure to release either lock would deadlock reentrant destroy
            // or block the existing backend worker completion check.
            if (!TryAcquireSRWLockExclusive(&ctx->resources_lock)) fixture_abort(__LINE__);
            ReleaseSRWLockExclusive(&ctx->resources_lock);
            if (native_runtime_status(ctx) != S_OK) fixture_abort(__LINE__);
            // This command-only fixture has no KMD context/heap callbacks;
            // completion is unsupported, but the worker must return unlocked.
            wait_backend_worker(&native_runtime_callbacks, ctx);
            VioGpuD3D12BridgeUnbindObject(&page->a); VioGpuD3D12BridgeUnbindObject(&page->b);
            table.pfnDestroyCommandList(create.hDrvDevice, request.hDrvCommandList);
            if (cancel) {
                table.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
                adapter.pfnDestroyDevice(create.hDrvDevice);
                std::memset(&storage, 0xa5, sizeof(storage));
            }
            if (!VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
            callback_seen = true;
        };
        commands.pfnResourceBarrier(request.hDrvCommandList, 4, page->barriers);
        REQUIRE(callback_seen && !backend_barriers_callback && command_errors.size() == errors && error_calls == device_errors &&
            !ctx->resources && !ctx->native_command_objects);
        if (!cancel) {
            table.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
            adapter.pfnDestroyDevice(create.hDrvDevice);
        }
        release(ctx);
    }
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    puts("PASS native alias barriers: four endpoint forms, late invalid/texture atomic rejection, unlocked callbacks, both resources/command retained and unmapped caller page at normal/device-cancel return (controlled peers)");
    return 0;
}
