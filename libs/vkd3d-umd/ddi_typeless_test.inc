/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int check_native_typeless_views() {
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    D3D12DDI_HDEVICE h{&ctx};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK);
    Object resource{}, rtv_heap{}, srv_heap{};
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap{D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV, 2,
        D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
    REQUIRE(device.pfnCreateDescriptorHeap(h, &heap, {&rtv_heap}) == S_OK);
    heap.Type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    REQUIRE(device.pfnCreateDescriptorHeap(h, &heap, {&srv_heap}) == S_OK);
    for (unsigned family = 0; family < 2; ++family) {
        const auto storage = family ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
        const DXGI_FORMAT formats[] = {family ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM,
            family ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
        REQUIRE(bind(&ctx, &resource, make_peer(ctx.backend, VKDU_TEXTURE2D, storage), VKDU_TEXTURE2D) == S_OK);
        D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 rtv{};
        rtv.hDrvResource = {&resource}; rtv.ResourceDimension = D3D12DDI_RD_TEXTURE2D; rtv.Tex2D.ArraySize = 1;
        D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 srv{};
        srv.hDrvResource = {&resource}; srv.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
        srv.Shader4ComponentMapping = 5768; srv.Tex2D.ArraySize = srv.Tex2D.MipLevels = 1;
        for (unsigned i = 0; i < 2; ++i) {
            rtv.Format = srv.Format = formats[i];
            const auto graphics = graphics_calls, samples = texture_srv_calls;
            device.pfnCreateRenderTargetView(h, &rtv, {static_cast<SIZE_T>(test_start(rtv_heap.backend, 0) + i * 32)});
            REQUIRE(graphics_calls == graphics + 1 && graphics_format == formats[i] && graphics_resource == resource.backend);
            device.pfnCreateShaderResourceView(h, &srv, {static_cast<SIZE_T>(test_start(srv_heap.backend, 0) + i * 32)});
            REQUIRE(texture_srv_calls == samples + 1 && texture_srv_format == formats[i] && srv_buffer == resource.backend);
        }
        for (auto bad : {DXGI_FORMAT_UNKNOWN, storage, DXGI_FORMAT_R32_FLOAT,
                family ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM}) {
            rtv.Format = srv.Format = bad;
            const auto graphics = graphics_calls, samples = texture_srv_calls;
            device.pfnCreateRenderTargetView(h, &rtv, {static_cast<SIZE_T>(test_start(rtv_heap.backend, 0))});
            REQUIRE(graphics_calls == graphics && reported == E_INVALIDARG);
            device.pfnCreateShaderResourceView(h, &srv, {static_cast<SIZE_T>(test_start(srv_heap.backend, 0))});
            REQUIRE(texture_srv_calls == samples && reported == E_INVALIDARG);
        }
        // R0 fully typed storage keeps its exact view type, unlike typeless.
        peer(resource.backend)->type = formats[0]; rtv.Format = srv.Format = formats[1];
        const auto graphics = graphics_calls, samples = texture_srv_calls;
        device.pfnCreateRenderTargetView(h, &rtv, {static_cast<SIZE_T>(test_start(rtv_heap.backend, 0))});
        device.pfnCreateShaderResourceView(h, &srv, {static_cast<SIZE_T>(test_start(srv_heap.backend, 0))});
        REQUIRE(graphics_calls == graphics && texture_srv_calls == samples && reported == E_INVALIDARG);
        VioGpuD3D12BridgeUnbindObject(&resource);
    }
    device.pfnDestroyDescriptorHeap(h, {&rtv_heap}); device.pfnDestroyDescriptorHeap(h, {&srv_heap});
    REQUIRE(ctx.references == 1 && !ctx.resources && !ctx.descriptor_heaps);
    ctx.backend = nullptr; reported = next_result = S_OK;
    puts("PASS native RGBA/BGRA typeless storage typed SRV/RTV translation, cross-family and typeless-view rejects, R0 typed-storage cast remains closed (controlled peers)");
    return 0;
}
