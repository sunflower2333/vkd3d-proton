/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual DDI admission and lifecycle; QueryAdapterInfo/KMT/backend are typed
 * controlled peers. No system runtime, GPU, or KMD support is inferred. */
static int test_native_queue_protocol() {
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    kt.pfnCreateContextCb = heap_context_create; kt.pfnDestroyContextCb = heap_context_destroy;
    kt.pfnEscapeCb = heap_escape; kt.pfnAllocateCb = heap_allocate; kt.pfnDeallocateCb = heap_deallocate;
    kt.pfnDeallocate2Cb = heap_deallocate2;
    kt.pfnLockCb = heap_lock; kt.pfnUnlockCb = heap_unlock;
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{};
    um.pfnCreateContextCb = queue_context_create; um.pfnDestroyContextCb = queue_context_destroy;
    um.pfnSetErrorCb = test_error;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> memory;
    memory.fill(0xa5);
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = memory.data(); create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    const unsigned initial_loads = loads, initial_creates = creates;
    const unsigned initial_contexts = heap_context_creates, initial_queues = runtime_context_creates;
    const unsigned initial_allocations = allocations, initial_renders = renders;
    const auto initial_runtime_contexts = runtime_contexts;
    // Literal wire mutations, independent of production field readers. Every
    // malformed prefix/trailer must refuse before any Vulkan or KMT side effect.
    const std::pair<UINT, UINT> mutations[] = {
        {0, 0}, {4, 1}, {8, 192}, {12, 1}, {16, 1}, {20, 1}, {24, 0},
        {112, 1}, {116, 1}, {120, 1}, {124, 1},
        {128, 0}, {132, 2}, {136, 64}, {140, 0}, {152, 2}, {156, 1},
        {160, 0}, {164, 0}, {164, 2}, {168, 0}, {168, 40},
        {172, 0}, {172, 3}, {176, 40}, {180, 32}, {184, 80}, {188, 1},
    };
    for (const auto &mutation : mutations) {
        queue_protocol_query_callback = [&](uint8_t *bytes, const D3DDDICB_QUERYADAPTERINFO *) {
            std::memcpy(bytes + mutation.first, &mutation.second, 4);
        };
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_UNSUPPORTED);
        for (auto byte : memory) REQUIRE(byte == 0xa5);
        REQUIRE(loads == initial_loads && creates == initial_creates &&
            heap_context_creates == initial_contexts && runtime_context_creates == initial_queues &&
            allocations == initial_allocations && renders == initial_renders);
    }
    for (unsigned malformed = 0; malformed < 3; ++malformed) {
        queue_protocol_query_callback = [=](uint8_t *bytes, const D3DDDICB_QUERYADAPTERINFO *args) {
            if (malformed == 0) const_cast<D3DDDICB_QUERYADAPTERINFO *>(args)->PrivateDriverDataSize = 160;
            else if (malformed == 1) const_cast<D3DDDICB_QUERYADAPTERINFO *>(args)->pPrivateDriverData = nullptr;
            else std::memset(bytes + 144, 0, 8);
        };
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_UNSUPPORTED);
        for (auto byte : memory) REQUIRE(byte == 0xa5);
        REQUIRE(loads == initial_loads && creates == initial_creates);
    }
    queue_protocol_query_callback = {};
    query_result = E_ACCESSDENIED;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == E_ACCESSDENIED && loads == initial_loads);
    query_result = S_OK;
    // Capability removed during backend construction cannot publish a device.
    const auto old_destroys = destroys, old_unloads = unloads;
    unsigned calls = 0;
    queue_protocol_query_callback = [&](uint8_t *bytes, const D3DDDICB_QUERYADAPTERINFO *) {
        if (++calls == 2) std::memset(bytes + 160, 0, 32);
    };
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == DXGI_ERROR_DEVICE_REMOVED);
    REQUIRE(calls == 2 && creates == initial_creates + 1 && destroys == old_destroys + 1 &&
        unloads == old_unloads + 1 && heap_context_creates == initial_contexts);
    for (auto byte : memory) REQUIRE(byte == 0xa5);
    queue_protocol_query_callback = {};
    const auto native_attempts_before = native_backend_attempts;
    native_backend_result = S_OK; // ICD support alone cannot enable KMD imports.
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    REQUIRE(ctx != nullptr);
    std::printf("PROTOCOL owned-only native_surface=%u native_optin_attempts=%u\n",
        unsigned(ctx->native_surfaces), native_backend_attempts - native_attempts_before);
    REQUIRE(!ctx->native_surfaces && native_backend_attempts == native_attempts_before);
    native_backend_result = DXGI_ERROR_UNSUPPORTED;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queue) == S_OK);
    NativeHeap *rejected_surface = reinterpret_cast<NativeHeap *>(uintptr_t(1));
    REQUIRE(native_surface_import(ctx, 60000, 70000, 0x2337, 65536,
        reinterpret_cast<HANDLE>(uintptr_t(0x886600)), &rejected_surface) == DXGI_ERROR_UNSUPPORTED);
    REQUIRE(rejected_surface == nullptr && heap_context_creates == initial_contexts &&
        allocations == initial_allocations && !ctx->native_heaps);
    // Closed public native-open entry cannot claim a slot or import a share.
    REQUIRE(table.pfnOpenHeapAndResource(create.hDrvDevice, nullptr, {}, {}, {}) == DXGI_ERROR_UNSUPPORTED);
    Object slots[2]{};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 request{};
    request.hDrvCommandQueue.pDrvPrivate = &slots[0];
    request.hRTCommandQueue.handle = reinterpret_cast<HANDLE>(uintptr_t(0x78110));
    request.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D; request.NodeMask = 1;
    const unsigned before_backend_queues = queue_creates;
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        queue_protocol_query_callback = [=](uint8_t *bytes, const D3DDDICB_QUERYADAPTERINFO *) {
            if (invalid == 0) std::memset(bytes + 160, 0, 32);
            else if (invalid == 1) ++bytes[144];
            else if (invalid == 2) ++bytes[24];
            else if (invalid == 3) bytes[180] = 32;
            else bytes[188] = 1;
        };
        REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice, &request) == DXGI_ERROR_DEVICE_REMOVED);
        REQUIRE(!ctx->native_heap_context && !ctx->native_queue_objects && !ctx->native_runtime_queues);
        REQUIRE(heap_context_creates == initial_contexts && runtime_context_creates == initial_queues &&
            queue_creates == before_backend_queues && allocations == initial_allocations && renders == initial_renders);
        REQUIRE(!slots[0].magic);
    }
    queue_protocol_query_callback = {};
    const auto before_queries = queue_protocol_queries;
    REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice, &request) == S_OK);
    request.hDrvCommandQueue.pDrvPrivate = &slots[1];
    request.hRTCommandQueue.handle = reinterpret_cast<HANDLE>(uintptr_t(0x78220));
    REQUIRE(table.pfnCreateCommandQueue(create.hDrvDevice, &request) == S_OK);
    REQUIRE(queue_protocol_queries == before_queries + 2 && heap_context_creates == initial_contexts + 1 &&
        runtime_context_creates == initial_queues + 2 && slots[0].runtime_route != slots[1].runtime_route &&
        slots[0].runtime_route->native_heap_context != slots[1].runtime_route->native_heap_context);
    table.pfnDestroyCommandQueue(create.hDrvDevice, {&slots[1]});
    table.pfnDestroyCommandQueue(create.hDrvDevice, {&slots[0]});
    adapter.pfnDestroyDevice(create.hDrvDevice);
    REQUIRE(runtime_contexts == initial_runtime_contexts);

    // Query reentry expires the request/output and device slots before return.
    // The native operation must retain only its independent Context; no late
    // owner or child context callback may use the expired runtime device.
    auto *page = static_cast<uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(page != nullptr);
    auto *cancel_request = reinterpret_cast<D3D12DDIARG_CREATECOMMANDQUEUE_0001 *>(page);
    auto *cancel_slot = reinterpret_cast<Object *>(page + 1024);
    auto *device_slot = page + 2048;
    create.hDrvDevice.pDrvPrivate = device_slot;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    ctx = context(create.hDrvDevice);
    std::weak_ptr<NativeAdapter> weak = ctx->native_adapter;
    request.hDrvCommandQueue.pDrvPrivate = cancel_slot;
    *cancel_request = request;
    const auto before_cancel_owner = heap_context_creates, before_cancel_child = runtime_context_creates;
    const auto before_cancel_backend = queue_creates, before_cancel_destroys = destroys;
    queue_protocol_query_callback = [&](uint8_t *, const D3DDDICB_QUERYADAPTERINFO *) {
        queue_protocol_query_callback = {};
        adapter.pfnDestroyDevice(create.hDrvDevice);
        if (adapter.pfnCloseAdapter(open.hAdapter) != S_OK || !VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
    };
    const auto cancel_hr = table.pfnCreateCommandQueue(create.hDrvDevice, cancel_request);
    REQUIRE(cancel_hr == DXGI_ERROR_DEVICE_REMOVED && !queue_protocol_query_callback && weak.expired());
    REQUIRE(heap_context_creates == before_cancel_owner && runtime_context_creates == before_cancel_child &&
        queue_creates == before_cancel_backend && destroys == before_cancel_destroys + 1 &&
        runtime_contexts == initial_runtime_contexts);
    std::printf("PASS VQDC: legacy160 refused before use; %zu malformed fields,3 reply bounds,5 live changes; "
        "publication rollback,owned-only/import closed,two distinct queues,query reentry/VirtualFree/no late context (controlled DDI)\n",
        sizeof(mutations) / sizeof(mutations[0]));
    return 0;
}
