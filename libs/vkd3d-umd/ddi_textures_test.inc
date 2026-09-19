/* SPDX-License-Identifier: LGPL-2.1-or-later */
static Context *texture_context;
static Object *texture_slots[3];
static D3D12DDIARG_BUFFER_PLACEMENT *texture_placement;
static D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *texture_layout;
static D3D12DDI_BOX *texture_box_input;
static bool texture_locks_free;
static void texture_poison() {
    auto *ctx = texture_context;
    texture_locks_free = TryAcquireSRWLockExclusive(&ctx->resources_lock) != 0;
    if (texture_locks_free) ReleaseSRWLockExclusive(&ctx->resources_lock);
    const auto depth = ctx->error_mutex.suspend(); ctx->error_mutex.resume(depth);
    texture_locks_free = texture_locks_free && !depth && ctx->native_backend_calls == 1;
    for (auto *slot : texture_slots) if (slot) {
        VioGpuD3D12BridgeUnbindObject(slot); std::memset(slot, 0xcc, sizeof(*slot));
    }
    if (texture_placement) std::memset(texture_placement, 0xcc, sizeof(*texture_placement));
    if (texture_layout) std::memset(texture_layout, 0xcc, sizeof(*texture_layout));
    if (texture_box_input) std::memset(texture_box_input, 0xcc, sizeof(*texture_box_input));
}
static int check_native_textures() {
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    texture_context = &ctx; next_result = reported = S_OK;
    D3D12DDI_HDEVICE h{&ctx};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queue) == S_OK && commands.pfnCopyTextureRegion);
    Object texture{}, heap{}, upload{}, command{};
    REQUIRE(bind(&ctx, &texture, make_peer(ctx.backend, VKDU_TEXTURE2D, DXGI_FORMAT_R32_FLOAT), VKDU_TEXTURE2D) == S_OK);
    REQUIRE(!texture.address && !find_buffer(&ctx, &texture) && find_resource(&ctx, &texture) == &texture);
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
        D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
    REQUIRE(device.pfnCreateDescriptorHeap(h, &heap_args, {&heap}) == S_OK);
    D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 srv{};
    srv.hDrvResource = {&texture}; srv.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
    srv.Format = DXGI_FORMAT_R32_FLOAT; srv.Shader4ComponentMapping = 5768;
    srv.Tex2D.MipLevels = srv.Tex2D.ArraySize = 1;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE target{static_cast<SIZE_T>(test_start(heap.backend, 0) + 3 * 32)};
    device.pfnCreateShaderResourceView(h, &srv, target);
    REQUIRE(texture_srv_calls == 1 && srv_index == 3 && srv_buffer == texture.backend);
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device.pfnCreateShaderResourceView(h, &srv, target);
    REQUIRE(texture_srv_calls == 1 && reported == E_INVALIDARG); srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.Tex2D.MipLevels = 2; device.pfnCreateShaderResourceView(h, &srv, target);
    REQUIRE(texture_srv_calls == 1 && reported == E_INVALIDARG); srv.Tex2D.MipLevels = 1;
    auto saved_flags = heap.descriptor_flags; heap.descriptor_flags = 0;
    device.pfnCreateShaderResourceView(h, &srv, target);
    REQUIRE(texture_srv_calls == 1 && reported == E_INVALIDARG); heap.descriptor_flags = saved_flags;
    texture_slots[0] = &heap; texture_hook = texture_poison; reported = S_OK;
    device.pfnCreateShaderResourceView(h, &srv, target);
    REQUIRE(texture_locks_free && texture_srv_calls == 2 && peer(texture.backend)->references == 1);
    texture_hook = nullptr; texture_slots[0] = nullptr;
    REQUIRE(bind(&ctx, &upload, make_peer(ctx.backend, VKDU_BUFFER), VKDU_BUFFER) == S_OK);
    REQUIRE(bind(&ctx, &command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
    D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
    dst.BaseAddress.UMD.hResource = {&texture}; src.BaseAddress.UMD.hResource = {&upload}; src.BaseAddress.UMD.Offset = 512;
    D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint{};
    footprint.Format = DXGI_FORMAT_R32_FLOAT; footprint.PhysicalWidth = footprint.PhysicalHeight = 4;
    footprint.PhysicalDepth = 1; footprint.Pitch = 256; footprint.SlicePitch = 1024;
    D3D12DDIARG_PLACED_RESOURCE image{}, buffer{};
    image.Layout = D3D12DDI_RL_SELECT_SUBRESOURCE;
    buffer.Layout = D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED; buffer.pLayout = &footprint;
    D3D12DDI_BOX box{1, 1, 0, 3, 3, 1};
    commands.pfnCopyTextureRegion({&command}, &dst, image, 2, 2, 0, &src, buffer, &box);
    REQUIRE(texture_copy_calls == 1 && seen_texture_src.offset == 512 && seen_texture_src.row_pitch == 256 &&
            seen_texture_src.width == 4 && seen_texture_src.height == 4 && seen_texture_src.depth == 1 &&
            seen_texture_src.format == 41 && seen_texture_box.left == 1 && seen_texture_box.right == 3 &&
            texture_x == 2 && texture_y == 2 && !texture_z && !seen_texture_dst.footprint);
    footprint.SlicePitch = 1023;
    commands.pfnCopyTextureRegion({&command}, &dst, image, 2, 2, 0, &src, buffer, &box);
    REQUIRE(texture_copy_calls == 1 && reported == E_INVALIDARG); footprint.SlicePitch = 1024;
    dst.BaseAddress.UMD.Offset = 1;
    commands.pfnCopyTextureRegion({&command}, &dst, image, 2, 2, 0, &src, buffer, &box);
    REQUIRE(texture_copy_calls == 1 && reported == E_INVALIDARG); dst.BaseAddress.UMD.Offset = 0;
    box.Left = -1;
    commands.pfnCopyTextureRegion({&command}, &dst, image, 2, 2, 0, &src, buffer, &box);
    REQUIRE(texture_copy_calls == 1 && reported == E_INVALIDARG); box.Left = 1;
    texture_slots[0] = &texture; texture_slots[1] = &upload; texture_slots[2] = &command;
    texture_placement = &src; texture_layout = &footprint; texture_box_input = &box; texture_hook = texture_poison;
    const unsigned before_destroy = destroys; next_result = E_FAIL;
    commands.pfnCopyTextureRegion({&command}, &dst, image, 2, 2, 0, &src, buffer, &box);
    REQUIRE(texture_locks_free && destroys == before_destroy + 3 && texture_copy_calls == 2 &&
            seen_texture_src.offset == 512 && seen_texture_src.row_pitch == 256 && seen_texture_box.right == 3 && reported == E_FAIL);
    texture_hook = nullptr; texture_placement = nullptr; texture_layout = nullptr; texture_box_input = nullptr;
    for (auto &slot : texture_slots) slot = nullptr;
    REQUIRE(ctx.references == 1 && !ctx.resources && !ctx.descriptor_heaps && !ctx.native_backend_calls);
    ctx.backend = nullptr; texture_context = nullptr; next_result = reported = S_OK;
    puts("PASS native texture SRV/copy fields, ownership rejection and callback-unlocked source/command poisoning");
    return 0;
}
