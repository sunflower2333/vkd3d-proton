/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_uav_clear() {
    struct Inputs { Object command{}, resource{}, cpu{}, gpu{}; uint32_t values[4]; D3D12DDI_RECT rects[19]; };
    static_assert(sizeof(Inputs) <= 4096);
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
    for (unsigned mode = 0; mode < 3; ++mode) {
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
        request.hRTCommandList.handle = reinterpret_cast<HANDLE>(uintptr_t(0xc1ea80 + mode));
        request.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; request.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &request) == S_OK);
        auto *cpu = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_DESCRIPTOR_HEAP, 0x20000, 8});
        auto *gpu = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_DESCRIPTOR_HEAP, 0x30000, 8});
        auto *resource = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_BUFFER, 0x40000, 4096});
        reinterpret_cast<ImportPeer *>(gpu)->format = 2;
        REQUIRE(bind(ctx, &p->cpu, cpu, VKDU_DESCRIPTOR_HEAP) == S_OK);
        REQUIRE(bind(ctx, &p->gpu, gpu, VKDU_DESCRIPTOR_HEAP) == S_OK);
        REQUIRE(bind(ctx, &p->resource, resource, VKDU_BUFFER) == S_OK);
        p->cpu.descriptor_flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
        p->gpu.descriptor_flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        p->cpu.descriptor_type = p->gpu.descriptor_type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        p->values[0] = 0x7fc12345; p->values[1] = 2; p->values[2] = 3; p->values[3] = 0xff800000;
        for (unsigned i = 0; i < 19; ++i) p->rects[i] = {LONG(i), 0, LONG(i + 4), 1};
        const auto calls = uav_clear_calls, device_errors = error_calls;
        const auto errors = command_errors.size(); bool retired = false;
        auto retire = [&]() {
            if (!TryAcquireSRWLockExclusive(&ctx->resources_lock)) fixture_abort(__LINE__);
            ReleaseSRWLockExclusive(&ctx->resources_lock);
            if (mode) wait_backend_worker(&native_runtime_callbacks, ctx);
            else if (ctx->native_backend_calls) fixture_abort(__LINE__);
            for (auto *slot : {&p->cpu, &p->gpu, &p->resource}) {
                if (!mode && reinterpret_cast<ImportPeer *>(slot->backend)->references != 1) fixture_abort(__LINE__);
                VioGpuD3D12BridgeUnbindObject(slot);
            }
            table.pfnDestroyCommandList(create.hDrvDevice, request.hDrvCommandList);
            table.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
            adapter.pfnDestroyDevice(create.hDrvDevice);
            std::memset(&storage, 0xa5, sizeof(storage));
            if (!VirtualFree(p, 0, MEM_RELEASE)) fixture_abort(__LINE__);
            retired = true;
        };
        uav_clear_result = mode == 1 ? S_OK : E_OUTOFMEMORY;
        if (!mode) nested_command_error = retire; else uav_clear_callback = retire;
        commands.pfnClearUnorderedAccessViewFloat(request.hDrvCommandList, {0x10030000 + 4 * 32},
                {0x20000 + 2 * 32}, {&p->resource}, reinterpret_cast<float *>(p->values), 19, p->rects);
        nested_command_error = {}; uav_clear_callback = {}; uav_clear_result = S_OK;
        REQUIRE(retired && uav_clear_calls == calls + 1 && error_calls == device_errors &&
            command_errors.size() == errors + (mode == 0) && !ctx->native_command_objects &&
            !ctx->descriptor_heaps && !ctx->resources);
        if (!mode) REQUIRE(command_errors.back() == std::make_pair(request.hRTCommandList.handle, HRESULT(E_OUTOFMEMORY)));
        release(ctx);
    }
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    puts("PASS actual native UAV clear: backend/error callbacks retire heaps/resource/command/allocator/device and VirtualFree all inputs, exact error owner, unlocked backend, no late callbacks (controlled peers)");
    return 0;
}
