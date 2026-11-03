/* SPDX-License-Identifier: LGPL-2.1-or-later */
static Context *sampler_context;
static Object *sampler_slot;
static D3D12DDI_SAMPLER_DESC *sampler_input;
static D3D12DDI_STATIC_SAMPLER *static_input;
static bool sampler_locks_free;
static void sampler_reentrant_destroy() {
    auto *ctx = sampler_context;
    sampler_locks_free = TryAcquireSRWLockExclusive(&ctx->resources_lock) != 0;
    if (sampler_locks_free) ReleaseSRWLockExclusive(&ctx->resources_lock);
    const auto depth = ctx->error_mutex.suspend(); ctx->error_mutex.resume(depth);
    sampler_locks_free = sampler_locks_free && !depth && ctx->native_backend_calls == 1;
    if (sampler_slot) {
        VioGpuD3D12BridgeUnbindObject(sampler_slot);
        std::memset(sampler_slot, 0xcc, sizeof(*sampler_slot));
    }
    if (sampler_input) std::memset(sampler_input, 0xcc, sizeof(*sampler_input));
    if (static_input) std::memset(static_input, 0xcc, sizeof(*static_input));
}
static void sampler_retire_context() {
    sampler_reentrant_destroy();
    sampler_context->native_retiring = true;
}
static int check_native_samplers() {
    Context ctx;
    ctx.backend = reinterpret_cast<vkdu_device *>(&next_base); ctx.report = capture;
    sampler_context = &ctx;
    D3D12DDI_HDEVICE h{&ctx};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queue) == S_OK && device.pfnCreateSampler);
    D3D12DDI_SAMPLER_DESC desc{};
    desc.Filter = D3D12DDI_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D12DDI_TEXTURE_ADDRESS_MODE_WRAP;
    desc.AddressV = D3D12DDI_TEXTURE_ADDRESS_MODE_MIRROR;
    desc.AddressW = D3D12DDI_TEXTURE_ADDRESS_MODE_BORDER;
    desc.MipLODBias = -2.5f; desc.MaxAnisotropy = 7; desc.ComparisonFunc = D3D12DDI_COMPARISON_FUNC_LESS_EQUAL;
    desc.BorderColor[0] = 0.125f; desc.BorderColor[1] = 0.25f; desc.BorderColor[2] = 0.5f; desc.BorderColor[3] = 0.75f;
    desc.MinLOD = -3; desc.MaxLOD = 15;
    D3D12DDIARG_CREATE_SAMPLER args{&desc};
    Object heap{}, wrong{}, root{}, rejected{};
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER, 8,
        D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
    REQUIRE(device.pfnCreateDescriptorHeap(h, &heap_args, {&heap}) == S_OK);
    heap_args.Type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    REQUIRE(device.pfnCreateDescriptorHeap(h, &heap_args, {&wrong}) == S_OK);
    D3D12DDI_CPU_DESCRIPTOR_HANDLE destination{static_cast<SIZE_T>(test_start(heap.backend, 0) + 5 * 32)};
    unsigned before = sampler_calls;
    device.pfnCreateSampler(h, &args, destination);
    REQUIRE(sampler_calls == before + 1 && sampler_index == 5 && reported == S_OK);
    REQUIRE(seen_sampler.filter == 0x15 && seen_sampler.address_u == 1 && seen_sampler.address_v == 2 &&
            seen_sampler.address_w == 4 && seen_sampler.mip_bias == -2.5f && seen_sampler.max_anisotropy == 7 &&
            seen_sampler.comparison == 4 && seen_sampler.min_lod == -3 && seen_sampler.max_lod == 15 &&
            !std::memcmp(seen_sampler.border, desc.BorderColor, sizeof(desc.BorderColor)));
    before = sampler_calls;
    device.pfnCreateSampler(h, nullptr, destination);
    REQUIRE(sampler_calls == before && reported == E_INVALIDARG);
    D3D12DDIARG_CREATE_SAMPLER empty{};
    device.pfnCreateSampler(h, &empty, destination);
    REQUIRE(sampler_calls == before && reported == E_INVALIDARG);
    device.pfnCreateSampler(h, &args, {destination.ptr + 1});
    REQUIRE(sampler_calls == before && reported == E_INVALIDARG);
    device.pfnCreateSampler(h, &args, {static_cast<SIZE_T>(test_start(wrong.backend, 0))});
    REQUIRE(sampler_calls == before && reported == E_INVALIDARG);
    auto saved_flags = heap.descriptor_flags; heap.descriptor_flags = 0;
    device.pfnCreateSampler(h, &args, destination);
    REQUIRE(sampler_calls == before && reported == E_INVALIDARG);
    heap.descriptor_flags = saved_flags;
    next_result = E_OUTOFMEMORY;
    device.pfnCreateSampler(h, &args, destination);
    REQUIRE(sampler_calls == before + 1 && reported == E_OUTOFMEMORY && peer(heap.backend)->references == 1);
    next_result = S_OK; reported = S_OK;
    sampler_input = &desc; sampler_slot = &heap; sampler_hook = sampler_reentrant_destroy;
    const auto destroys_before = destroys;
    device.pfnCreateSampler(h, &args, destination);
    REQUIRE(sampler_locks_free && destroys == destroys_before + 1 && seen_sampler.filter == 0x15 &&
            seen_sampler.border[0] == 0.125f && !ctx.native_backend_calls);
    sampler_hook = nullptr; sampler_slot = nullptr; sampler_input = nullptr;

    D3D12DDI_STATIC_SAMPLER stat{};
    stat.Filter = D3D12DDI_FILTER_MIN_MAG_MIP_LINEAR;
    stat.AddressU = D3D12DDI_TEXTURE_ADDRESS_MODE_BORDER;
    stat.AddressV = D3D12DDI_TEXTURE_ADDRESS_MODE_CLAMP;
    stat.AddressW = D3D12DDI_TEXTURE_ADDRESS_MODE_WRAP;
    stat.MipLODBias = 1.25f; stat.MaxAnisotropy = 4; stat.ComparisonFunc = D3D12DDI_COMPARISON_FUNC_ALWAYS;
    stat.BorderColor = D3D12DDI_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    stat.MinLOD = 2; stat.MaxLOD = 9; stat.ShaderRegister = 7; stat.RegisterSpace = 11;
    stat.ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_PIXEL;
    D3D12DDI_ROOT_SIGNATURE signature{};
    signature.NumStaticSamplers = 1; signature.pStaticSamplers = &stat;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args{&signature, 1};
    static_input = &stat; sampler_hook = sampler_reentrant_destroy;
    REQUIRE(device.pfnCreateRootSignature(h, &root_args, {&root}) == S_OK);
    REQUIRE(sampler_locks_free && static_count == 1 && seen_static.border_color == 2 &&
            seen_static.desc.filter == 0x15 && seen_static.desc.address_u == 4 && seen_static.desc.address_v == 3 &&
            seen_static.desc.address_w == 1 && seen_static.desc.mip_bias == 1.25f &&
            seen_static.desc.max_anisotropy == 4 && seen_static.desc.comparison == 8 &&
            seen_static.desc.min_lod == 2 && seen_static.desc.max_lod == 9 && seen_static.shader_register == 7 &&
            seen_static.register_space == 11 && seen_static.visibility == 5);
    sampler_hook = nullptr; static_input = nullptr;
    before = sampler_calls;
    signature.pStaticSamplers = nullptr;
    REQUIRE(device.pfnCreateRootSignature(h, &root_args, {&rejected}) == E_INVALIDARG && sampler_calls == before);
    signature.pStaticSamplers = &stat; signature.NumStaticSamplers = 2033;
    REQUIRE(device.pfnCreateRootSignature(h, &root_args, {&rejected}) == E_INVALIDARG && sampler_calls == before);
    signature.NumStaticSamplers = 1;
    sampler_hook = sampler_retire_context;
    REQUIRE(device.pfnCreateRootSignature(h, &root_args, {&rejected}) == DXGI_ERROR_DEVICE_REMOVED && !rejected.magic);
    sampler_hook = nullptr; ctx.native_retiring = false;
    device.pfnDestroyRootSignature(h, {&root}); device.pfnDestroyDescriptorHeap(h, {&wrong});
    REQUIRE(ctx.references == 1 && !ctx.descriptor_heaps && !ctx.native_backend_calls);
    ctx.backend = nullptr; sampler_context = nullptr;
    puts("PASS native dynamic/static sampler conversion, descriptor ownership, callback-unlocked reentrant destruction and root retirement");
    return 0;
}
