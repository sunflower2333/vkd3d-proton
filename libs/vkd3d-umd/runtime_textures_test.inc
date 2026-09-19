/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_textures(Context *ctx, D3D12DDI_HDEVICE device,
        const D3D12DDI_DEVICE_FUNCS_CORE_0003 &table) {
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = heap.Alignment = 65536; heap.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    heap.Flags = D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12DDIARG_CREATERESOURCE_0003 desc{};
    desc.ResourceType = D3D12DDI_RT_TEXTURE2D; desc.Width = 4; desc.Height = 4;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_DEST;
    alignas(NativeHeapSlot) std::array<unsigned char, sizeof(NativeHeapSlot) + 8> storage;
    alignas(Object) std::array<unsigned char, sizeof(Object) + 8> resource;
    D3D12DDI_HHEAP h{storage.data()}; D3D12DDI_HRESOURCE r{resource.data()};
    auto create = [&]() { return table.pfnCreateHeapAndResource(device, &heap, h, {}, &desc, nullptr, r); };
    for (unsigned round = 0; round < 3; ++round) {
        const bool rt = round == 2;
        const auto format = round ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32_FLOAT;
        heap.Flags = rt ? D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES : D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
        desc.Flags = static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(rt ? 1 : 0);
        desc.InitialResourceState = rt ? D3D12DDI_RESOURCE_STATE_RENDER_TARGET : D3D12DDI_RESOURCE_STATE_COPY_DEST;
        const unsigned prior_rt_queries = rt_queries, prior_rt_imports = rt_imports, prior_rt_placements = rt_placements;
        desc.Format = format; storage.fill(0xa5); resource.fill(0xa5);
        const unsigned before = allocations;
        REQUIRE(table.pfnCheckResourceAllocationInfo);
        D3D12DDI_RESOURCE_ALLOCATION_INFO info;
        std::memset(&info, 0xa5, sizeof(info));
        const auto original = info;
        const auto query_desc = desc;
        const unsigned before_queries = allocation_queries;
        texture_allocation_callback = [&]() { std::memset(&desc, 0xcc, sizeof(desc)); };
        table.pfnCheckResourceAllocationInfo(device, &desc, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_SHADER_RESOURCE,
                65536, 1, &info);
        texture_allocation_callback = {}; desc = query_desc;
        REQUIRE(info.ResourceDataSize == 65536 && info.ResourceDataAlignment == 65536 &&
                info.Layout == D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0 && !info.AdditionalDataSize &&
                !info.AdditionalDataHeaderSize && allocations == before && allocation_queries == before_queries + 1);
        const auto valid_info = info;
        auto rejected_query = [&](UINT64 restriction, UINT visible, D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags) {
            info = original;
            table.pfnCheckResourceAllocationInfo(device, &desc, flags, restriction, visible, &info);
            return !std::memcmp(&info, &original, sizeof(info));
        };
        REQUIRE(rejected_query(4096, 1, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE));
        REQUIRE(rejected_query(65536, 2, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE));
        REQUIRE(rejected_query(65536, 1, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_PRIMARY));
        REQUIRE(allocation_queries == before_queries + 1);
        allocation_query_result = E_OUTOFMEMORY;
        REQUIRE(rejected_query(65536, 1, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE)); allocation_query_result = S_OK;
        texture_allocation_callback = [&]() { ctx->native_retiring = true; };
        REQUIRE(rejected_query(65536, 1, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE));
        texture_allocation_callback = {}; ctx->native_retiring = false;
        info = valid_info;
        auto buffer_desc = desc; buffer_desc.ResourceType = D3D12DDI_RT_BUFFER; buffer_desc.Width = 65537;
        buffer_desc.Height = 1; buffer_desc.Format = DXGI_FORMAT_UNKNOWN; buffer_desc.Layout = D3D12DDI_TL_ROW_MAJOR;
        buffer_desc.Flags = static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(0); buffer_desc.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_DEST;
        table.pfnCheckResourceAllocationInfo(device, &buffer_desc, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
        REQUIRE(info.ResourceDataSize == 131072 && info.ResourceDataAlignment == 65536 && info.Layout == D3D12DDI_TL_ROW_MAJOR &&
                allocations == before);
        heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
        REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == before);
        heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        desc.MipLevels = 2; REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == before); desc.MipLevels = 1;
        desc.ReuseBufferGPUVA.BaseAddress.UMD.Offset = 65536;
        REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == before); desc.ReuseBufferGPUVA = {};
        texture_required_bytes = 131072;
        REQUIRE(create() == E_INVALIDARG && allocations == before); texture_required_bytes = 65536;
        import_failure = true; REQUIRE(create() == E_OUTOFMEMORY && !ctx->native_heaps && kernel_heaps.empty());
        import_failure = false; placement_failure = true;
        REQUIRE(create() == E_INVALIDARG && !ctx->native_heaps && kernel_heaps.empty()); placement_failure = false;
        for (auto byte : storage) REQUIRE(byte == 0xa5);
        for (auto byte : resource) REQUIRE(byte == 0xa5);
        const auto saved_heap = heap; const auto saved_desc = desc;
        texture_allocation_callback = [&]() { std::memset(&heap, 0xcc, sizeof(heap)); std::memset(&desc, 0xcc, sizeof(desc)); };
        REQUIRE(create() == S_OK);
        texture_allocation_callback = {}; heap = saved_heap; desc = saved_desc;
        auto *token = native_heap_find(ctx, h.pDrvPrivate);
        auto *view = object(r.pDrvPrivate);
        auto *peer = reinterpret_cast<ImportPeer *>(view->backend);
        REQUIRE(token && token->users == 2 && token->allocation && !token->cpu_visible && !last_import_cpu_visible);
        REQUIRE(view->kind == VKDU_TEXTURE2D && !view->address && !find_buffer(ctx, r.pDrvPrivate) &&
                find_resource(ctx, r.pDrvPrivate) == view && peer->width == 4 && peer->height == 4 && peer->format == format);
        REQUIRE(last_placement_state == (rt ? 4u : 0x400u));
        if (rt) REQUIRE(rt_queries > prior_rt_queries && rt_imports == prior_rt_imports + 3 && rt_placements == prior_rt_placements + 2);
        else REQUIRE(rt_queries == prior_rt_queries && rt_imports == prior_rt_imports && rt_placements == prior_rt_placements);
        for (size_t i = sizeof(Object); i < resource.size(); ++i) REQUIRE(resource[i] == 0xa5);
        for (size_t i = sizeof(NativeHeapSlot); i < storage.size(); ++i) REQUIRE(storage[i] == 0xa5);
        void *mapped = nullptr;
        REQUIRE(table.pfnMapHeap(device, h, &mapped) == E_INVALIDARG && !mapped);
        vkdu_object *alias = nullptr;
        const unsigned before_alias = allocations;
        REQUIRE(test_texture_import(ctx->backend, ctx, token, token->bytes, &alias) == S_OK && allocations == before_alias);
        table.pfnDestroyHeapAndResource(device, h, {});
        REQUIRE(native_token(ctx, token) && kernel_heaps.size() == 1);
        table.pfnDestroyHeapAndResource(device, {}, r);
        REQUIRE(native_token(ctx, token) && kernel_heaps.size() == 1);
        test_object_destroy(alias);
        REQUIRE(!ctx->native_heaps && kernel_heaps.empty());
    }
    puts("PASS native sampled/RT texture heap allocation/import, request poisoning, exact backing branch, alias lifetime and failure cleanup");
    puts("PASS native allocation-info query, backend requirements, no allocation, rejected-output preservation and retirement");
    return 0;
}
