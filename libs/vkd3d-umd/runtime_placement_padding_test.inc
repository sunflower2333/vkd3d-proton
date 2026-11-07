/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Valid placement fields must not depend on ABI padding. In particular i386
 * has four bytes between the 32-bit resource handle and the 64-bit offset. */
static int test_native_placement_padding(Context *ctx, D3D12DDI_HDEVICE device,
        const D3D12DDI_DEVICE_FUNCS_CORE_0003 &table) {
    constexpr size_t padding_begin = sizeof(D3D12DDI_HRESOURCE);
    constexpr size_t padding_end = offsetof(D3D12DDIARG_HRESOURCE_PLACEMENT, Offset);
    static_assert(padding_begin <= padding_end, "placement handle precedes offset");
    if (sizeof(void *) == 4) REQUIRE(padding_end - padding_begin == 4);
    REQUIRE(!ctx->native_heaps && kernel_heaps.empty() && kernel_resources.empty());
    const auto saved_error = ctx->last_error.load();
    D3D12DDI_DEVICE_FUNCS_CORE_0108 later{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0108 later_commands{};
    d0108_tables(&later, &later_commands);
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = heap.Alignment = 65536; heap.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    D3D12DDIARG_CREATERESOURCE_0003 resource{};
    resource.ResourceType = D3D12DDI_RT_BUFFER; resource.Width = 4096;
    resource.Height = resource.DepthOrArraySize = resource.MipLevels = resource.SampleDesc.Count = 1;
    resource.Layout = D3D12DDI_TL_ROW_MAJOR; resource.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_DEST;
    D3D12DDIARG_CREATERESOURCE_0088 modern{};
    modern.ResourceType = resource.ResourceType; modern.Width = resource.Width;
    modern.Height = modern.DepthOrArraySize = modern.MipLevels = modern.SampleDesc.Count = 1;
    modern.Layout = resource.Layout; modern.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COPY_DEST;
    auto poison_empty = [](D3D12DDIARG_BUFFER_PLACEMENT &placement) {
        std::memset(&placement, 0xa5, sizeof(placement));
        placement.BaseAddress.UMD.hResource.pDrvPrivate = nullptr;
        placement.BaseAddress.UMD.Offset = 0;
    };
    poison_empty(resource.ReuseBufferGPUVA); poison_empty(modern.ReuseBufferGPUVA);
    alignas(NativeHeapSlot) std::array<unsigned char, sizeof(NativeHeapSlot) + 16> heap_storage;
    alignas(Object) std::array<unsigned char, sizeof(Object) + 16> resource_storage;
    const D3D12DDI_HHEAP h{heap_storage.data()};
    const D3D12DDI_HRESOURCE r{resource_storage.data()};
    for (unsigned revision = 0; revision != 2; ++revision) {
        auto &placement = revision ? modern.ReuseBufferGPUVA : resource.ReuseBufferGPUVA;
        auto padding_intact = [&]() {
            const auto *bytes = reinterpret_cast<const unsigned char *>(&placement);
            for (size_t i = padding_begin; i < padding_end; ++i) if (bytes[i] != 0xa5) return false;
            return true;
        };
        auto create = [&]() {
            return revision ? later.pfnCreateHeapAndResource(device, &heap, h, {}, &modern, nullptr, {}, r) :
                table.pfnCreateHeapAndResource(device, &heap, h, {}, &resource, nullptr, r);
        };
        struct QueryGuard {
            uint64_t before = 0x1122334455667788ull;
            D3D12DDI_RESOURCE_ALLOCATION_INFO r0{};
            D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 converted{};
            uint64_t after = 0x8877665544332211ull;
        } output;
        auto query = [&]() {
            if (revision) later.pfnCheckResourceAllocationInfo(device, &modern,
                D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &output.converted);
            else table.pfnCheckResourceAllocationInfo(device, &resource,
                D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &output.r0);
        };
        const auto allocated = allocations, freed = deallocations, queried = allocation_queries, errors = error_calls;
        REQUIRE(padding_intact() && !placement.BaseAddress.UMD.hResource.pDrvPrivate && !placement.BaseAddress.UMD.Offset);
        query();
        REQUIRE(error_calls == errors && allocation_queries == queried + 1 && allocations == allocated && padding_intact());
        if (revision) REQUIRE(output.converted.ResourceDataSize == 65536 && output.converted.ResourceDataAlignment == 65536 &&
            output.converted.Layout == D3D12DDI_TL_ROW_MAJOR && !output.converted.AdditionalDataHeaderSize &&
            !output.converted.AdditionalDataSize && output.converted.AdditionalDataHeaderAlignment == 1 &&
            output.converted.AdditionalDataAlignment == 1);
        else REQUIRE(output.r0.ResourceDataSize == 65536 && output.r0.ResourceDataAlignment == 65536 &&
            output.r0.Layout == D3D12DDI_TL_ROW_MAJOR);
        heap_storage.fill(0xa5); resource_storage.fill(0xa5);
        REQUIRE(create() == S_OK && allocations == allocated + 1 && padding_intact() &&
            object(r.pDrvPrivate)->native_heap == native_heap_find(ctx, h.pDrvPrivate));
        table.pfnDestroyHeapAndResource(device, h, r);
        REQUIRE(deallocations == freed + 1 && !ctx->native_heaps && kernel_heaps.empty() && kernel_resources.empty());
        for (size_t i = sizeof(NativeHeapSlot); i < heap_storage.size(); ++i) REQUIRE(heap_storage[i] == 0xa5);
        for (size_t i = sizeof(Object); i < resource_storage.size(); ++i) REQUIRE(resource_storage[i] == 0xa5);
        for (unsigned invalid = 0; invalid != 3; ++invalid) {
            placement.BaseAddress.UMD.hResource.pDrvPrivate = invalid ? nullptr : reinterpret_cast<void *>(uintptr_t(1));
            placement.BaseAddress.UMD.Offset = invalid == 1 ? 1 : invalid == 2 ? 65536 : 0;
            heap_storage.fill(0xa5); resource_storage.fill(0xa5);
            std::memset(&output.r0, 0xa5, sizeof(output.r0)); const auto sentinel = output.r0;
            const auto rejects = error_calls, query_count = allocation_queries, allocation_count = allocations;
            query();
            REQUIRE(error_calls == rejects + 1 && ctx->last_error.load() == DXGI_ERROR_UNSUPPORTED &&
                allocation_queries == query_count && allocations == allocation_count && padding_intact());
            if (revision) REQUIRE(!output.converted.ResourceDataSize && !output.converted.ResourceDataAlignment);
            else REQUIRE(!std::memcmp(&output.r0, &sentinel, sizeof(sentinel)));
            REQUIRE(create() == DXGI_ERROR_UNSUPPORTED && allocations == allocation_count && padding_intact());
            for (auto byte : heap_storage) REQUIRE(byte == 0xa5);
            for (auto byte : resource_storage) REQUIRE(byte == 0xa5);
            REQUIRE(!ctx->native_heaps && kernel_heaps.empty() && kernel_resources.empty());
        }
        placement.BaseAddress.UMD.hResource.pDrvPrivate = nullptr; placement.BaseAddress.UMD.Offset = 0;
        REQUIRE(output.before == 0x1122334455667788ull && output.after == 0x8877665544332211ull && padding_intact());
    }
    ctx->last_error = saved_error;
    std::printf("PASS PLACEMENT_PADDING actual_UMD=1 R0=1 DDI0108=1 poisoned_padding=%zu logical_zero_query_create=1 nonzero_handle_offset_rejected=1 canaries=1 GPU=0 SYSTEM=0\n",
        padding_end - padding_begin);
    return 0;
}
