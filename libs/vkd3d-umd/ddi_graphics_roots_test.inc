/* SPDX-License-Identifier: LGPL-2.1-or-later */
static uint32_t *graphics_constants_input;
static void graphics_root_poison() {
    graphics_poison();
    if (graphics_constants_input) std::memset(graphics_constants_input, 0xcc, sizeof(uint32_t) * 3);
}
static int check_native_graphics_roots() {
    Context ctx, foreign; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    foreign.backend = reinterpret_cast<vkdu_device *>(&calls); foreign.report = capture;
    graphics_context = &ctx; next_result = reported = S_OK;
    D3D12DDI_HDEVICE h{&ctx};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queue) == S_OK);
    Object command{}, buffer{}, wrong{}, heap{};
    REQUIRE(bind(&ctx, &command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
    REQUIRE(bind(&ctx, &buffer, make_peer(ctx.backend, VKDU_BUFFER), VKDU_BUFFER) == S_OK);
    REQUIRE(bind(&foreign, &wrong, make_peer(foreign.backend, VKDU_BUFFER), VKDU_BUFFER) == S_OK);
    auto *buffer_peer = buffer.backend;
    commands.pfnSetGraphicsRootConstantBufferView({&command}, 7, buffer.address + 512);
    REQUIRE(graphics_root_kind == 2 && graphics_root_index == 7 && graphics_offset == 512 && graphics_resource == buffer_peer);
    commands.pfnSetGraphicsRootShaderResourceView({&command}, 9, buffer.address + 20);
    REQUIRE(graphics_root_kind == 3 && graphics_root_index == 9 && graphics_offset == 20);
    unsigned before = graphics_calls;
    commands.pfnSetGraphicsRootUnorderedAccessView({&command}, 3, wrong.address);
    REQUIRE(reported == E_INVALIDARG && graphics_calls == before);
    commands.pfnSetGraphicsRootShaderResourceView({&command}, 3, buffer.address + buffer.bytes);
    REQUIRE(reported == E_INVALIDARG && graphics_calls == before);
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 hd{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
        D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 1};
    REQUIRE(device.pfnCreateDescriptorHeap(h, &hd, {&heap}) == S_OK);
    const auto address = test_start(heap.backend, 1) + 5 * 32;
    commands.pfnSetGraphicsRootDescriptorTable({&command}, 11, {address});
    REQUIRE(graphics_root_kind == 0 && graphics_root_index == 11 && graphics_offset == 5 && graphics_resource == heap.backend);
    before = graphics_calls;
    commands.pfnSetGraphicsRootDescriptorTable({&command}, 11, {address + 1});
    REQUIRE(reported == E_INVALIDARG && graphics_calls == before);
    uint32_t values[3] = {0x12345678, 0x87654321, 0xabcdef01};
    graphics_constants_input = values; graphics_hook = graphics_root_poison;
    commands.pfnSetGraphicsRoot32BitConstants({&command}, 13, 3, values, 4);
    REQUIRE(graphics_locks_free && graphics_root_index == 13 && graphics_offset == 4 && graphics_root_count == 3 &&
        graphics_root_values[0] == 0x12345678 && graphics_root_values[2] == 0xabcdef01 && values[0] == 0xcccccccc);
    graphics_constants_input = nullptr; graphics_hook = nullptr;
    commands.pfnSetGraphicsRoot32BitConstant({&command}, 13, 0x31415926, 6);
    REQUIRE(graphics_root_values[0] == 0x31415926 && graphics_root_count == 1 && graphics_offset == 6);
    before = graphics_calls;
    commands.pfnSetGraphicsRoot32BitConstants({&command}, 13, 2, values, 63);
    REQUIRE(graphics_calls == before && reported == E_INVALIDARG);
    commands.pfnSetGraphicsRoot32BitConstants({&command}, 13, 1, nullptr, 0);
    REQUIRE(graphics_calls == before && reported == E_INVALIDARG);
    // Retained command/buffer/heap peers survive synchronous public-slot destruction.
    graphics_slots[0] = &buffer; graphics_hook = graphics_root_poison;
    commands.pfnSetGraphicsRootUnorderedAccessView({&command}, 17, buffer.address + 28);
    REQUIRE(graphics_locks_free && graphics_root_kind == 4 && graphics_root_index == 17 && graphics_offset == 28);
    graphics_slots[0] = &heap; graphics_slots[1] = &command;
    command.runtime_command.handle = reinterpret_cast<HANDLE>(0x2468);
    ctx.runtime_device.handle = &ctx; ctx.runtime_callbacks.pfnSetCommandListErrorCb = graphics_command_error;
    next_result = E_INVALIDARG; reported = S_OK;
    commands.pfnSetGraphicsRootDescriptorTable({&command}, 19, {address});
    REQUIRE(graphics_locks_free && graphics_root_index == 19 && graphics_offset == 5 &&
        reported == E_INVALIDARG && graphics_runtime_seen == reinterpret_cast<HANDLE>(0x2468));
    graphics_hook = nullptr; for (auto &slot : graphics_slots) slot = nullptr; next_result = S_OK;
    VioGpuD3D12BridgeUnbindObject(&wrong);
    REQUIRE(ctx.references == 1 && foreign.references == 1 && !ctx.resources && !ctx.descriptor_heaps);
    puts("PASS native graphics roots: CBV/SRV/UAV address offsets, GPU tables, copied constants, foreign bounds and recursive destruction");
    return 0;
}

static int check_native_buffer_alias_views() {
    Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    graphics_context = &ctx; graphics_hook = nullptr; next_result = reported = S_OK;
    NativeHeap allocation{}, foreign{};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queue) == S_OK);
    D3D12DDI_HDEVICE h{&ctx};
    Object parent{}, child{}, command{}, descriptors{};
    auto *large = make_peer(ctx.backend, VKDU_BUFFER); peer(large)->bytes = 16384;
    auto *small = make_peer(ctx.backend, VKDU_BUFFER); peer(small)->bytes = 256;
    peer(small)->cpu = peer(large)->cpu + 4096;
    const auto address = peer(small)->cpu;
    REQUIRE(bind(&ctx, &parent, large, VKDU_BUFFER, &allocation) == S_OK);
    REQUIRE(bind(&ctx, &child, small, VKDU_BUFFER) == E_INVALIDARG);
    REQUIRE(bind(&ctx, &child, small, VKDU_BUFFER, &foreign) == E_INVALIDARG);
    REQUIRE(bind(&ctx, &child, small, VKDU_BUFFER, &allocation) == S_OK);
    REQUIRE(bind(&ctx, &command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 hd{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
        D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
    REQUIRE(device.pfnCreateDescriptorHeap(h, &hd, {&descriptors}) == S_OK);
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE destination{static_cast<SIZE_T>(test_start(descriptors.backend, 0))};
    D3D12DDI_CONSTANT_BUFFER_VIEW_DESC cbv{address, 512, 0};
    device.pfnCreateConstantBufferView(h, &cbv, destination);
    REQUIRE(cbv_buffer == large && cbv_offset == 4096 && cbv_bytes == 512);
    commands.pfnSetComputeRootConstantBufferView({&command}, 3, address);
    REQUIRE(cbv_buffer == large && cbv_offset == 4096);
    commands.pfnSetComputeRootShaderResourceView({&command}, 3, address);
    REQUIRE(srv_buffer == large && srv_offset == 4096);
    commands.pfnSetGraphicsRootConstantBufferView({&command}, 3, address);
    REQUIRE(graphics_resource == large && graphics_offset == 4096);
    commands.pfnSetGraphicsRootShaderResourceView({&command}, 3, address);
    REQUIRE(graphics_resource == large && graphics_offset == 4096);
    commands.pfnSetGraphicsRootUnorderedAccessView({&command}, 3, address);
    REQUIRE(graphics_resource == large && graphics_offset == 4096);
    D3D12DDI_INDEX_BUFFER_VIEW ib{address, 1024, DXGI_FORMAT_R16_UINT};
    commands.pfnIASetIndexBuffer({&command}, &ib);
    REQUIRE(graphics_resource == large && graphics_offset == 4096 && graphics_size == 1024);
    D3D12DDI_VERTEX_BUFFER_VIEW vb{address, 1024, 16};
    commands.pfnIASetVertexBuffers({&command}, 2, 1, &vb);
    REQUIRE(graphics_vertex_buffers[0].buffer == large && graphics_vertex_buffers[0].offset == 4096);
    VioGpuD3D12BridgeUnbindObject(&parent);
    const auto before = calls;
    device.pfnCreateConstantBufferView(h, &cbv, destination);
    REQUIRE(calls == before && reported == E_INVALIDARG);
    cbv.SizeInBytes = 256; device.pfnCreateConstantBufferView(h, &cbv, destination);
    REQUIRE(cbv_buffer == small && !cbv_offset);
    commands.pfnSetGraphicsRootShaderResourceView({&command}, 3, address + 128);
    REQUIRE(graphics_resource == small && graphics_offset == 128);
    const auto graphics_before = graphics_calls;
    commands.pfnIASetIndexBuffer({&command}, &ib);
    commands.pfnIASetVertexBuffers({&command}, 2, 1, &vb);
    REQUIRE(graphics_calls == graphics_before && reported == E_INVALIDARG);
    VioGpuD3D12BridgeUnbindObject(&child); VioGpuD3D12BridgeUnbindObject(&command);
    VioGpuD3D12BridgeUnbindObject(&descriptors);
    REQUIRE(!ctx.resources && !ctx.descriptor_heaps && ctx.references == 1);
    puts("PASS overlapping buffer GPUVA: only same allocation aliases; full CBV/IBV/VBV ranges and widest compute/graphics roots, surviving child bounds after parent destruction");
    return 0;
}
