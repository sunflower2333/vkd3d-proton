/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* All callbacks copy caller storage and retain backend objects before dropping
 * the runtime callback lock. Graphics does not open native admission. */
Object *native_graphics_find(Context *ctx, void *slot, UINT tag) {
    for (auto *entry = ctx->graphics_objects; entry; entry = entry->next)
        if (entry == slot && entry->native_graphics_tag == tag) return entry;
    return nullptr;
}
HRESULT native_graphics_store(Context *ctx, void *slot, UINT tag, const void *data, size_t size) {
    if (!slot || object(slot) || !data || !size || size % 4 || size > UINT32_MAX) return E_INVALIDARG;
    auto *copy = new (std::nothrow) uint32_t[size / 4];
    if (!copy) return E_OUTOFMEMORY;
    std::memcpy(copy, data, size);
    auto *entry = new (slot) Object{object_magic, ctx, nullptr, VKDU_PIPELINE, copy, static_cast<UINT>(size / 4),
        ctx->graphics_objects, 0, 0};
    entry->native_graphics_tag = tag; ctx->graphics_objects = entry; ++ctx->references;
    return S_OK;
}
template<UINT Stage> void APIENTRY native_graphics_shader(D3D12DDI_HDEVICE h, const UINT *tokens,
        D3D12DDI_HROOTSIGNATURE root, D3D12DDI_HSHADER shader,
        const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures, D3D12DDI_CREATE_SHADER_FLAGS flags) {
    NativeHeapOperation op(context(h)); auto *ctx = op.ctx;
    if (!sampler_operation_live(op)) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    if (!tokens || !signatures || flags || !belongs(ctx, root.pDrvPrivate) || !backend(root.pDrvPrivate, VKDU_ROOT) ||
            signatures->NumInputSignatureEntries > 32 || signatures->NumOutputSignatureEntries > 32) {
        error(ctx, E_INVALIDARG); return;
    }
    const auto request = *signatures;
    vkdu_signature inputs[32]{}, outputs[32]{};
    const auto translate = [](const D3D12DDIARG_SIGNATURE_ENTRY *src, UINT count, vkdu_signature *dst) {
        if (count && !src) return false;
        for (UINT i = 0; i < count; ++i) {
            if (src[i].MinPrecision != D3D11_SB_OPERAND_MIN_PRECISION_DEFAULT) return false;
            dst[i] = {static_cast<UINT>(src[i].SystemValue), src[i].Register, src[i].Mask,
                static_cast<UINT>(src[i].RegisterComponentType)};
        }
        return true;
    };
    if (!translate(request.pInputSignatureDeprecated, request.NumInputSignatureEntries, inputs) ||
            !translate(request.pOutputSignatureDeprecated, request.NumOutputSignatureEntries, outputs)) {
        error(ctx, E_INVALIDARG); return;
    }
    void *code = nullptr; size_t bytes = 0;
    HRESULT hr = vkdu_shader_dxbc(Stage, tokens, tokens[1], inputs, request.NumInputSignatureEntries,
            outputs, request.NumOutputSignatureEntries, &code, &bytes);
    if (SUCCEEDED(hr)) hr = native_graphics_store(ctx, shader.pDrvPrivate, Stage == 1 ? 1 : 2, code, bytes);
    vkdu_shader_dxbc_free(code); error(ctx, hr);
}
template<class Desc> SIZE_T APIENTRY native_graphics_state_size(D3D12DDI_HDEVICE, const Desc *) { return sizeof(Object); }
template<class Desc, UINT Tag, class Handle> void APIENTRY native_graphics_state_create(D3D12DDI_HDEVICE h, const Desc *desc, Handle slot) {
    NativeHeapOperation op(context(h));
    if (!sampler_operation_live(op)) { error(op.ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    error(op.ctx, desc ? native_graphics_store(op.ctx, slot.pDrvPrivate, Tag, desc, sizeof(*desc)) : E_INVALIDARG);
}
template<UINT Tag, class Handle> void APIENTRY native_graphics_state_destroy(D3D12DDI_HDEVICE h, Handle slot) {
    NativeHeapOperation op(context(h));
    if (!op.ctx) return;
    if (!native_graphics_find(op.ctx, slot.pDrvPrivate, Tag)) { error(op.ctx, E_INVALIDARG); return; }
    VioGpuD3D12BridgeUnbindObject(slot.pDrvPrivate);
}
template<class Desc> bool native_graphics_state(Context *ctx, void *slot, UINT tag, Desc &out) {
    auto *value = native_graphics_find(ctx, slot, tag);
    if (!value || value->shader_words * 4 != sizeof(Desc)) return false;
    std::memcpy(&out, value->shader, sizeof(out)); return true;
}
HRESULT native_graphics_pipeline(Context *ctx, const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args) {
    NativeHeapOperation op(ctx);
    if (!sampler_operation_live(op)) return DXGI_ERROR_DEVICE_REMOVED;
    const auto request = *args;
    if (request.hComputeShader.pDrvPrivate || request.hHullShader.pDrvPrivate || request.hDomainShader.pDrvPrivate ||
            request.hGeometryShader.pDrvPrivate || request.hElementLayout.pDrvPrivate || request.IBStripCutValue ||
            request.PrimitiveTopologyType != D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ||
            request.NumRenderTargets != 1 || request.RTVFormats[0] != DXGI_FORMAT_R8G8B8A8_UNORM || request.DSVFormat ||
            request.SampleDesc.Count != 1 || request.SampleDesc.Quality) return DXGI_ERROR_UNSUPPORTED;
    for (UINT i = 1; i < 8; ++i) if (request.RTVFormats[i]) return E_INVALIDARG;
    auto *vs = native_graphics_find(ctx, request.hVertexShader.pDrvPrivate, 1);
    auto *ps = native_graphics_find(ctx, request.hPixelShader.pDrvPrivate, 2);
    D3D12DDI_BLEND_DESC blend{}; D3D12DDI_RASTERIZER_DESC raster{}; D3D12DDI_DEPTH_STENCIL_DESC depth{};
    if (!vs || !ps || !native_graphics_state(ctx, request.hBlendState.pDrvPrivate, 3, blend) ||
            !native_graphics_state(ctx, request.hRasterizerState.pDrvPrivate, 4, raster) ||
            !native_graphics_state(ctx, request.hDepthStencilState.pDrvPrivate, 5, depth)) return E_INVALIDARG;
    if (blend.AlphaToCoverageEnable || blend.IndependentBlendEnable || blend.RenderTarget[0].BlendEnable ||
            blend.RenderTarget[0].LogicOpEnable || depth.DepthEnable || depth.StencilEnable ||
            raster.DepthBias || raster.DepthBiasClamp != 0 || raster.SlopeScaledDepthBias != 0 ||
            !raster.ScissorEnable || raster.MultisampleEnable || raster.AntialiasedLineEnable ||
            raster.ForcedSampleCount || raster.ConservativeRasterizationMode) return DXGI_ERROR_UNSUPPORTED;
    vkdu_graphics_desc desc{static_cast<UINT>(request.RTVFormats[0]), request.SampleMask,
        static_cast<UINT>(raster.FillMode), static_cast<UINT>(raster.CullMode), static_cast<UINT>(raster.FrontCounterClockwise),
        static_cast<UINT>(raster.DepthClipEnable), blend.RenderTarget[0].RenderTargetWriteMask};
    // Runtime shader/state slots can be destroyed recursively inside Vulkan.
    std::unique_ptr<uint32_t[]> code(new (std::nothrow) uint32_t[vs->shader_words + ps->shader_words]);
    if (!code) return E_OUTOFMEMORY;
    const size_t vs_bytes = size_t(vs->shader_words) * 4, ps_bytes = size_t(ps->shader_words) * 4;
    std::memcpy(code.get(), vs->shader, vs_bytes); std::memcpy(reinterpret_cast<char *>(code.get()) + vs_bytes, ps->shader, ps_bytes);
    auto *root = backend(request.hRootSignature.pDrvPrivate, VKDU_ROOT);
    if (!root || !vkdu_object_retain(root)) return E_INVALIDARG;
    vkdu_object *value = nullptr; HRESULT hr;
    {
        NativeBackendCall call(ctx);
        hr = vkdu_graphics_pipeline_create(ctx->backend, root, code.get(), vs_bytes,
                reinterpret_cast<char *>(code.get()) + vs_bytes, ps_bytes, &desc, &value);
        vkdu_object_destroy(root);
    }
    if (!sampler_operation_live(op)) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) hr = bind(ctx, request.hDrvPipelineState.pDrvPrivate, value, VKDU_PIPELINE);
    if (FAILED(hr)) { NativeBackendCall call(ctx); vkdu_object_destroy(value); }
    return hr;
}

struct NativeGraphicsCommand {
    NativeHeapOperation op;
    Object error_owner{};
    vkdu_object *value = nullptr;
    explicit NativeGraphicsCommand(D3D12DDI_HCOMMANDLIST h)
        : op(object(h.pDrvPrivate) ? object(h.pDrvPrivate)->context : nullptr) {
        auto *slot = object(h.pDrvPrivate);
        if (!slot || slot->kind != VKDU_COMMAND_LIST || !sampler_operation_live(op)) return;
        error_owner.context = op.ctx; error_owner.kind = VKDU_COMMAND_LIST; error_owner.runtime_command = slot->runtime_command;
        if (vkdu_object_retain(slot->backend)) value = slot->backend;
    }
    ~NativeGraphicsCommand() {
        if (op.ctx && value) { NativeBackendCall call(op.ctx); vkdu_object_destroy(value); }
    }
    void report(HRESULT hr) {
        if (!sampler_operation_live(op)) hr = DXGI_ERROR_DEVICE_REMOVED;
        if (error_owner.context) error(&error_owner, hr);
    }
    template<class F> void run(F callback) {
        if (!value) { report(E_INVALIDARG); return; }
        HRESULT hr;
        { NativeBackendCall call(op.ctx); hr = callback(); }
        report(hr);
    }
};
vkdu_object *native_graphics_rtv_heap(Context *ctx, D3D12DDI_CPU_DESCRIPTOR_HANDLE handle, uint32_t &index) {
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *heap = find_heap(ctx, handle.ptr, false, &index);
    auto *value = heap && heap->descriptor_type == D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV &&
        (heap->descriptor_flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE) &&
        vkdu_object_retain(heap->backend) ? heap->backend : nullptr;
    ReleaseSRWLockShared(&ctx->resources_lock); return value;
}
void APIENTRY native_create_rtv(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 *args,
        D3D12DDI_CPU_DESCRIPTOR_HANDLE handle) {
    NativeHeapOperation op(context(h)); auto *ctx = op.ctx;
    if (!sampler_operation_live(op)) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    if (!args || args->ResourceDimension != D3D12DDI_RD_TEXTURE2D || args->Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            args->Tex2D.MipSlice || args->Tex2D.FirstArraySlice || args->Tex2D.ArraySize != 1 || args->Tex2D.PlaneSlice) {
        error(ctx, E_INVALIDARG); return;
    }
    const auto request = *args; uint32_t index = 0;
    auto *heap = native_graphics_rtv_heap(ctx, handle, index); vkdu_object *texture = nullptr;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *resource = find_resource(ctx, request.hDrvResource.pDrvPrivate);
    if (resource && resource->kind == VKDU_TEXTURE2D && vkdu_texture2d_format(resource->backend) == request.Format &&
            vkdu_object_retain(resource->backend)) texture = resource->backend;
    ReleaseSRWLockShared(&ctx->resources_lock);
    HRESULT hr;
    { NativeBackendCall call(ctx); hr = heap && texture ? vkdu_texture2d_rtv(heap, index, texture) : E_INVALIDARG;
        vkdu_object_destroy(heap); vkdu_object_destroy(texture); }
    if (!sampler_operation_live(op)) hr = DXGI_ERROR_DEVICE_REMOVED;
    error(ctx, hr);
}
void APIENTRY native_graphics_root(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HROOTSIGNATURE root) {
    NativeGraphicsCommand command(h); if (!command.value) return;
    auto *value = belongs(command.op.ctx, root.pDrvPrivate) ? backend(root.pDrvPrivate, VKDU_ROOT) : nullptr;
    if (!value || !vkdu_object_retain(value)) { command.report(E_INVALIDARG); return; }
    command.run([&] { auto hr = vkdu_command_graphics_root(command.value, value); vkdu_object_destroy(value); return hr; });
}
void APIENTRY native_draw(D3D12DDI_HCOMMANDLIST h, UINT vertices, UINT instances, UINT first, UINT first_instance) {
    NativeGraphicsCommand command(h); command.run([&] { return vkdu_command_draw(command.value, vertices, instances, first, first_instance); });
}
void APIENTRY native_draw_indexed(D3D12DDI_HCOMMANDLIST h, UINT indices, UINT instances, UINT first, INT base, UINT first_instance) {
    NativeGraphicsCommand command(h); command.run([&] { return vkdu_command_draw_indexed(command.value, indices, instances, first, base, first_instance); });
}
void APIENTRY native_topology(D3D12DDI_HCOMMANDLIST h, D3D12DDI_PRIMITIVE_TOPOLOGY topology) {
    NativeGraphicsCommand command(h); command.run([&] { return vkdu_command_topology(command.value, topology); });
}
void APIENTRY native_viewports(D3D12DDI_HCOMMANDLIST h, UINT count, const D3D12DDI_VIEWPORT *views) {
    NativeGraphicsCommand command(h); vkdu_viewport copy[16]{};
    if (count > 16 || (count && !views)) { command.report(E_INVALIDARG); return; }
    for (UINT i = 0; i < count; ++i) copy[i] = {views[i].TopLeftX, views[i].TopLeftY, views[i].Width, views[i].Height, views[i].MinDepth, views[i].MaxDepth};
    command.run([&] { return vkdu_command_viewports(command.value, count, copy); });
}
bool native_graphics_rects(UINT count, const D3D12DDI_RECT *rects, vkdu_rect *copy) {
    if (count > 16 || (count && !rects)) return false;
    for (UINT i = 0; i < count; ++i) copy[i] = {rects[i].left, rects[i].top, rects[i].right, rects[i].bottom};
    return true;
}
void APIENTRY native_scissors(D3D12DDI_HCOMMANDLIST h, UINT count, const D3D12DDI_RECT *rects) {
    NativeGraphicsCommand command(h); vkdu_rect copy[16]{};
    if (!native_graphics_rects(count, rects, copy)) { command.report(E_INVALIDARG); return; }
    command.run([&] { return vkdu_command_scissors(command.value, count, copy); });
}
void APIENTRY native_render_targets(D3D12DDI_HCOMMANDLIST h, UINT count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *rtvs,
        BOOL single_range, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *dsv) {
    NativeGraphicsCommand command(h); if (!command.value) return;
    if (count > 1 || (count && !rtvs) || (dsv && dsv->ptr) || (single_range != FALSE && single_range != TRUE)) {
        command.report(DXGI_ERROR_UNSUPPORTED); return;
    }
    uint32_t index = 0; auto *heap = count ? native_graphics_rtv_heap(command.op.ctx, *rtvs, index) : nullptr;
    if (count && !heap) { command.report(E_INVALIDARG); return; }
    command.run([&] { auto hr = vkdu_command_render_target(command.value, heap, index); vkdu_object_destroy(heap); return hr; });
}
void APIENTRY native_clear_rtv(D3D12DDI_HCOMMANDLIST h, D3D12DDI_CPU_DESCRIPTOR_HANDLE handle,
        const FLOAT color[4], UINT count, const D3D12DDI_RECT *rects) {
    NativeGraphicsCommand command(h); if (!command.value) return;
    vkdu_rect copy[16]{}; float rgba[4];
    if (!color || !native_graphics_rects(count, rects, copy)) { command.report(E_INVALIDARG); return; }
    std::memcpy(rgba, color, sizeof(rgba)); uint32_t index = 0;
    auto *heap = native_graphics_rtv_heap(command.op.ctx, handle, index);
    command.run([&] { auto hr = vkdu_command_clear_rtv(command.value, heap, index, rgba, count, copy); vkdu_object_destroy(heap); return hr; });
}
void APIENTRY native_index_buffer(D3D12DDI_HCOMMANDLIST h, const D3D12DDI_INDEX_BUFFER_VIEW *view) {
    NativeGraphicsCommand command(h); if (!command.value) return;
    const auto request = view ? *view : D3D12DDI_INDEX_BUFFER_VIEW{};
    vkdu_object *buffer = nullptr; uint64_t offset = 0;
    if (view) {
        auto *ctx = command.op.ctx;
        AcquireSRWLockShared(&ctx->resources_lock);
        for (auto *entry = ctx->resources; entry; entry = entry->next) {
            if (entry->kind == VKDU_BUFFER && request.BufferLocation >= entry->address &&
                    request.BufferLocation - entry->address < entry->bytes && vkdu_object_retain(entry->backend)) {
                buffer = entry->backend; offset = request.BufferLocation - entry->address; break;
            }
        }
        ReleaseSRWLockShared(&ctx->resources_lock);
        if (!buffer) { command.report(E_INVALIDARG); return; }
    }
    command.run([&] { auto hr = vkdu_command_index_buffer(command.value, buffer, offset, request.SizeInBytes, request.Format);
        vkdu_object_destroy(buffer); return hr; });
}
void native_graphics_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *device, D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *commands) {
    device->pfnCreateVertexShader = native_graphics_shader<1>; device->pfnCreatePixelShader = native_graphics_shader<0>;
    device->pfnCalcPrivateBlendStateSize = native_graphics_state_size<D3D12DDI_BLEND_DESC>;
    device->pfnCreateBlendState = native_graphics_state_create<D3D12DDI_BLEND_DESC, 3, D3D12DDI_HBLENDSTATE>;
    device->pfnDestroyBlendState = native_graphics_state_destroy<3, D3D12DDI_HBLENDSTATE>;
    device->pfnCalcPrivateRasterizerStateSize = native_graphics_state_size<D3D12DDI_RASTERIZER_DESC>;
    device->pfnCreateRasterizerState = native_graphics_state_create<D3D12DDI_RASTERIZER_DESC, 4, D3D12DDI_HRASTERIZERSTATE>;
    device->pfnDestroyRasterizerState = native_graphics_state_destroy<4, D3D12DDI_HRASTERIZERSTATE>;
    device->pfnCalcPrivateDepthStencilStateSize = native_graphics_state_size<D3D12DDI_DEPTH_STENCIL_DESC>;
    device->pfnCreateDepthStencilState = native_graphics_state_create<D3D12DDI_DEPTH_STENCIL_DESC, 5, D3D12DDI_HDEPTHSTENCILSTATE>;
    device->pfnDestroyDepthStencilState = native_graphics_state_destroy<5, D3D12DDI_HDEPTHSTENCILSTATE>;
    device->pfnCreateRenderTargetView = native_create_rtv;
    commands->pfnSetGraphicsRootSignature = native_graphics_root;
    commands->pfnDrawInstanced = native_draw; commands->pfnDrawIndexedInstanced = native_draw_indexed;
    commands->pfnIaSetTopology = native_topology; commands->pfnRsSetViewports = native_viewports;
    commands->pfnRsSetScissorRects = native_scissors; commands->pfnOMSetRenderTargets = native_render_targets;
    commands->pfnClearRenderTargetView = native_clear_rtv; commands->pfnIASetIndexBuffer = native_index_buffer;
}
