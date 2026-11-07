/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Calls the original R0 tables. Kernel/backend peers are controlled here;
 * sample storage and resolve pixels are verified by the real backend test. */
static int test_native_msaa_admission()
{
    D3DDDI_ADAPTERCALLBACKS callbacks{};
    callbacks.pfnQueryAdapterInfoCb = test_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = expected_adapter;
    open.pAdapterCallbacks = &callbacks; open.pAdapterFuncs = &adapter;
    REQUIRE(OpenAdapter12(&open) == S_OK);
    D3DDDI_DEVICECALLBACKS kt{};
    kt.pfnCreateContextCb = heap_context_create; kt.pfnDestroyContextCb = heap_context_destroy;
    kt.pfnEscapeCb = heap_escape; kt.pfnAllocateCb = heap_allocate;
    kt.pfnDeallocateCb = heap_deallocate; kt.pfnDeallocate2Cb = heap_deallocate2;
    kt.pfnLockCb = heap_lock; kt.pfnUnlockCb = heap_unlock; kt.pfnRenderCb = heap_render;
    kt.pfnSignalSynchronizationObject2Cb = heap_signal;
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{}; um.pfnSetErrorCb = test_error;
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> memory{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    create.hDrvDevice.pDrvPrivate = memory.data(); create.hRTDevice.handle = expected_device;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
    create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
    REQUIRE(adapter.pfnCreateDevice(open.hAdapter, &create) == S_OK);
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&table, &commands, &queue) == S_OK);
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = heap.Alignment = 4194304;
    heap.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    heap.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12DDIARG_CREATERESOURCE_0003 resource{};
    resource.ResourceType = D3D12DDI_RT_TEXTURE2D;
    resource.Width = 17; resource.Height = 9;
    resource.DepthOrArraySize = 3; resource.MipLevels = 1;
    resource.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource.Flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;
    resource.InitialResourceState = D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    int failures = 0;
    for (UINT samples : {2u, 4u}) {
        resource.SampleDesc.Count = samples;
        const auto sizes = table.pfnCalcPrivateHeapAndResourceSizes(create.hDrvDevice, &heap, &resource);
        if (!sizes.Heap || !sizes.Resource) {
            std::fprintf(stderr, "FAIL MSAA R0 admission count=%u quality=0 heap_alignment=4194304 private_heap=%zu private_resource=%zu\n",
                    samples, sizes.Heap, sizes.Resource);
            ++failures;
        }
    }
    if (!commands.pfnResourceResolveSubresource) {
        std::fprintf(stderr, "FAIL MSAA R0 ResolveSubresource callback is unavailable\n");
        ++failures;
    }
    if (!failures) {
        msaa_heap_fixture = true; texture_required_bytes = 4194304;
        auto *ctx = context(create.hDrvDevice);
        alignas(NativeHeapSlot) std::array<unsigned char, sizeof(NativeHeapSlot)+8> hs{};
        alignas(Object) std::array<unsigned char, sizeof(Object)+8> rs{};
        D3D12DDI_HHEAP hh{hs.data()}; D3D12DDI_HRESOURCE rh{rs.data()};
        for (UINT samples : {2u, 4u}) {
            resource.SampleDesc.Count = samples;
            D3D12DDI_RESOURCE_ALLOCATION_INFO info{};
            table.pfnCheckResourceAllocationInfo(create.hDrvDevice, &resource, {}, 4194304, 1, &info);
            REQUIRE(info.ResourceDataAlignment == 4194304 && info.ResourceDataSize == 4194304 &&
                info.Layout == D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0);
            auto unchanged = info;
            table.pfnCheckResourceAllocationInfo(create.hDrvDevice, &resource, {}, 65536, 1, &info);
            REQUIRE(!std::memcmp(&info, &unchanged, sizeof(info)));
            hs.fill(0xa5); rs.fill(0xa5); seen_rt_import_alignment = 0;
            REQUIRE(table.pfnCreateHeapAndResource(create.hDrvDevice, &heap, hh, {}, &resource, nullptr, rh) == S_OK);
            auto *backing = native_heap_find(ctx, hh.pDrvPrivate);
            REQUIRE(backing && backing->alignment == 4194304 && !(backing->address % 4194304) && backing->users == 2);
            REQUIRE(seen_rt_import_alignment == 4194304 && seen_texture_shape_query.sample_count == samples &&
                seen_texture_shape_place.sample_count == samples && !seen_texture_shape_place.sample_quality &&
                seen_texture_shape_place.layers == 3);
            REQUIRE(object(rh.pDrvPrivate)->native_resource_info.ResourceDataAlignment == 4194304);
            for (size_t i=sizeof(NativeHeapSlot);i<hs.size();++i) REQUIRE(hs[i] == 0xa5);
            for (size_t i=sizeof(Object);i<rs.size();++i) REQUIRE(rs[i] == 0xa5);
            table.pfnDestroyHeapAndResource(create.hDrvDevice, hh, {});
            REQUIRE(native_token(ctx, backing) && backing->users == 1 && kernel_heaps.size() == 1);
            table.pfnDestroyHeapAndResource(create.hDrvDevice, {}, rh);
            REQUIRE(kernel_heaps.empty() && !ctx->native_heaps);
            for (unsigned fault=0;fault<2;++fault) {
                import_failure = !fault; placement_failure = !!fault;
                hs.fill(0xa5); rs.fill(0xa5);
                REQUIRE(FAILED(table.pfnCreateHeapAndResource(create.hDrvDevice, &heap, hh, {}, &resource, nullptr, rh)));
                REQUIRE(kernel_heaps.empty() && !ctx->native_heaps);
                for (unsigned char c : hs) REQUIRE(c == 0xa5);
                for (unsigned char c : rs) REQUIRE(c == 0xa5);
                import_failure = placement_failure = false;
            }
            std::printf("PASS MSAA R0 count%u shape/query/4MiB RequestedIova/wire4096/import/rollback/heap-resource lifetime (controlled peers)\n", samples);
        }
        auto rejected = [&](const D3D12DDIARG_CREATEHEAP_0001 &h, const D3D12DDIARG_CREATERESOURCE_0003 &r) {
            // Private sizes reserve objects even for unsupported descriptions.
            // Require actual Create rejection, no allocation and untouched slots.
            hs.fill(0xa5); rs.fill(0xa5);
            const auto before = allocations;
            const auto sizes = table.pfnCalcPrivateHeapAndResourceSizes(create.hDrvDevice, &h, &r);
            return sizes.Heap == sizeof(NativeHeapSlot) && sizes.Resource == sizeof(Object) &&
                FAILED(table.pfnCreateHeapAndResource(create.hDrvDevice, &h, hh, {}, &r, nullptr, rh)) &&
                allocations == before && kernel_heaps.empty() && kernel_resources.empty() && !ctx->native_heaps &&
                std::all_of(hs.begin(), hs.end(), [](unsigned char c) { return c == 0xa5; }) &&
                std::all_of(rs.begin(), rs.end(), [](unsigned char c) { return c == 0xa5; });
        };
        auto bad_heap = heap; bad_heap.Alignment = 65536; REQUIRE(rejected(bad_heap,resource));
        bad_heap = heap; bad_heap.Alignment = 0; REQUIRE(rejected(bad_heap,resource));
        bad_heap = heap; bad_heap.ByteSize += 65536; REQUIRE(rejected(bad_heap,resource));
        for (UINT n : {0u,3u,8u,UINT32_MAX}) { auto bad = resource; bad.SampleDesc.Count=n; REQUIRE(rejected(heap,bad)); }
        auto bad = resource; bad.SampleDesc.Quality=1; REQUIRE(rejected(heap,bad));
        bad = resource; bad.MipLevels=2; REQUIRE(rejected(heap,bad));
        bad = resource; bad.Format=DXGI_FORMAT_D32_FLOAT; bad.Flags=D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL; REQUIRE(rejected(heap,bad));
        bad = resource; bad.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS; REQUIRE(rejected(heap,bad));
        bad = resource; bad.Layout=D3D12DDI_TL_ROW_MAJOR; REQUIRE(rejected(heap,bad));
        bad = resource; bad.ResourceType=D3D12DDI_RT_BUFFER; REQUIRE(rejected(heap,bad));
        REQUIRE(!native_contract_complete());
        {
            auto h = heap; auto r = resource;
            h.ByteSize = h.Alignment = 65536; h.Flags = D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
            r.SampleDesc.Count = 1; r.DepthOrArraySize = 1; r.Flags = D3D12DDI_RESOURCE_FLAG_0003_NONE;
            r.InitialResourceState = D3D12DDI_RESOURCE_STATE_RESOLVE_DEST;
            texture_required_bytes = 65536; msaa_heap_fixture = false;
            hs.fill(0xa5); rs.fill(0xa5);
            REQUIRE(table.pfnCreateHeapAndResource(create.hDrvDevice,&h,hh,{},&r,nullptr,rh) == S_OK);
            REQUIRE(last_placement_state == D3D12DDI_RESOURCE_STATE_RESOLVE_DEST);
            auto *backing = native_heap_find(ctx,hh.pDrvPrivate);
            REQUIRE(backing && backing->alignment == 65536 && backing->users == 2);
            table.pfnDestroyHeapAndResource(create.hDrvDevice,hh,{});
            REQUIRE(backing->users == 1 && kernel_heaps.size() == 1);
            table.pfnDestroyHeapAndResource(create.hDrvDevice,{},rh);
            REQUIRE(kernel_heaps.empty() && !ctx->native_heaps);
            for (size_t i=sizeof(NativeHeapSlot);i<hs.size();++i) REQUIRE(hs[i] == 0xa5);
            for (size_t i=sizeof(Object);i<rs.size();++i) REQUIRE(rs[i] == 0xa5);
            texture_required_bytes = 4194304; msaa_heap_fixture = true;
            std::puts("PASS MSAA native non-RT Count1/layer1/mip1 RESOLVE_DEST creation and lifetime (controlled peers)");
        }
        {
            D3D12DDI_RESOURCE_ALLOCATION_INFO canary;
            std::memset(&canary,0xa5,sizeof(canary)); const auto unchanged = canary;
            const auto errors_before = error_calls;
            texture_allocation_callback = [&]() { ++generation; };
            table.pfnCheckResourceAllocationInfo(create.hDrvDevice,&resource,{},4194304,1,&canary);
            texture_allocation_callback = {}; --generation;
            REQUIRE(!ctx->native_retiring && ctx->runtime_device.handle == expected_device);
            if (std::memcmp(&canary,&unchanged,sizeof(canary))) {
                std::fprintf(stderr,"FAIL MSAA allocation query published output across live generation change\n"); ++failures;
            } else {
                REQUIRE(error_calls == errors_before+1 && ctx->last_error == DXGI_ERROR_DEVICE_REMOVED);
                std::puts("PASS MSAA allocation query generation change preserves output canary");
            }
        }
        texture_required_bytes=65536; msaa_heap_fixture=false;
    }
    adapter.pfnDestroyDevice(create.hDrvDevice);
    REQUIRE(adapter.pfnCloseAdapter(open.hAdapter) == S_OK);
    if (failures) return 1;
    std::puts("PASS MSAA R0 2x/4x private admission and typed resolve callback (controlled peers; no GPU claim)");
    return 0;
}
