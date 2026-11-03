/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void format_retire() { graphics_poison(); graphics_context->native_retiring = true; }
static int check_native_formats() {
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    graphics_context = &ctx; next_result = reported = S_OK;
    D3D12DDI_HDEVICE h{&ctx};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queue) == S_OK);
    UINT out = 0x12345678;
    graphics_hook = graphics_poison;
    device.pfnCheckFormatSupport(h, DXGI_FORMAT_R8G8B8A8_UNORM, &out);
    REQUIRE(graphics_locks_free && format_seen == DXGI_FORMAT_R8G8B8A8_UNORM &&
        out == (D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE | D3D12DDI_FORMAT_SUPPORT_RENDERTARGET));
    device.pfnCheckMultisampleQualityLevels(h, DXGI_FORMAT_R32_FLOAT, 4,
        D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAG_TILED_RESOURCE, &out);
    REQUIRE(graphics_locks_free && format_seen == DXGI_FORMAT_R32_FLOAT && samples_seen == 4 && format_flags_seen == 1 && out == 1);
    next_result = E_INVALIDARG; out = 0x12345678;
    device.pfnCheckFormatSupport(h, DXGI_FORMAT_R8G8B8A8_UNORM, &out);
    REQUIRE(out == 0x12345678 && reported == E_INVALIDARG);
    device.pfnCheckMultisampleQualityLevels(h, DXGI_FORMAT_R32_FLOAT, 0,
        D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAG_NONE, &out);
    REQUIRE(out == 0x12345678 && reported == E_INVALIDARG);
    const auto before = format_calls;
    device.pfnCheckFormatSupport(h, DXGI_FORMAT_R32_FLOAT, nullptr);
    REQUIRE(format_calls == before && reported == E_INVALIDARG);
    graphics_hook = format_retire; next_result = S_OK;
    device.pfnCheckFormatSupport(h, DXGI_FORMAT_R32_FLOAT, &out);
    REQUIRE(graphics_locks_free && out == 0x12345678 && reported == DXGI_ERROR_DEVICE_REMOVED);
    ctx.native_retiring = false;
    graphics_hook = nullptr; next_result = S_OK;
    REQUIRE(ctx.references == 1);
    puts("PASS native format/MSAA WDK signatures, exact backend query and callback-unlocked atomic failure outputs");
    return 0;
}
