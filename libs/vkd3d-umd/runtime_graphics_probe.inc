/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../tests/shaders/render_target/headers/vs_flat_color.h"
#include "../../tests/shaders/render_target/headers/ps_flat_color.h"
const UINT *probe_graphics_tokens(const DWORD *code) {
    for (UINT i = 0; i < code[7]; ++i) {
        const DWORD *chunk = code + code[8 + i] / 4;
        if (chunk[0] == 0x58454853 || chunk[0] == 0x52444853) return chunk + 2;
    }
    throw ProbeFailure("graphics test shader has no tokens");
}
void check_shared_graphics(NativeSession &native, ProbeRuntime &runtime) {
    const auto h = native.create.hDrvDevice; auto *ctx = context(h);
    ProbeTexture target(native); const unsigned creates = runtime.creates;
    target.create(DXGI_FORMAT_R8G8B8A8_UNORM, true, 16, 12);
    auto *token = native_heap_find(ctx, target.heap.pDrvPrivate);
    require(token && token->allocation && token->users == 2 && runtime.creates == creates + 1,
        "one exact native allocation backs the placed render target");
    const auto allocation = token->allocation; runtime.target_handle = allocation;
    Owned alias, allocator, command, fence, indices;
    check(vkdu_render_target_heap_import(ctx->backend, ctx, token, token->bytes, &alias.value), "exact render-target alias import");
    require(runtime.creates == creates + 1, "render-target alias must not allocate replacement backing");
    NativeProbeQueue queue(native, runtime); queue.create();
    check(vkdu_allocator_create(ctx->backend, 0, &allocator.value), "graphics allocator");
    check(vkdu_command_create(ctx->backend, allocator.value, 0, &command.value), "graphics command");
    check(vkdu_fence_create(ctx->backend, 0, &fence.value), "graphics completion fence");
    check(vkdu_buffer_create(ctx->backend, 4096, 2, 0, 0xac3, &indices.value), "graphics index upload");
    ProbeBound cmd, index, root, vs, ps, blend, raster, depth, pso, rtv_heap;
    cmd.attach(ctx, command.value, VKDU_COMMAND_LIST); index.attach(ctx, indices.value, VKDU_BUFFER);
    D3D12DDI_ROOT_SIGNATURE signature{}; D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_desc{&signature, 1};
    check(native.table.pfnCreateRootSignature(h, &root_desc, {&root.slot}), "native graphics root");
    D3D12DDIARG_SIGNATURE_ENTRY input{}, output{};
    input.SystemValue = static_cast<D3D10_SB_NAME>(6); input.Mask = 1;
    input.RegisterComponentType = static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(1);
    output.SystemValue = static_cast<D3D10_SB_NAME>(1); output.Mask = 15;
    output.RegisterComponentType = static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(3);
    D3D12DDIARG_STAGE_IO_SIGNATURES io{};
    io.NumInputSignatureEntries = io.NumOutputSignatureEntries = 1;
    io.pInputSignatureDeprecated = &input; io.pOutputSignatureDeprecated = &output;
    native.table.pfnCreateVertexShader(h, probe_graphics_tokens(vs_flat_color_code_dxbc), {&root.slot}, {&vs.slot},
        &io, static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(0));
    io.NumInputSignatureEntries = 0; output.SystemValue = static_cast<D3D10_SB_NAME>(0);
    native.table.pfnCreatePixelShader(h, probe_graphics_tokens(ps_flat_color_code_dxbc), {&root.slot}, {&ps.slot},
        &io, static_cast<D3D12DDI_CREATE_SHADER_FLAGS>(0));
    check(VioGpuD3D12BridgeStatus(h), "native token-stream VS and PS");
    D3D12DDI_BLEND_DESC bs{}; bs.RenderTarget[0].RenderTargetWriteMask = 15;
    D3D12DDI_RASTERIZER_DESC rs{}; rs.FillMode = static_cast<D3D12DDI_FILL_MODE>(3);
    rs.CullMode = static_cast<D3D12DDI_CULL_MODE>(1); rs.ScissorEnable = rs.DepthClipEnable = TRUE;
    D3D12DDI_DEPTH_STENCIL_DESC ds{};
    native.table.pfnCreateBlendState(h, &bs, {&blend.slot}); native.table.pfnCreateRasterizerState(h, &rs, {&raster.slot});
    native.table.pfnCreateDepthStencilState(h, &ds, {&depth.slot});
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 desc{};
    desc.hRootSignature = {&root.slot}; desc.hVertexShader = {&vs.slot}; desc.hPixelShader = {&ps.slot};
    desc.hBlendState = {&blend.slot}; desc.hRasterizerState = {&raster.slot}; desc.hDepthStencilState = {&depth.slot};
    desc.hDrvPipelineState = {&pso.slot}; desc.NodeMask = 1;
    desc.PrimitiveTopologyType = D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = desc.SampleDesc.Count = 1; desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleMask = UINT32_MAX;
    check(native.table.pfnCreatePipelineState(h, &desc), "native graphics pipeline");
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_desc{D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV, 4,
        D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
    check(native.table.pfnCreateDescriptorHeap(h, &heap_desc, {&rtv_heap.slot}), "native RTV heap");
    auto handle = native.table.pfnGetCPUDescriptorHandleForHeapStart(h, {&rtv_heap.slot});
    handle.ptr += 2 * native.table.pfnGetDescriptorSizeInBytes(h, D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view{}; view.hDrvResource = target.resource;
    view.ResourceDimension = D3D12DDI_RD_TEXTURE2D; view.Format = DXGI_FORMAT_R8G8B8A8_UNORM; view.Tex2D.ArraySize = 1;
    native.table.pfnCreateRenderTargetView(h, &view, handle); check(VioGpuD3D12BridgeStatus(h), "native imported RTV");
    void *mapping = nullptr;
    check(vkdu_buffer_map(indices.value, 0, 0, &mapping), "map indices");
    auto *values = static_cast<uint16_t *>(mapping); values[0] = values[1] = 99; values[2] = 0; values[3] = 1; values[4] = 2;
    check(vkdu_buffer_unmap(indices.value, 0, 10), "unmap indices");
    for (UINT round = 0; round < 4; ++round) {
        const UINT sentinel = 0x71710000u ^ round;
        check(native.table.pfnMapHeap(h, native.heap, &mapping), "graphics readback sentinel");
        auto *words = static_cast<UINT *>(mapping); for (UINT i = 0; i < 16384; ++i) words[i] = sentinel;
        native.table.pfnUnmapHeap(h, native.heap);
        if (round) {
            check(vkdu_allocator_reset(allocator.value), "graphics allocator reset");
            check(vkdu_command_reset(command.value, allocator.value), "graphics command reset");
            check(vkdu_command_transition(command.value, target.resource_slot.backend, 0x800, 4), "render-target transition");
        }
        const D3D12DDI_HCOMMANDLIST c{&cmd.slot}; const float color[4] = {1, 0, 0, 1};
        const D3D12DDI_VIEWPORT viewport{0, 0, 16, 12, 0, 1};
        const D3D12DDI_RECT scissor{LONG(2 + round), LONG(1 + round), 12, 9};
        native.commands.pfnClearRenderTargetView(c, handle, color, 0, nullptr);
        native.commands.pfnSetGraphicsRootSignature(c, {&root.slot}); native.commands.pfnSetPipelineState(c, {&pso.slot});
        native.commands.pfnOMSetRenderTargets(c, 1, &handle, FALSE, nullptr);
        native.commands.pfnIaSetTopology(c, D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        native.commands.pfnRsSetViewports(c, 1, &viewport); native.commands.pfnRsSetScissorRects(c, 1, &scissor);
        if (round & 1) {
            const D3D12DDI_INDEX_BUFFER_VIEW ib{index.slot.address, 10, DXGI_FORMAT_R16_UINT};
            native.commands.pfnIASetIndexBuffer(c, &ib);
            native.commands.pfnDrawIndexedInstanced(c, round == 3 ? 0 : 3, 1, 2, 13, 7);
        } else native.commands.pfnDrawInstanced(c, 3, round == 2 ? 0 : 1, 0, 5);
        check(VioGpuD3D12BridgeStatus(h), "native graphics command recording");
        check(vkdu_command_transition(command.value, target.resource_slot.backend, 4, 0x800), "graphics readback transition");
        vkdu_texture_copy_location src{target.resource_slot.backend, 0, 0, 0, 0, 0, 0, 0};
        vkdu_texture_copy_location dst{object(native.resource.pDrvPrivate)->backend, 512, 1, 28, 16, 12, 1, 256};
        check(vkdu_command_texture_copy(command.value, &dst, 0, 0, 0, &src, nullptr), "native RT exact backing readback");
        const auto before = runtime.counts(queue.runtime.context);
        check(vkdu_command_close(command.value), "graphics command close");
        check(vkdu_queue_execute(queue.slot.backend, 1, &command.value), "graphics queue execute");
        require(runtime.counts(queue.runtime.context).targets > before.targets,
            "graphics Execute returns after actual target DMA reaches its runtime scheduler");
        check(vkdu_queue_signal(queue.slot.backend, fence.value, round + 1), "graphics queue signal");
        check(vkdu_fence_wait(fence.value, round + 1, 10000), "graphics GPU completion");
        require(runtime.counts(queue.runtime.context).events > before.events, "graphics ordered OS completion on runtime context");
        check(native.table.pfnMapHeap(h, native.heap, &mapping), "graphics full readback");
        words = static_cast<UINT *>(mapping); UINT mismatches = 0;
        for (UINT i = 0; i < 16384; ++i) {
            UINT expected = sentinel;
            if (i >= 128 && i < 896 && (i - 128) % 64 < 16) {
                const LONG x = (i - 128) % 64, y = (i - 128) / 64;
                expected = round < 2 && x >= scissor.left && x < scissor.right && y >= scissor.top && y < scissor.bottom ?
                    0xffffffffu : 0xff0000ffu;
            }
            if (words[i] != expected && mismatches++ < 4)
                std::fprintf(stderr, "native graphics round%u word%u=%08x expected%08x\n", round, i, words[i], expected);
        }
        native.table.pfnUnmapHeap(h, native.heap); require(!mismatches, "native graphics pixels and full sentinel preservation");
        std::printf("PASS native graphics round=%u allocation=%u runtime_context=%u checked_words=16384\n",
            round, allocation, queue.runtime.context);
    }
    cmd.reset(); command.reset(); allocator.reset(); fence.reset(); queue.reset();
    pso.reset(); vs.reset(); ps.reset(); blend.reset(); raster.reset(); depth.reset(); root.reset(); rtv_heap.reset();
    index.reset(); indices.reset();
    native.table.pfnDestroyHeapAndResource(h, target.heap, {}); target.heap = {};
    require(runtime.has(allocation), "render target retains destroyed native heap slot");
    native.table.pfnDestroyHeapAndResource(h, {}, target.resource); target.resource = {};
    require(runtime.has(allocation), "render-target alias retains exact allocation");
    alias.reset(); require(!runtime.has(allocation), "final RT import releases actual KMT allocation");
    check(VioGpuD3D12BridgeStatus(h), "graphics owner cleanup");
}
void run_graphics_probe(LUID luid) {
    Deadline deadline; ProbeRuntime runtime; runtime.open(luid);
    NativeSession native; native.initialize(runtime); native.create_readback();
    check_shared_graphics(native, runtime);
    runtime.native_cleanup = true; native.finish();
    require(!runtime.context_handle && runtime.contexts.empty() && runtime.allocations.empty(), "graphics all KMT resources retired");
    check(runtime.close(), "graphics final KMT owner cleanup");
    std::puts("PASS native graphics DDI and exact shared RT backing:4x16384 checked words");
    std::puts("BOUNDARY emulated runtime + real KMT/GPU, NOT ordinary D3D12CreateDevice or FL11_0 acceptance");
}
