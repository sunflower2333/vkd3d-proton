/* SPDX-License-Identifier: LGPL-2.1-or-later */
static HRESULT native_image_result = S_OK;
static std::function<void()> native_image_callback;
static unsigned native_image_calls;
static uint32_t native_image_width, native_image_height, native_image_format, native_image_stride;
static int32_t test_native_image_import(vkdu_device *device, void *owner, void *token,
        uint32_t width, uint32_t height, uint32_t format, uint32_t stride,
        int render_target, int preserve_contents, vkdu_object **out) {
    if (!render_target || !preserve_contents) fixture_abort(__LINE__);
    native_image_width = width; native_image_height = height;
    native_image_format = format; native_image_stride = stride; ++native_image_calls;
    *out = nullptr;
    wait_backend_worker(&native_runtime_callbacks, owner);
    if (SUCCEEDED(native_image_result)) {
        mwd_allocation allocation{};
        const HRESULT hr = native_runtime_retain(owner, token, &allocation);
        if (FAILED(hr)) return hr;
        if (allocation.flags != 20) fixture_abort(__LINE__);
        auto backing = std::make_shared<ImportOwner>();
        backing->context = static_cast<Context *>(owner); backing->allocation = allocation;
        auto *peer = new ImportPeer{backing, device, VKDU_TEXTURE2D, allocation.address, allocation.size};
        peer->width = width; peer->height = height; peer->format = format;
        *out = reinterpret_cast<vkdu_object *>(peer);
    }
    if (native_image_callback) { auto callback = std::move(native_image_callback); native_image_callback = {}; callback(); }
    return native_image_result;
}

static int test_native_open_ddi(Context *ctx, D3D12DDI_HDEVICE device) {
    REQUIRE(kernel_heaps.empty() && surface_associations.empty() && !ctx->native_heaps && !ctx->resources);
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queue) == S_OK);
    REQUIRE(table.pfnCalcPrivateOpenedHeapAndResourceSizes && table.pfnOpenHeapAndResource);
    NativeAllocationInfo metadata{};
    metadata.header = native_header(sizeof(metadata)); metadata.size = 65536; metadata.alignment = 4096;
    metadata.width = 7; metadata.height = 3; metadata.pitch = 64; metadata.format = 3;
    NativeResourceShare share{}; share.header = native_header(sizeof(share)); share.key = 0x2337; share.stride = 64; share.flags = 1;
    D3DDDI_OPENALLOCATIONINFO opened{};
    opened.hAllocation = 61000; opened.pPrivateDriverData = &metadata; opened.PrivateDriverDataSize = sizeof(metadata);
    D3D12DDIARG_OPENHEAP_0003 args{};
    args.NumAllocations = 1; args.pOpenAllocationInfo = &opened; args.hKMResource.handle = 71000;
    args.pPrivateDriverData = &share; args.PrivateDriverDataSize = sizeof(share);
    alignas(NativeHeapSlot) std::array<unsigned char, sizeof(NativeHeapSlot) + 16> heap_memory{};
    alignas(Object) std::array<unsigned char, sizeof(Object) + 16> resource_memory{};
    D3D12DDI_HHEAP heap_slot{}; heap_slot.pDrvPrivate = heap_memory.data();
    D3D12DDI_HRESOURCE resource_slot{}; resource_slot.pDrvPrivate = resource_memory.data();
    D3D12DDI_HRTRESOURCE runtime{}; runtime.handle = reinterpret_cast<HANDLE>(uintptr_t(0x886600));
    const auto original_metadata = metadata; const auto original_share = share;
    const auto original_opened = opened; const auto original_args = args;
    auto reset = [&]() {
        metadata = original_metadata; share = original_share; opened = original_opened; args = original_args;
        heap_memory.fill(0xa5); resource_memory.fill(0xa5);
    };
    auto open = [&]() { return table.pfnOpenHeapAndResource(device, &args, heap_slot, runtime, resource_slot); };
    auto runtime_open = [&]() {
        native_surfaces.emplace(share.key, NativeSurfaceFixture{opened.hAllocation, args.hKMResource.handle, runtime.handle, metadata.size});
        surface_associations.push_back({share.key, opened.hAllocation, args.hKMResource.handle, runtime.handle});
        kernel_heaps.emplace(opened.hAllocation, KernelHeap{0, metadata.size, false, runtime.handle});
        kernel_resources.insert(runtime.handle);
    };
    auto runtime_forget = [&]() {
        for (const auto &surface : native_surfaces) if (surface.second.imported) fixture_abort(__LINE__);
        native_surfaces.clear(); surface_associations.clear(); kernel_heaps.clear(); kernel_resources.clear();
    };
    reset();
    ctx->native_surfaces = false;
    REQUIRE(!table.pfnCalcPrivateOpenedHeapAndResourceSizes(device, &args).Heap && open() == DXGI_ERROR_UNSUPPORTED);
    ctx->native_surfaces = true;
    const auto deallocate2 = ctx->kernel_callbacks.pfnDeallocate2Cb;
    ctx->kernel_callbacks.pfnDeallocate2Cb = nullptr;
    REQUIRE(!table.pfnCalcPrivateOpenedHeapAndResourceSizes(device, &args).Heap && open() == DXGI_ERROR_UNSUPPORTED);
    ctx->kernel_callbacks.pfnDeallocate2Cb = deallocate2;
    auto sizes = table.pfnCalcPrivateOpenedHeapAndResourceSizes(device, &args);
    REQUIRE(sizes.Heap == sizeof(NativeHeapSlot) && sizes.Resource == sizeof(Object));
    const auto prior_resolves = surface_resolves, prior_imports = native_image_calls, prior_closes = surface_closes;
    const std::vector<std::function<void()>> invalid = {
        [&]() { args.NumAllocations = 2; }, [&]() { args.pOpenAllocationInfo = nullptr; },
        [&]() { args.hKMResource.handle = 0; }, [&]() { args.InitialResourceState = D3D12DDI_RESOURCE_STATE_RENDER_TARGET; },
        [&]() { args.pPrivateDriverData = nullptr; }, [&]() { --args.PrivateDriverDataSize; },
        [&]() { opened.hAllocation = 0; }, [&]() { opened.pPrivateDriverData = nullptr; }, [&]() { --opened.PrivateDriverDataSize; },
        [&]() { metadata.header.magic = 0; }, [&]() { metadata.header.version = 1; }, [&]() { metadata.header.reserved = 1; },
        [&]() { metadata.size = 65535; }, [&]() { metadata.alignment = 65536; }, [&]() { metadata.iova = 4096; },
        [&]() { metadata.generation = generation; }, [&]() { metadata.context_id = 83; }, [&]() { metadata.flags = 2; },
        [&]() { metadata.width = 0; }, [&]() { metadata.height = 16385; }, [&]() { metadata.pitch = 32; },
        [&]() { metadata.height = 16384; }, [&]() { metadata.format = 2; }, [&]() { metadata.refresh_numerator = 60; },
        [&]() { share.key = 0; }, [&]() { share.key = uint64_t(UINT32_MAX) + 1; }, [&]() { share.stride = 8; },
        [&]() { share.stride = metadata.pitch = 33; }, [&]() { share.flags = 2; }, [&]() { share.reserved[1] = 1; }
    };
    for (const auto &mutate : invalid) {
        reset(); mutate(); sizes = table.pfnCalcPrivateOpenedHeapAndResourceSizes(device, &args);
        REQUIRE(!sizes.Heap && !sizes.Resource && FAILED(open()));
        for (auto value : heap_memory) REQUIRE(value == 0xa5);
        for (auto value : resource_memory) REQUIRE(value == 0xa5);
    }
    REQUIRE(surface_resolves == prior_resolves && native_image_calls == prior_imports && surface_closes == prior_closes);
    for (unsigned variant = 0; variant != 4; ++variant) {
        reset(); metadata.format = (variant & 1) ? 1 : 3;
        if (variant & 2) { metadata.flags = 1; metadata.refresh_numerator = 165; metadata.refresh_denominator = 1; }
        const auto format = metadata.format == 1 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        runtime_open();
        identity_query_callback = [&]() {
            // Every field and pointee is caller-owned. None survives the first
            // callback; the Open transaction must already own all metadata.
            std::memset(&args, 0xa5, sizeof(args)); std::memset(&opened, 0xa5, sizeof(opened));
            std::memset(&metadata, 0xa5, sizeof(metadata)); std::memset(&share, 0xa5, sizeof(share));
        };
        native_image_callback = [&]() {
            if (!ctx->native_open_reservations || ctx->resources ||
                    native_heap_find(ctx, heap_slot.pDrvPrivate)) fixture_abort(__LINE__);
            // Restore only this request to attempt a reentrant duplicate Open.
            metadata = original_metadata; share = original_share; opened = original_opened; args = original_args;
            if (open() != E_INVALIDARG) fixture_abort(__LINE__);
        };
        REQUIRE(open() == S_OK && !ctx->native_open_reservations && !identity_query_callback && !native_image_callback);
        REQUIRE(native_image_width == 7 && native_image_height == 3 && native_image_stride == 64 && native_image_format == format);
        auto *heap = native_heap_find(ctx, heap_slot.pDrvPrivate);
        auto *resource = object(resource_slot.pDrvPrivate);
        REQUIRE(heap && resource && resource->native_heap == heap && heap->resource_created && heap->runtime_resource == runtime.handle);
        D3D12DDI_RESOURCE_ALLOCATION_INFO allocation_info{}; D3DKMT_HANDLE allocation = 0;
        table.pfnCheckExistingResourceAllocationInfo(device, resource_slot, &allocation_info, &allocation);
        REQUIRE(allocation == 61000 && allocation_info.ResourceDataSize == 65536 && allocation_info.ResourceDataAlignment == 4096 &&
            allocation_info.Layout == D3D12DDI_TL_ROW_MAJOR);
        D3D12DDI_SUBRESOURCE_INFO layout{};
        table.pfnCheckSubresourceInfo(device, resource_slot, 0, &layout);
        REQUIRE(layout.RowStride == 64 && layout.DepthStride == 192);
        REQUIRE(!table.pfnCheckResourceVirtualAddress(device, resource_slot));
        for (size_t i = sizeof(NativeHeapSlot); i < heap_memory.size(); ++i) REQUIRE(heap_memory[i] == 0xa5);
        for (size_t i = sizeof(Object); i < resource_memory.size(); ++i) REQUIRE(resource_memory[i] == 0xa5);
        alignas(NativeHeapSlot) std::array<unsigned char, sizeof(NativeHeapSlot)> alias_heap_memory{};
        alignas(Object) std::array<unsigned char, sizeof(Object)> alias_resource_memory{};
        D3D12DDI_HHEAP alias_heap{}; alias_heap.pDrvPrivate = alias_heap_memory.data();
        D3D12DDI_HRESOURCE alias_resource{}; alias_resource.pDrvPrivate = alias_resource_memory.data();
        D3D12DDI_HRTRESOURCE alias_runtime{}; alias_runtime.handle = reinterpret_cast<HANDLE>(uintptr_t(0x886601));
        if (variant < 2) {
            metadata.format = variant ? 1 : 3;
            opened.hAllocation += variant;
            surface_associations.push_back({share.key, opened.hAllocation, args.hKMResource.handle, alias_runtime.handle});
            kernel_heaps.emplace(opened.hAllocation, KernelHeap{heap->address, metadata.size, false, alias_runtime.handle});
            kernel_resources.insert(alias_runtime.handle);
            const auto imports_before_alias = surface_imports;
            REQUIRE(table.pfnOpenHeapAndResource(device, &args, alias_heap, alias_runtime, alias_resource) == S_OK &&
                surface_imports == imports_before_alias);
            auto *other = object(alias_resource.pDrvPrivate);
            REQUIRE(other && other->native_heap != heap && other->native_heap->native_mapping == heap->native_mapping);
            REQUIRE(test_native_alias_residency(ctx, device, heap, other->native_heap) == 0);
            REQUIRE(test_native_alias_residency_release(ctx, device, heap, other->native_heap, alias_resource) == 0);
            REQUIRE(!kernel_resources.count(alias_runtime.handle) && kernel_resources.count(runtime.handle));
            // Reopen an independent alias for Present's callback-destruction
            // transaction after the paging fixture has closed its own open.
            surface_associations.push_back({share.key, opened.hAllocation, args.hKMResource.handle, alias_runtime.handle});
            kernel_heaps.emplace(opened.hAllocation, KernelHeap{heap->address, metadata.size, false, alias_runtime.handle});
            kernel_resources.insert(alias_runtime.handle);
            REQUIRE(table.pfnOpenHeapAndResource(device, &args, alias_heap, alias_runtime, alias_resource) == S_OK);
            REQUIRE(test_native_present_ddi(ctx, device, resource_slot, heap,
                alias_resource, object(alias_resource.pDrvPrivate)->native_heap) == 0);
        }
        const auto close2 = pageable_deallocation_calls, close1 = legacy_deallocation_calls;
        table.pfnDestroyHeapAndResource(device, heap_slot, resource_slot);
        REQUIRE(!ctx->native_heaps && !ctx->resources && kernel_heaps.empty() && kernel_resources.empty());
        REQUIRE(pageable_deallocation_calls == close2 + 1 && legacy_deallocation_calls == close1);
        runtime_forget();
    }
    for (const HRESULT failure : {E_OUTOFMEMORY, S_FALSE}) {
        reset(); runtime_open(); native_image_result = failure;
        const auto closes_before_failure = surface_closes, close2 = pageable_deallocation_calls;
        REQUIRE(open() == (failure == S_FALSE ? E_FAIL : failure) && !ctx->native_heaps && !ctx->resources &&
            !ctx->native_open_reservations && surface_closes == closes_before_failure &&
            pageable_deallocation_calls == close2 && kernel_resources.count(runtime.handle));
        for (auto value : heap_memory) REQUIRE(value == 0xa5);
        for (auto value : resource_memory) REQUIRE(value == 0xa5);
        native_image_result = S_OK; runtime_forget();
    }
    std::printf("PASS OpenHeapAndResource: four RGBA/BGRA shared/primary metadata cases, %zu rejections, poisoned requests, reserved slots, exact linear layout and backend-failure rollback\n", invalid.size());
    return 0;
}

static int test_native_open_cancel(Context *ctx, D3D12DDI_HDEVICE device, const std::function<void()> &terminate) {
    NativeAllocationInfo metadata{};
    metadata.header = native_header(sizeof(metadata)); metadata.size = 65536; metadata.alignment = 4096;
    metadata.width = 7; metadata.height = 3; metadata.pitch = 64; metadata.format = 3;
    NativeResourceShare share{}; share.header = native_header(sizeof(share)); share.key = 0x3337; share.stride = 64; share.flags = 1;
    D3DDDI_OPENALLOCATIONINFO opened{};
    opened.hAllocation = 62000; opened.pPrivateDriverData = &metadata; opened.PrivateDriverDataSize = sizeof(metadata);
    D3D12DDIARG_OPENHEAP_0003 args{};
    args.NumAllocations = 1; args.pOpenAllocationInfo = &opened; args.hKMResource.handle = 72000;
    args.pPrivateDriverData = &share; args.PrivateDriverDataSize = sizeof(share);
    alignas(NativeHeapSlot) std::array<unsigned char, sizeof(NativeHeapSlot)> heap_memory;
    alignas(Object) std::array<unsigned char, sizeof(Object)> resource_memory;
    heap_memory.fill(0xa5); resource_memory.fill(0xa5);
    D3D12DDI_HHEAP heap_slot{}; heap_slot.pDrvPrivate = heap_memory.data();
    D3D12DDI_HRESOURCE resource_slot{}; resource_slot.pDrvPrivate = resource_memory.data();
    D3D12DDI_HRTRESOURCE runtime{}; runtime.handle = reinterpret_cast<HANDLE>(uintptr_t(0x996600));
    native_surfaces.emplace(share.key, NativeSurfaceFixture{opened.hAllocation, args.hKMResource.handle, runtime.handle, metadata.size});
    surface_associations.push_back({share.key, opened.hAllocation, args.hKMResource.handle, runtime.handle});
    kernel_heaps.emplace(opened.hAllocation, KernelHeap{0, metadata.size, false, runtime.handle});
    kernel_resources.insert(runtime.handle);
    native_image_callback = terminate;
    const auto previous_closes = surface_closes;
    REQUIRE(native_open_heap(device, &args, heap_slot, runtime, resource_slot) == DXGI_ERROR_DEVICE_REMOVED);
    REQUIRE(!ctx->native_open_reservations && !ctx->native_heaps && !ctx->resources && surface_closes == previous_closes);
    for (auto value : heap_memory) REQUIRE(value == 0xa5);
    for (auto value : resource_memory) REQUIRE(value == 0xa5);
    return 0;
}

static int test_native_present_cancel(Context *ctx, D3D12DDI_HDEVICE device, bool in_backend,
        const std::function<void()> &terminate) {
    NativeAllocationInfo metadata{};
    metadata.header = native_header(sizeof(metadata)); metadata.size = 65536; metadata.alignment = 4096;
    metadata.width = 7; metadata.height = 3; metadata.pitch = 64; metadata.format = 3;
    NativeResourceShare share{}; share.header = native_header(sizeof(share)); share.key = 0x4437; share.stride = 64; share.flags = 1;
    D3DDDI_OPENALLOCATIONINFO opened{};
    opened.hAllocation = 64000; opened.pPrivateDriverData = &metadata; opened.PrivateDriverDataSize = sizeof(metadata);
    D3D12DDIARG_OPENHEAP_0003 opening{};
    opening.NumAllocations = 1; opening.pOpenAllocationInfo = &opened; opening.hKMResource.handle = 74000;
    opening.pPrivateDriverData = &share; opening.PrivateDriverDataSize = sizeof(share);
    NativeHeapSlot heap_memory{};
    Object resource_memory{}, queue_memory{}, command_memory{}, allocator_memory{};
    D3D12DDI_HHEAP heap{&heap_memory}; D3D12DDI_HRESOURCE resource{&resource_memory};
    D3D12DDI_HRTRESOURCE runtime{reinterpret_cast<HANDLE>(uintptr_t(0xbb6600))};
    native_surfaces.emplace(share.key, NativeSurfaceFixture{opened.hAllocation, opening.hKMResource.handle, runtime.handle, metadata.size});
    surface_associations.push_back({share.key, opened.hAllocation, opening.hKMResource.handle, runtime.handle});
    kernel_heaps.emplace(opened.hAllocation, KernelHeap{0, metadata.size, false, runtime.handle}); kernel_resources.insert(runtime.handle);
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queues) == S_OK);
    REQUIRE(table.pfnOpenHeapAndResource(device, &opening, heap, runtime, resource) == S_OK);
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue{};
    queue.hDrvCommandQueue.pDrvPrivate = &queue_memory; queue.NodeMask = 1;
    queue.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D; queue.hRTCommandQueue.handle = reinterpret_cast<HANDLE>(uintptr_t(0xbb110));
    REQUIRE(table.pfnCreateCommandQueue(device, &queue) == S_OK);
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator{};
    allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_memory;
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    REQUIRE(table.pfnCreateCommandAllocator(device, &allocator) == S_OK);
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 list{};
    list.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
    list.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; list.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    list.hDrvCommandList.pDrvPrivate = &command_memory; list.hRTCommandList.handle = reinterpret_cast<HANDLE>(uintptr_t(0xbb220));
    REQUIRE(table.pfnCreateCommandList(device, &list) == S_OK);
    struct PresentPage { D3D12DDIARG_PRESENT_0001 args; D3D12DDI_ARG_PRESENTSURFACE surface; D3D12DDI_PRESENT_0003 out; };
    auto *page = static_cast<PresentPage *>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(page); *page = {};
    page->surface = {resource, 0}; page->args.phSurfacesToPresent = &page->surface;
    page->args.SurfacesToPresent = 1; page->args.Flags.Flip = 1;
    const auto calls = native_present_calls, closes = surface_closes, releases = surface_releases;
    const auto command_error_count = command_errors.size();
    auto cancel = [&]() {
        const D3D12DDI_PRESENT_0003 zero{};
        if (std::memcmp(&page->out, &zero, sizeof(zero))) fixture_abort(__LINE__);
        table.pfnDestroyHeapAndResource(device, heap, resource);
        table.pfnDestroyCommandList(device, list.hDrvCommandList);
        table.pfnDestroyCommandAllocator(device, allocator.hDrvCommandAllocator);
        table.pfnDestroyCommandQueue(device, queue.hDrvCommandQueue);
        terminate();
        std::memset(&heap_memory, 0xa5, sizeof(heap_memory)); std::memset(&resource_memory, 0xa5, sizeof(resource_memory));
        std::memset(&command_memory, 0xa5, sizeof(command_memory)); std::memset(&queue_memory, 0xa5, sizeof(queue_memory));
        std::memset(&allocator_memory, 0xa5, sizeof(allocator_memory));
        if (!VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
    };
    if (in_backend) native_present_callback = cancel; else identity_query_callback = cancel;
    commands.pfnPresent(list.hDrvCommandList, queue.hDrvCommandQueue, &page->args, &page->out);
    REQUIRE(ctx->native_retiring && native_present_calls == calls + in_backend &&
        surface_closes == closes && surface_releases == releases && command_errors.size() == command_error_count &&
        !ctx->resources && !ctx->native_heaps && !ctx->native_queue_objects && !ctx->native_command_objects && !ctx->native_runtime_queues);
    REQUIRE(!native_present_callback && !identity_query_callback);
    std::puts("PASS native Present cancellation: actual Open, destroyed resource/list/allocator/queue, expired request/output, no late callback or retained token");
    return 0;
}
