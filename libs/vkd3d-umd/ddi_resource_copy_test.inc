/* SPDX-License-Identifier: LGPL-2.1-or-later */
static int check_native_resource_copy() {
    for (auto kind : {VKDU_BUFFER, VKDU_TEXTURE2D}) {
        Context ctx, foreign; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
        foreign.backend = reinterpret_cast<vkdu_device *>(&foreign);
        graphics_context = &ctx; next_result = reported = S_OK;
        D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
        REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queue) == S_OK && commands.pfnResourceCopy);
        Object command{}, dst{}, src{}, other{};
        REQUIRE(bind(&ctx, &command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
        REQUIRE(bind(&ctx, &dst, make_peer(ctx.backend, kind), kind) == S_OK);
        REQUIRE(bind(&ctx, &src, make_peer(ctx.backend, kind), kind) == S_OK);
        REQUIRE(bind(&foreign, &other, make_peer(foreign.backend, kind), kind) == S_OK);
        auto *expected_dst = dst.backend, *expected_src = src.backend;
        unsigned before = resource_copy_calls;
        commands.pfnResourceCopy({&command}, {&dst}, {&src});
        REQUIRE(resource_copy_calls == before + 1 && resource_copy_dst == expected_dst && resource_copy_src == expected_src);
        before = resource_copy_calls;
        commands.pfnResourceCopy({&command}, {&dst}, {&other});
        REQUIRE(resource_copy_calls == before && reported == E_INVALIDARG);
        commands.pfnResourceCopy({&command}, {&dst}, {reinterpret_cast<void *>(1)});
        REQUIRE(resource_copy_calls == before && reported == E_INVALIDARG);
        commands.pfnResourceCopy({&command}, {&src}, {&src});
        REQUIRE(resource_copy_calls == before && reported == E_INVALIDARG);
        commands.pfnResourceCopy({&command}, {&dst}, {&command});
        REQUIRE(resource_copy_calls == before && reported == E_INVALIDARG);
        graphics_slots[0] = &command; graphics_slots[1] = &dst; graphics_slots[2] = &src;
        graphics_hook = graphics_poison; next_result = E_FAIL;
        command.runtime_command.handle = reinterpret_cast<HANDLE>(0x4359);
        ctx.runtime_device.handle = &ctx; ctx.runtime_callbacks.pfnSetCommandListErrorCb = graphics_command_error;
        commands.pfnResourceCopy({&command}, {&dst}, {&src});
        REQUIRE(resource_copy_calls == before + 1 && resource_copy_dst == expected_dst && resource_copy_src == expected_src &&
            graphics_locks_free && reported == E_FAIL && graphics_runtime_seen == reinterpret_cast<HANDLE>(0x4359));
        graphics_hook = nullptr; for (auto &slot : graphics_slots) slot = nullptr; next_result = S_OK;
        VioGpuD3D12BridgeUnbindObject(&other);
        REQUIRE(ctx.references == 1 && foreign.references == 1 && !ctx.resources && !foreign.resources);
    }
    puts("PASS native ResourceCopy: buffer/image live registry, destination/source order, foreign/unknown/self rejection, unlocked callback and recursive slot retirement");
    return 0;
}
