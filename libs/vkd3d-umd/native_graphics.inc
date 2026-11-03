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
    if (SUCCEEDED(hr)) hr = native_graphics_store(ctx, shader.pDrvPrivate, Stage == 1 ? 1 : Stage == 2 ? 7 : 2, code, bytes);
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
struct NativeInputLayout {
    UINT count;
    vkdu_input_element elements[32];
};
#include "native_shader_stages.inc"
#include "native_tessellation.inc"
void APIENTRY native_element_layout(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEELEMENTLAYOUT *desc,
        D3D12DDI_HELEMENTLAYOUT handle) {
    NativeHeapOperation op(context(h));
    if (!sampler_operation_live(op)) { error(op.ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    if (!desc || desc->NumElements > 32 || (desc->NumElements && !desc->pVertexElements)) {
        error(op.ctx, E_INVALIDARG); return;
    }
    NativeInputLayout copy{}; copy.count = desc->NumElements;
    for (UINT i = 0; i < copy.count; i++) {
        const auto &e = desc->pVertexElements[i];
        copy.elements[i] = {e.InputSlot, e.AlignedByteOffset, static_cast<UINT>(e.Format),
            static_cast<UINT>(e.InputSlotClass), e.InstanceDataStepRate, e.InputRegister};
    }
    HRESULT hr = vkdu_input_layout_validate(copy.count, copy.elements);
    if (SUCCEEDED(hr)) hr = native_graphics_store(op.ctx, handle.pDrvPrivate, 6, &copy, sizeof(copy));
    error(op.ctx, hr);
}
HRESULT native_graphics_pipeline(Context *ctx, const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args) {
    NativeHeapOperation op(ctx);
    if (!sampler_operation_live(op)) return DXGI_ERROR_DEVICE_REMOVED;
    const auto request = *args;
    if (request.NumRenderTargets > VKDU_MAX_RENDER_TARGETS) return E_INVALIDARG;
    if (request.hComputeShader.pDrvPrivate ||
            request.IBStripCutValue > D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF ||
            request.PrimitiveTopologyType < D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_POINT ||
            request.PrimitiveTopologyType > D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_PATCH ||
            (request.DSVFormat && !vkdu_depth_format(request.DSVFormat)) ||
            (request.SampleDesc.Count != 1 && request.SampleDesc.Count != 2 && request.SampleDesc.Count != 4) || request.SampleDesc.Quality) return DXGI_ERROR_UNSUPPORTED;
    if (request.SampleDesc.Count > 1 && (request.DSVFormat || !request.NumRenderTargets)) return DXGI_ERROR_UNSUPPORTED;
    for (UINT i = 0; i < VKDU_MAX_RENDER_TARGETS; ++i) {
        if (i < request.NumRenderTargets) {
            if (!vkdu_color_target_format(request.RTVFormats[i]) || (request.SampleDesc.Count > 1 &&
                    request.RTVFormats[i] != DXGI_FORMAT_R8G8B8A8_UNORM && request.RTVFormats[i] != DXGI_FORMAT_B8G8R8A8_UNORM))
                return DXGI_ERROR_UNSUPPORTED;
        } else if (request.RTVFormats[i]) return E_INVALIDARG;
    }
    auto *vs = native_graphics_find(ctx, request.hVertexShader.pDrvPrivate, 1);
    auto *ps = native_graphics_find(ctx, request.hPixelShader.pDrvPrivate, 2);
    auto *gs = native_graphics_find(ctx, request.hGeometryShader.pDrvPrivate, 7);
    auto *so = native_graphics_find(ctx, request.hGeometryShader.pDrvPrivate, 8);
    auto *hs = native_graphics_find(ctx, request.hHullShader.pDrvPrivate, 9);
    auto *ds = native_graphics_find(ctx, request.hDomainShader.pDrvPrivate, 10);
    D3D12DDI_BLEND_DESC blend{}; D3D12DDI_RASTERIZER_DESC raster{}; D3D12DDI_DEPTH_STENCIL_DESC depth{};
    if (!vs || (request.hPixelShader.pDrvPrivate && !ps) ||
            (request.hGeometryShader.pDrvPrivate && !gs && !so) ||
            (request.hHullShader.pDrvPrivate && !hs) || (request.hDomainShader.pDrvPrivate && !ds) ||
            (!hs != !ds) || (hs ? request.PrimitiveTopologyType != D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_PATCH :
                request.PrimitiveTopologyType == D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_PATCH) ||
            !native_graphics_state(ctx, request.hBlendState.pDrvPrivate, 3, blend) ||
            !native_graphics_state(ctx, request.hRasterizerState.pDrvPrivate, 4, raster) ||
            !native_graphics_state(ctx, request.hDepthStencilState.pDrvPrivate, 5, depth)) return E_INVALIDARG;
    if (blend.AlphaToCoverageEnable ||
            raster.DepthBias || raster.DepthBiasClamp != 0 || raster.SlopeScaledDepthBias != 0 ||
            !raster.ScissorEnable || raster.MultisampleEnable || raster.AntialiasedLineEnable ||
            raster.ForcedSampleCount || raster.ConservativeRasterizationMode) return DXGI_ERROR_UNSUPPORTED;
    if (blend.IndependentBlendEnable != FALSE && blend.IndependentBlendEnable != TRUE) return E_INVALIDARG;
    vkdu_render_target_desc targets[VKDU_MAX_RENDER_TARGETS]{};
    for (UINT i = 0; i < request.NumRenderTargets; ++i) {
        const auto &rt = blend.RenderTarget[blend.IndependentBlendEnable ? i : 0];
        if (rt.LogicOpEnable) return DXGI_ERROR_UNSUPPORTED;
        if ((rt.BlendEnable != FALSE && rt.BlendEnable != TRUE) || rt.RenderTargetWriteMask > 15) return E_INVALIDARG;
        targets[i] = {static_cast<UINT>(request.RTVFormats[i]), rt.RenderTargetWriteMask,
            static_cast<UINT>(rt.BlendEnable), static_cast<UINT>(rt.SrcBlend), static_cast<UINT>(rt.DestBlend),
            static_cast<UINT>(rt.BlendOp), static_cast<UINT>(rt.SrcBlendAlpha), static_cast<UINT>(rt.DestBlendAlpha),
            static_cast<UINT>(rt.BlendOpAlpha)};
    }
    vkdu_graphics_desc desc{static_cast<UINT>(request.RTVFormats[0]), request.SampleMask,
        static_cast<UINT>(raster.FillMode), static_cast<UINT>(raster.CullMode), static_cast<UINT>(raster.FrontCounterClockwise),
        static_cast<UINT>(raster.DepthClipEnable), blend.RenderTarget[0].RenderTargetWriteMask, 0, nullptr};
    desc.render_target_count = request.NumRenderTargets; desc.render_targets = targets;
    desc.depth_format = request.DSVFormat; desc.depth_enable = depth.DepthEnable;
    desc.topology_type = request.PrimitiveTopologyType; desc.strip_cut = request.IBStripCutValue;
    desc.depth_write = depth.DepthWriteMask; desc.depth_func = depth.DepthFunc;
    desc.stencil = {static_cast<UINT>(depth.StencilEnable), static_cast<UINT>(depth.FrontEnable),
        static_cast<UINT>(depth.BackEnable), depth.StencilReadMask, depth.StencilWriteMask,
        {static_cast<UINT>(depth.FrontFace.StencilFailOp), static_cast<UINT>(depth.FrontFace.StencilDepthFailOp),
         static_cast<UINT>(depth.FrontFace.StencilPassOp), static_cast<UINT>(depth.FrontFace.StencilFunc)},
        {static_cast<UINT>(depth.BackFace.StencilFailOp), static_cast<UINT>(depth.BackFace.StencilDepthFailOp),
         static_cast<UINT>(depth.BackFace.StencilPassOp), static_cast<UINT>(depth.BackFace.StencilFunc)}};
    desc.blend_enable = blend.RenderTarget[0].BlendEnable;
    desc.src_blend = blend.RenderTarget[0].SrcBlend; desc.dst_blend = blend.RenderTarget[0].DestBlend;
    desc.blend_op = blend.RenderTarget[0].BlendOp; desc.src_blend_alpha = blend.RenderTarget[0].SrcBlendAlpha;
    desc.dst_blend_alpha = blend.RenderTarget[0].DestBlendAlpha; desc.blend_op_alpha = blend.RenderTarget[0].BlendOpAlpha;
    NativeInputLayout layout{};
    if (request.hElementLayout.pDrvPrivate) {
        if (!native_graphics_state(ctx, request.hElementLayout.pDrvPrivate, 6, layout)) return E_INVALIDARG;
        desc.input_count = layout.count; desc.inputs = layout.elements;
    }
    // Runtime shader/state slots can be destroyed recursively inside Vulkan.
    NativeGeometryOutput geometry{};
    const uint32_t *gs_code = gs ? gs->shader : nullptr;
    size_t gs_bytes = gs ? size_t(gs->shader_words) * 4 : 0;
    if (so) {
        if (so->shader_words * size_t(4) < sizeof(geometry)) return E_INVALIDARG;
        std::memcpy(&geometry, so->shader, sizeof(geometry));
        if (size_t(geometry.code_words) * 4 != size_t(so->shader_words) * 4 - sizeof(geometry)) return E_INVALIDARG;
        desc.stream_output = &geometry.output;
        if (geometry.shader_stage == 2) {
            gs_code = so->shader + sizeof(geometry) / 4; gs_bytes = size_t(geometry.code_words) * 4;
        }
    }
    const size_t vs_bytes = size_t(vs->shader_words) * 4, ps_bytes = ps ? size_t(ps->shader_words) * 4 : 0;
    const size_t hs_bytes = hs ? size_t(hs->shader_words) * 4 : 0, ds_bytes = ds ? size_t(ds->shader_words) * 4 : 0;
    std::unique_ptr<uint32_t[]> code(new (std::nothrow) uint32_t[(vs_bytes + ps_bytes + gs_bytes + hs_bytes + ds_bytes) / 4]);
    if (!code) return E_OUTOFMEMORY;
    std::memcpy(code.get(), vs->shader, vs_bytes);
    if (ps_bytes) std::memcpy(reinterpret_cast<char *>(code.get()) + vs_bytes, ps->shader, ps_bytes);
    if (gs_bytes) {
        desc.gs = reinterpret_cast<char *>(code.get()) + vs_bytes + ps_bytes; desc.gs_size = gs_bytes;
        std::memcpy(const_cast<void *>(desc.gs), gs_code, gs_bytes);
    }
    if (hs_bytes) {
        desc.hs = reinterpret_cast<char *>(code.get()) + vs_bytes + ps_bytes + gs_bytes; desc.hs_size = hs_bytes;
        desc.ds = reinterpret_cast<const char *>(desc.hs) + hs_bytes; desc.ds_size = ds_bytes;
        std::memcpy(const_cast<void *>(desc.hs), hs->shader, hs_bytes);
        std::memcpy(const_cast<void *>(desc.ds), ds->shader, ds_bytes);
    }
    auto *root = belongs(ctx, request.hRootSignature.pDrvPrivate) ? backend(request.hRootSignature.pDrvPrivate, VKDU_ROOT) : nullptr;
    if (!root || !vkdu_object_retain(root)) return E_INVALIDARG;
    vkdu_object *value = nullptr; HRESULT hr;
    {
        NativeBackendCall call(ctx);
        hr = request.SampleDesc.Count == 1 ?
            vkdu_graphics_pipeline_create(ctx->backend, root, code.get(), vs_bytes,
                ps_bytes ? reinterpret_cast<char *>(code.get()) + vs_bytes : nullptr, ps_bytes, &desc, &value) :
            vkdu_graphics_pipeline_create_sampled(ctx->backend, root, code.get(), vs_bytes,
                ps_bytes ? reinterpret_cast<char *>(code.get()) + vs_bytes : nullptr, ps_bytes, &desc,
                request.SampleDesc.Count, request.SampleDesc.Quality, &value);
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
        if (SUCCEEDED(hr) && op.ctx->native_adapter) hr = native_heap_generation(op.ctx);
        report(hr);
    }
};
void APIENTRY native_execute_bundle(D3D12DDI_HCOMMANDLIST parent, D3D12DDI_HCOMMANDLIST bundle) {
    NativeGraphicsCommand command(parent);
    if (!command.value) return;
    auto *slot = command.op.ctx->native_adapter
        ? native_submission_find(command.op.ctx->native_command_objects, bundle.pDrvPrivate) : object(bundle.pDrvPrivate);
    auto *value = slot && slot->context == command.op.ctx && slot->kind == VKDU_COMMAND_LIST &&
        vkdu_object_retain(slot->backend) ? slot->backend : nullptr;
    command.run([&] {
        const HRESULT hr = value ? vkdu_command_execute_bundle(command.value, value) : E_INVALIDARG;
        vkdu_object_destroy(value);
        return hr;
    });
}
#include "native_stream_output.inc"
vkdu_object *native_graphics_view_heap(Context *ctx, D3D12DDI_CPU_DESCRIPTOR_HANDLE handle, uint32_t &index,
        D3D12DDI_DESCRIPTOR_HEAP_TYPE type) {
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *heap = find_heap(ctx, handle.ptr, false, &index);
    auto *value = heap && heap->descriptor_type == type &&
        (heap->descriptor_flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE) &&
        vkdu_object_retain(heap->backend) ? heap->backend : nullptr;
    ReleaseSRWLockShared(&ctx->resources_lock); return value;
}
vkdu_object *native_graphics_rtv_heap(Context *ctx, D3D12DDI_CPU_DESCRIPTOR_HANDLE handle, uint32_t &index) {
    return native_graphics_view_heap(ctx, handle, index, D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV);
}
void APIENTRY native_create_rtv(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 *args,
        D3D12DDI_CPU_DESCRIPTOR_HANDLE handle) {
    NativeHeapOperation op(context(h)); auto *ctx = op.ctx;
    if (!sampler_operation_live(op)) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    if (!args || args->ResourceDimension != D3D12DDI_RD_TEXTURE2D || !vkdu_color_target_format(args->Format) ||
            !args->Tex2D.ArraySize || args->Tex2D.PlaneSlice) {
        error(ctx, E_INVALIDARG); return;
    }
    const auto request = *args; uint32_t index = 0;
    auto *heap = native_graphics_rtv_heap(ctx, handle, index); vkdu_object *texture = nullptr;
    AcquireSRWLockShared(&ctx->resources_lock);
    auto *resource = find_resource(ctx, request.hDrvResource.pDrvPrivate);
    if (resource && resource->kind == VKDU_TEXTURE2D &&
            vkdu_texture_view_compatible(vkdu_texture2d_format(resource->backend), request.Format) &&
            vkdu_object_retain(resource->backend)) texture = resource->backend;
    ReleaseSRWLockShared(&ctx->resources_lock);
    HRESULT hr;
    { NativeBackendCall call(ctx); hr = heap && texture ? vkdu_texture2d_rtv_range(heap, index, texture, request.Format,
            request.Tex2D.MipSlice, request.Tex2D.FirstArraySlice, request.Tex2D.ArraySize) : E_INVALIDARG;
        vkdu_object_destroy(heap); vkdu_object_destroy(texture); }
    if (!sampler_operation_live(op)) hr = DXGI_ERROR_DEVICE_REMOVED;
    error(ctx, hr);
}
void native_root_signature(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HROOTSIGNATURE root, bool graphics) {
    NativeGraphicsCommand command(h); if (!command.value) return;
    auto *value = belongs(command.op.ctx, root.pDrvPrivate) ? backend(root.pDrvPrivate, VKDU_ROOT) : nullptr;
    /* A null handle unbinds. A non-null foreign/retired/wrong-kind handle must
     * not collapse to that legal operation when backend() rejects it. */
    if (root.pDrvPrivate && (!value || !vkdu_object_retain(value))) { command.report(E_INVALIDARG); return; }
    command.run([&] {
        auto hr = graphics ? vkdu_command_graphics_root(command.value, value) : vkdu_command_root(command.value, value);
        vkdu_object_destroy(value); return hr;
    });
}
void APIENTRY native_graphics_root(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HROOTSIGNATURE root) { native_root_signature(h, root, true); }
void APIENTRY root_set(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HROOTSIGNATURE root) { native_root_signature(h, root, false); }
#include "native_graphics_roots.inc"
#include "native_format.inc"
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
    if (count > VKDU_MAX_RENDER_TARGETS || (count && !rtvs) || (single_range != FALSE && single_range != TRUE)) {
        command.report(E_INVALIDARG); return;
    }
    D3D12DDI_CPU_DESCRIPTOR_HANDLE handles[VKDU_MAX_RENDER_TARGETS]{};
    if (count) std::memcpy(handles, rtvs, sizeof(*rtvs) * (single_range ? 1 : count));
    const auto depth_handle = dsv ? *dsv : D3D12DDI_CPU_DESCRIPTOR_HANDLE{};
    vkdu_render_target_binding targets[VKDU_MAX_RENDER_TARGETS]{};
    vkdu_object *depth = nullptr;
    uint32_t depth_index = 0;
    HRESULT status = S_OK;
    for (UINT i = 0; i < count; ++i) {
        if (single_range && i) {
            if (i > UINT32_MAX - targets[0].index || !vkdu_object_retain(targets[0].heap)) {
                status = E_INVALIDARG; break;
            }
            targets[i] = {targets[0].heap, targets[0].index + i};
        } else {
            targets[i].heap = native_graphics_rtv_heap(command.op.ctx, handles[i], targets[i].index);
            if (!targets[i].heap) { status = E_INVALIDARG; break; }
        }
    }
    if (SUCCEEDED(status) && dsv) {
        depth = native_graphics_view_heap(command.op.ctx, depth_handle, depth_index, D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV);
        if (!depth) status = E_INVALIDARG;
    }
    command.run([&] {
        const auto hr = FAILED(status) ? status : vkdu_command_render_targets(command.value, count,
            count ? targets : nullptr, depth, depth_index);
        for (auto &target : targets) vkdu_object_destroy(target.heap);
        vkdu_object_destroy(depth);
        return hr;
    });
}
void APIENTRY native_clear_rtv(D3D12DDI_HCOMMANDLIST h, D3D12DDI_CPU_DESCRIPTOR_HANDLE handle,
        const FLOAT color[4], UINT count, const D3D12DDI_RECT *rects) {
    NativeGraphicsCommand command(h); if (!command.value) return;
    vkdu_rect copy[16]{}; float rgba[4];
    if (!color || !native_graphics_rects(count, rects, copy)) {
        command.run([&] { vkdu_object_destroy(command.value); command.value = nullptr; return E_INVALIDARG; });
        return;
    }
    std::memcpy(rgba, color, sizeof(rgba)); uint32_t index = 0;
    auto *heap = native_graphics_rtv_heap(command.op.ctx, handle, index);
    command.run([&] {
        auto hr = vkdu_command_clear_rtv(command.value, heap, index, rgba, count, copy);
        vkdu_object_destroy(heap); vkdu_object_destroy(command.value); command.value = nullptr; return hr;
    });
}
void APIENTRY native_index_buffer(D3D12DDI_HCOMMANDLIST h, const D3D12DDI_INDEX_BUFFER_VIEW *view) {
    NativeGraphicsCommand command(h); if (!command.value) return;
    const auto request = view ? *view : D3D12DDI_INDEX_BUFFER_VIEW{};
    vkdu_object *buffer = nullptr; uint64_t offset = 0;
    if (view) {
        auto *ctx = command.op.ctx;
        AcquireSRWLockShared(&ctx->resources_lock);
        auto *entry = find_buffer_address(ctx, request.BufferLocation, request.SizeInBytes);
        if (entry && vkdu_object_retain(entry->backend)) {
            buffer = entry->backend; offset = request.BufferLocation - entry->address;
        }
        ReleaseSRWLockShared(&ctx->resources_lock);
        if (!buffer) { command.report(E_INVALIDARG); return; }
    }
    command.run([&] { auto hr = vkdu_command_index_buffer(command.value, buffer, offset, request.SizeInBytes, request.Format);
        vkdu_object_destroy(buffer); return hr; });
}
void APIENTRY native_vertex_buffers(D3D12DDI_HCOMMANDLIST h, UINT first, UINT count,
        const D3D12DDI_VERTEX_BUFFER_VIEW *views) {
    NativeGraphicsCommand command(h); if (!command.value) return;
    if (first > 32 || count > 32 - first) { command.report(E_INVALIDARG); return; }
    D3D12DDI_VERTEX_BUFFER_VIEW copy[32]{};
    vkdu_vertex_buffer buffers[32]{};
    if (views) std::memcpy(copy, views, count * sizeof(*views));
    auto *ctx = command.op.ctx;
    HRESULT hr = ctx->native_adapter ? native_heap_generation(ctx) : S_OK;
    AcquireSRWLockShared(&ctx->resources_lock);
    for (UINT i = 0; i < count && SUCCEEDED(hr); i++) {
        const auto &v = copy[i];
        if (!v.BufferLocation) {
            if (v.SizeInBytes || v.StrideInBytes) hr = E_INVALIDARG;
            continue;
        }
        if (v.StrideInBytes > 2048) { hr = E_INVALIDARG; break; }
        auto *entry = find_buffer_address(ctx, v.BufferLocation, v.SizeInBytes);
        if (entry && vkdu_object_retain(entry->backend))
            buffers[i] = {entry->backend, v.BufferLocation - entry->address, v.SizeInBytes, v.StrideInBytes};
        if (!buffers[i].buffer) hr = E_INVALIDARG;
    }
    ReleaseSRWLockShared(&ctx->resources_lock);
    {
        NativeBackendCall call(ctx);
        if (SUCCEEDED(hr)) hr = vkdu_command_vertex_buffers(command.value, first, count, buffers);
        for (UINT i = 0; i < count; i++) vkdu_object_destroy(buffers[i].buffer);
    }
    if (SUCCEEDED(hr) && ctx->native_adapter) hr = native_heap_generation(ctx);
    command.report(hr);
}
#include "native_depth.inc"
#include "native_resource_copy.inc"
#include "native_resolve.inc"
#include "native_barriers.inc"
#include "native_uav_clear.inc"
#include "native_predication.inc"
void APIENTRY native_blend_factor(D3D12DDI_HCOMMANDLIST h, const FLOAT values[4]) {
    NativeGraphicsCommand command(h); if (!command.value) return;
    if (!values) { command.report(E_INVALIDARG); return; }
    float copy[4]; std::memcpy(copy, values, sizeof(copy));
    command.run([&] { return vkdu_command_blend_factor(command.value, copy); });
}
void APIENTRY native_stencil_ref(D3D12DDI_HCOMMANDLIST h, UINT reference) {
    NativeGraphicsCommand command(h);
    command.run([&] { return vkdu_command_stencil_ref(command.value, reference); });
}
void APIENTRY native_clear_root_arguments(D3D12DDI_HCOMMANDLIST h) {
    NativeGraphicsCommand command(h);
    if (!command.value) return;
    command.run([&] {
#ifdef VKDU_TEST_BUNDLE_CLEAR_DDI_DROP
        const HRESULT hr = S_OK;
#else
        const HRESULT hr = vkdu_command_clear_root_arguments(command.value);
#endif
        vkdu_object_destroy(command.value);
        command.value = nullptr;
        return hr;
    });
}
void native_graphics_tables(D3D12DDI_DEVICE_FUNCS_CORE_0003 *device, D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *commands) {
    device->pfnCreateVertexShader = native_graphics_shader<1>; device->pfnCreatePixelShader = native_graphics_shader<0>;
    device->pfnCreateGeometryShader = native_graphics_shader<2>;
    device->pfnCalcPrivateGeometryShaderWithStreamOutput = native_geometry_output_size;
    device->pfnCreateGeometryShaderWithStreamOutput = native_geometry_output;
    commands->pfnSOSetTargets = native_stream_output;
    device->pfnCalcPrivateTessellationShaderSize = native_tessellation_size;
    device->pfnCreateHullShader = native_tessellation_shader<3>;
    device->pfnCreateDomainShader = native_tessellation_shader<4>;
    device->pfnCalcPrivateElementLayoutSize = native_graphics_state_size<D3D12DDIARG_CREATEELEMENTLAYOUT>;
    device->pfnCreateElementLayout = native_element_layout;
    device->pfnDestroyElementLayout = native_graphics_state_destroy<6, D3D12DDI_HELEMENTLAYOUT>;
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
    device->pfnCreateDepthStencilView = native_create_dsv;
    commands->pfnClearDepthStencilView = native_clear_dsv;
    commands->pfnOmSetBlendFactor = native_blend_factor;
    commands->pfnOmSetStencilRef = native_stencil_ref;
    commands->pfnClearRootArguments = native_clear_root_arguments;
    commands->pfnClearUnorderedAccessViewUint = native_clear_uav_uint;
    commands->pfnClearUnorderedAccessViewFloat = native_clear_uav_float;
    commands->pfnSetPredication = native_predication;
    commands->pfnExecuteBundle = native_execute_bundle;
    commands->pfnResourceCopy = native_resource_copy;
    commands->pfnResourceResolveSubresource = native_resolve;
    commands->pfnSetGraphicsRootSignature = native_graphics_root;
    device->pfnCheckFormatSupport = native_format_support;
    device->pfnCheckMultisampleQualityLevels = native_multisample_quality;
    commands->pfnSetGraphicsRootDescriptorTable = native_graphics_table;
    commands->pfnSetGraphicsRoot32BitConstant = native_graphics_constant;
    commands->pfnSetGraphicsRoot32BitConstants = native_graphics_constants;
    commands->pfnSetGraphicsRootConstantBufferView = native_graphics_descriptor<2>;
    commands->pfnSetGraphicsRootShaderResourceView = native_graphics_descriptor<3>;
    commands->pfnSetGraphicsRootUnorderedAccessView = native_graphics_descriptor<4>;
    commands->pfnDrawInstanced = native_draw; commands->pfnDrawIndexedInstanced = native_draw_indexed;
    commands->pfnIaSetTopology = native_topology; commands->pfnRsSetViewports = native_viewports;
    commands->pfnRsSetScissorRects = native_scissors; commands->pfnOMSetRenderTargets = native_render_targets;
    commands->pfnClearRenderTargetView = native_clear_rtv; commands->pfnIASetIndexBuffer = native_index_buffer;
    commands->pfnIASetVertexBuffers = native_vertex_buffers;
}
