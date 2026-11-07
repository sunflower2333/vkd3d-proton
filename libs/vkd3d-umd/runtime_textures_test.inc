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
    D3D12DDI_CLEAR_VALUES optimized{};
    const D3D12DDI_CLEAR_VALUES *optimized_pointer = nullptr;
    auto create = [&]() { return table.pfnCreateHeapAndResource(device, &heap, h, {}, &desc, optimized_pointer, r); };
    for (unsigned attachment = 0; attachment < 7; ++attachment) {
        const auto original_desc = desc;
        const auto original_heap = heap;
        desc.Width = 9; desc.Height = 7; desc.MipLevels = 4; desc.DepthOrArraySize = 3;
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_TYPELESS,
            DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM,
            DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
        desc.Format = formats[attachment];
        desc.Flags = static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(attachment >= 3 ? 2 : attachment ? 17 : 0);
        heap.Flags = attachment ? D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES : D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
        D3D12DDI_RESOURCE_ALLOCATION_INFO info{};
        table.pfnCheckResourceAllocationInfo(device, &desc, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
        REQUIRE(info.ResourceDataSize == 65536 && info.ResourceDataAlignment == 65536 &&
                info.Layout == D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0);
        REQUIRE(seen_texture_shape_query.width == 9 && seen_texture_shape_query.height == 7 &&
                seen_texture_shape_query.mips == 4 && seen_texture_shape_query.layers == 3 &&
                seen_texture_shape_query.flags == (attachment >= 3 ? 2u : attachment ? 1u : 0u));
        REQUIRE(create() == S_OK);
        REQUIRE(!std::memcmp(&seen_texture_shape_place, &seen_texture_shape_query, sizeof(seen_texture_shape_place)));
        REQUIRE(object(r.pDrvPrivate)->native_resource_info.ResourceDataSize == info.ResourceDataSize);
        table.pfnDestroyHeapAndResource(device, {}, r); table.pfnDestroyHeapAndResource(device, h, {});
        REQUIRE(!ctx->native_heaps && kernel_heaps.empty());
        const auto valid = desc;
        const unsigned before = allocations;
        for (unsigned invalid = 0; invalid < 7; ++invalid) {
            desc = valid;
            if (invalid == 0) desc.MipLevels = 0;
            if (invalid == 1) desc.MipLevels = 5;
            if (invalid == 2) desc.DepthOrArraySize = 0;
            if (invalid == 3) desc.DepthOrArraySize = 2049;
            if (invalid == 4) desc.SampleDesc.Count = 2;
            if (invalid == 5) desc.Width = 16385;
            if (invalid == 6) desc.Flags = static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(3);
            REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == before);
            std::memset(&info, 0xa5, sizeof(info)); const auto untouched = info;
            table.pfnCheckResourceAllocationInfo(device, &desc, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
            REQUIRE(!std::memcmp(&info, &untouched, sizeof(info)) && allocations == before);
        }
        desc = valid;
        texture_allocation_callback = [&]() { ctx->native_retiring = true; };
        REQUIRE(create() == DXGI_ERROR_DEVICE_REMOVED && allocations == before);
        texture_allocation_callback = {}; ctx->native_retiring = false;
        storage.fill(0xa5); resource.fill(0xa5);
        for (unsigned failure = 0; failure < 2; ++failure) {
            import_failure = !failure; placement_failure = failure;
            const unsigned closed = pageable_deallocation_calls;
            REQUIRE(create() == (failure ? E_INVALIDARG : E_OUTOFMEMORY));
            REQUIRE(!ctx->native_heaps && kernel_heaps.empty() && pageable_deallocation_calls == closed + 1);
            for (auto byte : storage) REQUIRE(byte == 0xa5);
            for (auto byte : resource) REQUIRE(byte == 0xa5);
        }
        import_failure = placement_failure = false;
        for (unsigned destroy_heap_first = 0; destroy_heap_first < 2; ++destroy_heap_first) {
            void *page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            REQUIRE(page);
            auto *heap_page = static_cast<D3D12DDIARG_CREATEHEAP_0001 *>(page);
            auto *resource_page = reinterpret_cast<D3D12DDIARG_CREATERESOURCE_0003 *>(static_cast<char *>(page) + 1024);
            *heap_page = heap; *resource_page = desc;
            bool unmapped = false;
            texture_allocation_callback = [&]() { unmapped = VirtualFree(page, 0, MEM_RELEASE) != 0; page = nullptr; };
            REQUIRE(table.pfnCreateHeapAndResource(device, heap_page, h, {}, resource_page, nullptr, r) == S_OK);
            texture_allocation_callback = {};
            REQUIRE(unmapped && !page && seen_texture_shape_place.width == 9 && seen_texture_shape_place.height == 7 &&
                    seen_texture_shape_place.mips == 4 && seen_texture_shape_place.layers == 3);
            const unsigned closed = pageable_deallocation_calls;
            if (destroy_heap_first) table.pfnDestroyHeapAndResource(device, h, {});
            else table.pfnDestroyHeapAndResource(device, {}, r);
            REQUIRE(kernel_heaps.size() == 1 && pageable_deallocation_calls == closed);
            if (destroy_heap_first) table.pfnDestroyHeapAndResource(device, {}, r);
            else table.pfnDestroyHeapAndResource(device, h, {});
            REQUIRE(!ctx->native_heaps && kernel_heaps.empty() && pageable_deallocation_calls == closed + 1);
        }
        desc = original_desc; heap = original_heap;
        printf("PASS native 9x7/4mip/3layer query/create attachment%u: seven invalid shapes, query cancellation before allocation, import/placement failure transaction, VirtualFree of caller requests, both destroy orders and one Deallocate2 flags3 close (controlled backend)\n", attachment);
    }
    const DXGI_FORMAT color_formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    for (unsigned round = 0; round < 34; ++round) {
        const unsigned shape = round >= 30 ? 3 : round % 11;
        const bool typeless = round >= 22 && round < 30;
        const unsigned extra = typeless ? round - 22 : 0;
        const bool rt = typeless ? (extra & 2) != 0 : shape == 2 || shape >= 8;
        const bool depth = !typeless && (shape == 3 || shape == 4);
        const auto format = round >= 30 ? (round % 2 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT) :
            typeless ? (extra & 1 ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS) :
            shape >= 5 ? color_formats[(shape - 5) % 3] :
            depth ? (shape == 3 ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D16_UNORM) : shape ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32_FLOAT;
        const auto clear_format = !typeless ? format : extra & 1 ?
            (extra & 4 ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM) :
            (extra & 4 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM);
        heap.Flags = rt || depth ? D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES : D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
        // Exercise both runtime depth bindings: 0x2 and 0x12 for every typed
        // depth format. The independent binding bit does not create an SRV.
        const bool shader_resource = round >= 11 && round != 30 && round != 31;
        desc.Flags = static_cast<D3D12DDI_RESOURCE_FLAGS_0003>((depth ? 2 : rt ? 1 : 0) | (shader_resource ? 16 : 0));
        if (depth) REQUIRE(static_cast<UINT>(desc.Flags) == (shader_resource ? 0x12u : 0x2u));
        optimized = {}; optimized.Format = clear_format;
        if (depth) { optimized.DepthStencil.Depth = 0.75f; optimized.DepthStencil.Stencil = 17; }
        else { optimized.Color[0] = 0.25f; optimized.Color[1] = 0.5f; optimized.Color[2] = 0.75f; optimized.Color[3] = 1; }
        optimized_pointer = round >= 11 && (rt || depth) ? &optimized : nullptr;
        desc.InitialResourceState = depth ? D3D12DDI_RESOURCE_STATE_DEPTH_WRITE :
            rt ? D3D12DDI_RESOURCE_STATE_RENDER_TARGET : D3D12DDI_RESOURCE_STATE_COPY_DEST;
        const unsigned prior_rt_queries = rt_queries, prior_rt_imports = rt_imports, prior_rt_placements = rt_placements;
        const unsigned prior_depth_queries = depth_queries, prior_depth_placements = depth_placements;
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
        if (depth) std::printf("DEPTH_QUERY round=%u format=%u flags=0x%x bytes=%llu alignment=%llu query_delta=%u output_changed=%u\n",
                round, static_cast<unsigned>(format), static_cast<unsigned>(desc.Flags),
                static_cast<unsigned long long>(info.ResourceDataSize), static_cast<unsigned long long>(info.ResourceDataAlignment),
                allocation_queries - before_queries, std::memcmp(&info, &original, sizeof(info)) != 0 ? 1u : 0u);
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
        const auto single_node_info = info;
        info = {};
        table.pfnCheckResourceAllocationInfo(device, &buffer_desc, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &info);
        REQUIRE(!std::memcmp(&info, &single_node_info, sizeof(info)) && allocations == before);
        heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
        REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == before);
        heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        desc.MipLevels = 32; REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == before); desc.MipLevels = 1;
        desc.ReuseBufferGPUVA.BaseAddress.UMD.Offset = 65536;
        REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == before); desc.ReuseBufferGPUVA = {};
        const auto valid_flags = desc.Flags;
        if (depth) {
            const unsigned before_invalid_queries = allocation_queries;
            desc.Flags |= D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;
            REQUIRE(rejected_query(65536, 1, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE));
            REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == before &&
                    allocation_queries == before_invalid_queries);
            desc.Flags = valid_flags;
        }
        if (round >= 30) {
            desc.Format = round % 2 ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
            REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == before); desc.Format = format;
        }
        desc.Flags |= D3D12DDI_RESOURCE_FLAG_0003_SIMULTANEOUS_ACCESS;
        REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == before); desc.Flags = valid_flags;
        if (optimized_pointer) {
            optimized.Format = DXGI_FORMAT_UNKNOWN;
            REQUIRE(create() == E_INVALIDARG && allocations == before); optimized.Format = clear_format;
            if (typeless) {
                optimized.Format = format;
                REQUIRE(create() == E_INVALIDARG && allocations == before);
                optimized.Format = extra & 1 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
                REQUIRE(create() == E_INVALIDARG && allocations == before); optimized.Format = clear_format;
            }
            if (depth) {
                optimized.DepthStencil.Depth = -0.25f;
                REQUIRE(create() == E_INVALIDARG && allocations == before);
                optimized.DepthStencil.Depth = std::numeric_limits<float>::quiet_NaN();
                REQUIRE(create() == E_INVALIDARG && allocations == before);
                optimized.DepthStencil.Depth = 1.25f;
                REQUIRE(create() == E_INVALIDARG && allocations == before);
                optimized.DepthStencil.Depth = 0.75f;
            }
        } else if (!rt && !depth) {
            optimized_pointer = &optimized;
            REQUIRE(create() == E_INVALIDARG && allocations == before); optimized_pointer = nullptr;
        }
        texture_required_bytes = 131072;
        REQUIRE(create() == E_INVALIDARG && allocations == before); texture_required_bytes = 65536;
        import_failure = true; REQUIRE(create() == E_OUTOFMEMORY && !ctx->native_heaps && kernel_heaps.empty());
        import_failure = false; placement_failure = true;
        REQUIRE(create() == E_INVALIDARG && !ctx->native_heaps && kernel_heaps.empty()); placement_failure = false;
        for (auto byte : storage) REQUIRE(byte == 0xa5);
        for (auto byte : resource) REQUIRE(byte == 0xa5);
        const auto saved_heap = heap; const auto saved_desc = desc; const auto saved_clear = optimized;
        void *clear_page = nullptr;
        bool clear_unmapped = !optimized_pointer;
        if (optimized_pointer) {
            clear_page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            REQUIRE(clear_page);
            std::memcpy(clear_page, optimized_pointer, sizeof(optimized));
            optimized_pointer = static_cast<const D3D12DDI_CLEAR_VALUES *>(clear_page);
        }
        texture_allocation_callback = [&]() {
            std::memset(&heap, 0xcc, sizeof(heap)); std::memset(&desc, 0xcc, sizeof(desc));
            std::memset(&optimized, 0xcc, sizeof(optimized));
            if (clear_page && VirtualFree(clear_page, 0, MEM_RELEASE)) {
                clear_page = nullptr;
                clear_unmapped = true;
            }
        };
        const unsigned before_successful_create_closed = pageable_deallocation_calls;
        REQUIRE(create() == S_OK);
        REQUIRE(clear_unmapped && !clear_page);
        texture_allocation_callback = {}; heap = saved_heap; desc = saved_desc; optimized = saved_clear;
        optimized_pointer = round >= 11 && (rt || depth) ? &optimized : nullptr;
        auto *token = native_heap_find(ctx, h.pDrvPrivate);
        auto *view = object(r.pDrvPrivate);
        auto *peer = reinterpret_cast<ImportPeer *>(view->backend);
        REQUIRE(token && token->users == 2 && token->allocation && !token->cpu_visible && !last_import_cpu_visible);
        REQUIRE(view->kind == VKDU_TEXTURE2D && !view->address && !find_buffer(ctx, r.pDrvPrivate) &&
                find_resource(ctx, r.pDrvPrivate) == view && peer->width == 4 && peer->height == 4 && peer->format == format);
        REQUIRE(test_native_resource_queries(ctx, device, table, r, token,
                D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0, 0) == 0);
        REQUIRE(last_placement_state == (depth ? 0x10u : rt ? 4u : 0x400u));
        if (rt) REQUIRE(rt_queries > prior_rt_queries && rt_imports == prior_rt_imports + 3 && rt_placements == prior_rt_placements + 2);
        else if (depth) REQUIRE(rt_queries == prior_rt_queries && rt_imports == prior_rt_imports + 3 &&
            rt_placements == prior_rt_placements && depth_queries > prior_depth_queries && depth_placements == prior_depth_placements + 2);
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
        if (depth && shader_resource) REQUIRE(pageable_deallocation_calls == before_successful_create_closed + 1);
    }
    puts("PASS native texture heap allocation/import, request poisoning, format ownership, alias lifetime and failure cleanup");
    puts("PASS native RT shared allocation/import/placement and retained alias backing");
    puts("PASS native D32/D16/D24S8/D32S8 depth allocation/import/placement, poisoned requests, rejected layout and failure cleanup");
    puts("PASS native D32/D16/D24S8/D32S8 depth flags 0x2/0x12 allocation-info query/create/import/placement, mixed RT/DS rejection and final-owner destruction (controlled backend)");
    puts("PASS native allocation-info query, backend requirements, no allocation, rejected-output preservation and retirement");
    puts("PASS native ordinary/RT/depth SHADER_RESOURCE creation and optimized color/depth clear hints: 34 shapes including combined DS and RGBA/BGRA typeless, DS typeless resource rejection, early invalid hint rejection, callback-expired hint inputs, no initial-clear side effect");
    return 0;
}
