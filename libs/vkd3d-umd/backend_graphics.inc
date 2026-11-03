/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SM5.0/5.1 token encodings match dxbc-spirv/dxbc/dxbc_types.h. This scanner only
 * selects declared VS/PS interface registers; the real compiler validates all
 * executable instructions. Reject unsupported declaration encodings explicitly. */
static HRESULT vkdu_graphics_declarations(const uint32_t *tokens, uint32_t words,
        uint32_t masks[2][32], uint32_t systems[2][32])
{
    uint32_t pos = 2;
    while (pos < words) {
        const uint32_t opcode = tokens[pos] & 0x7ff;
        uint32_t length = (tokens[pos] >> 24) & 0x7f;
        if (opcode == 53) {
            if (words - pos < 2) return E_INVALIDARG;
            length = tokens[pos + 1];
            if (length < 2) return E_INVALIDARG;
        }
        if (!length || length > words - pos) return E_INVALIDARG;
        if (opcode >= 95 && opcode <= 103) {
            const uint32_t output = opcode >= 101;
            const uint32_t system = opcode != 95 && opcode != 98 && opcode != 101;
            uint32_t operand, reg, mask, name;
            if (length != 3 + system || (tokens[pos] & 0x80000000u)) return E_INVALIDARG;
            operand = tokens[pos + 1]; reg = tokens[pos + 2]; name = system ? tokens[pos + 3] : 0;
            /* One immediate register index, no operand extensions, mask mode. */
            if ((operand & 0xffc00000u) || ((operand >> 20) & 3) != 1 ||
                    ((operand >> 12) & 255) != (output ? 2u : 1u) || reg >= 32 ||
                    ((operand & 3) != 1 && (operand & 3) != 2) || (operand & 12)) return E_INVALIDARG;
            mask = (operand & 3) == 1 ? 1 : (operand >> 4) & 15;
            if (!mask || (masks[output][reg] && systems[output][reg] != name)) return E_INVALIDARG;
            masks[output][reg] |= mask; systems[output][reg] = name;
        }
        pos += length;
    }
    return S_OK;
}
static HRESULT vkdu_graphics_filter(const struct vkdu_signature *src, uint32_t count,
        const uint32_t masks[32], const uint32_t systems[32], struct vkdu_signature *dst, uint32_t *out_count)
{
    uint32_t i, used[32] = {0}, seen[32] = {0};
    *out_count = 0;
    for (i = 0; i < count; ++i) {
        struct vkdu_signature entry = src[i];
        if (entry.reg >= 32 || !entry.mask || entry.mask > 15 || (seen[entry.reg] & entry.mask) ||
                entry.component_type < 1 || entry.component_type > 3) return E_INVALIDARG;
        seen[entry.reg] |= entry.mask;
        entry.mask &= masks[entry.reg];
        if (!entry.mask) continue;
        if (entry.system_value != systems[entry.reg]) return E_INVALIDARG;
        used[entry.reg] |= entry.mask;
        dst[(*out_count)++] = entry;
    }
    for (i = 0; i < 32; ++i) if (used[i] != masks[i]) return E_INVALIDARG;
    return S_OK;
}
static const char *vkdu_signature_name(uint32_t system, uint32_t stage, int output)
{
    switch (system) {
    case 0: return stage == 0 && output ? "SV_Target" : "ATTR";
    case 1: return "SV_Position";
    case 2: return "SV_ClipDistance";
    case 3: return "SV_CullDistance";
    case 4: return "SV_RTArrayIndex";
    case 5: return "SV_Viewport";
    case 6: return "SV_VertexID";
    case 7: return "SV_PrimitiveID";
    case 8: return "SV_InstanceID";
    default: return NULL; /* Other builtins require their exact stage contract. */
    }
}
static uint32_t vkdu_signature_index(const struct vkdu_signature *entries, uint32_t count, uint32_t i)
{
    const struct vkdu_signature *e = &entries[i];
    uint32_t j, index = 0;
    if (!e->system_value) return e->reg;
    if (e->system_value != 2 && e->system_value != 3) return 0;
    /* R0 carries registers, not application semantic indices. Assign distance
     * indices by register order so separately converted linked stages agree. */
    for (j = 0; j < count; ++j)
        if (entries[j].system_value == e->system_value && entries[j].reg < e->reg) ++index;
    return index;
}
static int vkdu_signature_distances_valid(const struct vkdu_signature *entries, uint32_t count)
{
    uint32_t i, regs = 0, reg_count = 0;
    for (i = 0; i < count; ++i) {
        const struct vkdu_signature *e = &entries[i];
        if (e->system_value != 2 && e->system_value != 3) continue;
        if (e->reg >= 32 || e->component_type != 3 || vkdu_signature_index(entries,count,i) >= 2) return 0;
        if (!(regs & (1u << e->reg))) { regs |= 1u << e->reg; if (++reg_count > 2) return 0; }
    }
    return 1;
}
static HRESULT vkdu_signature_write(uint32_t *chunk, uint32_t stage, int output,
        const struct vkdu_signature *entries, uint32_t count)
{
    uint32_t i, occupied[32] = {0};
    char *names = (char *)(chunk + 2 + count * 6);
    if (!vkdu_signature_distances_valid(entries,count)) return E_INVALIDARG;
    chunk[0] = count; chunk[1] = 8;
    for (i = 0; i < count; ++i) {
        const struct vkdu_signature *e = &entries[i];
        const char *name = vkdu_signature_name(e->system_value, stage, output);
        uint32_t *entry = chunk + 2 + i * 6;
        if (!name || e->reg >= 32 || !e->mask || e->mask > 15 || (occupied[e->reg] & e->mask) ||
                e->component_type < 1 || e->component_type > 3 ||
                ((e->system_value == 6 || e->system_value == 8) && (stage != 1 || output)) ||
                (stage == 0 && output && e->system_value)) return E_INVALIDARG;
        occupied[e->reg] |= e->mask;
        entry[0] = (uint32_t)(names - (char *)chunk);
        /* Register layout is the native interstage contract. User semantic
         * names are absent at this DDI; use a stable name and register index. */
        entry[1] = vkdu_signature_index(entries,count,i);
        entry[2] = e->system_value; entry[3] = e->component_type; entry[4] = e->reg;
        entry[5] = e->mask | ((output ? 0 : e->mask) << 8);
        strcpy(names, name); names += 16;
    }
    return S_OK;
}
/* SM6: the DDI hands over the DXIL program part (version, size, 'DXIL'
 * bitcode header, bitcode). Wrap it with the runtime's signatures; the
 * embedded vkd3d-proton selects dxil-spirv by the DXIL chunk. Interstage
 * locations come from the bitcode's own signature metadata. */
int vkdu_shader_is_dxil(const uint32_t *tokens, uint32_t words)
{
    return tokens && words >= 6 && ((tokens[0] >> 4) & 15) >= 6 && tokens[2] == 0x4c495844;
}
int32_t vkdu_dxil_container(uint32_t stage, const uint32_t *tokens, uint32_t words,
        const struct vkdu_signature *inputs, uint32_t input_count,
        const struct vkdu_signature *outputs, uint32_t output_count, void **code, size_t *size)
{
    uint32_t *container, offset, lengths[2], count;
    HRESULT hr;
    if (!code || !size) return E_POINTER;
    *code = NULL; *size = 0;
    if (!vkdu_shader_is_dxil(tokens, words) || tokens[1] != words || words > 16 * 1024 * 1024 ||
            (tokens[0] >> 16) != stage || input_count > 32 || output_count > 32 ||
            (input_count && !inputs) || (output_count && !outputs))
        return E_INVALIDARG;
    lengths[0] = 8 + input_count * 40; lengths[1] = 8 + output_count * 40;
    count = 11 + 2 + lengths[0] / 4 + 2 + lengths[1] / 4 + 2 + words;
    if (!(container = calloc(count, 4))) return E_OUTOFMEMORY;
    container[0] = 0x43425844; container[5] = 1; container[6] = count * 4; container[7] = 3;
    offset = 11; container[8] = offset * 4; container[offset] = 0x4e475349; container[offset + 1] = lengths[0];
    hr = vkdu_signature_write(container + offset + 2, stage, 0, inputs, input_count);
    offset += 2 + lengths[0] / 4; container[9] = offset * 4;
    container[offset] = 0x4e47534f; container[offset + 1] = lengths[1];
    if (SUCCEEDED(hr)) hr = vkdu_signature_write(container + offset + 2, stage, 1, outputs, output_count);
    offset += 2 + lengths[1] / 4; container[10] = offset * 4;
    container[offset] = 0x4c495844; container[offset + 1] = words * 4;
    memcpy(container + offset + 2, tokens, words * 4);
    if (FAILED(hr)) { vkdu_trace("dxil: signature write hr=%08x stage=%u", (unsigned)hr, stage); free(container); return hr; }
    vkd3d_compute_dxbc_checksum(container, count * 4, container + 1);
    *code = container; *size = count * 4;
    return S_OK;
}
#include "backend_shader_stages.inc"
#include "backend_tessellation.inc"
int32_t vkdu_shader_dxbc(uint32_t stage, const uint32_t *tokens, uint32_t words,
        const struct vkdu_signature *inputs, uint32_t input_count,
        const struct vkdu_signature *outputs, uint32_t output_count, void **code, size_t *size)
{
    uint32_t *container, offset, lengths[2], count;
    uint32_t masks[2][32] = {{0}}, systems[2][32] = {{0}}, filtered_inputs, filtered_outputs;
    struct vkdu_signature input_entries[32], output_entries[32];
    HRESULT hr;
    if (!code || !size) return E_POINTER;
    *code = NULL; *size = 0;
    if (vkdu_shader_is_dxil(tokens, words))
        return vkdu_dxil_container(stage, tokens, words, inputs, input_count, outputs, output_count, code, size);
    if (stage == 2) return vkdu_geometry_dxbc(tokens, words, inputs, input_count, outputs, output_count, code, size);
    if (stage > 1 || !tokens || words < 2 || words > 1024 * 1024 || tokens[1] != words ||
            (tokens[0] >> 16) != stage || ((tokens[0] & 255) != 0x50 && (tokens[0] & 255) != 0x51 &&
             (tokens[0] & 255) != 0x40 && (tokens[0] & 255) != 0x41) ||
            input_count > 32 || output_count > 32 || (input_count && !inputs) || (output_count && !outputs))
        return E_INVALIDARG;
    if (FAILED(hr = vkdu_graphics_declarations(tokens, words, masks, systems))) {
        uint32_t k; char dump[400]; size_t used = 0;
        for (k = 0; k < words && k < 40 && used + 10 < sizeof(dump); ++k) used += snprintf(dump + used, sizeof(dump) - used, " %08x", tokens[k]);
        vkdu_trace("shader_dxbc: declarations hr=%08x tokens:%s", (unsigned)hr, dump);
        return hr;
    }
    if (FAILED(hr = vkdu_graphics_filter(inputs, input_count, masks[0], systems[0], input_entries, &filtered_inputs)) ||
            FAILED(hr = vkdu_graphics_filter(outputs, output_count, masks[1], systems[1], output_entries, &filtered_outputs))) {
        uint32_t k; char dump[400]; size_t used = 0;
        for (k = 0; k < words && k < 40 && used + 10 < sizeof(dump); ++k) used += snprintf(dump + used, sizeof(dump) - used, " %08x", tokens[k]);
        vkdu_trace("shader_dxbc: signature filter hr=%08x out0 sv=%u reg=%u mask=%x type=%u tokens:%s", (unsigned)hr,
            output_count ? outputs[0].system_value : 0, output_count ? outputs[0].reg : 0,
            output_count ? outputs[0].mask : 0, output_count ? outputs[0].component_type : 0, dump);
        return hr;
    }
    /* A UAV-only pixel shader legitimately has no output signature. Vertex
     * shaders still need their position output for this rasterization path. */
    if (stage == 1 && !filtered_outputs) { vkdu_trace("shader_dxbc: VS without outputs"); return E_INVALIDARG; }
    inputs = input_entries; outputs = output_entries; input_count = filtered_inputs; output_count = filtered_outputs;
    lengths[0] = 8 + input_count * 40; lengths[1] = 8 + output_count * 40;
    count = 11 + 2 + lengths[0] / 4 + 2 + lengths[1] / 4 + 2 + words;
    if (!(container = calloc(count, 4))) return E_OUTOFMEMORY;
    container[0] = 0x43425844; container[5] = 1; container[6] = count * 4; container[7] = 3;
    offset = 11; container[8] = offset * 4; container[offset] = 0x4e475349; container[offset + 1] = lengths[0];
    hr = vkdu_signature_write(container + offset + 2, stage, 0, inputs, input_count);
    offset += 2 + lengths[0] / 4; container[9] = offset * 4;
    container[offset] = 0x4e47534f; container[offset + 1] = lengths[1];
    if (SUCCEEDED(hr)) hr = vkdu_signature_write(container + offset + 2, stage, 1, outputs, output_count);
    offset += 2 + lengths[1] / 4; container[10] = offset * 4;
    /* SM4.x token streams live in an SHDR chunk, SM5.x in SHEX. */
    container[offset] = (tokens[0] & 255) >= 0x50 ? 0x58454853 : 0x52444853; container[offset + 1] = words * 4;
    memcpy(container + offset + 2, tokens, words * 4);
    if (FAILED(hr)) { vkdu_trace("shader_dxbc: signature write hr=%08x", (unsigned)hr); free(container); return hr; }
    vkd3d_compute_dxbc_checksum(container, count * 4, container + 1);
    *code = container; *size = count * 4;
    return S_OK;
}
void vkdu_shader_dxbc_free(void *code) { free(code); }

static HRESULT vkdu_input_layout(uint32_t count, const struct vkdu_input_element *elements,
        D3D12_INPUT_ELEMENT_DESC native[32])
{
    uint32_t i, used = 0, slots = 0, offsets[32] = {0}, classes[32] = {0}, steps[32] = {0};
    if (count > 32 || (count && !elements)) return E_INVALIDARG;
    for (i = 0; i < count; i++) {
        const struct vkdu_input_element *e = &elements[i];
        uint32_t bytes, alignment, offset;
        switch (e->format) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
            bytes = 16; alignment = 4; break;
        case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT:
            bytes = 12; alignment = 4; break;
        case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
            bytes = 8; alignment = 4; break;
        case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
            bytes = 4; alignment = 4; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SINT:
            bytes = 4; alignment = 1; break;
        default: return DXGI_ERROR_UNSUPPORTED;
        }
        if (e->slot >= 32 || e->reg >= 32 || (used & (1u << e->reg)) || e->per_instance > 1 ||
                (!e->per_instance && e->step_rate)) return E_INVALIDARG;
        if ((slots & (1u << e->slot)) && (classes[e->slot] != e->per_instance || steps[e->slot] != e->step_rate))
            return E_INVALIDARG;
        offset = e->offset == UINT32_MAX ? (offsets[e->slot] + alignment - 1) & ~(alignment - 1) : e->offset;
        if (offset > 2048 - bytes || offset % alignment) return E_INVALIDARG;
        used |= 1u << e->reg; slots |= 1u << e->slot;
        offsets[e->slot] = offset + bytes; classes[e->slot] = e->per_instance; steps[e->slot] = e->step_rate;
        native[i].SemanticName = "ATTR"; native[i].SemanticIndex = e->reg;
        native[i].Format = e->format; native[i].InputSlot = e->slot; native[i].AlignedByteOffset = offset;
        native[i].InputSlotClass = e->per_instance; native[i].InstanceDataStepRate = e->step_rate;
    }
    return S_OK;
}
int32_t vkdu_input_layout_validate(uint32_t count, const struct vkdu_input_element *elements)
{
    D3D12_INPUT_ELEMENT_DESC native[32];
    return vkdu_input_layout(count, elements, native);
}
static int vkdu_blend_factor_valid(uint32_t factor, int alpha)
{
    if (!((factor >= D3D12_BLEND_ZERO && factor <= D3D12_BLEND_SRC_ALPHA_SAT) ||
          (factor >= D3D12_BLEND_BLEND_FACTOR && factor <= D3D12_BLEND_INV_SRC1_ALPHA))) return 0;
    return !alpha || (factor != D3D12_BLEND_SRC_COLOR && factor != D3D12_BLEND_INV_SRC_COLOR &&
        factor != D3D12_BLEND_DEST_COLOR && factor != D3D12_BLEND_INV_DEST_COLOR &&
        factor != D3D12_BLEND_SRC1_COLOR && factor != D3D12_BLEND_INV_SRC1_COLOR);
}
#include "backend_distance_link.inc"
static HRESULT vkdu_pipeline_targets(const struct vkdu_graphics_desc *state,
        D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc)
{
    struct vkdu_render_target_desc legacy = {state->format, state->write_mask, state->blend_enable,
        state->src_blend, state->dst_blend, state->blend_op, state->src_blend_alpha,
        state->dst_blend_alpha, state->blend_op_alpha};
    const struct vkdu_render_target_desc *targets = state->render_targets ? state->render_targets : &legacy;
    uint32_t i, count = state->render_targets ? state->render_target_count : !!state->format;
    if (state->render_target_count > VKDU_MAX_RENDER_TARGETS ||
            (!state->render_targets && state->render_target_count)) return E_INVALIDARG;
    desc->NumRenderTargets = count;
    desc->BlendState.IndependentBlendEnable = count > 1;
    for (i = 0; i < count || (!state->render_targets && !i); ++i) {
        const struct vkdu_render_target_desc *target = &targets[i];
        D3D12_RENDER_TARGET_BLEND_DESC *blend = &desc->BlendState.RenderTarget[i];
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_MRT_BROADCAST_BLEND)
        if (count > 1) target = &targets[0];
#endif
        if ((count && !vkdu_color_target_format(targets[i].format)) || target->write_mask > 15 ||
                target->blend_enable > 1) return E_INVALIDARG;
        desc->RTVFormats[i] = targets[i].format;
        blend->SrcBlend = blend->SrcBlendAlpha = D3D12_BLEND_ONE;
        blend->DestBlend = blend->DestBlendAlpha = D3D12_BLEND_ZERO;
        blend->BlendOp = blend->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend->LogicOp = D3D12_LOGIC_OP_NOOP;
        blend->RenderTargetWriteMask = target->write_mask;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_BGRA_DROP_BLUE)
        if (targets[i].format == DXGI_FORMAT_B8G8R8A8_UNORM || targets[i].format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            blend->RenderTargetWriteMask &= ~D3D12_COLOR_WRITE_ENABLE_BLUE;
#endif
        if (!target->blend_enable) continue;
        if (!vkdu_blend_factor_valid(target->src_blend, 0) || !vkdu_blend_factor_valid(target->dst_blend, 0) ||
                !vkdu_blend_factor_valid(target->src_blend_alpha, 1) || !vkdu_blend_factor_valid(target->dst_blend_alpha, 1) ||
                target->blend_op < D3D12_BLEND_OP_ADD || target->blend_op > D3D12_BLEND_OP_MAX ||
                target->blend_op_alpha < D3D12_BLEND_OP_ADD || target->blend_op_alpha > D3D12_BLEND_OP_MAX ||
                (count > 1 && (target->src_blend >= D3D12_BLEND_SRC1_COLOR || target->dst_blend >= D3D12_BLEND_SRC1_COLOR ||
                    target->src_blend_alpha >= D3D12_BLEND_SRC1_COLOR || target->dst_blend_alpha >= D3D12_BLEND_SRC1_COLOR)))
            return E_INVALIDARG;
        blend->BlendEnable = TRUE;
        blend->SrcBlend = target->src_blend; blend->DestBlend = target->dst_blend;
        blend->BlendOp = target->blend_op; blend->SrcBlendAlpha = target->src_blend_alpha;
        blend->DestBlendAlpha = target->dst_blend_alpha; blend->BlendOpAlpha = target->blend_op_alpha;
    }
    return S_OK;
}
static HRESULT vkdu_stencil_face(const struct vkdu_stencil_face *state, int enabled,
        D3D12_DEPTH_STENCILOP_DESC *out)
{
    /* R0 has per-face enables; D3D12 has one enable. A disabled face must
     * always pass and preserve stencil on all three possible outcomes. */
    out->StencilFailOp = out->StencilDepthFailOp = out->StencilPassOp = D3D12_STENCIL_OP_KEEP;
    out->StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    if (!enabled) return S_OK;
    if (state->fail_op < 1 || state->fail_op > 8 || state->depth_fail_op < 1 || state->depth_fail_op > 8 ||
            state->pass_op < 1 || state->pass_op > 8 || state->func < 1 || state->func > 8) return E_INVALIDARG;
    out->StencilFailOp = state->fail_op; out->StencilDepthFailOp = state->depth_fail_op;
    out->StencilPassOp = state->pass_op; out->StencilFunc = state->func;
    return S_OK;
}
static HRESULT vkdu_pipeline_stencil(const struct vkdu_stencil_state *state, D3D12_DEPTH_STENCIL_DESC *out)
{
    HRESULT hr;
    if (state->enable > 1 || state->front_enable > 1 || state->back_enable > 1 ||
            (state->enable && ((!state->front_enable && !state->back_enable) ||
             state->read_mask > 255 || state->write_mask > 255))) return E_INVALIDARG;
    if (FAILED(hr = vkdu_stencil_face(&state->front, state->enable && state->front_enable, &out->FrontFace)) ||
            FAILED(hr = vkdu_stencil_face(&state->back, state->enable && state->back_enable, &out->BackFace))) return hr;
    out->StencilEnable = state->enable;
    out->StencilReadMask = state->enable ? state->read_mask : 255;
    out->StencilWriteMask = state->enable ? state->write_mask : 255;
    return S_OK;
}
int32_t vkdu_graphics_pipeline_create(vkdu_device *device, vkdu_object *root,
        const void *vs, size_t vs_size, const void *ps, size_t ps_size,
        const struct vkdu_graphics_desc *state, vkdu_object **out)
{ return vkdu_graphics_pipeline_create_sampled(device, root, vs, vs_size, ps, ps_size, state, 1, 0, out); }
int32_t vkdu_graphics_pipeline_create_sampled(vkdu_device *device, vkdu_object *root,
        const void *vs, size_t vs_size, const void *ps, size_t ps_size,
        const struct vkdu_graphics_desc *state, uint32_t samples, uint32_t quality, vkdu_object **out)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {0};
    D3D12_INPUT_ELEMENT_DESC elements[32];
    D3D12_SO_DECLARATION_ENTRY so_entries[VKDU_MAX_SO_ELEMENTS];
    struct vkd3d_shader_signature so_signature = {0};
    ID3D12PipelineState *pipeline = NULL;
    void *linked[4]={0}; uint32_t link;
    const uint32_t topology = state && state->topology_type ? state->topology_type : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !VALID(root, VKDU_ROOT) || root->owner != device->object || !vs || !vs_size ||
            (!ps != !ps_size) || !state || (!state->render_targets && state->format && !vkdu_color_target_format(state->format)) ||
            (!state->gs != !state->gs_size) || (!state->hs != !state->hs_size) || (!state->ds != !state->ds_size) ||
            (!state->hs != !state->ds) || (state->hs ? topology != 4 : topology == 4) ||
            topology < 1 || topology > 4 || state->strip_cut > 2 ||
            (state->fill_mode != 2 && state->fill_mode != 3) || state->cull_mode < 1 || state->cull_mode > 3 ||
            state->front_ccw > 1 || state->depth_clip > 1 || (!state->render_targets && state->write_mask > 15)) return E_INVALIDARG;
    if ((state->depth_format && !vkdu_depth_format(state->depth_format)) ||
            state->depth_enable > 1 || state->depth_write > 1 ||
            (state->depth_enable && (!state->depth_format || state->depth_func < 1 || state->depth_func > 8)) ||
            (state->stencil.enable && !state->depth_format))
        return E_INVALIDARG;
    if (FAILED(hr = vkdu_pipeline_stencil(&state->stencil, &desc.DepthStencilState))) return hr;
    if (FAILED(hr = vkdu_input_layout(state->input_count, state->inputs, elements))) return hr;
    desc.InputLayout.NumElements = state->input_count; desc.InputLayout.pInputElementDescs = elements;
    desc.pRootSignature = OBJ(ID3D12RootSignature, root);
    desc.VS.pShaderBytecode = vs; desc.VS.BytecodeLength = vs_size;
    desc.PS.pShaderBytecode = ps; desc.PS.BytecodeLength = ps_size;
    desc.GS.pShaderBytecode = state->gs; desc.GS.BytecodeLength = state->gs_size;
    desc.HS.pShaderBytecode = state->hs; desc.HS.BytecodeLength = state->hs_size;
    desc.DS.pShaderBytecode = state->ds; desc.DS.BytecodeLength = state->ds_size;
    if (FAILED(hr = vkdu_pipeline_targets(state, &desc))) return hr;
    if (quality || (samples != 1 && samples != 2 && samples != 4) ||
            (samples > 1 && (state->depth_format || !desc.NumRenderTargets))) return E_INVALIDARG;
    for (uint32_t i = 0; i < desc.NumRenderTargets; ++i)
        if (FAILED(hr = vkdu_private_color_samples(device, desc.RTVFormats[i], samples, quality))) return hr;
    desc.SampleMask = state->sample_mask;
    desc.RasterizerState.FillMode = state->fill_mode; desc.RasterizerState.CullMode = state->cull_mode;
    desc.RasterizerState.FrontCounterClockwise = state->front_ccw; desc.RasterizerState.DepthClipEnable = state->depth_clip;
    desc.DepthStencilState.DepthEnable = state->depth_enable;
    desc.DepthStencilState.DepthWriteMask = state->depth_write;
    desc.DepthStencilState.DepthFunc = state->depth_enable ? state->depth_func : D3D12_COMPARISON_FUNC_ALWAYS;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_DEPTH_DROP_COMPARE)
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
#endif
    desc.DSVFormat = state->depth_format;
    desc.PrimitiveTopologyType = topology; desc.IBStripCutValue = state->strip_cut;
    desc.SampleDesc.Count = samples; desc.SampleDesc.Quality = quality;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_MSAA_IGNORE_SAMPLE_MASK)
    if (samples > 1) desc.SampleMask = ~0u;
#endif
    if (state->stream_output) {
        hr = vkdu_stream_output_layout(state->stream_output, state->gs ? state->gs : state->ds ? state->ds : vs,
            state->gs ? state->gs_size : state->ds ? state->ds_size : vs_size, so_entries, &desc.StreamOutput.NumEntries, &so_signature);
        if (FAILED(hr)) { vkd3d_shader_free_shader_signature(&so_signature); return hr; }
        desc.StreamOutput.pSODeclaration = so_entries;
        desc.StreamOutput.pBufferStrides = state->stream_output->strides;
        desc.StreamOutput.NumStrides = state->stream_output->stride_count;
        desc.StreamOutput.RasterizedStream = state->stream_output->rasterized_stream;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_SO_DROP_RASTER)
        if (desc.StreamOutput.RasterizedStream < 4) desc.StreamOutput.RasterizedStream = 0;
#endif
    }
    {
        D3D12_SHADER_BYTECODE *previous=&desc.VS, *stages[]={&desc.HS,&desc.DS,&desc.GS,&desc.PS};
        for (link=0;link<4;++link) {
            D3D12_SHADER_BYTECODE *next=stages[link];size_t length=0;
            if (!next->BytecodeLength || (link==3 && desc.StreamOutput.RasterizedStream==D3D12_SO_NO_RASTERIZED_STREAM)) continue;
            hr=vkdu_shader_link_distances(previous->pShaderBytecode,previous->BytecodeLength,
                next->pShaderBytecode,next->BytecodeLength,link==3 && desc.GS.BytecodeLength ? desc.StreamOutput.RasterizedStream : 0,
                &linked[link],&length);
            if (FAILED(hr)) goto linked_done;
            if (linked[link]) {next->pShaderBytecode=linked[link];next->BytecodeLength=length;}
            previous=next;
        }
    }
    hr = ID3D12Device_CreateGraphicsPipelineState(device->object, &desc, &IID_ID3D12PipelineState, (void **)&pipeline);
 linked_done:
    for (link=0;link<4;++link) free(linked[link]);
    vkd3d_shader_free_shader_signature(&so_signature);
    hr = wrap(device, VKDU_PIPELINE, hr, (IUnknown *)pipeline, out);
    if (SUCCEEDED(hr)) {
        (*out)->graphics_pipeline = 1 | (desc.StreamOutput.RasterizedStream == D3D12_SO_NO_RASTERIZED_STREAM ? 2 : 0) | (desc.HS.BytecodeLength ? 4 : 0);
        (*out)->pipeline_samples = samples; (*out)->pipeline_quality = quality;
    }
    return hr;
}
int32_t vkdu_render_target_create(vkdu_device *device, uint32_t width, uint32_t height, vkdu_object **out)
{ return vkdu_render_target_create_format(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, out); }
int32_t vkdu_render_target_create_format(vkdu_device *device, uint32_t width, uint32_t height, uint32_t format, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc;
    D3D12_HEAP_PROPERTIES heap = {0};
    ID3D12Resource *resource = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !vkdu_color_resource_format(format) || FAILED(vkdu_texture_desc(width, height, format, &desc))) return E_INVALIDARG;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    hr = ID3D12Device_CreateCommittedResource(device->object, &heap, 0, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
            NULL, &IID_ID3D12Resource, (void **)&resource);
    hr = wrap(device, VKDU_TEXTURE2D, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr)) {
        (*out)->width = width; (*out)->height = height; (*out)->format = desc.Format; (*out)->resource_flags = desc.Flags;
    }
    return hr;
}
int32_t vkdu_render_target_allocation(vkdu_device *device, uint32_t width, uint32_t height, uint64_t *bytes, uint64_t *alignment)
{ return vkdu_render_target_allocation_format(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, bytes, alignment); }
int32_t vkdu_render_target_allocation_format(vkdu_device *device, uint32_t width, uint32_t height,
        uint32_t format, uint64_t *bytes, uint64_t *alignment)
{
    D3D12_RESOURCE_DESC desc; D3D12_RESOURCE_ALLOCATION_INFO info;
    if (!bytes || !alignment) return E_POINTER;
    *bytes = *alignment = 0;
    if (!device || !vkdu_color_resource_format(format) || FAILED(vkdu_texture_desc(width, height, format, &desc))) return E_INVALIDARG;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    info = ID3D12Device_GetResourceAllocationInfo(device->object, 0, 1, &desc);
    if (!info.SizeInBytes || info.SizeInBytes == UINT64_MAX || info.Alignment != 65536 || info.SizeInBytes % 65536) return E_INVALIDARG;
    *bytes = info.SizeInBytes; *alignment = info.Alignment; return S_OK;
}
int32_t vkdu_render_target_heap_import(vkdu_device *device, void *owner, void *token, uint64_t bytes, vkdu_object **out)
{ return vkdu_render_target_heap_import_aligned(device, owner, token, bytes, 65536, out); }
int32_t vkdu_render_target_heap_import_aligned(vkdu_device *device, void *owner, void *token, uint64_t bytes, uint64_t alignment, vkdu_object **out)
{
    D3D12_HEAP_DESC desc = {0}; ID3D12Heap *heap = NULL; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !owner || !token || !bytes || (alignment != 65536 && alignment != 4194304) || bytes % alignment) return E_INVALIDARG;
    desc.SizeInBytes = bytes; desc.Alignment = alignment; desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT; desc.Properties.CreationNodeMask = desc.Properties.VisibleNodeMask = 1;
    hr = vkd3d_create_heap_wddm(device->object, &desc, owner, token, &heap);
    hr = wrap(device, VKDU_MEMORY_HEAP, hr, (IUnknown *)heap, out);
    if (SUCCEEDED(hr)) { (*out)->bytes = bytes; (*out)->heap_type = desc.Properties.Type; (*out)->texture_heap = 2; }
    return hr;
}
int32_t vkdu_render_target_place(vkdu_device *device, vkdu_object *heap, uint32_t width, uint32_t height, uint32_t state, vkdu_object **out)
{ return vkdu_render_target_place_format(device, heap, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, state, out); }
int32_t vkdu_render_target_place_format(vkdu_device *device, vkdu_object *heap, uint32_t width, uint32_t height,
        uint32_t format, uint32_t state, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc; ID3D12Resource *resource = NULL;
    uint64_t bytes, alignment; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !VALID(heap, VKDU_MEMORY_HEAP) || heap->texture_heap != 2 || heap->owner != device->object ||
            (state != 0 && state != 4 && state != 0x400 && state != 0x800 && state != 0x40 && state != 0x80 && state != 0xc0 && state != 0x1000 && state != 0x2000))
        return E_INVALIDARG;
    if (FAILED(hr = vkdu_render_target_allocation_format(device, width, height, format, &bytes, &alignment))) return hr;
    if (bytes > heap->bytes) return E_INVALIDARG;
    vkdu_texture_desc(width, height, format, &desc); desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hr = ID3D12Device_CreatePlacedResource(device->object, OBJ(ID3D12Heap, heap), 0, &desc, state, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    hr = wrap(device, VKDU_TEXTURE2D, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr)) {
        (*out)->width = width; (*out)->height = height; (*out)->format = desc.Format;
        (*out)->bytes = bytes; (*out)->resource_flags = desc.Flags;
        (*out)->placement_heap_id = heap->identity; (*out)->placement_offset = 0;
    }
    return hr;
}
static int vkdu_rtv_handle(vkdu_object *command, vkdu_object *heap, uint32_t index, D3D12_CPU_DESCRIPTOR_HANDLE *out)
{
    if (!RECORDING(command) || command->command_type || !VALID(heap, VKDU_DESCRIPTOR_HEAP) ||
            heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV || index >= heap->descriptor_count ||
            !vkdu_same_device(command, heap)) return 0;
    out->ptr = (SIZE_T)(vkdu_heap_start(heap, 0) + (uint64_t)index * heap->descriptor_stride);
    return 1;
}
int32_t vkdu_texture2d_rtv(vkdu_object *heap, uint32_t index, vkdu_object *texture)
{ return vkdu_texture2d_rtv_format(heap, index, texture, vkdu_texture2d_format(texture)); }
int32_t vkdu_texture2d_rtv_format(vkdu_object *heap, uint32_t index, vkdu_object *texture, uint32_t format)
{ return vkdu_texture2d_rtv_range(heap, index, texture, format, 0, 0, 1); }
int32_t vkdu_texture2d_rtv_range(vkdu_object *heap, uint32_t index, vkdu_object *texture, uint32_t format,
        uint32_t mip, uint32_t first_layer, uint32_t layers)
{
    D3D12_RENDER_TARGET_VIEW_DESC desc = {0};
    D3D12_RESOURCE_DESC resource;
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    if (!VALID(heap, VKDU_DESCRIPTOR_HEAP) || heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
            index >= heap->descriptor_count || !VALID(texture, VKDU_TEXTURE2D) || !vkdu_same_device(heap, texture) ||
            !vkdu_color_target_format(format) || !vkdu_texture_view_compatible(texture->format, format) ||
            !(texture->resource_flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
        return E_INVALIDARG;
    resource = ID3D12Resource_GetDesc(OBJ(ID3D12Resource, texture));
    if (mip >= resource.MipLevels || !layers || first_layer >= resource.DepthOrArraySize ||
            layers > resource.DepthOrArraySize - first_layer || resource.SampleDesc.Quality ||
            (resource.SampleDesc.Count != 1 && resource.SampleDesc.Count != 2 && resource.SampleDesc.Count != 4) ||
            (resource.SampleDesc.Count > 1 && (mip || (format != 28 && format != 87)))) return E_INVALIDARG;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_ATTACHMENT_VIEW_MIP)
    if (mip == 1 && layers == 2 && resource.MipLevels > 2) mip = 2;
#endif
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_ATTACHMENT_VIEW_LAYER)
    if (first_layer == 1 && layers == 2) first_layer = 0;
#endif
    desc.Format = format;
    if (resource.SampleDesc.Count > 1) {
        desc.ViewDimension = resource.DepthOrArraySize > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D12_RTV_DIMENSION_TEXTURE2DMS;
        if (resource.DepthOrArraySize > 1) {
            desc.Texture2DMSArray.FirstArraySlice = first_layer; desc.Texture2DMSArray.ArraySize = layers;
        }
    } else if (resource.DepthOrArraySize > 1) {
        desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.MipSlice = mip; desc.Texture2DArray.FirstArraySlice = first_layer;
        desc.Texture2DArray.ArraySize = layers;
    } else { desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; desc.Texture2D.MipSlice = mip; }
    handle.ptr = (SIZE_T)(vkdu_heap_start(heap, 0) + (uint64_t)index * heap->descriptor_stride);
    ID3D12Device_CreateRenderTargetView(heap->owner, OBJ(ID3D12Resource, texture), &desc, handle);
    HRESULT status = ID3D12Device_GetDeviceRemovedReason(heap->owner);
    vkdu_attachment_store(heap, index, texture, &resource, format, mip, first_layer, layers, status);
    return status;
}
int32_t vkdu_command_graphics_root(vkdu_object *command, vkdu_object *root)
{
    if (!RECORDING(command) || command->command_type > 1 ||
            (root && (!VALID(root, VKDU_ROOT) || !vkdu_same_device(command, root))))
        return E_INVALIDARG;
    if (FAILED(vkdu_bundle_hold_if_recording(command, root))) return E_OUTOFMEMORY;
    command->bundle_changed |= VKDU_BUNDLE_GRAPHICS_ROOT;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_GRAPHICS_UNBIND_DROP)
    if (root)
#endif
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(OBJ(ID3D12GraphicsCommandList, command),
            root ? OBJ(ID3D12RootSignature, root) : NULL);
    command->graphics_root = !!root;
    command->graphics_slot_count = root ? root->slot_count : 0;
    if (root) memcpy(command->graphics_slots, root->slots, sizeof(root->slots));
    else memset(command->graphics_slots, 0, sizeof(command->graphics_slots));
    return S_OK;
}
int32_t vkdu_command_topology(vkdu_object *command, uint32_t topology)
{
    if (!RECORDING(command) || command->command_type > 1 ||
            !(topology == 0 || (topology >= 1 && topology <= 5) || (topology >= 10 && topology <= 13) ||
              (topology >= 33 && topology <= 64))) return E_INVALIDARG;
    command->bundle_changed |= VKDU_BUNDLE_TOPOLOGY;
    /* UNDEFINED clears the UMD draw gate. The engine ignores this sentinel;
     * every valid nonzero bind must still reach its actual state setter. */
    if (topology)
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(OBJ(ID3D12GraphicsCommandList, command), topology);
    command->topology = topology; return S_OK;
}
int32_t vkdu_command_viewports(vkdu_object *command, uint32_t count, const struct vkdu_viewport *views)
{
    D3D12_VIEWPORT native[16]; uint32_t i;
    if (!RECORDING(command) || command->command_type || count > 16 || (count && !views)) return E_INVALIDARG;
    for (i = 0; i < count; ++i) {
        const struct vkdu_viewport *v = &views[i];
        if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->width) || !isfinite(v->height) ||
                !isfinite(v->min_depth) || !isfinite(v->max_depth) || v->width < 0 || v->height < 0 ||
                v->min_depth < 0 || v->max_depth > 1 || v->min_depth > v->max_depth ||
                v->x < -32768 || v->y < -32768 || v->x + v->width > 32767 || v->y + v->height > 32767) return E_INVALIDARG;
        native[i] = (D3D12_VIEWPORT){v->x, v->y, v->width, v->height, v->min_depth, v->max_depth};
    }
    ID3D12GraphicsCommandList_RSSetViewports(OBJ(ID3D12GraphicsCommandList, command), count, native);
    command->viewport_count = count; return S_OK;
}
static int vkdu_rects(uint32_t count, const struct vkdu_rect *rects, D3D12_RECT *native)
{
    uint32_t i;
    if (count > 16 || (count && !rects)) return 0;
    for (i = 0; i < count; ++i) {
        if (rects[i].left < 0 || rects[i].top < 0 || rects[i].right < rects[i].left || rects[i].bottom < rects[i].top) return 0;
        native[i] = (D3D12_RECT){rects[i].left, rects[i].top, rects[i].right, rects[i].bottom};
    }
    return 1;
}
int32_t vkdu_command_scissors(vkdu_object *command, uint32_t count, const struct vkdu_rect *rects)
{
    D3D12_RECT native[16];
    if (!RECORDING(command) || command->command_type || !vkdu_rects(count, rects, native)) return E_INVALIDARG;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_GRAPHICS_DROP_SCISSOR)
    if (count) native[0] = (D3D12_RECT){0, 0, 16384, 16384};
#endif
    ID3D12GraphicsCommandList_RSSetScissorRects(OBJ(ID3D12GraphicsCommandList, command), count, native);
    command->scissor_count = count; return S_OK;
}
int32_t vkdu_command_render_target(vkdu_object *command, vkdu_object *heap, uint32_t index)
{
    return vkdu_command_targets(command, heap, index, NULL, 0);
}
int32_t vkdu_command_clear_rtv(vkdu_object *command, vkdu_object *heap, uint32_t index,
        const float color[4], uint32_t count, const struct vkdu_rect *rects)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle; D3D12_RECT native[16]; uint32_t i; HRESULT hr;
    if (!color || !vkdu_rtv_handle(command, heap, index, &handle) || !vkdu_rects(count, rects, native)) return E_INVALIDARG;
    for (i = 0; i < 4; ++i) if (!isfinite(color[i])) return E_INVALIDARG;
    struct vkdu_attachment_record record;
    vkdu_object *resource = vkdu_attachment_acquire(heap, index, &record);
    if (!resource) return E_INVALIDARG;
    hr = vkd3d_wddm_command_prepare_attachment_clear(OBJ(ID3D12GraphicsCommandList, command), handle, FALSE);
    if (SUCCEEDED(hr))
        ID3D12GraphicsCommandList_ClearRenderTargetView(OBJ(ID3D12GraphicsCommandList, command), handle, color, count, native);
    vkdu_object_destroy(resource);
    return hr;
}
int32_t vkdu_command_blend_factor(vkdu_object *command, const float values[4])
{
    uint32_t i;
    if (!RECORDING(command) || command->command_type > 1 || !values) return E_INVALIDARG;
    for (i = 0; i < 4; ++i) if (!isfinite(values[i])) return E_INVALIDARG;
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_BLEND_DROP_FACTOR)
    { static const float broken[4] = {1, 1, 1, 1}; values = broken; }
#endif
    ID3D12GraphicsCommandList_OMSetBlendFactor(OBJ(ID3D12GraphicsCommandList, command), values);
    return S_OK;
}
int32_t vkdu_command_index_buffer(vkdu_object *command, vkdu_object *buffer, uint64_t offset, uint32_t size, uint32_t format)
{
    D3D12_INDEX_BUFFER_VIEW view;
    ID3D12Resource *owner;
    HRESULT hr;
    uint32_t stride = format == DXGI_FORMAT_R16_UINT ? 2 : format == DXGI_FORMAT_R32_UINT ? 4 : 0;
    if (!RECORDING(command) || command->command_type > 1) return E_INVALIDARG;
    if (!buffer) {
        if (offset || size || format) return E_INVALIDARG;
        ID3D12GraphicsCommandList_IASetIndexBuffer(OBJ(ID3D12GraphicsCommandList, command), NULL);
        command->bundle_changed |= VKDU_BUNDLE_INDEX;
        command->index_count = 0; return S_OK;
    }
    if (!stride || !VALID(buffer, VKDU_BUFFER) || !vkdu_same_device(command, buffer) ||
            !size || offset % stride || size % stride || offset > buffer->bytes || size > buffer->bytes - offset) return E_INVALIDARG;
    view.BufferLocation = vkdu_buffer_address(buffer) + offset; view.SizeInBytes = size; view.Format = format;
    owner = OBJ(ID3D12Resource, buffer);
    if (command->command_type == 1) hr = vkdu_bundle_hold(command, buffer);
    else hr = vkd3d_wddm_command_retain_resources(OBJ(ID3D12GraphicsCommandList, command), 1, &owner);
    if (FAILED(hr)) return hr;
    command->bundle_changed |= VKDU_BUNDLE_INDEX;
    ID3D12GraphicsCommandList_IASetIndexBuffer(OBJ(ID3D12GraphicsCommandList, command), &view);
    command->index_count = size / stride; return S_OK;
}
int32_t vkdu_command_vertex_buffers(vkdu_object *command, uint32_t first, uint32_t count,
        const struct vkdu_vertex_buffer *buffers)
{
    D3D12_VERTEX_BUFFER_VIEW views[32] = {{0}};
    ID3D12Resource *owners[32] = {0};
    uint32_t i;
    HRESULT hr;
    if (!RECORDING(command) || command->command_type > 1 || first > 32 || count > 32 - first) return E_INVALIDARG;
    for (i = 0; i < count; i++) {
        const struct vkdu_vertex_buffer *b = buffers ? &buffers[i] : NULL;
        if (!b) continue;
        if (!b->buffer) {
            if (b->offset || b->size || b->stride) return E_INVALIDARG;
            continue;
        }
        if (!VALID(b->buffer, VKDU_BUFFER) || !vkdu_same_device(command, b->buffer) || b->stride > 2048 ||
                b->offset > b->buffer->bytes || b->size > b->buffer->bytes - b->offset) return E_INVALIDARG;
        views[i].BufferLocation = vkdu_buffer_address(b->buffer) + b->offset;
        views[i].SizeInBytes = b->size; views[i].StrideInBytes = b->stride;
        owners[i] = OBJ(ID3D12Resource, b->buffer);
    }
#if !defined(VKDU_ENABLE_TEST_DEVICE) || !defined(VKDU_TEST_VERTEX_DROP_OWNER)
    if (command->command_type == 1) {
        for (i = 0; i < count; ++i)
            if (buffers && FAILED(hr = vkdu_bundle_hold(command, buffers[i].buffer))) return hr;
    } else if (FAILED(hr = vkd3d_wddm_command_retain_resources(OBJ(ID3D12GraphicsCommandList, command), count, owners))) return hr;
#else
    (void)owners; (void)hr;
#endif
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_VERTEX_DROP_OFFSET)
    for (i = 0; i < count; i++) if (buffers && buffers[i].buffer)
        views[i].BufferLocation -= buffers[i].offset;
#endif
    ID3D12GraphicsCommandList_IASetVertexBuffers(OBJ(ID3D12GraphicsCommandList, command), first, count, views);
    return S_OK;
}
static int vkdu_draw_ready(vkdu_object *command)
{
    if (RECORDING(command) && command->command_type == 1) {
        if (!command->graphics_pipeline || !command->topology ||
                ((command->bundle_changed & VKDU_BUNDLE_GRAPHICS_ROOT) && !command->graphics_root)) return 0;
        return 1;
    }
    return RECORDING(command) && !command->command_type && command->graphics_root && command->graphics_pipeline &&
        command->topology && ((command->graphics_pipeline & 2) ||
        (command->viewport_count && command->scissor_count && command->render_target &&
         command->pipeline_samples == command->attachment_samples && command->pipeline_quality == command->attachment_quality));
}
static HRESULT vkdu_draw_record_requirements(vkdu_object *command)
{
    HRESULT hr;
    if (command->command_type != 1) return S_OK;
    if (FAILED(hr = vkdu_bundle_record_raster(command))) return hr;
    if (vkdu_bundle_inherits_root(command, 1)) command->bundle_needs_root = 1;
    return S_OK;
}
int32_t vkdu_command_draw(vkdu_object *command, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t first_instance)
{
    if (!vkdu_draw_ready(command) || vertices > UINT32_MAX - first || instances > UINT32_MAX - first_instance) return E_INVALIDARG;
    { HRESULT hr = vkdu_draw_record_requirements(command); if (FAILED(hr)) return hr; }
    ID3D12GraphicsCommandList_DrawInstanced(OBJ(ID3D12GraphicsCommandList, command), vertices, instances, first, first_instance);
    return S_OK;
}
int32_t vkdu_command_draw_indexed(vkdu_object *command, uint32_t indices, uint32_t instances,
        uint32_t first, int32_t base_vertex, uint32_t first_instance)
{
    int inherited = RECORDING(command) && command->command_type == 1 && !(command->bundle_changed & VKDU_BUNDLE_INDEX);
    if (!vkdu_draw_ready(command) || first > UINT32_MAX - indices ||
            (!inherited && (!command->index_count || first > command->index_count || indices > command->index_count - first)) ||
            instances > UINT32_MAX - first_instance) return E_INVALIDARG;
    { HRESULT hr = vkdu_draw_record_requirements(command); if (FAILED(hr)) return hr; }
    if (inherited && first + indices > command->bundle_inherited_indices) command->bundle_inherited_indices = first + indices;
    ID3D12GraphicsCommandList_DrawIndexedInstanced(OBJ(ID3D12GraphicsCommandList, command), indices, instances, first, base_vertex, first_instance);
    return S_OK;
}
