/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_buffer_children(Context *ctx, D3D12DDI_HDEVICE device,
        const D3D12DDI_DEVICE_FUNCS_CORE_0003 &table) {
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = 262144; heap.Alignment = 65536; heap.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    D3D12DDIARG_CREATERESOURCE_0003 parent{};
    parent.ResourceType = D3D12DDI_RT_BUFFER; parent.Width = heap.ByteSize;
    parent.Height = parent.DepthOrArraySize = parent.MipLevels = parent.SampleDesc.Count = 1;
    parent.Layout = D3D12DDI_TL_ROW_MAJOR;
    parent.InitialResourceState = static_cast<D3D12DDI_RESOURCE_STATES>(0xac3);
    alignas(NativeHeapSlot) std::array<unsigned char, sizeof(NativeHeapSlot) + 8> hs;
    alignas(Object) std::array<unsigned char, sizeof(Object) + 8> ps, cs;
    hs.fill(0xa5); ps.fill(0xa5); cs.fill(0xa5);
    D3D12DDI_HHEAP h{hs.data()}; D3D12DDI_HRESOURCE p{ps.data()}, c{cs.data()};
    REQUIRE(table.pfnCreateHeapAndResource(device, &heap, h, {}, &parent, nullptr, p) == S_OK);
    auto child = parent; child.Width = 4096;
    child.ReuseBufferGPUVA.BaseAddress.UMD = {p, 65536};
    auto create = [&]() { return table.pfnCreateHeapAndResource(device, nullptr, {},
        {reinterpret_cast<HANDLE>(uintptr_t(0x9988))}, &child, nullptr, c); };
    const unsigned before = allocations, released = deallocations;
    auto sizes = table.pfnCalcPrivateHeapAndResourceSizes(device, nullptr, &child);
    REQUIRE(!sizes.Heap && sizes.Resource == sizeof(Object));
    auto *allocation = native_heap_find(ctx, h.pDrvPrivate);
    const auto valid = child;
    for (auto offset : {uint64_t(1), uint64_t(262144), UINT64_MAX - 65535}) {
        child.ReuseBufferGPUVA.BaseAddress.UMD.Offset = offset;
        REQUIRE(FAILED(create()) && allocations == before && deallocations == released);
    }
    child = valid; child.Width = 262144;
    REQUIRE(FAILED(create()));
    child = valid; child.ReuseBufferGPUVA.BaseAddress.UMD.hResource.pDrvPrivate = reinterpret_cast<void *>(uintptr_t(1));
    REQUIRE(FAILED(create()));
    child = valid;
    REQUIRE(table.pfnCreateHeapAndResource(device, nullptr, h, {}, &child, nullptr, c) == E_INVALIDARG);
    REQUIRE(table.pfnCreateHeapAndResource(device, nullptr, {}, {}, &child, nullptr, p) == E_INVALIDARG);
    D3D12DDI_CLEAR_VALUES clear{};
    REQUIRE(table.pfnCreateHeapAndResource(device, nullptr, {}, {}, &child, &clear, c) == E_INVALIDARG);
    for (unsigned failure = 0; failure < 3; ++failure) {
        allocation_query_result = failure == 0 ? E_ACCESSDENIED : S_OK;
        import_failure = failure == 1; placement_failure = failure == 2;
        REQUIRE(FAILED(create()) && allocation->users == 2 && allocations == before && deallocations == released);
        for (auto value : cs) REQUIRE(value == 0xa5);
    }
    allocation_query_result = S_OK; import_failure = placement_failure = false;
    REQUIRE(create() == S_OK);
    REQUIRE(allocations == before && deallocations == released && kernel_resources.empty() &&
        object(c.pDrvPrivate)->native_heap == allocation && object(c.pDrvPrivate)->address == allocation->address + 65536);
    REQUIRE(test_native_resource_queries(ctx, device, table, c, allocation, D3D12DDI_TL_ROW_MAJOR,
        allocation->address + 65536) == 0);
    REQUIRE(find_buffer_address(ctx, allocation->address + 65536, 8192) == object(p.pDrvPrivate));
    table.pfnDestroyHeapAndResource(device, h, p);
    REQUIRE(kernel_heaps.size() == 1 && native_token(ctx, allocation));
    REQUIRE(find_buffer_address(ctx, allocation->address + 65536, 4096) == object(c.pDrvPrivate));
    REQUIRE(!find_buffer_address(ctx, allocation->address + 65536, 4097));
    table.pfnDestroyHeapAndResource(device, {}, c);
    REQUIRE(kernel_heaps.empty() && !ctx->native_heaps && deallocations == released + 1);
    // All permutations of parent resource, public heap and child destruction.
    const unsigned orders[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    for (unsigned cached = 0; cached < 2; ++cached) for (const auto &order : orders) {
        heap.CPUPageProperty = cached ? D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK : D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        hs.fill(0xa5); ps.fill(0xa5); cs.fill(0xa5);
        REQUIRE(table.pfnCreateHeapAndResource(device, &heap, h, {}, &parent, nullptr, p) == S_OK);
        child = valid; child.Width = 131072;
        if (!cached) child.ReuseBufferGPUVA.BaseAddress.UMD.Offset = 0;
        REQUIRE(create() == S_OK);
        alignas(Object) std::array<unsigned char, sizeof(Object)> nested_storage{};
        D3D12DDI_HRESOURCE nested{nested_storage.data()};
        auto nested_desc = child; nested_desc.Width = 4096;
        nested_desc.ReuseBufferGPUVA.BaseAddress.UMD = {c, 65536};
        REQUIRE(table.pfnCreateHeapAndResource(device, nullptr, {}, {}, &nested_desc, nullptr, nested) == S_OK);
        allocation = native_heap_find(ctx, h.pDrvPrivate);
        const auto nested_address = allocation->address + child.ReuseBufferGPUVA.BaseAddress.UMD.Offset + 65536;
        REQUIRE(object(nested.pDrvPrivate)->address == nested_address);
        REQUIRE(find_buffer_address(ctx, nested_address, 8192) == object(p.pDrvPrivate));
        const auto releases = deallocations;
        for (unsigned action : order) {
            if (action == 0) table.pfnDestroyHeapAndResource(device, {}, p);
            if (action == 1) table.pfnDestroyHeapAndResource(device, h, {});
            if (action == 2) table.pfnDestroyHeapAndResource(device, {}, c);
            REQUIRE(kernel_heaps.size() == 1 && deallocations == releases && native_token(ctx, allocation));
        }
        REQUIRE(test_native_resource_queries(ctx, device, table, nested, allocation,
            D3D12DDI_TL_ROW_MAJOR, nested_address) == 0);
        table.pfnDestroyHeapAndResource(device, {}, nested);
        REQUIRE(kernel_heaps.empty() && !ctx->native_heaps && deallocations == releases + 1);
        for (size_t i = sizeof(Object); i < cs.size(); ++i) REQUIRE(cs[i] == 0xa5);
    }
    // Parent/output slots and descriptors are runtime-owned. Retain the backend
    // before entering callbacks; a nested create of the reserved output fails.
    for (unsigned phase = 0; phase < 4; ++phase) {
        hs.fill(0xa5); ps.fill(0xa5); cs.fill(0xa5);
        REQUIRE(table.pfnCreateHeapAndResource(device, &heap, h, {}, &parent, nullptr, p) == S_OK);
        child = valid; allocation = native_heap_find(ctx, h.pDrvPrivate);
        const auto address = allocation->address;
        auto retire_parent = [&]() {
            if (create() != E_INVALIDARG) fixture_abort(__LINE__);
            if (table.pfnCreateHeapAndResource(device, nullptr, {}, {}, &child, nullptr,
                    {cs.data() + 8}) != E_INVALIDARG) fixture_abort(__LINE__);
            table.pfnDestroyHeapAndResource(device, h, p);
            hs.fill(0xcc); ps.fill(0xcc); std::memset(&child, 0xcc, sizeof(child));
        };
        if (phase == 0) texture_allocation_callback = retire_parent;
        else if (phase == 1) identity_query_callback = retire_parent;
        else if (phase == 2) buffer_import_callback = retire_parent;
        else buffer_placement_callback = retire_parent;
        REQUIRE(create() == S_OK);
        texture_allocation_callback = {};
        REQUIRE(object(c.pDrvPrivate)->address == address + 65536 && !native_heap_find(ctx, h.pDrvPrivate));
        table.pfnDestroyHeapAndResource(device, {}, c);
        REQUIRE(kernel_heaps.empty() && !ctx->native_heaps && !ctx->native_open_reservations);
    }
    puts("PASS native child buffers: one KMT allocation, exact nested GPUVA, invalid bounds/foreign handles, three failure rollbacks, 12 destruction permutations and four callback parent-retire/poison phases (controlled peers)");
    return 0;
}

static int test_native_buffer_child_cancel(Context *ctx, D3D12DDI_HDEVICE device,
        const D3D12DDI_DEVICE_FUNCS_CORE_0003 &table, unsigned phase, const std::function<void()> &terminate) {
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = 131072; heap.Alignment = 65536; heap.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    D3D12DDIARG_CREATERESOURCE_0003 desc{};
    desc.ResourceType = D3D12DDI_RT_BUFFER; desc.Width = heap.ByteSize;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12DDI_TL_ROW_MAJOR;
    desc.InitialResourceState = static_cast<D3D12DDI_RESOURCE_STATES>(0xac3);
    NativeHeapSlot storage{}; Object parent{};
    D3D12DDI_HHEAP h{&storage}; D3D12DDI_HRESOURCE p{&parent};
    REQUIRE(table.pfnCreateHeapAndResource(device, &heap, h, {}, &desc, nullptr, p) == S_OK);
    struct Page { D3D12DDIARG_CREATERESOURCE_0003 desc; Object output; };
    auto *page = static_cast<Page *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(page); std::memset(page, 0xa5, sizeof(*page)); page->desc = desc;
    page->desc.Width = 4096; page->desc.ReuseBufferGPUVA.BaseAddress.UMD = {p, 65536};
    unsigned closes_after = 0;
    auto cancel = [&]() {
        for (size_t i = 0; i < sizeof(page->output); ++i)
            if (reinterpret_cast<unsigned char *>(&page->output)[i] != 0xa5) fixture_abort(__LINE__);
        table.pfnDestroyHeapAndResource(device, h, p);
        terminate(); closes_after = deallocations;
        std::memset(&storage, 0xcc, sizeof(storage)); std::memset(&parent, 0xcc, sizeof(parent));
        if (!VirtualFree(page, 0, MEM_RELEASE)) fixture_abort(__LINE__);
    };
    if (phase == 0) texture_allocation_callback = cancel;
    else if (phase == 1) identity_query_callback = cancel;
    else if (phase == 2) buffer_import_callback = cancel;
    else buffer_placement_callback = cancel;
    REQUIRE(table.pfnCreateHeapAndResource(device, nullptr, {}, {}, &page->desc, nullptr,
        {&page->output}) == DXGI_ERROR_DEVICE_REMOVED);
    texture_allocation_callback = {};
    REQUIRE(ctx->native_retiring && !ctx->native_heaps && !ctx->resources && !ctx->native_open_reservations &&
        deallocations == closes_after && !buffer_import_callback && !buffer_placement_callback && !identity_query_callback);
    puts("PASS native child buffer cancellation: expired request/output page, parent-before-device teardown and no late callbacks/retained token (controlled peers)");
    return 0;
}

static int test_native_buffer_read_states(Context *ctx, D3D12DDI_HDEVICE device,
        const D3D12DDI_DEVICE_FUNCS_CORE_0003 &table) {
    const UINT states[] = {0xac3, 0, 1, 2, 0x40, 0x80, 0x200, 0x800, 0x400};
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = heap.Alignment = 65536; heap.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12DDIARG_CREATERESOURCE_0003 desc{};
    desc.ResourceType = D3D12DDI_RT_BUFFER; desc.Width = 4096;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12DDI_TL_ROW_MAJOR;
    alignas(NativeHeapSlot) std::array<unsigned char, sizeof(NativeHeapSlot) + 8> storage;
    alignas(Object) std::array<unsigned char, sizeof(Object) + 8> resource;
    D3D12DDI_HHEAP h{storage.data()}; D3D12DDI_HRESOURCE r{resource.data()};
    auto create = [&]() { return table.pfnCreateHeapAndResource(device, &heap, h, {}, &desc, nullptr, r); };
    for (unsigned mode = 0; mode < 4; ++mode) {
        const bool cached = !(mode & 1);
        heap.CPUPageProperty = cached ? D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK : D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        desc.Flags = mode & 2 ? D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE : D3D12DDI_RESOURCE_FLAG_0003_NONE;
        for (UINT state : states) {
            storage.fill(0xa5); resource.fill(0xa5);
            desc.InitialResourceState = static_cast<D3D12DDI_RESOURCE_STATES>(state);
            const unsigned before = allocations;
            auto sizes = table.pfnCalcPrivateHeapAndResourceSizes(device, &heap, &desc);
            REQUIRE(sizes.Heap == sizeof(NativeHeapSlot) && sizes.Resource == sizeof(Object));
            for (UINT invalid : {0x404u, 0xc00u, 4u, 0x10u, 0x1000u, 0x10000u}) {
                desc.InitialResourceState = static_cast<D3D12DDI_RESOURCE_STATES>(invalid);
                REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == before);
            }
            desc.InitialResourceState = static_cast<D3D12DDI_RESOURCE_STATES>(state);
            const auto valid_heap = heap; const auto valid_desc = desc;
            texture_allocation_callback = [&]() {
                std::memset(&heap, 0xcc, sizeof(heap)); std::memset(&desc, 0xcc, sizeof(desc));
            };
            REQUIRE(create() == S_OK && allocations == before + 1);
            texture_allocation_callback = {}; heap = valid_heap; desc = valid_desc;
            auto *token = native_heap_find(ctx, h.pDrvPrivate);
            auto *view = object(r.pDrvPrivate);
            REQUIRE(token && token->cpu_visible == cached && token->users == 2 &&
                    last_import_cpu_visible == cached && last_placement_state == state &&
                    view && view->kind == VKDU_BUFFER && view->address == token->address);
            REQUIRE(test_native_resource_queries(ctx, device, table, r, token,
                    D3D12DDI_TL_ROW_MAJOR, token->address) == 0);
            void *mapped = reinterpret_cast<void *>(uintptr_t(1));
            const auto map_result = table.pfnMapHeap(device, h, &mapped);
            REQUIRE(cached ? map_result == S_OK && mapped == mapped_heap.data() :
                    map_result == E_INVALIDARG && !mapped);
            if (cached) table.pfnUnmapHeap(device, h);
            for (size_t i = sizeof(NativeHeapSlot); i < storage.size(); ++i) REQUIRE(storage[i] == 0xa5);
            for (size_t i = sizeof(Object); i < resource.size(); ++i) REQUIRE(resource[i] == 0xa5);
            table.pfnDestroyHeapAndResource(device, h, {});
            REQUIRE(native_token(ctx, token) && token->users == 1 && kernel_heaps.size() == 1);
            table.pfnDestroyHeapAndResource(device, {}, r);
            REQUIRE(!ctx->native_heaps && kernel_heaps.empty());
        }
    }
    puts("PASS native buffer cached/default creation: 36 read/COPY_DEST states and SRV flag combinations, mapped permissions, poisoned inputs and retained resource backing (controlled peers)");
    return 0;
}
