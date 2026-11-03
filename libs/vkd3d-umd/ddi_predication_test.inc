/* SPDX-License-Identifier: LGPL-2.1-or-later */
struct PredicationInputs { Object command{}, resource{}; };
static PredicationInputs *pred_inputs;
static Context *pred_context;
static unsigned pred_mode, pred_errors;
static bool pred_retired;
static void predication_retire() {
    auto *ctx = pred_context;
    if (!TryAcquireSRWLockExclusive(&ctx->resources_lock)) std::abort();
    ReleaseSRWLockExclusive(&ctx->resources_lock);
    const auto depth = ctx->error_mutex.suspend(); ctx->error_mutex.resume(depth);
    if ((pred_mode < 3 && (depth || ctx->native_backend_calls != 1)) ||
            (pred_mode >= 3 && ctx->native_backend_calls)) std::abort();
    for (auto *slot : {&pred_inputs->resource, &pred_inputs->command}) {
        if (pred_mode >= 3 && peer(slot->backend)->references != 1) std::abort();
        VioGpuD3D12BridgeUnbindObject(slot);
    }
    if (!VirtualFree(pred_inputs, 0, MEM_RELEASE)) std::abort();
    pred_inputs = nullptr; pred_retired = true;
    if (pred_mode == 2) ctx->native_retiring = true;
}
static void APIENTRY predication_command_error(D3D12DDI_HRTCOMMANDLIST runtime, HRESULT hr) {
    ++pred_errors; graphics_runtime_seen = runtime.handle; reported = hr;
    if (pred_mode >= 3) predication_retire();
}
static int check_native_predication() {
    for (unsigned mode = 0; mode < 5; ++mode) {
        Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
        ctx.runtime_device.handle = reinterpret_cast<HANDLE>(0x1234);
        ctx.runtime_callbacks.pfnSetCommandListErrorCb = predication_command_error;
        pred_context = &ctx; pred_mode = 0; pred_retired = false;
        next_result = reported = S_OK; graphics_hook = nullptr;
        D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
        REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK);
        pred_inputs = static_cast<PredicationInputs *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(pred_inputs); new (pred_inputs) PredicationInputs{};
        auto *p = pred_inputs;
        REQUIRE(bind(&ctx, &p->command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
        REQUIRE(bind(&ctx, &p->resource, make_peer(ctx.backend, VKDU_BUFFER), VKDU_BUFFER) == S_OK);
        p->command.runtime_command.handle = reinterpret_cast<HANDLE>(uintptr_t(0xabc450 + mode));
        const auto runtime = p->command.runtime_command.handle;
        D3D12DDI_HCOMMANDLIST command{&p->command}; D3D12DDI_HRESOURCE resource{&p->resource};
        const auto calls = predication_calls, errors = pred_errors;
        commands.pfnSetPredication(command, resource, 0x123456780ull, D3D12DDI_PREDICATION_OP_EQUAL_ZERO);
        REQUIRE(predication_calls == calls + 1 && predication_offset == 0x123456780ull && !predication_operation && !predication_disabled);
        commands.pfnSetPredication(command, {}, UINT64_MAX, D3D12DDI_PREDICATION_OP_NOT_EQUAL_ZERO);
        REQUIRE(predication_calls == calls + 2 && predication_offset == UINT64_MAX && predication_operation == 1 && predication_disabled);
        commands.pfnSetPredication(command, {reinterpret_cast<void *>(1)}, 0, D3D12DDI_PREDICATION_OP_EQUAL_ZERO);
        commands.pfnSetPredication(command, resource, 0, static_cast<D3D12DDI_PREDICATION_OP>(2));
        REQUIRE(predication_calls == calls + 2 && pred_errors == errors + 2 && reported == E_INVALIDARG);
        const auto before = pred_errors; pred_mode = mode;
        if (mode == 1 || mode == 2) graphics_hook = predication_retire;
        if (mode == 2 || mode == 3) next_result = E_OUTOFMEMORY;
        commands.pfnSetPredication(command, resource, 64, static_cast<D3D12DDI_PREDICATION_OP>(mode == 4 ? 2 : 1));
        graphics_hook = nullptr; next_result = S_OK;
        if (mode) {
            REQUIRE(pred_retired && !pred_inputs && !ctx.resources && ctx.references == 1);
            REQUIRE(pred_errors == before + (mode >= 3));
            if (mode >= 3) REQUIRE(graphics_runtime_seen == runtime && reported == (mode == 3 ? E_OUTOFMEMORY : E_INVALIDARG));
            else REQUIRE(predication_offset == 64 && predication_operation == 1);
        } else { pred_mode = 3; predication_retire(); REQUIRE(ctx.references == 1 && !ctx.resources); }
    }
    pred_mode = 0; pred_context = nullptr;
    puts("PASS actual SetPredication DDI: 64-bit offset, NULL disable, invalid handle/enum, unlocked reentrant VirtualFree, cancellation and pre-error borrow release (controlled peers)");
    return 0;
}
