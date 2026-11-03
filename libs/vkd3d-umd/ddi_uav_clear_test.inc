/* SPDX-License-Identifier: LGPL-2.1-or-later */
struct UavClearInputs {
    Object command{}, resource{}, gpu{}, cpu{};
    UINT values[4];
    D3D12DDI_RECT rects[19];
};
static UavClearInputs *uav_inputs;
static Context *uav_context;
static unsigned uav_retire_mode, uav_error_calls;
static bool uav_retired;
static void uav_clear_retire() {
    auto *ctx = uav_context;
    if (!TryAcquireSRWLockExclusive(&ctx->resources_lock)) std::abort();
    ReleaseSRWLockExclusive(&ctx->resources_lock);
    const auto depth = ctx->error_mutex.suspend(); ctx->error_mutex.resume(depth);
    if ((uav_retire_mode < 3 && (depth || ctx->native_backend_calls != 1)) ||
            (uav_retire_mode >= 3 && ctx->native_backend_calls)) std::abort();
    for (auto *slot : {&uav_inputs->cpu, &uav_inputs->gpu, &uav_inputs->resource, &uav_inputs->command}) {
        // Error notification follows release of every temporary backend borrow.
        if (uav_retire_mode >= 3 && peer(slot->backend)->references != 1) std::abort();
        VioGpuD3D12BridgeUnbindObject(slot);
    }
    if (!VirtualFree(uav_inputs, 0, MEM_RELEASE)) std::abort();
    uav_inputs = nullptr; uav_retired = true;
    if (uav_retire_mode == 2) ctx->native_retiring = true;
}
static void APIENTRY uav_clear_command_error(D3D12DDI_HRTCOMMANDLIST runtime, HRESULT hr) {
    ++uav_error_calls; graphics_runtime_seen = runtime.handle; reported = hr;
    if (uav_retire_mode >= 3) uav_clear_retire();
}
static int check_native_uav_clears() {
    static_assert(sizeof(UavClearInputs) <= 4096);
    for (unsigned mode = 0; mode < 5; ++mode) {
        Context ctx; ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
        ctx.runtime_device.handle = reinterpret_cast<HANDLE>(0x1234);
        ctx.runtime_callbacks.pfnSetCommandListErrorCb = uav_clear_command_error;
        uav_context = &ctx; uav_retire_mode = 0; uav_retired = false;
        next_result = reported = S_OK; graphics_hook = nullptr;
        D3D12DDI_HDEVICE h{&ctx};
        D3D12DDI_DEVICE_FUNCS_CORE_0003 device{}; D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
        REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queues) == S_OK);
        uav_inputs = static_cast<UavClearInputs *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(uav_inputs); new (uav_inputs) UavClearInputs{};
        auto *p = uav_inputs;
        REQUIRE(bind(&ctx, &p->command, make_peer(ctx.backend, VKDU_COMMAND_LIST), VKDU_COMMAND_LIST) == S_OK);
        REQUIRE(bind(&ctx, &p->resource, make_peer(ctx.backend, VKDU_BUFFER), VKDU_BUFFER) == S_OK);
        p->command.runtime_command.handle = reinterpret_cast<HANDLE>(uintptr_t(0x123450 + mode));
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 desc{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            8, D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
        REQUIRE(device.pfnCreateDescriptorHeap(h, &desc, {&p->cpu}) == S_OK);
        desc.Flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        REQUIRE(device.pfnCreateDescriptorHeap(h, &desc, {&p->gpu}) == S_OK);
        D3D12DDI_HCOMMANDLIST command{&p->command}; D3D12DDI_HRESOURCE resource{&p->resource};
        const auto runtime = p->command.runtime_command.handle;
        auto gpu = device.pfnGetGPUDescriptorHandleForHeapStart(h, {&p->gpu}); gpu.ptr += 5 * 32;
        auto cpu = device.pfnGetCPUDescriptorHandleForHeapStart(h, {&p->cpu}); cpu.ptr += 2 * 32;
        const UINT values[4] = {0xdead1234, 0x3f800000, 0x7fc12345, 0xff800000};
        std::memcpy(p->values, values, sizeof(values));
        for (unsigned i = 0; i < 19; ++i) p->rects[i] = {LONG(i - 7), -1, LONG(i + 7), 1};
        const auto calls = clear_uav_calls;
        commands.pfnClearUnorderedAccessViewUint(command, gpu, cpu, resource, p->values, 19, p->rects);
        REQUIRE(clear_uav_calls == calls + 1 && !clear_uav_floating && clear_uav_gpu_index == 5 &&
            clear_uav_cpu_index == 2 && clear_uav_count == 19 && !std::memcmp(clear_uav_values, values, sizeof(values)));
        REQUIRE(clear_uav_rects[18].left == 11 && clear_uav_rects[18].right == 25);
        commands.pfnClearUnorderedAccessViewFloat(command, gpu, cpu, resource,
            reinterpret_cast<float *>(p->values), 0, nullptr);
        REQUIRE(clear_uav_calls == calls + 2 && clear_uav_floating && !clear_uav_count &&
            !std::memcmp(clear_uav_values, values, sizeof(values)));
        const auto errors = uav_error_calls;
        auto invalid = cpu; ++invalid.ptr;
        commands.pfnClearUnorderedAccessViewUint(command, gpu, invalid, resource, p->values, 0, nullptr);
        commands.pfnClearUnorderedAccessViewUint(command, gpu, cpu, {reinterpret_cast<void *>(1)}, p->values, 0, nullptr);
        p->cpu.descriptor_flags |= D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        commands.pfnClearUnorderedAccessViewUint(command, gpu, cpu, resource, p->values, 0, nullptr);
        p->cpu.descriptor_flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
        p->gpu.descriptor_type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        commands.pfnClearUnorderedAccessViewUint(command, gpu, cpu, resource, p->values, 0, nullptr);
        p->gpu.descriptor_type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        REQUIRE(clear_uav_calls == calls + 2 && uav_error_calls == errors + 4 && reported == E_INVALIDARG);
        const auto before = uav_error_calls;
        uav_retire_mode = mode;
        if (mode == 1 || mode == 2) graphics_hook = uav_clear_retire;
        if (mode == 3) next_result = E_OUTOFMEMORY;
        commands.pfnClearUnorderedAccessViewUint(command, gpu, cpu, resource,
            mode == 4 ? nullptr : p->values, 19, p->rects);
        graphics_hook = nullptr; next_result = S_OK;
        if (mode) {
            REQUIRE(uav_retired && !uav_inputs && !ctx.resources && !ctx.descriptor_heaps && ctx.references == 1);
            REQUIRE(uav_error_calls == before + (mode >= 3));
            if (mode >= 3) REQUIRE(graphics_runtime_seen == runtime && reported == (mode == 3 ? E_OUTOFMEMORY : E_INVALIDARG));
            if (mode < 3) REQUIRE(clear_uav_values[0] == values[0] && clear_uav_rects[18].right == 25);
        } else {
            uav_retire_mode = 3; uav_clear_retire();
            REQUIRE(ctx.references == 1 && !ctx.resources && !ctx.descriptor_heaps);
        }
    }
    uav_retire_mode = 0; uav_context = nullptr;
    puts("PASS actual native Uint/Float UAV clear DDIs: 19-rect/value snapshots, CPU/GPU/resource lookup, unlocked retirement with VirtualFree, cancellation, exact command error identity and pre-error borrow release (controlled peers)");
    return 0;
}
