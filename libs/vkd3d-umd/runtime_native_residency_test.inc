/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual residency DDIs after real OpenHeapAndResource publication, with
 * controlled runtime peers. No host paging or native GPU claim. */
static int test_native_alias_residency(Context *ctx, D3D12DDI_HDEVICE device,
        NativeHeap *first, NativeHeap *alias) {
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queues) == S_OK);
    ctx->runtime_callbacks.pfnMakeResidentCb = residency_make;
    ctx->runtime_callbacks.pfnEvictCb = residency_evict;
    expected_residency = {first->allocation};
    if (first->allocation != alias->allocation) expected_residency.push_back(alias->allocation);
    expected_residency_flags = 0; make_result = evict_result = S_OK; partial_residency = false;
    D3D12DDI_HANDLE_AND_TYPE objects[] = {{first->slot, D3D12DDI_HT_HEAP}, {alias->slot, D3D12DDI_HT_HEAP}};
    D3D12DDI_HRTPAGINGQUEUE paging{expected_paging};
    UINT64 fence = 0xa5a5;
    D3D12DDIARG_MAKERESIDENT_0001 request{};
    request.NumAdapters = 1; request.pRTPagingQueue = &paging;
    request.NumObjects = 2; request.pObjects = objects; request.pPagingFenceValue = &fence;
    D3D12DDIARG_EVICT evict{}; evict.NumObjects = 2; evict.pObjects = objects;
    const auto first_users = first->users, alias_users = alias->users;
    const auto before_make = make_calls, before_evict = evict_calls;
    REQUIRE(table.pfnMakeResident(device, &request) == S_OK && !fence && !request.WaitMask);
    returned_paging = 0x13572468abcdef01ull; make_result = E_PENDING;
    REQUIRE(table.pfnMakeResident(device, &request) == E_PENDING && fence == returned_paging && request.WaitMask == 1);
    REQUIRE(table.pfnEvict(device, &evict) == S_OK && make_calls == before_make + 2 && evict_calls == before_evict + 1);
    for (HRESULT result : {E_OUTOFMEMORY, E_ACCESSDENIED}) {
        make_result = evict_result = result; fence = 0xa5a5; request.WaitMask = 0xa5;
        REQUIRE(table.pfnMakeResident(device, &request) == result && fence == 0xa5a5 && request.WaitMask == 0xa5);
        REQUIRE(table.pfnEvict(device, &evict) == result);
    }
    make_result = evict_result = S_OK;
    REQUIRE(first->users == first_users && alias->users == alias_users &&
        !first->residency_operations && !alias->residency_operations);

    mwd_allocation ordinary{};
    REQUIRE(native_runtime_allocate(ctx, 4096, 4096, 0, 6, &ordinary) == S_OK);
    std::array<unsigned char, 16> stream{};
    mwd_reference refs[] = {{alias, 0, alias->bytes, 3, MWD_NATIVE_SURFACE_REFERENCE},
        {ordinary.token, 0, 4096, 1, 0}};
    // A callback working on just one open excludes ALL aliases from nested
    // MakeResident/Evict/Render, including aliases with distinct KMT handles.
    request.NumObjects = 1; evict.NumObjects = 1; expected_residency = {first->allocation};
    auto alias_make = request; alias_make.pObjects = &objects[1];
    auto alias_evict = evict; alias_evict.pObjects = &objects[1];
    unsigned nested_checks = 0;
    nested_residency = [&]() {
        auto nested = std::move(nested_residency); nested_residency = {};
        if (table.pfnMakeResident(device, &alias_make) != DXGI_ERROR_WAS_STILL_DRAWING ||
                table.pfnEvict(device, &alias_evict) != DXGI_ERROR_WAS_STILL_DRAWING ||
                native_runtime_submit(ctx, ctx->native_last_submitted + 1,
                    stream.data(), stream.size(), refs, 2) != DXGI_ERROR_WAS_STILL_DRAWING)
            fixture_abort(__LINE__);
        ++nested_checks;
    };
    REQUIRE(table.pfnMakeResident(device, &request) == S_OK && nested_checks == 1 && !nested_residency);
    REQUIRE(first->users == first_users && alias->users == alias_users);

    // An outstanding packet using the first association prevents eviction
    // through the other one until the actual notification event is signalled.
    refs[0].token = first;
    const auto serial = ctx->native_last_submitted + 1;
    defer_completion = true;
    REQUIRE(native_runtime_submit(ctx, serial, stream.data(), stream.size(), refs, 2) == S_OK);
    expected_residency = {alias->allocation};
    const auto evictions_before_dma = evict_calls;
    REQUIRE(table.pfnEvict(device, &alias_evict) == DXGI_ERROR_WAS_STILL_DRAWING && evict_calls == evictions_before_dma);
    complete_events(); uint32_t completed = 0;
    REQUIRE(native_runtime_completed(ctx, &completed) == S_OK && completed == serial);
    REQUIRE(table.pfnEvict(device, &alias_evict) == S_OK && evict_calls == evictions_before_dma + 1);
    defer_completion = false;
    REQUIRE(native_runtime_release(ctx, ordinary.token) == S_OK);
    REQUIRE(first->users == first_users && alias->users == alias_users);
    std::puts("PASS opened native aliases: unique allocation residency, pending fence, callback exclusion and DMA eviction guard");
    return 0;
}

static int test_native_alias_residency_release(Context *ctx, D3D12DDI_HDEVICE device,
        NativeHeap *first, NativeHeap *alias, D3D12DDI_HRESOURCE alias_resource) {
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queues) == S_OK);
    const auto alias_runtime = alias->runtime_resource;
    D3D12DDI_HHEAP alias_slot{alias->slot};
    D3D12DDI_HANDLE_AND_TYPE requested{first->slot, D3D12DDI_HT_HEAP};
    D3D12DDI_HRTPAGINGQUEUE paging{expected_paging}; UINT64 fence = 0;
    D3D12DDIARG_MAKERESIDENT_0001 request{};
    request.NumAdapters = 1; request.pRTPagingQueue = &paging; request.pPagingFenceValue = &fence;
    request.NumObjects = 1; request.pObjects = &requested;
    expected_residency = {first->allocation};
    const auto releases = surface_releases, closes = surface_closes;
    nested_residency = [&]() {
        table.pfnDestroyHeapAndResource(device, alias_slot, alias_resource);
        if (surface_closes != closes || surface_releases != releases ||
                !kernel_resources.count(alias_runtime)) fixture_abort(__LINE__);
    };
    native_residency_alias_release_active = true;
    REQUIRE(table.pfnMakeResident(device, &request) == S_OK);
    native_residency_alias_release_active = false;
    nested_residency = {};
    if (kernel_resources.count(alias_runtime) || surface_closes != closes + 1) {
        std::fputs("FAIL unlisted native alias lost final release during residency callback\n", stderr);
        return 1;
    }
    REQUIRE(surface_releases == releases && !native_heap_find(ctx, alias_slot.pDrvPrivate));
    std::puts("PASS native alias destruction during paging: every mapping owner pinned, per-open close after callback");
    return 0;
}

static int test_native_alias_paging_cancel(Context *ctx, D3D12DDI_HDEVICE device,
        bool evicting, const std::function<void()> &terminate) {
    const uint64_t key = 0x4337;
    const D3DKMT_HANDLE allocation = 63000, resource = 73000;
    NativeHeapSlot slots[2]{};
    NativeHeap *heaps[2]{};
    for (unsigned i = 0; i != 2; ++i) {
        const HANDLE runtime = reinterpret_cast<HANDLE>(uintptr_t(0xaa6600 + i));
        if (!i) native_surfaces.emplace(key, NativeSurfaceFixture{allocation, resource, runtime, 65536});
        kernel_heaps.emplace(allocation + i, KernelHeap{0, 65536, false, runtime});
        kernel_resources.insert(runtime);
        surface_associations.push_back({key, allocation + i, resource, runtime});
        REQUIRE(native_surface_import(ctx, allocation + i, resource, key, 65536, runtime, &heaps[i]) == S_OK);
        REQUIRE(native_surface_complete_open(ctx, heaps[i]) == S_OK);
        heaps[i]->slot = &slots[i]; slots[i].magic = native_heap_magic;
    }
    REQUIRE(heaps[0]->native_mapping == heaps[1]->native_mapping);
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queues) == S_OK);
    ctx->runtime_callbacks.pfnMakeResidentCb = residency_make;
    ctx->runtime_callbacks.pfnEvictCb = residency_evict;
    make_result = evict_result = S_OK; expected_residency_flags = 0;
    expected_residency = {allocation};
    auto *page = static_cast<unsigned char *>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(page);
    auto *objects = new (page) D3D12DDI_HANDLE_AND_TYPE{&slots[0], D3D12DDI_HT_HEAP};
    auto *request = new (page + 512) D3D12DDIARG_MAKERESIDENT_0001{};
    auto *paging = new (page + 1024) D3D12DDI_HRTPAGINGQUEUE{expected_paging};
    request->NumAdapters = 1; request->pRTPagingQueue = paging;
    request->NumObjects = 1; request->pObjects = objects;
    request->pPagingFenceValue = reinterpret_cast<UINT64 *>(page + 1536);
    auto *evict = new (page + 2048) D3D12DDIARG_EVICT{};
    evict->NumObjects = 1; evict->pObjects = objects;
    const auto closes = surface_closes, releases = surface_releases;
    nested_residency = [&]() {
        terminate();
        if (!VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
    };
    const HRESULT hr = evicting ? table.pfnEvict(device, evict) : table.pfnMakeResident(device, request);
    nested_residency = {};
    REQUIRE(hr == DXGI_ERROR_DEVICE_REMOVED && ctx->native_retiring);
    // The alias absent from the submitted list is protected by the shared
    // mapping too. Retirement cannot close it through a nested callback.
    REQUIRE(surface_closes == closes && surface_releases == releases);
    for (auto *heap : heaps) REQUIRE(heap->users == 1 && !heap->residency_operations &&
        !heap->native_mapping->residency_operations);
    for (auto *heap : heaps) REQUIRE(native_runtime_release(ctx, heap) == S_OK);
    REQUIRE(!ctx->native_heaps);
    std::puts("PASS native alias paging cancellation: unlisted alias retained, expired request pages, no late callbacks");
    return 0;
}
