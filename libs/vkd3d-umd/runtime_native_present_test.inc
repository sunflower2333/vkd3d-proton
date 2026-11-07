/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Production Present DDI, controlled recording peer. Exact production Vulkan
 * barriers and allocator ownership are exercised by native_present_test.py. */
static HRESULT native_present_result = S_OK;
static std::function<void()> native_present_callback;
static unsigned native_present_calls;
static bool force_present_alias_unpinned;
static int32_t test_native_present(vkdu_object *command, vkdu_object *queue,
        vkdu_object *source, vkdu_object *destination) {
    if (!test_object_is(command, VKDU_COMMAND_LIST) || !test_object_is(queue, VKDU_QUEUE) ||
            !test_object_is(source, VKDU_TEXTURE2D)) fixture_abort(__LINE__);
    auto *list = reinterpret_cast<ImportPeer *>(command);
    auto *src = reinterpret_cast<ImportPeer *>(source);
    auto *route = reinterpret_cast<ImportPeer *>(queue);
    if (list->references < 2 || src->references < 2 || route->references < 2 || !src->owner ||
            (destination && !test_object_is(destination, VKDU_TEXTURE2D))) fixture_abort(__LINE__);
    auto *ctx = src->owner->context;
    wait_backend_worker(&native_runtime_callbacks, ctx);
    ++native_present_calls;
    if (native_present_result == S_OK) {
        list->present_owners.push_back(src->owner);
        if (destination) list->present_owners.push_back(reinterpret_cast<ImportPeer *>(destination)->owner);
    }
    if (native_present_callback) { auto callback = std::move(native_present_callback); native_present_callback = {}; callback(); }
    return native_present_result;
}

static int test_native_present_ddi(Context *ctx, D3D12DDI_HDEVICE device,
        D3D12DDI_HRESOURCE source, NativeHeap *source_heap,
        D3D12DDI_HRESOURCE alias, NativeHeap *alias_heap) {
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queues) == S_OK && commands.pfnPresent &&
        table.pfnGetPresentPrivateDriverDataSize && !native_contract_complete());
    GuardedProxyQueue guarded_queue{};
    auto &queue_memory = guarded_queue.object;
    Object allocator_memory{}, command_memory{};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue{};
    queue.hDrvCommandQueue.pDrvPrivate = &queue_memory; queue.NodeMask = 1;
    queue.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    queue.hRTCommandQueue.handle = reinterpret_cast<HANDLE>(uintptr_t(0x99110));
    REQUIRE(table.pfnCreateCommandQueue(device, &queue) == S_OK);
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator{};
    allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_memory;
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    REQUIRE(table.pfnCreateCommandAllocator(device, &allocator) == S_OK);
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 list{};
    list.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
    list.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; list.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    list.hDrvCommandList.pDrvPrivate = &command_memory;
    list.hRTCommandList.handle = reinterpret_cast<HANDLE>(uintptr_t(0x99220));
    REQUIRE(table.pfnCreateCommandList(device, &list) == S_OK);
    auto *route = queue_memory.runtime_route;
    const auto context_handle = route->native_heap_context;
    REQUIRE(guarded_queue.intact() && route->proxy && context_handle != expected_context &&
        runtime_contexts.at(context_handle) == queue.hRTCommandQueue.handle &&
        proxy_contexts.at(context_handle).runtime == queue.hRTCommandQueue.handle &&
        proxy_fence_contexts.at(route->hop_in) == context_handle &&
        proxy_fence_contexts.at(route->hop_out) == context_handle);
    const auto hop_in = route->hop_in, hop_out = route->hop_out;
    const auto sync_destroys_before = proxy_sync_destroys;
    D3D12DDI_ARG_PRESENTSURFACE surface{source, 0};
    D3D12DDIARG_PRESENT_0001 args{};
    args.phSurfacesToPresent = &surface; args.SurfacesToPresent = 1; args.Flags.Flip = 1;
    D3D12DDI_PRESENT_0003 out{};
    const auto original = args;
    const auto zero = D3D12DDI_PRESENT_0003{};
    const auto invokes = native_present_calls;
    identity_query_callback = [&]() {
        std::memset(&args, 0xa5, sizeof(args)); std::memset(&surface, 0xa5, sizeof(surface));
        D3D12DDIARG_PRESENT_0001 nested = original;
        D3D12DDI_ARG_PRESENTSURFACE nested_surface{source, 0}; nested.phSurfacesToPresent = &nested_surface;
        D3D12DDI_PRESENT_0003 nested_out{};
        commands.pfnPresent(list.hDrvCommandList, queue.hDrvCommandQueue, &nested, &nested_out);
        if (std::memcmp(&nested_out, &zero, sizeof(zero)) || command_errors.back().second != DXGI_ERROR_WAS_STILL_DRAWING)
            fixture_abort(__LINE__);
        const auto executes = queue_executes;
        queues.pfnExecuteCommandLists(queue.hDrvCommandQueue, 1, &list.hDrvCommandList);
        if (queue_executes != executes || ctx->last_error != DXGI_ERROR_WAS_STILL_DRAWING) fixture_abort(__LINE__);
    };
    commands.pfnPresent(list.hDrvCommandList, queue.hDrvCommandQueue, &args, &out);
    REQUIRE(native_present_calls == invokes + 1 && out.AddedGpuWork && out.hSrcAllocation == source_heap->allocation &&
        !out.hDstAllocation && out.hContext == ctx->native_heap_context && out.hContext == expected_context &&
        !out.BroadcastContextCount && out.BackBufferMultiplicity == 1 && guarded_queue.intact());
    for (auto handle : out.BroadcastContext) REQUIRE(!handle);
    REQUIRE(!route->preparing_present && !source_heap->native_mapping->residency_operations);
    args = original; surface = {source, 0};
    REQUIRE(table.pfnGetPresentPrivateDriverDataSize(device, &args) == 0);
    const std::vector<std::function<void()>> invalid = {
        [&] { args.SurfacesToPresent = 2; }, [&] { args.phSurfacesToPresent = nullptr; },
        [&] { surface.SubResourceIndex = 1; }, [&] { surface.hSurface.pDrvPrivate = reinterpret_cast<void *>(uintptr_t(1)); },
        [&] { args.hDstResource = source; }, [&] { args.DstSubResourceIndex = 1; },
        [&] { args.Flags.PreferRight = 1; }, [&] { args.Flags.TemporaryMono = 1; },
        [&] { args.Flags.Blt = 1; }, [&] { args.DirtyRects = 1; }, [&] { args.PrivateDriverDataSize = 1; },
        [&] { args.VidPnSourceID = 1; }, [&] { args.FlipInterval = static_cast<DXGI_DDI_FLIP_INTERVAL_TYPE>(5); }
    };
    for (const auto &mutate : invalid) {
        args = original; surface = {source, 0}; mutate();
        std::memset(&out, 0xa5, sizeof(out));
        const auto calls = native_present_calls;
        const auto errors = command_errors.size();
        commands.pfnPresent(list.hDrvCommandList, queue.hDrvCommandQueue, &args, &out);
        REQUIRE(native_present_calls == calls && command_errors.size() == errors + 1 && !std::memcmp(&out, &zero, sizeof(out)));
    }
    args = original; surface = {source, 0};
    args.Flags.Flip = 0; args.Flags.Blt = 1; args.hDstResource = alias;
    const auto calls_before_alias = native_present_calls;
    commands.pfnPresent(list.hDrvCommandList, queue.hDrvCommandQueue, &args, &out);
    REQUIRE(native_present_calls == calls_before_alias && !out.AddedGpuWork && command_errors.back().second == E_INVALIDARG);
    args = original;
    for (const HRESULT failure : {E_OUTOFMEMORY, S_FALSE}) {
        native_present_result = failure;
        commands.pfnPresent(list.hDrvCommandList, queue.hDrvCommandQueue, &args, &out);
        REQUIRE(!std::memcmp(&out, &zero, sizeof(out)) && command_errors.back().second == (failure == S_FALSE ? E_FAIL : failure));
        REQUIRE(!route->preparing_present && !source_heap->native_mapping->residency_operations);
    }
    native_present_result = S_OK;
    const auto alias_runtime = alias_heap->runtime_resource;
    const auto prior_closes = surface_closes;
    native_present_callback = [&]() {
        const D3D12DDI_HHEAP alias_handle{alias_heap->slot};
        table.pfnDestroyHeapAndResource(device, alias_handle, alias);
        if (!kernel_resources.count(alias_runtime) || surface_closes != prior_closes) fixture_abort(__LINE__);
        std::memset(alias.pDrvPrivate, 0xa5, sizeof(Object));
    };
    native_residency_alias_release_active = true;
    const bool previous_alias_control = native_residency_skip_alias_holds;
    if (force_present_alias_unpinned) native_residency_skip_alias_holds = true;
    commands.pfnPresent(list.hDrvCommandList, queue.hDrvCommandQueue, &args, &out);
    native_residency_skip_alias_holds = previous_alias_control;
    native_residency_alias_release_active = false;
    REQUIRE(out.AddedGpuWork && surface_closes == prior_closes + 1 && !kernel_resources.count(alias_runtime) &&
        source_heap->native_mapping->residency_operations == 0);
    commands.pfnCloseCommandList(list.hDrvCommandList);
    const auto executes = queue_executes;
    queues.pfnExecuteCommandLists(queue.hDrvCommandQueue, 1, &list.hDrvCommandList);
    REQUIRE(queue_executes == executes + 1 && submitted_commands.size() == 1 && submitted_commands[0] == command_memory.backend);
    table.pfnDestroyCommandList(device, list.hDrvCommandList);
    // A new recording list can lose its queue from inside the backend. The
    // output and request pages expire there; failure must not publish afterward.
    REQUIRE(table.pfnCreateCommandList(device, &list) == S_OK);
    struct PresentPage { D3D12DDIARG_PRESENT_0001 args; D3D12DDI_ARG_PRESENTSURFACE surface; D3D12DDI_PRESENT_0003 out; };
    auto *page = static_cast<PresentPage *>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(page);
    *page = {}; page->args = original; page->surface = {source, 0}; page->args.phSurfacesToPresent = &page->surface;
    native_present_callback = [&]() {
        if (std::memcmp(&page->out, &zero, sizeof(zero))) fixture_abort(__LINE__);
        table.pfnDestroyCommandQueue(device, queue.hDrvCommandQueue);
        std::memset(&queue_memory, 0xa5, sizeof(queue_memory));
        if (!VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
    };
    commands.pfnPresent(list.hDrvCommandList, queue.hDrvCommandQueue, &page->args, &page->out);
    REQUIRE(command_errors.back().second == DXGI_ERROR_DEVICE_REMOVED && !ctx->native_queue_objects && !ctx->native_runtime_queues);
    table.pfnDestroyCommandList(device, list.hDrvCommandList);
    table.pfnDestroyCommandAllocator(device, allocator.hDrvCommandAllocator);
    REQUIRE(!ctx->native_command_objects && !ctx->native_queue_objects && !ctx->native_runtime_queues &&
        guarded_queue.intact() && !proxy_contexts.count(context_handle) && !runtime_contexts.count(context_handle) &&
        !proxy_fence_contexts.count(hop_in) && !proxy_fence_contexts.count(hop_out) &&
        proxy_sync_destroys == sync_destroys_before + 2);
    std::printf("PASS native Present DDI: proxy queue identity, owner context output, actual backend recording dispatch, %zu rejects, request snapshots, reentry, failure rollback, alias close, Execute routing and queue/fence destruction with expired output and slot canaries (controlled backend)\n", invalid.size());
    return 0;
}
