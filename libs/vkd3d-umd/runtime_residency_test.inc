/* SPDX-License-Identifier: LGPL-2.1-or-later */
static const HANDLE expected_paging = reinterpret_cast<HANDLE>(uintptr_t(0x12345678));
static std::vector<D3DKMT_HANDLE> expected_residency;
static UINT expected_residency_flags;
static unsigned make_calls, evict_calls;
static HRESULT make_result = S_OK, evict_result = S_OK;
static UINT64 returned_paging;
static bool partial_residency;
static std::function<void()> nested_residency;
static void check_residency(UINT number, const D3DKMT_HANDLE *handles, UINT flags) {
    if (number != expected_residency.size() || flags != expected_residency_flags || !handles)
        fixture_abort(__LINE__);
    for (UINT i = 0; i < number; ++i)
        if (handles[i] != expected_residency[i] || !kernel_heaps.count(handles[i])) fixture_abort(__LINE__);
}
static HRESULT APIENTRY residency_make(D3D12DDI_HRTDEVICE device, D3D12DDI_HRTPAGINGQUEUE paging,
        D3DDDI_MAKERESIDENT *args) {
    valid_kernel_callback(device.handle);
    if (paging.handle != expected_paging || args->hPagingQueue || args->PriorityList ||
            args->PagingFenceValue || args->NumBytesToTrim) fixture_abort(__LINE__);
    check_residency(args->NumAllocations, args->AllocationList, args->Flags.Value);
    ++make_calls;
    if (nested_residency) nested_residency();
    // The original runtime array may have been poisoned or unmapped by now.
    check_residency(args->NumAllocations, args->AllocationList, args->Flags.Value);
    args->PagingFenceValue = returned_paging;
    if (partial_residency) --args->NumAllocations;
    return make_result;
}
static HRESULT APIENTRY residency_evict(D3D12DDI_HRTDEVICE device, const D3DDDICB_EVICT *args) {
    valid_kernel_callback(device.handle);
    check_residency(args->NumAllocations, args->AllocationList, args->Flags.Value);
    ++evict_calls;
    if (nested_residency) nested_residency();
    check_residency(args->NumAllocations, args->AllocationList, args->Flags.Value);
    return evict_result;
}

static int test_native_residency() {
    D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    kt.pfnCreateContextCb = heap_context_create; kt.pfnDestroyContextCb = heap_context_destroy;
    kt.pfnEscapeCb = heap_escape; kt.pfnAllocateCb = heap_allocate; kt.pfnDeallocateCb = heap_deallocate;
    kt.pfnLockCb = heap_lock; kt.pfnUnlockCb = heap_unlock; kt.pfnRenderCb = heap_render;
    kt.pfnSignalSynchronizationObject2Cb = heap_signal;
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{}; um.pfnSetErrorCb = test_error;
    um.pfnMakeResidentCb = residency_make; um.pfnEvictCb = residency_evict;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> memory{}, foreign_memory{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = memory.data(); create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    auto *ctx = context(create.hDrvDevice);
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queue) == S_OK);
    REQUIRE(table.pfnMakeResident && table.pfnEvict && !native_contract_complete());
    D3D12DDIARG_CREATEHEAP_0001 desc{};
    desc.ByteSize = desc.Alignment = 65536; desc.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    desc.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK; desc.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
    desc.CreationNodeMask = desc.VisibleNodeMask = 1;
    NativeHeapSlot a{}, b{};
    D3D12DDI_HHEAP ha{&a}, hb{&b};
    auto allocate = [&](D3D12DDI_HHEAP heap) {
        return table.pfnCreateHeapAndResource(create.hDrvDevice, &desc, heap, {}, nullptr, nullptr, {});
    };
    auto destroy = [&](D3D12DDI_HHEAP heap) { table.pfnDestroyHeapAndResource(create.hDrvDevice, heap, {}); };
    REQUIRE(allocate(ha) == S_OK && allocate(hb) == S_OK);
    auto *first = native_heap_find(ctx, &a), *second = native_heap_find(ctx, &b);
    expected_residency = {first->allocation, second->allocation};
    D3D12DDI_HANDLE_AND_TYPE objects[] = {{&a, D3D12DDI_HT_HEAP}, {&b, D3D12DDI_HT_HEAP}};
    D3D12DDI_HRTPAGINGQUEUE paging{expected_paging};
    UINT64 fence = 0;
    D3D12DDIARG_MAKERESIDENT_0001 request{};
    request.NumAdapters = 1; request.pRTPagingQueue = &paging;
    request.NumObjects = 2; request.pObjects = objects; request.pPagingFenceValue = &fence;
    D3D12DDIARG_EVICT evict{}; evict.NumObjects = 2; evict.pObjects = objects;
    const auto original = request;
    for (UINT flags : {0u, 1u, 3u}) {
        request.Flags.Value = expected_residency_flags = flags;
        make_result = S_OK; returned_paging = 0xffffeeeeaaaabbbbull;
        fence = UINT64_MAX; request.WaitMask = 0xdeadbeefu;
        const auto calls = make_calls;
        REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == S_OK);
        REQUIRE(make_calls == calls + 1 && !fence && !request.WaitMask);
        for (UINT64 value : {UINT64(0), UINT64(7), UINT64(0xfedcba9876543210ull)}) {
            make_result = E_PENDING; returned_paging = value;
            fence = UINT64_MAX; request.WaitMask = 0xdeadbeefu;
            const HRESULT hr = table.pfnMakeResident(create.hDrvDevice, &request);
            if (hr != E_PENDING || fence != value || request.WaitMask != 1) {
                std::fputs("FAIL native residency lost accepted pending paging fence\n", stderr); return 1;
            }
        }
    }
    request = original; expected_residency_flags = 0;
    for (HRESULT result : {E_OUTOFMEMORY, E_ACCESSDENIED, DXGI_ERROR_DEVICE_REMOVED}) {
        make_result = result; fence = UINT64_MAX; request.WaitMask = 0xdeadbeefu;
        REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == result);
        REQUIRE(fence == UINT64_MAX && request.WaitMask == 0xdeadbeefu);
    }
    make_result = S_OK;
    for (UINT flags : {0u, 1u}) {
        evict.Flags.Value = expected_residency_flags = flags;
        const auto calls = evict_calls;
        REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == S_OK);
        if (evict_calls != calls + 1) {
            std::fputs("FAIL native residency omitted runtime eviction\n", stderr); return 1;
        }
    }
    evict.Flags.Value = expected_residency_flags = 0;
    evict_result = E_OUTOFMEMORY;
    REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == E_OUTOFMEMORY);
    evict_result = S_OK;
    const auto old_make = make_calls, old_evict = evict_calls;
    for (UINT flags : {2u, 4u, UINT32_MAX}) {
        request.Flags.Value = flags; evict.Flags.Value = flags;
        REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == E_INVALIDARG);
        REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == E_INVALIDARG);
    }
    request = original; evict.Flags.Value = 0;
    for (void *invalid : {static_cast<void *>(nullptr), reinterpret_cast<void *>(uintptr_t(1)), static_cast<void *>(&a)}) {
        objects[1].Handle = invalid;
        REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == E_INVALIDARG);
        REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == E_INVALIDARG);
    }
    objects[1] = {&b, D3D12DDI_HT_PIPELINE_STATE};
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == DXGI_ERROR_UNSUPPORTED);
    REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == DXGI_ERROR_UNSUPPORTED);
    objects[1] = {&b, D3D12DDI_HT_HEAP};
    auto foreign = create; foreign.hDrvDevice.pDrvPrivate = foreign_memory.data();
    foreign.hRTDevice.handle = reinterpret_cast<HANDLE>(uintptr_t(0x123abc));
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &foreign) == S_OK);
    REQUIRE(table.pfnMakeResident(foreign.hDrvDevice, &request) == E_INVALIDARG);
    REQUIRE(table.pfnEvict(foreign.hDrvDevice, &evict) == E_INVALIDARG);
    adapter.pfnDestroyDevice(foreign.hDrvDevice);
    REQUIRE(make_calls == old_make && evict_calls == old_evict);
    REQUIRE(first->users == 1 && second->users == 1 && !first->residency_operations && !second->residency_operations);

    // No callback is needed for an empty batch; real objects cannot silently skip it.
    ctx->runtime_callbacks.pfnMakeResidentCb = nullptr; ctx->runtime_callbacks.pfnEvictCb = nullptr;
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == DXGI_ERROR_UNSUPPORTED);
    REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == DXGI_ERROR_UNSUPPORTED);
    request.NumObjects = evict.NumObjects = 0;
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == S_OK && !request.WaitMask && !fence);
    REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == S_OK);
    request = original; evict.NumObjects = 2;
    ctx->runtime_callbacks.pfnMakeResidentCb = residency_make; ctx->runtime_callbacks.pfnEvictCb = residency_evict;

    // Mapping may rename allocation handles; submission must not borrow one
    // while the residency callback owns it. Recursive Make/Evict also reject.
    nested_residency = [&] {
        void *mapped = reinterpret_cast<void *>(uintptr_t(1));
        uint64_t stream[2]{}; mwd_reference ref{first, 0, 8, 1, 0};
        if (table.pfnMapHeap(create.hDrvDevice, ha, &mapped) != DXGI_ERROR_WAS_STILL_DRAWING || mapped ||
                native_runtime_unmap(ctx, first) != DXGI_ERROR_WAS_STILL_DRAWING ||
                native_runtime_submit(ctx, 1, stream, sizeof(stream), &ref, 1) != DXGI_ERROR_WAS_STILL_DRAWING ||
                table.pfnMakeResident(create.hDrvDevice, &request) != DXGI_ERROR_WAS_STILL_DRAWING ||
                table.pfnEvict(create.hDrvDevice, &evict) != DXGI_ERROR_WAS_STILL_DRAWING) fixture_abort(__LINE__);
        objects[0].Handle = objects[1].Handle = reinterpret_cast<void *>(uintptr_t(1));
        request.NumObjects = UINT32_MAX; request.pRTPagingQueue = nullptr; request.pPagingFenceValue = nullptr;
    };
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == S_OK);
    nested_residency = {}; request = original;
    objects[0].Handle = &a; objects[1].Handle = &b;
    void *mapped = nullptr;
    REQUIRE(table.pfnMapHeap(create.hDrvDevice, ha, &mapped) == S_OK);
    REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == DXGI_ERROR_WAS_STILL_DRAWING);
    table.pfnUnmapHeap(create.hDrvDevice, ha);
    ++first->submitted;
    REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == DXGI_ERROR_WAS_STILL_DRAWING);
    --first->submitted;

    // Actual descriptor/query backing comes from a retained backend owner,
    // never descriptor CPU/GPU visibility or an allocation-list snapshot.
    Object descriptor_memory{}, host_memory{}, query_memory{};
    auto *descriptor_peer = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_DESCRIPTOR_HEAP, 0, 0});
    auto *host_peer = reinterpret_cast<vkdu_object *>(new ImportPeer{{}, ctx->backend, VKDU_DESCRIPTOR_HEAP, 0, 0});
    REQUIRE(VioGpuD3D12BridgeBindObject(create.hDrvDevice, &descriptor_memory, descriptor_peer, VKDU_DESCRIPTOR_HEAP) == S_OK);
    REQUIRE(VioGpuD3D12BridgeBindObject(create.hDrvDevice, &host_memory, host_peer, VKDU_DESCRIPTOR_HEAP) == S_OK);
    descriptor_memory.descriptor_flags = host_memory.descriptor_flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    D3D12DDIARG_CREATE_QUERY_HEAP_0001 query_desc{D3D12DDI_QUERY_HEAP_TYPE_TIMESTAMP, 16, 1};
    D3D12DDI_HQUERYHEAP query_handle{&query_memory};
    REQUIRE(table.pfnCalcPrivateQueryHeapSize(create.hDrvDevice, &query_desc) == sizeof(Object));
    REQUIRE(table.pfnCreateQueryHeap(create.hDrvDevice, &query_desc, query_handle) == S_OK);
    REQUIRE(table.pfnCreateQueryHeap(create.hDrvDevice, &query_desc, query_handle) == E_INVALIDARG);
    auto *query_peer = query_memory.backend;
    pageable_tokens[descriptor_peer] = {first, second, first};
    pageable_tokens[query_peer] = {second}; pageable_tokens[host_peer] = {};
    D3D12DDI_HANDLE_AND_TYPE pageables[] = {{&descriptor_memory, D3D12DDI_HT_DESCRIPTOR_HEAP},
        {&query_memory, D3D12DDI_HT_QUERY_HEAP}, {&host_memory, D3D12DDI_HT_DESCRIPTOR_HEAP}};
    request.pObjects = pageables; request.NumObjects = 3;
    evict.pObjects = pageables; evict.NumObjects = 3;
    make_result = E_PENDING; returned_paging = 0xfedcba9876543210ull;
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == E_PENDING &&
        fence == returned_paging && request.WaitMask == 1);
    REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == S_OK);
    REQUIRE(first->users == 1 && second->users == 1 && !first->residency_operations && !second->residency_operations);
    REQUIRE(reinterpret_cast<ImportPeer *>(descriptor_peer)->references == 1 &&
        reinterpret_cast<ImportPeer *>(query_peer)->references == 1);
    // A persistent ICD mapping is handed to the runtime's actual eviction
    // policy. Its rejection must be returned unchanged, with no fake success.
    REQUIRE(table.pfnMapHeap(create.hDrvDevice, ha, &mapped) == S_OK);
    evict_result = DXGI_ERROR_WAS_STILL_DRAWING;
    const auto before_pinned_evict = evict_calls;
    REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == DXGI_ERROR_WAS_STILL_DRAWING &&
        evict_calls == before_pinned_evict + 1);
    evict_result = S_OK; table.pfnUnmapHeap(create.hDrvDevice, ha);
    make_result = S_OK;
    const auto before_backing_make = make_calls, before_backing_evict = evict_calls;
    pageable_result = DXGI_ERROR_UNSUPPORTED;
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == DXGI_ERROR_UNSUPPORTED);
    REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == DXGI_ERROR_UNSUPPORTED);
    pageable_result = S_OK; corrupt_pageable_reply = true;
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == E_INVALIDARG);
    corrupt_pageable_reply = false;
    REQUIRE(first->users == 1 && second->users == 1);
    REQUIRE(make_calls == before_backing_make && evict_calls == before_backing_evict);
    // An explicit zero-backing descriptor requires no residency callback.
    request.NumObjects = evict.NumObjects = 1; request.pObjects = evict.pObjects = &pageables[2];
    ctx->runtime_callbacks.pfnMakeResidentCb = nullptr; ctx->runtime_callbacks.pfnEvictCb = nullptr;
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == S_OK && !fence && !request.WaitMask);
    REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == S_OK);
    ctx->runtime_callbacks.pfnMakeResidentCb = residency_make; ctx->runtime_callbacks.pfnEvictCb = residency_evict;
    // Reentrant destruction poisons the runtime slot and caller's input array,
    // while the copied request and independently retained backend stay valid.
    request.NumObjects = 1; request.pObjects = pageables;
    pageable_callback = [&] {
        table.pfnDestroyDescriptorHeap(create.hDrvDevice, {&descriptor_memory});
        std::memset(&descriptor_memory, 0xa5, sizeof(descriptor_memory));
        pageables[0].Handle = reinterpret_cast<void *>(uintptr_t(1));
        request.NumObjects = UINT32_MAX; request.pObjects = nullptr;
    };
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == S_OK);
    pageable_callback = {};
    REQUIRE(first->users == 1 && second->users == 1);
    pageables[0] = {&descriptor_memory, D3D12DDI_HT_DESCRIPTOR_HEAP};
    request.NumObjects = 1; request.pObjects = pageables;
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == E_INVALIDARG);
    table.pfnDestroyQueryHeap(create.hDrvDevice, query_handle);
    std::memset(&query_memory, 0xa5, sizeof(query_memory));
    request.pObjects = &pageables[1];
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == E_INVALIDARG);
    table.pfnDestroyDescriptorHeap(create.hDrvDevice, {&host_memory});
    pageable_tokens.clear();
    REQUIRE(!ctx->query_heaps && !ctx->descriptor_heaps);
    Object rejected_query{}; D3D12DDI_HQUERYHEAP rejected_handle{&rejected_query};
    query_desc.Count = 0;
    REQUIRE(!table.pfnCalcPrivateQueryHeapSize(create.hDrvDevice, &query_desc));
    REQUIRE(table.pfnCreateQueryHeap(create.hDrvDevice, &query_desc, rejected_handle) == E_INVALIDARG);
    query_desc.Count = 1; query_desc.Type = static_cast<D3D12DDI_QUERY_HEAP_TYPE>(4);
    REQUIRE(table.pfnCreateQueryHeap(create.hDrvDevice, &query_desc, rejected_handle) == DXGI_ERROR_UNSUPPORTED);
    query_desc.Type = D3D12DDI_QUERY_HEAP_TYPE_TIMESTAMP; query_heap_result = E_OUTOFMEMORY;
    REQUIRE(table.pfnCreateQueryHeap(create.hDrvDevice, &query_desc, rejected_handle) == E_OUTOFMEMORY);
    query_heap_result = S_OK;
    REQUIRE(!rejected_query.magic);
    request = original; evict.NumObjects = 2; evict.pObjects = objects;
    std::puts("PASS native descriptor/query backing residency, deduplication, host-only proof, runtime eviction policy and retained object retirement");

    partial_residency = true;
    fence = UINT64_MAX; request.WaitMask = 0xdeadbeefu;
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == DXGI_ERROR_DRIVER_INTERNAL_ERROR);
    REQUIRE(fence == UINT64_MAX && request.WaitMask == 0xdeadbeefu);
    partial_residency = false; ctx->last_error = S_OK;
    make_result = S_FALSE;
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == DXGI_ERROR_DRIVER_INTERNAL_ERROR);
    make_result = S_OK; ctx->last_error = S_OK;
    evict_result = S_FALSE;
    REQUIRE(table.pfnEvict(create.hDrvDevice, &evict) == DXGI_ERROR_DRIVER_INTERNAL_ERROR);
    evict_result = S_OK; ctx->last_error = S_OK;

    // DestroyHeap may retire runtime slots inside the callback while allocation
    // handles remain pinned. A failed cleanup retains the orphan for teardown.
    nested_residency = [&] { destroy(ha); a.magic = 0xa5a5a5a5u; };
    fail_deallocate = 1;
    REQUIRE(table.pfnMakeResident(create.hDrvDevice, &request) == DXGI_ERROR_WAS_STILL_DRAWING);
    nested_residency = {};
    REQUIRE(!native_heap_find(ctx, &a) && native_token(ctx, first) == first && first->allocation);
    REQUIRE(a.magic == 0xa5a5a5a5u && fence == UINT64_MAX && request.WaitMask == 0xdeadbeefu);
    adapter.pfnDestroyDevice(create.hDrvDevice);
    REQUIRE(kernel_heaps.empty() && kernel_resources.empty());

    // Retirement from both the operation and its cleanup callback must never
    // write outputs after the runtime unmaps the original request storage.
    for (unsigned mode = 0; mode < 3; ++mode) {
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
        ctx = context(create.hDrvDevice);
        REQUIRE(allocate(ha) == S_OK && allocate(hb) == S_OK);
        first = native_heap_find(ctx, &a); second = native_heap_find(ctx, &b);
        expected_residency = {first->allocation, second->allocation};
        auto *page = static_cast<unsigned char *>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(page);
        auto *args = new (page) D3D12DDIARG_MAKERESIDENT_0001(original);
        auto *owned_objects = reinterpret_cast<D3D12DDI_HANDLE_AND_TYPE *>(page + 512);
        owned_objects[0] = {&a, D3D12DDI_HT_HEAP}; owned_objects[1] = {&b, D3D12DDI_HT_HEAP};
        args->pObjects = owned_objects; args->pPagingFenceValue = reinterpret_cast<UINT64 *>(page + 1024);
        D3D12DDIARG_EVICT eviction{}; eviction.NumObjects = 2; eviction.pObjects = owned_objects;
        ++ctx->references;
        auto retire = [&] {
            adapter.pfnDestroyDevice(create.hDrvDevice);
            memory.fill(0xa5); a.magic = b.magic = 0xa5a5a5a5u;
            heap_callbacks_retired = true;
            if (!VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
        };
        if (mode == 2) {
            nested_residency = [&] { destroy(ha); destroy(hb); };
            nested_deallocate = retire;
        } else nested_residency = retire;
        const HRESULT result = mode == 1 ? table.pfnEvict(create.hDrvDevice, &eviction)
            : table.pfnMakeResident(create.hDrvDevice, args);
        nested_residency = {}; nested_deallocate = {};
        REQUIRE(result == DXGI_ERROR_DEVICE_REMOVED && ctx->native_retiring);
        REQUIRE(a.magic == 0xa5a5a5a5u && b.magic == 0xa5a5a5a5u);
        release(ctx);
        kernel_heaps.clear(); kernel_resources.clear(); heap_callbacks_retired = false;
    }
    // Backing enumeration can retire the query, request storage and entire
    // device before returning. Both paging DDIs must unwind retained tokens
    // without touching that storage or calling a retired runtime callback.
    for (unsigned mode = 0; mode < 2; ++mode) {
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
        ctx = context(create.hDrvDevice);
        REQUIRE(allocate(ha) == S_OK && allocate(hb) == S_OK);
        first = native_heap_find(ctx, &a); second = native_heap_find(ctx, &b);
        auto *page = static_cast<unsigned char *>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(page);
        auto *slot = new (page) Object{};
        D3D12DDI_HQUERYHEAP heap{slot};
        REQUIRE(table.pfnCreateQueryHeap(create.hDrvDevice, &query_desc, heap) == S_OK);
        pageable_tokens[slot->backend] = {first, second};
        auto *entry = new (page + 512) D3D12DDI_HANDLE_AND_TYPE{slot, D3D12DDI_HT_QUERY_HEAP};
        auto *args = new (page + 1024) D3D12DDIARG_MAKERESIDENT_0001(original);
        args->NumObjects = 1; args->pObjects = entry; args->pPagingFenceValue = reinterpret_cast<UINT64 *>(page + 2048);
        D3D12DDIARG_EVICT eviction{}; eviction.NumObjects = 1; eviction.pObjects = entry;
        const auto before_make = make_calls, before_evict = evict_calls;
        ++ctx->references;
        pageable_callback = [&] {
            table.pfnDestroyQueryHeap(create.hDrvDevice, heap);
            adapter.pfnDestroyDevice(create.hDrvDevice);
            memory.fill(0xa5);
            heap_callbacks_retired = true;
            if (!VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
        };
        const HRESULT result = mode ? table.pfnEvict(create.hDrvDevice, &eviction)
            : table.pfnMakeResident(create.hDrvDevice, args);
        pageable_callback = {}; pageable_tokens.clear();
        REQUIRE(result == DXGI_ERROR_DEVICE_REMOVED && ctx->native_retiring && !query_peers && !ctx->query_heaps);
        REQUIRE(make_calls == before_make && evict_calls == before_evict);
        REQUIRE(first->users == 1 && second->users == 1 && !first->residency_operations && !second->residency_operations);
        release(ctx);
        kernel_heaps.clear(); kernel_resources.clear(); heap_callbacks_retired = false;
    }
    // Constructor callbacks may reset the adapter, or retire both the device
    // and query slot before the backend returns. Never publish into that slot.
    for (unsigned mode = 0; mode < 2; ++mode) {
        REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
        ctx = context(create.hDrvDevice); ++ctx->references;
        auto *page = static_cast<unsigned char *>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(page);
        auto *slot = new (page) Object{};
        query_create_callback = [&] {
            if (!mode) { ++generation; return; }
            adapter.pfnDestroyDevice(create.hDrvDevice);
            memory.fill(0xa5); heap_callbacks_retired = true;
            if (!VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
        };
        REQUIRE(table.pfnCreateQueryHeap(create.hDrvDevice, &query_desc, {slot}) == DXGI_ERROR_DEVICE_REMOVED);
        query_create_callback = {};
        REQUIRE(!query_peers && !ctx->query_heaps);
        if (!mode) {
            REQUIRE(!slot->magic);
            REQUIRE(VirtualFree(page, 0, MEM_RELEASE));
            --generation;
            adapter.pfnDestroyDevice(create.hDrvDevice);
        }
        release(ctx); heap_callbacks_retired = false;
    }
    std::puts("PASS native query constructor reset and pageable enumeration device retirement without stale runtime access");
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK && !native_contract_complete());
    std::puts("PASS native heap residency identity, pending paging fences, atomic validation, callback retirement and cleanup retention");
    return 0;
}
