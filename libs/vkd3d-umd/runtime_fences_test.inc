/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_fences() {
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    kt.pfnCreateContextCb = heap_context_create; kt.pfnDestroyContextCb = heap_context_destroy;
    kt.pfnEscapeCb = heap_escape; kt.pfnAllocateCb = heap_allocate; kt.pfnDeallocateCb = heap_deallocate;
    kt.pfnLockCb = heap_lock; kt.pfnUnlockCb = heap_unlock;
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
    um.pfnCreateContextCb = queue_context_create; um.pfnDestroyContextCb = queue_context_destroy;
    um.pfnSetErrorCb = test_error;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> memory{}, foreign_memory{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = memory.data(); create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto foreign = create;
    foreign.hDrvDevice.pDrvPrivate = foreign_memory.data();
    foreign.hRTDevice.handle = reinterpret_cast<HANDLE>(uintptr_t(0x78910));
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &foreign) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queue) == S_OK);
    REQUIRE(table.pfnCalcPrivateFenceSize && table.pfnCreateFence && table.pfnDestroyFence);
    REQUIRE(queue.pfnSignalFence && queue.pfnWaitForFence && !native_contract_complete());
    Object queue_memory{};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 request{};
    request.hDrvCommandQueue.pDrvPrivate = &queue_memory;
    request.hRTCommandQueue.handle = reinterpret_cast<HANDLE>(uintptr_t(0x78220));
    request.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D; request.NodeMask = 1;
    REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice, &request) == S_OK);
    REQUIRE(runtime_contexts.at(queue_memory.runtime_route->native_heap_context) == request.hRTCommandQueue.handle);

    alignas(NativeFenceSlot) std::array<unsigned char, sizeof(NativeFenceSlot) + 16> storage, foreign_storage;
    storage.fill(0xa5); foreign_storage.fill(0xa5);
    D3D12DDI_HFENCE handle{storage.data()}, other{foreign_storage.data()};
    D3D12DDI_FENCE description{};
    // Deliberately non-CPU addresses. A pointer cast/dereference would fail.
    description.FenceValue.BaseAddress = 0xfedcba9800000040ull;
    description.FenceMonitoredValue.BaseAddress = 0xabcd987600000080ull;
    description.Flags = D3D12DDI_FENCE_FLAG_BOTTOM_OF_PIPE;
    const auto original_description = description;
    D3D12DDIARG_CREATE_FENCE args{1, &description};
    const auto initial_references = ctx->references.load();
    REQUIRE(table.pfnCalcPrivateFenceSize(create.hDrvDevice, &args) == sizeof(NativeFenceSlot));
    REQUIRE(!table.pfnCalcPrivateFenceSize(create.hDrvDevice, nullptr));
    REQUIRE(table.pfnCreateFence(create.hDrvDevice, handle, nullptr) == E_INVALIDARG);
    args.FenceCount = 0;
    REQUIRE(table.pfnCreateFence(create.hDrvDevice, handle, &args) == E_INVALIDARG);
    args.FenceCount = 2;
    REQUIRE(table.pfnCreateFence(create.hDrvDevice, handle, &args) == DXGI_ERROR_UNSUPPORTED);
    args.FenceCount = 1; args.Fences = nullptr;
    REQUIRE(table.pfnCreateFence(create.hDrvDevice, handle, &args) == E_INVALIDARG);
    args.Fences = &description; description.Flags = static_cast<D3D12DDI_FENCE_FLAGS>(2);
    REQUIRE(table.pfnCreateFence(create.hDrvDevice, handle, &args) == DXGI_ERROR_UNSUPPORTED);
    REQUIRE(!table.pfnCalcPrivateFenceSize(create.hDrvDevice, &args));
    for (auto byte : storage) REQUIRE(byte == 0xa5);
    REQUIRE(ctx->references == initial_references && !ctx->native_fences);
    description = original_description;
    REQUIRE(table.pfnCreateFence(create.hDrvDevice, {}, &args) == E_INVALIDARG);
    const auto before_creates = allocations, before_renders = renders, before_signals = signals;
    REQUIRE(table.pfnCreateFence(create.hDrvDevice, handle, &args) == S_OK);
    REQUIRE(table.pfnCreateFence(create.hDrvDevice, handle, &args) == E_INVALIDARG);
    REQUIRE(ctx->references == initial_references + 1);
    for (size_t i = sizeof(NativeFenceSlot); i < storage.size(); ++i) REQUIRE(storage[i] == 0xa5);
    description = {}; args = {}; // Neither this request nor its array is retained.
    const auto *record = native_fence_find(ctx, handle.pDrvPrivate);
    REQUIRE(record && record->description.FenceValue.BaseAddress == original_description.FenceValue.BaseAddress &&
        record->description.FenceMonitoredValue.BaseAddress == original_description.FenceMonitoredValue.BaseAddress &&
        record->description.Flags == original_description.Flags);
    D3D12DDIARG_CREATE_FENCE software{1, &description};
    REQUIRE(table.pfnCreateFence(foreign.hDrvDevice, other, &software) == S_OK);
    const auto before_errors = error_calls;
    const auto before_command_errors = command_errors.size();
    for (auto operation : {queue.pfnSignalFence, queue.pfnWaitForFence}) {
        for (UINT64 value : {UINT64(0), UINT64(9), UINT64(2), UINT64_MAX}) {
            D3D12DDIARG_FENCE_OPERATION packet{handle, value, 0xdeadbeefu};
            operation(request.hDrvCommandQueue, &packet);
            if (packet.PhysicalAdapterMask != 1) {
                std::fputs("FAIL native fence omitted single-node runtime broadcast mask\n", stderr); return 1;
            }
            REQUIRE(packet.Fence.pDrvPrivate == handle.pDrvPrivate && packet.Value == value);
        }
        for (void *invalid : {other.pDrvPrivate, static_cast<void *>(&queue_memory),
                reinterpret_cast<void *>(uintptr_t(1)), static_cast<void *>(nullptr)}) {
            D3D12DDIARG_FENCE_OPERATION packet{{invalid}, 27, 0xffffffffu};
            const auto old_errors = error_calls;
            operation(request.hDrvCommandQueue, &packet);
            if (packet.PhysicalAdapterMask || error_calls != old_errors + 1) {
                std::fputs("FAIL native fence accepted foreign or stale runtime ownership\n", stderr); return 1;
            }
            REQUIRE(ctx->last_error == E_INVALIDARG && last_runtime.handle == expected_device);
            REQUIRE(packet.Value == 27 && packet.Fence.pDrvPrivate == invalid);
        }
        operation(request.hDrvCommandQueue, nullptr);
    }
    REQUIRE(error_calls == before_errors + 10 && command_errors.size() == before_command_errors);
    REQUIRE(allocations == before_creates && renders == before_renders && signals == before_signals);
    D3D12DDIARG_FENCE_OPERATION recursive_render{handle, 33, 0xffffffffu};
    ctx->native_submitting = true;
    queue.pfnSignalFence(request.hDrvCommandQueue, &recursive_render);
    ctx->native_submitting = false;
    REQUIRE(!recursive_render.PhysicalAdapterMask && ctx->last_error == DXGI_ERROR_WAS_STILL_DRAWING);
    // The DDI has only selected a mask. No private fence, KMT packet, CPU store
    // or blocking wait can masquerade as the runtime's external synchronization.
    REQUIRE(runtime_contexts.at(queue_memory.runtime_route->native_heap_context) == request.hRTCommandQueue.handle);
    table.pfnDestroyFence(foreign.hDrvDevice, handle);
    REQUIRE(native_fence_find(ctx, handle.pDrvPrivate) == record);
    table.pfnDestroyFence(create.hDrvDevice, handle);
    REQUIRE(!ctx->native_fences && ctx->references == initial_references);
    D3D12DDIARG_FENCE_OPERATION stale{handle, 8, 0xffffffffu};
    queue.pfnWaitForFence(request.hDrvCommandQueue, &stale);
    REQUIRE(!stale.PhysicalAdapterMask);
    storage.fill(0xa5);
    REQUIRE(table.pfnCreateFence(create.hDrvDevice, handle, &software) == S_OK);
    table.pfnDestroyFence(foreign.hDrvDevice, other);
    adapter.pfnDestroyDevice(foreign.hDrvDevice);

    // An invalid request clears output before reporting to the actual owning
    // runtime device. That callback unmaps the request, destroys and poisons
    // queue storage, and retires the device with a live fence still attached.
    auto *page = static_cast<D3D12DDIARG_FENCE_OPERATION *>(VirtualAlloc(nullptr, 4096,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(page != nullptr);
    *page = {{reinterpret_cast<void *>(uintptr_t(1))}, 31, 0xffffffffu};
    ++ctx->references; // Observe metadata retirement independently of runtime memory.
    nested_error_callback = [&] {
        if (page->PhysicalAdapterMask || last_runtime.handle != expected_device) fixture_abort(__LINE__);
        table.pfnDestroyCommandQueue(create.hDrvDevice, request.hDrvCommandQueue);
        std::memset(&queue_memory, 0xa5, sizeof(queue_memory));
        adapter.pfnDestroyDevice(create.hDrvDevice);
        memory.fill(0xa5); storage.fill(0xa5);
        if (!VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
    };
    queue.pfnSignalFence(request.hDrvCommandQueue, page);
    nested_error_callback = {};
    REQUIRE(ctx->native_retiring && !ctx->native_fences && !ctx->runtime_device.handle);
    REQUIRE(ctx->references == 1); // No forgotten fence owner pins the backend.
    for (auto byte : storage) REQUIRE(byte == 0xa5);
    release(ctx);
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK && !native_contract_complete());
    std::puts("PASS native single-node fence copies, runtime broadcast selection, foreign/stale rejection and reentrant callback retirement");
    return 0;
}
