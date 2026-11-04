/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int test_native_resource_queries(Context *ctx, D3D12DDI_HDEVICE device,
        const D3D12DDI_DEVICE_FUNCS_CORE_0003 &table, D3D12DDI_HRESOURCE resource,
        NativeHeap *heap, D3D12DDI_TEXTURE_LAYOUT layout, uint64_t address) {
    REQUIRE(table.pfnCheckExistingResourceAllocationInfo && table.pfnCheckResourceAllocationHandle &&
            table.pfnCheckResourceVirtualAddress);
    const auto saved_error = ctx->last_error.load();
    D3D12DDI_RESOURCE_ALLOCATION_INFO info;
    std::memset(&info, 0xa5, sizeof(info));
    D3DKMT_HANDLE allocation = 0;
    table.pfnCheckExistingResourceAllocationInfo(device, resource, &info, &allocation);
    REQUIRE(allocation == heap->allocation && info.ResourceDataSize == 65536 &&
            info.ResourceDataAlignment == 65536 && info.Layout == layout &&
            !info.AdditionalDataSize && !info.AdditionalDataHeaderSize);
    REQUIRE(table.pfnCheckResourceAllocationHandle(device, {resource.pDrvPrivate}) == heap->allocation);
    REQUIRE(table.pfnCheckResourceVirtualAddress(device, resource) == address);
    D3D12DDI_SUBRESOURCE_INFO subresource{};
    std::memset(&subresource, 0xa5, sizeof(subresource));
    const auto saved_subresource = subresource;
    table.pfnCheckSubresourceInfo(device, resource, 0, &subresource);
    if (layout == D3D12DDI_TL_ROW_MAJOR) {
        REQUIRE(!subresource.Offset && subresource.RowStride == object(resource.pDrvPrivate)->bytes &&
            subresource.DepthStride == subresource.RowStride && !subresource.RowBytePreSwizzleOffset &&
            !subresource.ColumnPreSwizzleOffset && !subresource.DepthPreSwizzleOffset);
    } else REQUIRE(!std::memcmp(&subresource, &saved_subresource, sizeof(subresource)));
    subresource = saved_subresource;
    table.pfnCheckSubresourceInfo(device, resource, 1, &subresource);
    REQUIRE(!std::memcmp(&subresource, &saved_subresource, sizeof(subresource)));
    auto reject = [&](D3D12DDI_HDEVICE owner, D3D12DDI_HRESOURCE handle) {
        std::memset(&info, 0xa5, sizeof(info));
        const auto sentinel = info;
        allocation = 0xa5a5a5a5;
        table.pfnCheckExistingResourceAllocationInfo(owner, handle, &info, &allocation);
        subresource = saved_subresource;
        table.pfnCheckSubresourceInfo(owner, handle, 0, &subresource);
        return allocation == 0xa5a5a5a5 && !std::memcmp(&info, &sentinel, sizeof(info)) &&
            !std::memcmp(&subresource, &saved_subresource, sizeof(subresource)) &&
            !table.pfnCheckResourceAllocationHandle(owner, {handle.pDrvPrivate}) &&
            !table.pfnCheckResourceVirtualAddress(owner, handle);
    };
    REQUIRE(reject(device, {}));
    REQUIRE(reject(device, {reinterpret_cast<void *>(uintptr_t(1))}));
    Context foreign;
    foreign.native_adapter = ctx->native_adapter;
    foreign.runtime_device = ctx->runtime_device;
    REQUIRE(reject({&foreign}, resource));
    heap->pending = true;
    REQUIRE(reject(device, resource));
    heap->pending = false;
    ++generation;
    REQUIRE(reject(device, resource));
    --generation;
    query_result = E_ACCESSDENIED;
    REQUIRE(reject(device, resource));
    query_result = S_OK;
    ctx->native_retiring = true;
    REQUIRE(reject(device, resource));
    ctx->native_retiring = false;
    ctx->last_error = saved_error;
    return 0;
}
