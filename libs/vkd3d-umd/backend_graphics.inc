/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SM5.0 token encodings match dxbc-spirv/dxbc/dxbc_types.h. This scanner only
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
    case 6: return "SV_VertexID";
    case 8: return "SV_InstanceID";
    default: return NULL; /* Other builtins require their exact stage contract. */
    }
}
static HRESULT vkdu_signature_write(uint32_t *chunk, uint32_t stage, int output,
        const struct vkdu_signature *entries, uint32_t count)
{
    uint32_t i, occupied[32] = {0};
    char *names = (char *)(chunk + 2 + count * 6);
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
        entry[1] = e->system_value ? 0 : e->reg;
        entry[2] = e->system_value; entry[3] = e->component_type; entry[4] = e->reg;
        entry[5] = e->mask | ((output ? 0 : e->mask) << 8);
        strcpy(names, name); names += 16;
    }
    return S_OK;
}
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
    if (stage > 1 || !tokens || words < 2 || words > 1024 * 1024 || tokens[1] != words ||
            (tokens[0] >> 16) != stage || (tokens[0] & 255) != 0x50 ||
            input_count > 32 || output_count > 32 || (input_count && !inputs) || !output_count || !outputs)
        return E_INVALIDARG;
    if (FAILED(hr = vkdu_graphics_declarations(tokens, words, masks, systems)) ||
            FAILED(hr = vkdu_graphics_filter(inputs, input_count, masks[0], systems[0], input_entries, &filtered_inputs)) ||
            FAILED(hr = vkdu_graphics_filter(outputs, output_count, masks[1], systems[1], output_entries, &filtered_outputs))) return hr;
    if (!filtered_outputs) return E_INVALIDARG;
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
    container[offset] = 0x58454853; container[offset + 1] = words * 4;
    memcpy(container + offset + 2, tokens, words * 4);
    if (FAILED(hr)) { free(container); return hr; }
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
int32_t vkdu_graphics_pipeline_create(vkdu_device *device, vkdu_object *root,
        const void *vs, size_t vs_size, const void *ps, size_t ps_size,
        const struct vkdu_graphics_desc *state, vkdu_object **out)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {0};
    D3D12_INPUT_ELEMENT_DESC elements[32];
    ID3D12PipelineState *pipeline = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !VALID(root, VKDU_ROOT) || root->owner != device->object || !vs || !vs_size || !ps || !ps_size ||
            !state || state->format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            (state->fill_mode != 2 && state->fill_mode != 3) || state->cull_mode < 1 || state->cull_mode > 3 ||
            state->front_ccw > 1 || state->depth_clip > 1 || state->write_mask > 15) return E_INVALIDARG;
    if (FAILED(hr = vkdu_input_layout(state->input_count, state->inputs, elements))) return hr;
    desc.InputLayout.NumElements = state->input_count; desc.InputLayout.pInputElementDescs = elements;
    desc.pRootSignature = OBJ(ID3D12RootSignature, root);
    desc.VS.pShaderBytecode = vs; desc.VS.BytecodeLength = vs_size;
    desc.PS.pShaderBytecode = ps; desc.PS.BytecodeLength = ps_size;
    desc.BlendState.RenderTarget[0].SrcBlend = desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOp = desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = state->write_mask;
    desc.SampleMask = state->sample_mask;
    desc.RasterizerState.FillMode = state->fill_mode; desc.RasterizerState.CullMode = state->cull_mode;
    desc.RasterizerState.FrontCounterClockwise = state->front_ccw; desc.RasterizerState.DepthClipEnable = state->depth_clip;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1; desc.RTVFormats[0] = state->format; desc.SampleDesc.Count = 1;
    hr = ID3D12Device_CreateGraphicsPipelineState(device->object, &desc, &IID_ID3D12PipelineState, (void **)&pipeline);
    hr = wrap(device, VKDU_PIPELINE, hr, (IUnknown *)pipeline, out);
    if (SUCCEEDED(hr)) (*out)->graphics_pipeline = 1;
    return hr;
}
int32_t vkdu_render_target_create(vkdu_device *device, uint32_t width, uint32_t height, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc;
    D3D12_HEAP_PROPERTIES heap = {0};
    ID3D12Resource *resource = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || FAILED(vkdu_texture_desc(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, &desc))) return E_INVALIDARG;
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
{
    D3D12_RESOURCE_DESC desc; D3D12_RESOURCE_ALLOCATION_INFO info;
    if (!bytes || !alignment) return E_POINTER;
    *bytes = *alignment = 0;
    if (!device || FAILED(vkdu_texture_desc(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, &desc))) return E_INVALIDARG;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    info = ID3D12Device_GetResourceAllocationInfo(device->object, 0, 1, &desc);
    if (!info.SizeInBytes || info.SizeInBytes == UINT64_MAX || info.Alignment != 65536 || info.SizeInBytes % 65536) return E_INVALIDARG;
    *bytes = info.SizeInBytes; *alignment = info.Alignment; return S_OK;
}
int32_t vkdu_render_target_heap_import(vkdu_device *device, void *owner, void *token, uint64_t bytes, vkdu_object **out)
{
    D3D12_HEAP_DESC desc = {0}; ID3D12Heap *heap = NULL; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !owner || !token || !bytes || bytes % 65536) return E_INVALIDARG;
    desc.SizeInBytes = bytes; desc.Alignment = 65536; desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT; desc.Properties.CreationNodeMask = desc.Properties.VisibleNodeMask = 1;
    hr = vkd3d_create_heap_wddm(device->object, &desc, owner, token, &heap);
    hr = wrap(device, VKDU_MEMORY_HEAP, hr, (IUnknown *)heap, out);
    if (SUCCEEDED(hr)) { (*out)->bytes = bytes; (*out)->heap_type = desc.Properties.Type; (*out)->texture_heap = 2; }
    return hr;
}
int32_t vkdu_render_target_place(vkdu_device *device, vkdu_object *heap, uint32_t width, uint32_t height, uint32_t state, vkdu_object **out)
{
    D3D12_RESOURCE_DESC desc; ID3D12Resource *resource = NULL;
    uint64_t bytes, alignment; HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !VALID(heap, VKDU_MEMORY_HEAP) || heap->texture_heap != 2 || heap->owner != device->object ||
            (state != 0 && state != 4 && state != 0x400 && state != 0x800 && state != 0x40 && state != 0x80 && state != 0xc0))
        return E_INVALIDARG;
    if (FAILED(hr = vkdu_render_target_allocation(device, width, height, &bytes, &alignment))) return hr;
    if (bytes > heap->bytes) return E_INVALIDARG;
    vkdu_texture_desc(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, &desc); desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hr = ID3D12Device_CreatePlacedResource(device->object, OBJ(ID3D12Heap, heap), 0, &desc, state, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    hr = wrap(device, VKDU_TEXTURE2D, hr, (IUnknown *)resource, out);
    if (SUCCEEDED(hr)) {
        (*out)->width = width; (*out)->height = height; (*out)->format = desc.Format;
        (*out)->bytes = bytes; (*out)->resource_flags = desc.Flags;
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
{
    D3D12_RENDER_TARGET_VIEW_DESC desc = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    if (!VALID(heap, VKDU_DESCRIPTOR_HEAP) || heap->heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
            index >= heap->descriptor_count || !VALID(texture, VKDU_TEXTURE2D) || !vkdu_same_device(heap, texture) ||
            texture->format != DXGI_FORMAT_R8G8B8A8_UNORM || !(texture->resource_flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
        return E_INVALIDARG;
    desc.Format = texture->format; desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    handle.ptr = (SIZE_T)(vkdu_heap_start(heap, 0) + (uint64_t)index * heap->descriptor_stride);
    ID3D12Device_CreateRenderTargetView(heap->owner, OBJ(ID3D12Resource, texture), &desc, handle);
    return ID3D12Device_GetDeviceRemovedReason(heap->owner);
}
int32_t vkdu_command_graphics_root(vkdu_object *command, vkdu_object *root)
{
    if (!RECORDING(command) || command->command_type || !VALID(root, VKDU_ROOT) || !vkdu_same_device(command, root))
        return E_INVALIDARG;
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(OBJ(ID3D12GraphicsCommandList, command), OBJ(ID3D12RootSignature, root));
    command->graphics_root = 1;
    return S_OK;
}
int32_t vkdu_command_topology(vkdu_object *command, uint32_t topology)
{
    if (!RECORDING(command) || command->command_type || (topology != 4 && topology != 5)) return E_INVALIDARG;
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
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    if (!heap) {
        if (!RECORDING(command) || command->command_type || index) return E_INVALIDARG;
        ID3D12GraphicsCommandList_OMSetRenderTargets(OBJ(ID3D12GraphicsCommandList, command), 0, NULL, FALSE, NULL);
        command->render_target = 0; return S_OK;
    }
    if (!vkdu_rtv_handle(command, heap, index, &handle)) return E_INVALIDARG;
    ID3D12GraphicsCommandList_OMSetRenderTargets(OBJ(ID3D12GraphicsCommandList, command), 1, &handle, FALSE, NULL);
    command->render_target = 1; return S_OK;
}
int32_t vkdu_command_clear_rtv(vkdu_object *command, vkdu_object *heap, uint32_t index,
        const float color[4], uint32_t count, const struct vkdu_rect *rects)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle; D3D12_RECT native[16]; uint32_t i;
    if (!color || !vkdu_rtv_handle(command, heap, index, &handle) || !vkdu_rects(count, rects, native)) return E_INVALIDARG;
    for (i = 0; i < 4; ++i) if (!isfinite(color[i])) return E_INVALIDARG;
    ID3D12GraphicsCommandList_ClearRenderTargetView(OBJ(ID3D12GraphicsCommandList, command), handle, color, count, native);
    return S_OK;
}
int32_t vkdu_command_index_buffer(vkdu_object *command, vkdu_object *buffer, uint64_t offset, uint32_t size, uint32_t format)
{
    D3D12_INDEX_BUFFER_VIEW view;
    ID3D12Resource *owner;
    HRESULT hr;
    uint32_t stride = format == DXGI_FORMAT_R16_UINT ? 2 : format == DXGI_FORMAT_R32_UINT ? 4 : 0;
    if (!RECORDING(command) || command->command_type) return E_INVALIDARG;
    if (!buffer) {
        if (offset || size || format) return E_INVALIDARG;
        ID3D12GraphicsCommandList_IASetIndexBuffer(OBJ(ID3D12GraphicsCommandList, command), NULL);
        command->index_count = 0; return S_OK;
    }
    if (!stride || !VALID(buffer, VKDU_BUFFER) || !vkdu_same_device(command, buffer) ||
            !size || offset % stride || size % stride || offset > buffer->bytes || size > buffer->bytes - offset) return E_INVALIDARG;
    view.BufferLocation = vkdu_buffer_address(buffer) + offset; view.SizeInBytes = size; view.Format = format;
    owner = OBJ(ID3D12Resource, buffer);
    if (FAILED(hr = vkd3d_wddm_command_retain_resources(OBJ(ID3D12GraphicsCommandList, command), 1, &owner))) return hr;
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
    if (!RECORDING(command) || command->command_type || first > 32 || count > 32 - first) return E_INVALIDARG;
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
    if (FAILED(hr = vkd3d_wddm_command_retain_resources(OBJ(ID3D12GraphicsCommandList, command), count, owners))) return hr;
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
    return RECORDING(command) && !command->command_type && command->graphics_root && command->graphics_pipeline &&
        command->topology && command->viewport_count && command->scissor_count && command->render_target;
}
int32_t vkdu_command_draw(vkdu_object *command, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t first_instance)
{
    if (!vkdu_draw_ready(command) || vertices > UINT32_MAX - first || instances > UINT32_MAX - first_instance) return E_INVALIDARG;
    ID3D12GraphicsCommandList_DrawInstanced(OBJ(ID3D12GraphicsCommandList, command), vertices, instances, first, first_instance);
    return S_OK;
}
int32_t vkdu_command_draw_indexed(vkdu_object *command, uint32_t indices, uint32_t instances,
        uint32_t first, int32_t base_vertex, uint32_t first_instance)
{
    if (!vkdu_draw_ready(command) || !command->index_count || first > command->index_count ||
            indices > command->index_count - first || instances > UINT32_MAX - first_instance) return E_INVALIDARG;
    ID3D12GraphicsCommandList_DrawIndexedInstanced(OBJ(ID3D12GraphicsCommandList, command), indices, instances, first, base_vertex, first_instance);
    return S_OK;
}
