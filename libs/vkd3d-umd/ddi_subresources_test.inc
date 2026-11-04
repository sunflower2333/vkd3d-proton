/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int check_native_subresources() {
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    D3D12DDI_HDEVICE h{&ctx}; next_result = reported = S_OK;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK);
    Object texture{}, command{}, heap{}, upload{};
    REQUIRE(bind(&ctx, &texture, make_peer(ctx.backend, VKDU_TEXTURE2D, DXGI_FORMAT_R32_FLOAT), VKDU_TEXTURE2D) == S_OK);
    REQUIRE(bind(&ctx, &command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
    REQUIRE(bind(&ctx, &upload, make_peer(ctx.backend, VKDU_BUFFER), VKDU_BUFFER) == S_OK);
    peer(texture.backend)->mips = 4; peer(texture.backend)->layers = 3;
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 hd{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
        D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
    REQUIRE(device.pfnCreateDescriptorHeap(h, &hd, {&heap}) == S_OK);
    const auto handle = device.pfnGetCPUDescriptorHandleForHeapStart(h, {&heap});
    D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 srv{};
    srv.hDrvResource = {&texture}; srv.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
    srv.Format = DXGI_FORMAT_R32_FLOAT; srv.Shader4ComponentMapping = 5768;
    srv.Tex2D = {1, 1, 2, 2, 0, 0};
    unsigned before = texture_srv_calls;
    device.pfnCreateShaderResourceView(h, &srv, handle);
    REQUIRE(texture_srv_calls == before + 1 && seen_texture_range[0] == 1 && seen_texture_range[1] == 2 &&
            seen_texture_range[2] == 1 && seen_texture_range[3] == 2);
    srv.Tex2D.MostDetailedMip = 4; device.pfnCreateShaderResourceView(h, &srv, handle);
    REQUIRE(texture_srv_calls == before + 1 && reported == E_INVALIDARG);
    D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
    dst.BaseAddress.UMD.hResource = {&texture}; dst.BaseAddress.UMD.Offset = 11;
    src.BaseAddress.UMD.hResource = {&upload}; src.BaseAddress.UMD.Offset = 512;
    D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT pitched{DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256, 256};
    D3D12DDIARG_PLACED_RESOURCE image{D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr};
    D3D12DDIARG_PLACED_RESOURCE linear{D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &pitched};
    before = texture_copy_calls;
    commands.pfnCopyTextureRegion({&command}, &dst, image, 0, 0, 0, &src, linear, nullptr);
    REQUIRE(texture_copy_calls == before + 1 && seen_texture_dst.offset == 11 && seen_texture_src.offset == 512);
    dst.BaseAddress.UMD.Offset = UINT64_C(1) << 32;
    commands.pfnCopyTextureRegion({&command}, &dst, image, 0, 0, 0, &src, linear, nullptr);
    REQUIRE(texture_copy_calls == before + 1 && reported == E_INVALIDARG);
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier{};
    barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.hResource = {&texture}; barrier.Transition.Subresource = 11;
    barrier.Transition.StateBefore = D3D12DDI_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    commands.pfnResourceBarrier({&command}, 1, &barrier);
    REQUIRE(seen_subresource_barrier.subresource_plus_one == 12 && seen_subresource_barrier.resource == texture.backend);
    barrier.Transition.Subresource = UINT_MAX;
    commands.pfnResourceBarrier({&command}, 1, &barrier);
    REQUIRE(!seen_subresource_barrier.subresource_plus_one);
    texture_context = &ctx; texture_slots[0] = &texture; texture_slots[1] = &command;
    texture_hook = texture_poison; barrier.Transition.Subresource = 5;
    commands.pfnResourceBarrier({&command}, 1, &barrier);
    texture_hook = nullptr; texture_slots[0] = texture_slots[1] = nullptr; texture_context = nullptr;
    REQUIRE(texture_locks_free && seen_subresource_barrier.subresource_plus_one == 6);
    VioGpuD3D12BridgeUnbindObject(&heap); VioGpuD3D12BridgeUnbindObject(&upload);
    REQUIRE(!ctx.resources && ctx.references == 1);
    ctx.backend = nullptr;
    puts("PASS actual native mip/layer SRV and SELECT_SUBRESOURCE/barrier field translation, 64-bit range rejection and unlocked callback teardown (controlled peers)");
    return 0;
}
