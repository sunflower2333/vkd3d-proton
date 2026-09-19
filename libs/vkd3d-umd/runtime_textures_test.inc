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
    for (auto format : {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM}) {
        desc.Format = format; storage.fill(0xa5); resource.fill(0xa5);
        const unsigned before = allocations;
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
    puts("PASS native texture heap allocation/import, request poisoning, format ownership, alias lifetime and failure cleanup");
    return 0;
}
