/* SPDX-License-Identifier: LGPL-2.1-or-later */
struct AttachmentClearInputs { Object command{}, heap{}; FLOAT color[4]; D3D12DDI_RECT rect; };
static AttachmentClearInputs *attachment_inputs;
static Context *attachment_context;
static unsigned attachment_mode, attachment_errors;
static bool attachment_retired;
static void attachment_retire() {
    auto *ctx = attachment_context;
    if (!TryAcquireSRWLockExclusive(&ctx->resources_lock)) std::abort();
    ReleaseSRWLockExclusive(&ctx->resources_lock);
    const auto depth = ctx->error_mutex.suspend(); ctx->error_mutex.resume(depth);
    if ((attachment_mode < 3 && (depth || ctx->native_backend_calls != 1)) ||
            (attachment_mode >= 3 && ctx->native_backend_calls)) std::abort();
    for (auto *slot : {&attachment_inputs->heap, &attachment_inputs->command}) {
        if (attachment_mode >= 3 && peer(slot->backend)->references != 1) {
            std::fprintf(stderr, "FAIL attachment error callback retains temporary owner kind=%u refs=%u\n",
                    slot->kind, peer(slot->backend)->references); std::exit(1);
        }
        VioGpuD3D12BridgeUnbindObject(slot);
    }
    if (!VirtualFree(attachment_inputs, 0, MEM_RELEASE)) std::abort();
    attachment_inputs = nullptr; attachment_retired = true;
    if (attachment_mode == 2) ctx->native_retiring = true;
}
static void APIENTRY attachment_command_error(D3D12DDI_HRTCOMMANDLIST runtime, HRESULT hr) {
    ++attachment_errors; graphics_runtime_seen = runtime.handle; reported = hr;
    if (attachment_mode >= 3) attachment_retire();
}
static int check_native_attachment_clears() {
    for (unsigned depth = 0; depth < 2; ++depth) for (unsigned mode = 0; mode < 5; ++mode) {
        Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
        ctx.runtime_device.handle = reinterpret_cast<HANDLE>(0x1234);
        ctx.runtime_callbacks.pfnSetCommandListErrorCb = attachment_command_error;
        attachment_context = &ctx; attachment_mode = 0; attachment_retired = false;
        next_result = reported = S_OK; graphics_hook = nullptr;
        D3D12DDI_HDEVICE h{&ctx};
        D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
        REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK);
        attachment_inputs = static_cast<AttachmentClearInputs *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(attachment_inputs); new (attachment_inputs) AttachmentClearInputs{};
        auto *p = attachment_inputs;
        REQUIRE(bind(&ctx, &p->command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
        p->command.runtime_command.handle = reinterpret_cast<HANDLE>(uintptr_t(0xc1ea80 + depth * 5 + mode));
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 desc{depth ? D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV : D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV,
            4, D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
        REQUIRE(device.pfnCreateDescriptorHeap(h, &desc, {&p->heap}) == S_OK);
        auto handle = device.pfnGetCPUDescriptorHandleForHeapStart(h, {&p->heap}); handle.ptr += 2 * 32;
        p->color[0] = 0.25f; p->color[1] = 0.5f; p->color[2] = 0.75f; p->color[3] = 1;
        p->rect = {1, 2, 4, 5};
        D3D12DDI_HCOMMANDLIST command{&p->command}; const auto runtime = p->command.runtime_command.handle;
        const auto calls = graphics_calls, errors = attachment_errors;
        if (depth) commands.pfnClearDepthStencilView(command, handle, 1, 0.5f, 0, 1, &p->rect);
        else commands.pfnClearRenderTargetView(command, handle, p->color, 1, &p->rect);
        REQUIRE(graphics_calls == calls + 1 && attachment_errors == errors && graphics_rect.right == 4);
        attachment_mode = mode;
        if (mode == 1 || mode == 2) graphics_hook = attachment_retire;
        if (mode == 2 || mode == 3) next_result = E_OUTOFMEMORY;
        if (depth) commands.pfnClearDepthStencilView(command, handle, 1, 0.75f, 0, mode == 4 ? 17 : 1, &p->rect);
        else commands.pfnClearRenderTargetView(command, handle, mode == 4 ? nullptr : p->color, 1, &p->rect);
        graphics_hook = nullptr; next_result = S_OK;
        if (mode) {
            REQUIRE(attachment_retired && !attachment_inputs && !ctx.resources && ctx.references == 1);
            REQUIRE(attachment_errors == errors + (mode >= 3));
            if (mode >= 3) REQUIRE(graphics_runtime_seen == runtime && reported == (mode == 3 ? E_OUTOFMEMORY : E_INVALIDARG));
            else REQUIRE(graphics_rect.top == 2 && graphics_rect.bottom == 5);
        } else { attachment_mode = 3; attachment_retire(); REQUIRE(!ctx.resources && ctx.references == 1); }
    }
    attachment_mode = 0; attachment_context = nullptr;
    puts("PASS actual RTV/DSV clear DDI: snapshotted caller data, unlocked VirtualFree reentry, cancellation and pre-error command/descriptor borrow release (controlled peers)");
    return 0;
}
