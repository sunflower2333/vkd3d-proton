/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_indirect() {
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
    Object signature{}, buffer{}, count{}, allocator_memory{}, command_memory{};
    D3D12DDI_HCOMMANDSIGNATURE signature_handle{&signature};
    D3D12DDI_INDIRECT_ARGUMENT_DESC argument_desc{};
    argument_desc.Type = D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 desc{};
    desc.ByteStride = 16; desc.NumArgumentDescs = 1; desc.pArgumentDescs = &argument_desc; desc.NodeMask = 1;
    REQUIRE(table.pfnCalcPrivateCommandSignatureSize(create.hDrvDevice, &desc) == sizeof(Object));
    auto make_signature = [&]() { return table.pfnCreateCommandSignature(create.hDrvDevice, &desc, signature_handle); };
    const unsigned old_creates = signature_creates;
    for (UINT stride : {0u, 8u, 13u}) {
        desc.ByteStride = stride;
        REQUIRE(table.pfnCalcPrivateCommandSignatureSize(create.hDrvDevice, &desc) == 0);
        REQUIRE(make_signature() == E_INVALIDARG);
    }
    desc.ByteStride = 16; desc.NodeMask = 2; REQUIRE(make_signature() == E_INVALIDARG);
    desc.NodeMask = 1; desc.NumArgumentDescs = 2; REQUIRE(make_signature() == DXGI_ERROR_UNSUPPORTED);
    desc.NumArgumentDescs = 1; desc.pArgumentDescs = nullptr; REQUIRE(make_signature() == E_INVALIDARG);
    desc.pArgumentDescs = &argument_desc; argument_desc.Type = D3D12DDI_INDIRECT_ARGUMENT_TYPE_DRAW;
    REQUIRE(make_signature() == DXGI_ERROR_UNSUPPORTED);
    argument_desc.Type = D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    desc.hRootSignature.pDrvPrivate = reinterpret_cast<void *>(uintptr_t(1)); REQUIRE(make_signature() == E_INVALIDARG);
    desc.hRootSignature = {}; REQUIRE(signature_creates == old_creates);
    signature_result = E_OUTOFMEMORY; REQUIRE(make_signature() == E_OUTOFMEMORY && !signature.magic);
    signature_result = S_OK;
    backend_signature_create_callback = [&] { desc.ByteStride = 32; };
    REQUIRE(make_signature() == S_OK && recorded_indirect_stride == 16);
    backend_signature_create_callback = {}; desc.ByteStride = 16;
    const auto after_create = signature_creates;
    REQUIRE(make_signature() == E_INVALIDARG && signature_creates == after_create);
    auto *argument_peer = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_BUFFER, 0x100000, 4096});
    auto *count_peer = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_BUFFER, 0x200000, 4096});
    REQUIRE(VioGpuD3D12BridgeBindObject(create.hDrvDevice, &buffer, argument_peer, VKDU_BUFFER) == S_OK);
    REQUIRE(VioGpuD3D12BridgeBindObject(create.hDrvDevice, &count, count_peer, VKDU_BUFFER) == S_OK);
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator{};
    allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_memory;
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    REQUIRE(table.pfnCreateCommandAllocator(create.hDrvDevice, &allocator) == S_OK);
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 list{};
    list.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
    list.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; list.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    list.hRTCommandList.handle = reinterpret_cast<HANDLE>(uintptr_t(0x99550));
    list.hDrvCommandList.pDrvPrivate = &command_memory;
    REQUIRE(table.pfnCreateCommandList(create.hDrvDevice, &list) == S_OK);
    D3D12DDIARG_BUFFER_PLACEMENT args{}, counts{};
    args.BaseAddress.UMD.hResource = {&buffer}; args.BaseAddress.UMD.Offset = 64;
    counts.BaseAddress.UMD.hResource = {&count}; counts.BaseAddress.UMD.Offset = 20;
    command_errors.clear();
    commands.pfnExecuteIndirect(list.hDrvCommandList, signature_handle, 3, args, counts);
    if (!command_errors.empty() || recorded_indirect_maximum != 3 || recorded_arguments != argument_peer ||
        recorded_count != count_peer || recorded_signature != signature.backend || recorded_argument_offset != 64 || recorded_count_offset != 20) {
        std::fputs("FAIL native indirect dispatch lost GPU count or buffer placement\n", stderr); return 1;
    }
    commands.pfnExecuteIndirect(list.hDrvCommandList, signature_handle, 2, args, {});
    REQUIRE(recorded_count == nullptr && recorded_count_offset == 0 && recorded_indirect_maximum == 2);
    for (void *invalid : {reinterpret_cast<void *>(uintptr_t(1)), static_cast<void *>(&allocator_memory), static_cast<void *>(&command_memory)}) {
        const auto before = indirect_calls;
        commands.pfnExecuteIndirect(list.hDrvCommandList, {invalid}, 3, args, counts);
        REQUIRE(indirect_calls == before && command_errors.back() == std::make_pair(list.hRTCommandList.handle, HRESULT(E_INVALIDARG)));
        auto bad = args; bad.BaseAddress.UMD.hResource = {invalid};
        commands.pfnExecuteIndirect(list.hDrvCommandList, signature_handle, 3, bad, counts);
        REQUIRE(indirect_calls == before && command_errors.back().second == E_INVALIDARG);
        bad = counts; bad.BaseAddress.UMD.hResource = {invalid};
        commands.pfnExecuteIndirect(list.hDrvCommandList, signature_handle, 3, args, bad);
        REQUIRE(indirect_calls == before && command_errors.back().second == E_INVALIDARG);
    }
    auto *registry = ctx->indirect_signatures; ctx->indirect_signatures = nullptr;
    const auto before_stale = indirect_calls;
    commands.pfnExecuteIndirect(list.hDrvCommandList, signature_handle, 3, args, counts);
    REQUIRE(indirect_calls == before_stale && command_errors.back().second == E_INVALIDARG);
    ctx->indirect_signatures = registry;
    indirect_result = E_OUTOFMEMORY;
    nested_command_error = [&] {
        table.pfnDestroyCommandSignature(create.hDrvDevice, signature_handle);
        std::memset(&signature, 0xa5, sizeof(signature));
    };
    commands.pfnExecuteIndirect(list.hDrvCommandList, signature_handle, 3, args, counts);
    nested_command_error = {};
    REQUIRE(command_errors.back() == std::make_pair(list.hRTCommandList.handle, HRESULT(E_OUTOFMEMORY)));
    REQUIRE(!ctx->indirect_signatures);
    REQUIRE(make_signature() == S_OK);
    indirect_result = S_OK;
    const auto errors_before_retire = error_calls;
    backend_indirect_callback = [&] {
        // Every backend owner must survive immediate destruction/poisoning of
        // the runtime's private slots, including argument/count/signature slots.
        table.pfnDestroyCommandSignature(create.hDrvDevice, signature_handle);
        table.pfnDestroyCommandList(create.hDrvDevice, list.hDrvCommandList);
        VioGpuD3D12BridgeUnbindObject(&buffer); VioGpuD3D12BridgeUnbindObject(&count);
        table.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
        adapter.pfnDestroyDevice(create.hDrvDevice);
        std::memset(&signature, 0xa5, sizeof(signature)); std::memset(&command_memory, 0xa5, sizeof(command_memory));
        std::memset(&buffer, 0xa5, sizeof(buffer)); std::memset(&count, 0xa5, sizeof(count)); device_memory.fill(0xa5);
    };
    indirect_result = DXGI_ERROR_DEVICE_REMOVED;
    const auto prior_command_errors = command_errors.size();
    commands.pfnExecuteIndirect(list.hDrvCommandList, signature_handle, 3, args, counts);
    backend_indirect_callback = {}; indirect_result = S_OK;
    REQUIRE(error_calls == errors_before_retire && command_errors.size() == prior_command_errors);
    for (auto byte : device_memory) REQUIRE(byte == 0xa5);
    // Constructor cancellation must never publish to retired runtime memory.
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    std::memset(&signature, 0xa5, sizeof(signature));
    backend_signature_create_callback = [&] { adapter.pfnDestroyDevice(create.hDrvDevice); device_memory.fill(0xa5); };
    REQUIRE(make_signature() == DXGI_ERROR_DEVICE_REMOVED);
    backend_signature_create_callback = {};
    for (size_t i = 0; i < sizeof(signature); ++i) REQUIRE(reinterpret_cast<unsigned char *>(&signature)[i] == 0xa5);
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    std::puts("PASS native indirect signatures, GPU argument/count forwarding, rejected handles and reentrant ownership");
    return 0;
}
