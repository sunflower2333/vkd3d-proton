/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_queries() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
        D3D12DDI_ADAPTERFUNCS adapter{};
        D3D12DDIARG_OPENADAPTER open{};
        open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
        REQUIRE(OpenAdapter12(&open) == S_OK);
        D3DDDI_DEVICECALLBACKS kt{};
        D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
        um.pfnSetErrorCb = test_error; um.pfnSetCommandListErrorCb = test_command_error;
        alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> device_memory{};
        D3D12DDIARG_CREATEDEVICE_0003 create{};
        create.hDrvDevice.pDrvPrivate = device_memory.data(); create.hRTDevice.handle = expected_device;
        create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
        create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
        auto *ctx = context(create.hDrvDevice);
        D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
        REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queue) == S_OK);
        REQUIRE(commands.pfnBeginQuery && commands.pfnEndQuery && commands.pfnResolveQueryData && !native_contract_complete());
        Object allocator_memory{}, command_memory{}, buffer{};
        auto *query_slot = static_cast<Object *>(VirtualAlloc(nullptr, sizeof(Object), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(query_slot);
        D3D12DDI_HQUERYHEAP query{query_slot};
        D3D12DDIARG_CREATE_QUERY_HEAP_0001 desc{D3D12DDI_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 16, 1};
        REQUIRE(table.pfnCreateQueryHeap(create.hDrvDevice, &desc, query) == S_OK);
        auto *resource = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_BUFFER, 0x400000, 8192});
        REQUIRE(VioGpuD3D12BridgeBindObject(create.hDrvDevice, &buffer, resource, VKDU_BUFFER) == S_OK);
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator{};
        allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_memory;
        allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice, &allocator) == S_OK);
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 list{};
        list.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
        list.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; list.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        list.hDrvCommandList.pDrvPrivate = &command_memory;
        list.hRTCommandList.handle = reinterpret_cast<HANDLE>(uintptr_t(0x773390));
        REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &list) == S_OK);
        command_errors.clear(); query_execution_result = S_OK;
        commands.pfnBeginQuery(list.hDrvCommandList, query, D3D12DDI_QUERY_TYPE_PIPELINE_STATISTICS, 7);
        if (!command_errors.empty() || recorded_query_type != 3 || recorded_query_first != 7 || recorded_query_operation != 0) {
            std::fputs("FAIL native query CORE_0003 field order\n", stderr); return 1;
        }
        commands.pfnEndQuery(list.hDrvCommandList, query, D3D12DDI_QUERY_TYPE_PIPELINE_STATISTICS, 7);
        REQUIRE(command_errors.empty() && recorded_query_operation == 1);
        commands.pfnResolveQueryData(list.hDrvCommandList, query, D3D12DDI_QUERY_TYPE_PIPELINE_STATISTICS, 5, 2, {&buffer}, 4096);
        REQUIRE(command_errors.empty() && recorded_query_first == 5 && recorded_query_count == 2 &&
            recorded_query_offset == 4096 && recorded_query_operation == 2);
        for (void *invalid : {reinterpret_cast<void *>(uintptr_t(1)), static_cast<void *>(&buffer), static_cast<void *>(&allocator_memory)}) {
            const auto calls = query_execution_calls;
            commands.pfnEndQuery(list.hDrvCommandList, {invalid}, D3D12DDI_QUERY_TYPE_PIPELINE_STATISTICS, 7);
            REQUIRE(query_execution_calls == calls && command_errors.back().second == E_INVALIDARG);
            commands.pfnResolveQueryData(list.hDrvCommandList, query, D3D12DDI_QUERY_TYPE_PIPELINE_STATISTICS,
                0, 1, {reinterpret_cast<void *>(uintptr_t(1))}, 0);
            REQUIRE(query_execution_calls == calls && command_errors.back().second == E_INVALIDARG);
        }
        auto *registry = ctx->query_heaps; ctx->query_heaps = nullptr;
        const auto calls = query_execution_calls;
        commands.pfnBeginQuery(list.hDrvCommandList, query, D3D12DDI_QUERY_TYPE_PIPELINE_STATISTICS, 7);
        REQUIRE(query_execution_calls == calls && command_errors.back().second == E_INVALIDARG);
        ctx->query_heaps = registry;
        const auto saved_generation = generation;
        const auto errors_before_callback = command_errors.size();
        query_execution_callback = [&] {
            if (mode == 1) { ++generation; return; }
            table.pfnDestroyQueryHeap(create.hDrvDevice, query);
            if (!VirtualFree(query_slot, 0, MEM_RELEASE)) fixture_abort(__LINE__);
            query_slot = nullptr;
            VioGpuD3D12BridgeUnbindObject(&buffer); std::memset(&buffer, 0xa5, sizeof(buffer));
            table.pfnDestroyCommandList(create.hDrvDevice, list.hDrvCommandList);
            std::memset(&command_memory, 0xa5, sizeof(command_memory));
            if (mode == 2) {
                table.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
                adapter.pfnDestroyDevice(create.hDrvDevice);
                device_memory.fill(0xa5);
            }
        };
        query_execution_result = mode == 0 ? E_OUTOFMEMORY : S_OK;
        commands.pfnResolveQueryData(list.hDrvCommandList, query, D3D12DDI_QUERY_TYPE_PIPELINE_STATISTICS, 5, 2, {&buffer}, 4096);
        query_execution_callback = {}; query_execution_result = S_OK;
        if (mode == 2) REQUIRE(command_errors.size() == errors_before_callback);
        else REQUIRE(command_errors.back() == std::make_pair(list.hRTCommandList.handle,
            HRESULT(mode == 0 ? E_OUTOFMEMORY : DXGI_ERROR_DEVICE_REMOVED)));
        generation = saved_generation;
        if (mode == 1) {
            table.pfnDestroyCommandList(create.hDrvDevice, list.hDrvCommandList);
            VioGpuD3D12BridgeUnbindObject(&buffer);
            table.pfnDestroyQueryHeap(create.hDrvDevice, query);
            REQUIRE(VirtualFree(query_slot, 0, MEM_RELEASE));
        }
        if (mode != 2) table.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
        if (mode != 2) adapter.pfnDestroyDevice(create.hDrvDevice);
        REQUIRE(!query_peers && adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    }
    std::puts("PASS native query execution CORE_0003 ABI, owned handles, freed caller slots, reset and retirement");
    return 0;
}
