/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int check_native_root_unbind() {
    Context ctx, foreign; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    foreign.backend = reinterpret_cast<vkdu_device *>(&calls); foreign.report = capture;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK);
    const PFND3D12DDI_SET_ROOT_SIGNATURE setters[] = {commands.pfnSetGraphicsRootSignature, commands.pfnSetComputeRootSignature};
    graphics_context = &ctx;
    for (unsigned domain = 1; domain <= 2; ++domain) {
        const auto set = setters[domain - 1];
        Object command{}, root{}, wrong_kind{}, wrong_owner{}, retired{};
        REQUIRE(bind(&ctx, &command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
        REQUIRE(bind(&ctx, &root, make_peer(ctx.backend, VKDU_ROOT), VKDU_ROOT) == S_OK);
        REQUIRE(bind(&ctx, &wrong_kind, make_peer(ctx.backend, VKDU_ALLOCATOR), VKDU_ALLOCATOR) == S_OK);
        REQUIRE(bind(&foreign, &wrong_owner, make_peer(foreign.backend, VKDU_ROOT), VKDU_ROOT) == S_OK);
        REQUIRE(bind(&ctx, &retired, make_peer(ctx.backend, VKDU_ROOT), VKDU_ROOT) == S_OK);
        VioGpuD3D12BridgeUnbindObject(&retired);
        Object malformed = root; malformed.backend = nullptr;
        auto *root_peer = root.backend;
        next_result = reported = S_OK; graphics_hook = nullptr;
        const auto start = root_signature_calls;
        set({&command}, {&root}); REQUIRE(root_signature_calls == start + 1 && root_signature_seen == root_peer && reported == S_OK && root_signature_domain == domain);
        set({&command}, {}); REQUIRE(root_signature_calls == start + 2 && !root_signature_seen && reported == S_OK && root_signature_domain == domain);
        set({&command}, {}); REQUIRE(root_signature_calls == start + 3 && !root_signature_seen && reported == S_OK && root_signature_domain == domain);
        set({&command}, {&root}); REQUIRE(root_signature_calls == start + 4 && root_signature_seen == root_peer && reported == S_OK && root_signature_domain == domain);
        reject_retain_peer = root_peer; reported = S_OK; set({&command}, {&root}); reject_retain_peer = nullptr;
        REQUIRE(reported == E_INVALIDARG && root_signature_calls == start + 4 && peer(root_peer)->references == 1);
        for (auto *invalid : {&wrong_kind, &wrong_owner, &retired, &malformed}) {
            reported = S_OK; set({&command}, {invalid});
            REQUIRE(reported == E_INVALIDARG && root_signature_calls == start + 4);
        }
        ctx.native_retiring = true; set({&command}, {}); ctx.native_retiring = false;
        REQUIRE(root_signature_calls == start + 4);
        graphics_slots[0] = &root; graphics_slots[1] = &command;
        graphics_hook = graphics_poison; reported = S_OK; next_result = E_INVALIDARG;
        command.runtime_command.handle = reinterpret_cast<HANDLE>(0x712345);
        ctx.runtime_device.handle = &ctx; ctx.runtime_callbacks.pfnSetCommandListErrorCb = graphics_command_error;
        set({&command}, {&root});
        REQUIRE(graphics_locks_free && root_signature_calls == start + 5 && reported == E_INVALIDARG &&
            graphics_runtime_seen == reinterpret_cast<HANDLE>(0x712345));
        graphics_hook = nullptr; for (auto &slot : graphics_slots) slot = nullptr;
        command = {}; REQUIRE(bind(&ctx, &command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
        command.runtime_command.handle = reinterpret_cast<HANDLE>(0x798765);
        graphics_slots[0] = &command; graphics_hook = graphics_poison; reported = S_OK;
        set({&command}, {});
        REQUIRE(graphics_locks_free && root_signature_calls == start + 6 && !root_signature_seen &&
            reported == E_INVALIDARG && graphics_runtime_seen == reinterpret_cast<HANDLE>(0x798765));
        graphics_hook = nullptr; for (auto &slot : graphics_slots) slot = nullptr;
        VioGpuD3D12BridgeUnbindObject(&wrong_kind); VioGpuD3D12BridgeUnbindObject(&wrong_owner);
        REQUIRE(ctx.references == 1 && foreign.references == 1);
    }
    next_result = reported = S_OK;
    puts("PASS actual compute/graphics root DDIs: exact independent domains, NULL forwarded, non-NULL owner/kind/retired/malformed/retain rejection, locked lifetime retained, recursive unbind+slot poison, copied command-error identity");
    return 0;
}
