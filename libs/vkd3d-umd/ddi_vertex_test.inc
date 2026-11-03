/* SPDX-License-Identifier: LGPL-2.1-or-later */
static D3D12DDI_VERTEX_BUFFER_VIEW *vertex_request;
static void vertex_poison() {
    graphics_poison();
    if (!VirtualFree(vertex_request, 0, MEM_RELEASE)) std::abort();
    vertex_request = nullptr;
}
static int check_native_vertex_buffers() {
    Context ctx, foreign; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    foreign.backend = reinterpret_cast<vkdu_device *>(&calls); foreign.report = capture;
    graphics_context = &ctx; graphics_calls = 0; next_result = reported = S_OK;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK && commands.pfnIASetVertexBuffers);
    Object command{}, a{}, b{}, wrong{};
    REQUIRE(bind(&ctx, &command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
    REQUIRE(bind(&ctx, &a, make_peer(ctx.backend, VKDU_BUFFER), VKDU_BUFFER) == S_OK);
    REQUIRE(bind(&ctx, &b, make_peer(ctx.backend, VKDU_BUFFER), VKDU_BUFFER) == S_OK);
    REQUIRE(bind(&foreign, &wrong, make_peer(foreign.backend, VKDU_BUFFER), VKDU_BUFFER) == S_OK);
    D3D12DDI_VERTEX_BUFFER_VIEW views[3]{{a.address + 128, 320, 20}, {}, {b.address + 64, 128, 16}};
    commands.pfnIASetVertexBuffers({&command}, 5, 3, views);
    if (graphics_vertex_first != 5 || graphics_vertex_count != 3) {
        std::fputs("FAIL native vertex CORE_0003 field order\n", stderr); return 1;
    }
    REQUIRE(graphics_vertex_buffers[0].buffer == a.backend && graphics_vertex_buffers[0].offset == 128 &&
        graphics_vertex_buffers[0].size == 320 && graphics_vertex_buffers[0].stride == 20 &&
        !graphics_vertex_buffers[1].buffer && graphics_vertex_buffers[2].buffer == b.backend &&
        graphics_vertex_buffers[2].offset == 64);
    const auto calls_before = graphics_calls;
    const auto a_refs = peer(a.backend)->references, b_refs = peer(b.backend)->references;
    for (unsigned mode = 0; mode < 5; mode++) {
        auto invalid = views[2];
        if (mode == 0) invalid.BufferLocation = wrong.address;
        if (mode == 1) { invalid.BufferLocation = b.address + 4090; invalid.SizeInBytes = 16; }
        if (mode == 2) invalid.BufferLocation = UINT64_MAX - 3;
        if (mode == 3) invalid.StrideInBytes = 2049;
        if (mode == 4) invalid.BufferLocation = 0;
        views[2] = invalid; commands.pfnIASetVertexBuffers({&command}, 5, 3, views);
        REQUIRE(graphics_calls == calls_before && reported == E_INVALIDARG &&
            peer(a.backend)->references == a_refs && peer(b.backend)->references == b_refs);
        views[2] = {b.address + 64, 128, 16};
    }
    commands.pfnIASetVertexBuffers({&command}, 31, 3, views);
    REQUIRE(graphics_calls == calls_before && reported == E_INVALIDARG);
    auto *registry = ctx.resources; ctx.resources = nullptr;
    commands.pfnIASetVertexBuffers({&command}, 5, 3, views);
    REQUIRE(graphics_calls == calls_before && reported == E_INVALIDARG); ctx.resources = registry;
    commands.pfnIASetVertexBuffers({&command}, 5, 3, nullptr);
    REQUIRE(graphics_calls == calls_before + 1 && !graphics_vertex_buffers[0].buffer && !graphics_vertex_buffers[2].buffer);
    commands.pfnIASetVertexBuffers({&command}, 32, 0, nullptr);
    REQUIRE(graphics_vertex_first == 32 && !graphics_vertex_count);
    vertex_request = static_cast<D3D12DDI_VERTEX_BUFFER_VIEW *>(VirtualAlloc(nullptr, sizeof(views), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(vertex_request); std::memcpy(vertex_request, views, sizeof(views));
    command.runtime_command.handle = reinterpret_cast<HANDLE>(uintptr_t(0x553311));
    ctx.runtime_device.handle = &ctx; ctx.runtime_callbacks.pfnSetCommandListErrorCb = graphics_command_error;
    graphics_slots[0] = &command; graphics_slots[1] = &a; graphics_slots[2] = &b;
    graphics_hook = vertex_poison; next_result = E_INVALIDARG; graphics_runtime_seen = nullptr;
    auto *saved_a = a.backend, *saved_b = b.backend;
    commands.pfnIASetVertexBuffers({&command}, 5, 3, vertex_request);
    REQUIRE(!vertex_request && graphics_locks_free && reported == E_INVALIDARG &&
        graphics_runtime_seen == reinterpret_cast<HANDLE>(uintptr_t(0x553311)) &&
        graphics_vertex_buffers[0].buffer == saved_a && graphics_vertex_buffers[2].buffer == saved_b &&
        graphics_vertex_buffers[0].offset == 128 && graphics_vertex_buffers[2].stride == 16);
    graphics_hook = nullptr; for (auto &slot : graphics_slots) slot = nullptr;
    next_result = S_OK; VioGpuD3D12BridgeUnbindObject(&wrong);
    REQUIRE(ctx.references == 1 && foreign.references == 1 && !ctx.resources && !foreign.resources);
    puts("PASS native vertex buffers: CORE_0003 order, range/foreign ownership, atomic batch, copied freed request and recursive slot retirement");
    return 0;
}
