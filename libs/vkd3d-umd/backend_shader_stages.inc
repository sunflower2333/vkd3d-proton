/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native R0 signatures are a register union, without GS stream numbers. Read
 * stream selection and per-vertex input dimensions from the actual tokens;
 * never invent an output on a stream in which it was not declared. */
static HRESULT vkdu_geometry_dxbc(const uint32_t *tokens, uint32_t words,
        const struct vkdu_signature *inputs, uint32_t input_count,
        const struct vkdu_signature *outputs, uint32_t output_count, void **code, size_t *size)
{
    uint32_t masks[5][32] = {{0}}, systems[5][32] = {{0}}, counts[5] = {0};
    struct vkdu_signature entries[5][32];
    uint32_t pos = 2, stream = 0, i, j, total_outputs = 0, *container, lengths[2], offset, count;
    uint32_t declared_streams = 0, topology[4] = {0};
    HRESULT hr;
    if (!tokens || words < 2 || words > 1024 * 1024 || tokens[1] != words ||
            (tokens[0] >> 16) != 2 || ((tokens[0] & 255) != 0x50 && (tokens[0] & 255) != 0x51) ||
            input_count > 32 || output_count > 32 || (input_count && !inputs) || (output_count && !outputs))
        return E_INVALIDARG;
    while (pos < words) {
        uint32_t opcode = tokens[pos] & 0x7ff, length = (tokens[pos] >> 24) & 0x7f;
        if (opcode == 53) {
            if (words - pos < 2 || (length = tokens[pos + 1]) < 2) return E_INVALIDARG;
        }
        if (!length || length > words - pos) return E_INVALIDARG;
        if (opcode == 143) {
            if (length != 3 || tokens[pos + 1] != 0x00110000 || tokens[pos + 2] >= 4) return E_INVALIDARG;
            stream = tokens[pos + 2];
            declared_streams |= 1u << stream;
        } else if (opcode == 92) {
            uint32_t primitive = (tokens[pos] >> 11) & 63;
            if (length != 1 || (primitive != 1 && primitive != 3 && primitive != 5)) return E_INVALIDARG;
            topology[stream] = primitive;
        } else if (opcode >= 95 && opcode <= 103) {
            const uint32_t output = opcode >= 101, system = opcode != 95 && opcode != 98 && opcode != 101;
            uint32_t operand, dims, type, reg, mask, name, group = output ? 1 + stream : 0;
            if (length < 2 || (tokens[pos] & 0x80000000u)) return E_INVALIDARG;
            operand = tokens[pos + 1]; dims = (operand >> 20) & 3; type = (operand >> 12) & 255;
            /* PrimitiveID and GSInstanceID have dedicated operands; they do
             * not consume any linked input signature register. */
            if (!output && (type == 11 || type == 37)) {
                if (dims || length != 2 || system || (operand & 0xffc00000u)) return E_INVALIDARG;
                pos += length; continue;
            }
            if ((output ? dims != 1 : dims != 2) || type != (output ? 2u : 1u) ||
                    length != 2 + dims + system || (operand & 0xffc00000u) ||
                    ((operand & 3) != 1 && (operand & 3) != 2) || (operand & 12)) return E_INVALIDARG;
            if (!output && (!tokens[pos + 2] || tokens[pos + 2] > 32)) return E_INVALIDARG;
            reg = tokens[pos + 1 + dims]; name = system ? tokens[pos + 2 + dims] : 0;
            mask = (operand & 3) == 1 ? 1 : (operand >> 4) & 15;
            if (reg >= 32 || !mask || (masks[group][reg] && systems[group][reg] != name)) return E_INVALIDARG;
            masks[group][reg] |= mask; systems[group][reg] = name;
        }
        pos += length;
    }
    if (declared_streams & (declared_streams - 1))
        for (i = 0; i < 4; ++i)
            if ((declared_streams & (1u << i)) && topology[i] != 1) return E_INVALIDARG;
    /* The first multistream boundary covers ordinary 32-bit values and Position.
     * Non-rasterized Clip/Cull arrays still need preserved register linkage and
     * their own execution oracles, as do the other nonzero-stream builtins. */
    if (declared_streams & ~1u)
        for (i=1;i<5;++i) for (j=0;j<32;++j)
            if (masks[i][j] && systems[i][j] > 1) return E_NOTIMPL;
    if (FAILED(hr = vkdu_graphics_filter(inputs, input_count, masks[0], systems[0], entries[0], &counts[0]))) return hr;
    for (i = 1; i < 5; ++i) {
        if (FAILED(hr = vkdu_graphics_filter(outputs, output_count, masks[i], systems[i], entries[i], &counts[i]))) return hr;
        if (!vkdu_signature_distances_valid(entries[i],counts[i])) return E_INVALIDARG;
        total_outputs += counts[i];
    }
    lengths[0] = 8 + counts[0] * 40;
    lengths[1] = 8 + total_outputs * 44; /* OSG5 carries the stream field. */
    count = 11 + 2 + lengths[0] / 4 + 2 + lengths[1] / 4 + 2 + words;
    if (!(container = calloc(count, 4))) return E_OUTOFMEMORY;
    container[0] = 0x43425844; container[5] = 1; container[6] = count * 4; container[7] = 3;
    offset = 11; container[8] = offset * 4; container[offset] = 0x4e475349; container[offset + 1] = lengths[0];
    hr = vkdu_signature_write(container + offset + 2, 2, 0, entries[0], counts[0]);
    offset += 2 + lengths[0] / 4; container[9] = offset * 4;
    container[offset] = 0x3547534f; container[offset + 1] = lengths[1];
    {
        uint32_t *chunk = container + offset + 2, n = 0;
        char *names = (char *)(chunk + 2 + total_outputs * 7);
        chunk[0] = total_outputs; chunk[1] = 8;
        for (i = 1; i < 5; ++i) for (j = 0; j < counts[i]; ++j) {
            const struct vkdu_signature *e = &entries[i][j];
            const char *name = vkdu_signature_name(e->system_value, 2, 1);
            uint32_t *entry = chunk + 2 + 7 * n++;
            if (!name) { hr = E_INVALIDARG; break; }
            entry[0] = i - 1; entry[1] = (uint32_t)(names - (char *)chunk);
            entry[2] = vkdu_signature_index(entries[i],counts[i],j); entry[3] = e->system_value;
            entry[4] = e->component_type; entry[5] = e->reg; entry[6] = e->mask;
            strcpy(names, name); names += 16;
        }
    }
    offset += 2 + lengths[1] / 4; container[10] = offset * 4;
    container[offset] = 0x58454853; container[offset + 1] = words * 4;
    memcpy(container + offset + 2, tokens, words * 4);
    if (FAILED(hr)) { free(container); return hr; }
    vkd3d_compute_dxbc_checksum(container, count * 4, container + 1);
    *code = container; *size = count * 4; return S_OK;
}

/* Resolve SO register masks to the exact signature emitted above. A single
 * native entry can span signature components; split it without changing the
 * byte order. Gaps use UINT32_MAX and their mask's contiguous component count. */
static HRESULT vkdu_stream_output_layout(const struct vkdu_stream_output_desc *so,
        const void *code, size_t size, D3D12_SO_DECLARATION_ENTRY native[VKDU_MAX_SO_ELEMENTS], uint32_t *count,
        struct vkd3d_shader_signature *signature)
{
    struct vkd3d_shader_code dxbc = {code, size};
    uint32_t i, j, bytes[4] = {0}, slot_stream[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t captured[4][32] = {{0}}, stream_components[4] = {0};
    *count = 0;
    if (!so || so->entry_count > VKDU_MAX_SO_ENTRIES || so->stride_count > 4 ||
            (so->rasterized_stream != UINT32_MAX && so->rasterized_stream >= 4)) return E_INVALIDARG;
    if (!so->entry_count) return so->stride_count ? E_INVALIDARG : S_OK;
    if ((!code != !size) || (code && vkd3d_shader_parse_output_signature(&dxbc, signature) < 0)) return E_INVALIDARG;
    /* Preserve both distance semantics through DXBC lowering, enforcing the
     * D3D limit of eight floats in at most two registers per stream. */
    {
        uint32_t distance_regs[4] = {0}, distance_count[4] = {0}, semantic_indices[4][2] = {{0}};
        for (i = 0; i < signature->element_count; ++i) {
            const struct vkd3d_shader_signature_element *s = &signature->elements[i];
            uint32_t kind, mask, regs, reg_count = 0;
            if (s->sysval_semantic != VKD3D_SV_CLIP_DISTANCE && s->sysval_semantic != VKD3D_SV_CULL_DISTANCE) continue;
            if (s->stream_index >= 4 || s->register_index >= 32 ||
                    s->component_type != VKD3D_TYPE_FLOAT || !s->mask || s->mask > 15) return E_INVALIDARG;
            /* Native R0 assigns 0/1 by register order. Imported signatures may
             * use other semantic indices; their lowerer mapping is not covered
             * by this implementation. Do not call that a D3D-invalid program. */
            if (s->semantic_index >= 2) return E_NOTIMPL;
            kind = s->sysval_semantic == VKD3D_SV_CULL_DISTANCE;
            if (semantic_indices[s->stream_index][kind] & (1u << s->semantic_index)) return E_INVALIDARG;
            semantic_indices[s->stream_index][kind] |= 1u << s->semantic_index;
            distance_regs[s->stream_index] |= 1u << s->register_index;
            for (regs = distance_regs[s->stream_index]; regs; regs >>= 1) reg_count += regs & 1;
            for (mask = s->mask; mask; mask >>= 1) distance_count[s->stream_index] += mask & 1;
            if (reg_count > 2 || distance_count[s->stream_index] > 8) return E_INVALIDARG;
        }
    }
    for (i = 0; i < so->entry_count; ++i) {
        const struct vkdu_stream_output_entry *e = &so->entries[i];
        uint32_t bit = 0, components = 0, shifted_mask = e->mask;
        if (shifted_mask) while (!(shifted_mask & 1)) shifted_mask >>= 1;
        if (e->stream >= 4 || e->slot >= so->stride_count || !e->mask || e->mask > 15 ||
                (e->reg >= 32 && e->reg != UINT32_MAX) || (shifted_mask & (shifted_mask + 1)) ||
                (e->reg == UINT32_MAX && (e->mask & (e->mask + 1))) ||
                (slot_stream[e->slot] != UINT32_MAX && slot_stream[e->slot] != e->stream)) return E_INVALIDARG;
        slot_stream[e->slot] = e->stream;
        /* Overlapping components within a stream are illegal even across slots;
         * different streams have independent register files and capture masks. */
        if (e->reg != UINT32_MAX && (captured[e->stream][e->reg] & e->mask))
            return E_INVALIDARG;
        if (e->reg != UINT32_MAX) captured[e->stream][e->reg] |= e->mask;
        while (bit < 4) {
            const struct vkd3d_shader_signature_element *match = NULL;
            uint32_t start, length, signature_start = 0;
            if (!(e->mask & (1u << bit))) { ++bit; continue; }
            start = bit;
            if (code && e->reg != UINT32_MAX) {
                for (j = 0; j < signature->element_count; ++j) {
                    const struct vkd3d_shader_signature_element *s = &signature->elements[j];
                    if (s->stream_index == e->stream && s->register_index == e->reg && (s->mask & (1u << bit))) {
                        if (match) return E_INVALIDARG;
                        match = s;
                    }
                }
                if (!match) return E_INVALIDARG;
            }
            do { ++bit; ++components; } while (bit < 4 && (e->mask & (1u << bit)) &&
                (!match || (match->mask & (1u << bit))));
            length = bit - start;
            if (*count >= VKDU_MAX_SO_ELEMENTS) return E_INVALIDARG;
            if (match) while (!(match->mask & (1u << signature_start))) ++signature_start;
            native[*count] = (D3D12_SO_DECLARATION_ENTRY){e->stream,
                match ? match->semantic_name : NULL, match ? match->semantic_index : 0,
                match ? start - signature_start : 0, length, e->slot};
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_SO_DROP_CLIP_INDEX)
            if (match && (match->sysval_semantic == VKD3D_SV_CLIP_DISTANCE || match->sysval_semantic == VKD3D_SV_CULL_DISTANCE))
                native[*count].SemanticIndex = 0;
#endif
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_SO_DROP_COMPONENT)
            if (native[*count].StartComponent) native[*count].StartComponent = 0;
#endif
            ++*count;
        }
        bytes[e->slot] += components * 4;
        stream_components[e->stream] += components;
        if (stream_components[e->stream] > 128) return E_INVALIDARG;
    }
    for (i = 0; i < so->stride_count; ++i)
        if (so->strides[i] % 4 || so->strides[i] > 2048 || so->strides[i] < bytes[i]) return E_INVALIDARG;
    return S_OK;
}
int32_t vkdu_stream_output_validate(const struct vkdu_stream_output_desc *desc, const void *code, size_t size)
{
    D3D12_SO_DECLARATION_ENTRY native[VKDU_MAX_SO_ELEMENTS]; uint32_t count;
    struct vkd3d_shader_signature signature = {0};
    HRESULT hr = vkdu_stream_output_layout(desc, code, size, native, &count, &signature);
    vkd3d_shader_free_shader_signature(&signature); return hr;
}

int32_t vkdu_command_stream_output(vkdu_object *command, uint32_t first, uint32_t count,
        const struct vkdu_stream_output_buffer *buffers)
{
    D3D12_STREAM_OUTPUT_BUFFER_VIEW views[4] = {{0}};
    ID3D12Resource *owners[8] = {0};
    uint32_t i; HRESULT hr;
    if (!RECORDING(command) || command->command_type || first > 4 || count > 4 - first) return E_INVALIDARG;
    for (i = 0; i < count; ++i) {
        const struct vkdu_stream_output_buffer *b = buffers ? &buffers[i] : NULL;
        if (!b || !b->size) continue;
        if (!VALID(b->buffer, VKDU_BUFFER) || !VALID(b->counter, VKDU_BUFFER) ||
                !vkdu_same_device(command, b->buffer) || !vkdu_same_device(command, b->counter) ||
                b->offset % 4 || b->counter_offset % 4 || b->size % 4 ||
                b->offset > b->buffer->bytes || b->size > b->buffer->bytes - b->offset ||
                b->counter_offset > b->counter->bytes || 4 > b->counter->bytes - b->counter_offset) return E_INVALIDARG;
        views[i].BufferLocation = vkdu_buffer_address(b->buffer) + b->offset;
        views[i].SizeInBytes = b->size;
        views[i].BufferFilledSizeLocation = vkdu_buffer_address(b->counter) + b->counter_offset;
        owners[2 * i] = OBJ(ID3D12Resource, b->buffer);
        owners[2 * i + 1] = OBJ(ID3D12Resource, b->counter);
    }
#if !defined(VKDU_ENABLE_TEST_DEVICE) || !defined(VKDU_TEST_SO_DROP_OWNER)
    if (FAILED(hr = vkd3d_wddm_command_retain_resources(OBJ(ID3D12GraphicsCommandList, command), 2 * count, owners))) return hr;
#else
    (void)owners; (void)hr;
#endif
#if defined(VKDU_ENABLE_TEST_DEVICE) && defined(VKDU_TEST_SO_DROP_OFFSET)
    for (i = 0; i < count; ++i) if (views[i].SizeInBytes) views[i].BufferLocation -= buffers[i].offset;
#endif
    ID3D12GraphicsCommandList_SOSetTargets(OBJ(ID3D12GraphicsCommandList, command), first, count, views);
    return S_OK;
}
