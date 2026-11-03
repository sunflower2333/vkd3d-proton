/* SPDX-License-Identifier: LGPL-2.1-or-later */
static D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW *depth_input;
static void depth_poison() {
    graphics_poison();
    if (depth_input) std::memset(depth_input, 0xcc, sizeof(*depth_input));
}
static int check_native_depth_format(DXGI_FORMAT format) {
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    graphics_context = &ctx; next_result = reported = S_OK;
    D3D12DDI_HDEVICE h{&ctx};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queue) == S_OK);
    Object command{}, texture{}, color{}, heap{};
    REQUIRE(bind(&ctx, &command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
    REQUIRE(bind(&ctx, &texture, make_peer(ctx.backend, VKDU_TEXTURE2D, format), VKDU_TEXTURE2D) == S_OK);
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 hd{D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV, 8,
        D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
    REQUIRE(device.pfnCreateDescriptorHeap(h, &hd, {&heap}) == S_OK);
    hd.Type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV;
    REQUIRE(device.pfnCreateDescriptorHeap(h, &hd, {&color}) == S_OK);
    D3D12DDI_CPU_DESCRIPTOR_HANDLE handle{static_cast<SIZE_T>(test_start(heap.backend, 0) + 3 * 32)};
    D3D12DDI_CPU_DESCRIPTOR_HANDLE color_handle{static_cast<SIZE_T>(test_start(color.backend, 0) + 5 * 32)};
    D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW request{};
    request.hDrvResource = {&texture}; request.Format = format;
    request.ResourceDimension = D3D12DDI_RD_TEXTURE2D; request.Tex2D.ArraySize = 1;
    request.Flags = D3D12DDI_CREATE_DSV_FLAG_READ_ONLY_DEPTH;
    device.pfnCreateDepthStencilView(h, &request, handle);
    REQUIRE(depth_resource == texture.backend && depth_index == 3 && depth_flags == 1);
    unsigned before = graphics_calls;
    device.pfnCreateDepthStencilView(h, &request, color_handle);
    REQUIRE(graphics_calls == before && reported == E_INVALIDARG);
    request.hDrvResource = {reinterpret_cast<void *>(1)};
    device.pfnCreateDepthStencilView(h, &request, handle);
    REQUIRE(graphics_calls == before && reported == E_INVALIDARG); request.hDrvResource = {&texture};
    request.Tex2D.ArraySize = 0; device.pfnCreateDepthStencilView(h, &request, handle);
    REQUIRE(graphics_calls == before && reported == E_INVALIDARG);
    request.Tex2D.MipSlice = 1; request.Tex2D.FirstArraySlice = 1; request.Tex2D.ArraySize = 2;
    request.Flags = D3D12DDI_CREATE_DSV_FLAG_NONE;
    graphics_slots[0] = &texture; graphics_hook = depth_poison; depth_input = &request;
    device.pfnCreateDepthStencilView(h, &request, handle);
    REQUIRE(graphics_locks_free && depth_index == 3 && depth_flags == 0 &&
        attachment_view_mip == 1 && attachment_view_first_layer == 1 && attachment_view_layers == 2);
    depth_input = nullptr; graphics_slots[0] = nullptr; graphics_hook = nullptr;
    commands.pfnOMSetRenderTargets({&command}, 1, &color_handle, FALSE, &handle);
    REQUIRE(graphics_resource == color.backend && graphics_index == 5 && depth_resource == heap.backend && depth_index == 3);
    before = graphics_calls;
    commands.pfnOMSetRenderTargets({&command}, 1, &color_handle, TRUE, &color_handle);
    REQUIRE(graphics_calls == before && reported == E_INVALIDARG);
    D3D12DDI_RECT rect{1, 2, 3, 4}; graphics_rect_input = &rect;
    graphics_slots[0] = &heap; graphics_slots[1] = &command; graphics_hook = depth_poison;
    command.runtime_command.handle = reinterpret_cast<HANDLE>(0x9753);
    ctx.runtime_device.handle = &ctx; ctx.runtime_callbacks.pfnSetCommandListErrorCb = graphics_command_error;
    next_result = E_INVALIDARG;
    commands.pfnClearDepthStencilView({&command}, handle, 1, 0.75f, 73, 1, &rect);
    REQUIRE(graphics_locks_free && depth_index == 3 && depth_flags == 1 && depth_value == 0.75f && depth_stencil == 73 &&
        graphics_size == 1 && graphics_rect.left == 1 && graphics_rect.bottom == 4 &&
        reported == E_INVALIDARG && graphics_runtime_seen == reinterpret_cast<HANDLE>(0x9753));
    graphics_hook = nullptr; graphics_rect_input = nullptr; for (auto &slot : graphics_slots) slot = nullptr; next_result = S_OK;
    VioGpuD3D12BridgeUnbindObject(&color);
    REQUIRE(ctx.references == 1 && !ctx.resources && !ctx.descriptor_heaps);
    printf("PASS native depth format%u DSV/OM/clear: typed live ownership, indices/flags/depth/rects and recursive request/slot retirement\n", unsigned(format));
    return 0;
}
static int check_native_depth() {
    REQUIRE(check_native_depth_format(DXGI_FORMAT_D32_FLOAT) == 0);
    REQUIRE(check_native_depth_format(DXGI_FORMAT_D16_UNORM) == 0);
    return 0;
}
