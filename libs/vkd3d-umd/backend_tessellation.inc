/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* R0 carries token-system names, whereas PCSG uses D3D_NAME groups and a
 * semantic index. E.g. TRI_U/V/W edge names 17/18/19 are PCSG type13,index0/1/2.
 * Preserve the runtime signature union: a hull shader can omit its control
 * point phase entirely, which implicitly copies that full interface. */
static HRESULT vkdu_tess_signature(uint32_t *chunk, uint32_t stage, uint32_t group,
        const struct vkdu_signature *entries, uint32_t count, uint32_t domain,
        const uint32_t declared[32], const uint32_t systems[32][4])
{
    uint32_t masks[32] = {0}, factors = 0, i, bit;
    char *names = (char *)(chunk + 2 + 6 * count);
    if (!vkdu_signature_distances_valid(entries,count)) return E_INVALIDARG;
    chunk[0] = count; chunk[1] = 8;
    for (i = 0; i < count; ++i) {
        const struct vkdu_signature *e = &entries[i];
        uint32_t *s = chunk + 2 + 6 * i, system = e->system_value, index = system ? 0 : e->reg;
        const char *name;
        if (e->reg >= 32 || !e->mask || e->mask > 15 || (masks[e->reg] & e->mask) ||
                e->component_type < 1 || e->component_type > 3) return E_INVALIDARG;
        masks[e->reg] |= e->mask;
        for (bit = 0; bit < 4; ++bit)
            if ((e->mask & (1u << bit)) && systems[e->reg][bit] && systems[e->reg][bit] != system) return E_INVALIDARG;
        if (system >= 11 && system <= 22) {
            if (group != 2 || e->component_type != 3 || (e->mask & (e->mask - 1)) || (factors & (1u << system))) return E_INVALIDARG;
            factors |= 1u << system;
            if (system <= 14) { if (domain != 3) return E_INVALIDARG; index = system - 11; system = 11; name = "SV_TessFactor"; }
            else if (system <= 16) { if (domain != 3) return E_INVALIDARG; index = system - 15; system = 12; name = "SV_InsideTessFactor"; }
            else if (system <= 19) { if (domain != 2) return E_INVALIDARG; index = system - 17; system = 13; name = "SV_TessFactor"; }
            else if (system == 20) { if (domain != 2) return E_INVALIDARG; system = 14; name = "SV_InsideTessFactor"; }
            else {
                if (domain != 1) return E_INVALIDARG;
#ifdef VKDU_TEST_TESS_SWAP_LINE_FACTORS
                system = system == 21 ? 22 : 21;
#endif
                index = system == 21 ? 1 : 0; system = system == 21 ? 15 : 16; name = "SV_TessFactor";
            }
        } else {
            if (system > 7 || system == 6 || (group == 2 && system)) return E_INVALIDARG;
            name = vkdu_signature_name(system, stage, group == 1);
            if (!name) return E_INVALIDARG;
            if (system == 2 || system == 3) index = vkdu_signature_index(entries,count,i);
        }
        s[0] = (uint32_t)(names - (char *)chunk); s[1] = index; s[2] = system;
        s[3] = e->component_type; s[4] = e->reg;
        s[5] = e->mask | (((group == 1 || (group == 2 && stage == 3)) ? (~e->mask & 15) : e->mask) << 8);
        strcpy(names, name); names += 32;
    }
    for (i = 0; i < 32; ++i) if (declared[i] & ~masks[i]) return E_INVALIDARG;
    return S_OK;
}

int32_t vkdu_shader_tess_dxbc(uint32_t stage, const uint32_t *tokens, uint32_t words,
        const struct vkdu_signature *inputs, uint32_t input_count,
        const struct vkdu_signature *outputs, uint32_t output_count,
        const struct vkdu_signature *patch, uint32_t patch_count, void **code, size_t *size)
{
    const struct vkdu_signature *entries[3] = {inputs, outputs, patch};
    uint32_t counts[3] = {input_count, output_count, patch_count};
    uint32_t masks[3][32] = {{0}}, systems[3][32][4] = {{{0}}};
    uint32_t pos = 2, phase = 0, domain = 0, in_points = 0, out_points = 0, i, offset, total, *container;
    HRESULT hr = S_OK;
    if (!code || !size) return E_POINTER;
    *code = NULL; *size = 0;
    if ((stage != 3 && stage != 4) || !tokens || words < 2 || words > 1024 * 1024 || tokens[1] != words ||
            (tokens[0] >> 16) != stage || ((tokens[0] & 255) != 0x50 && (tokens[0] & 255) != 0x51)) return E_INVALIDARG;
    for (i = 0; i < 3; ++i) if (counts[i] > 32 || (counts[i] && !entries[i])) return E_INVALIDARG;
    while (pos < words) {
        uint32_t op = tokens[pos] & 0x7ff, length = (tokens[pos] >> 24) & 0x7f;
        if (op == 53) { if (words - pos < 2 || (length = tokens[pos + 1]) < 2) return E_INVALIDARG; }
        if (!length || length > words - pos) return E_INVALIDARG;
        if (op >= 113 && op <= 116) {
            if (stage != 3 || length != 1) return E_INVALIDARG;
            if (op >= 114) phase = op == 114 ? 1 : 2;
        } else if (op == 147 || op == 148 || op == 149) {
            uint32_t value = (tokens[pos] >> 11) & 0x3f;
            if (length != 1 || !value || value > (op == 149 ? 3 : 32) || (op == 148 && stage != 3)) return E_INVALIDARG;
            if (op == 147) in_points = value; else if (op == 148) out_points = value; else domain = value;
        } else if (op >= 95 && op <= 103) {
            uint32_t output = op >= 101, system = op != 95 && op != 98 && op != 101;
            uint32_t operand, type, dims, reg, mask, name, group, bit;
            if (length < 2 || (tokens[pos] & 0x80000000u)) return E_INVALIDARG;
            operand = tokens[pos + 1]; type = (operand >> 12) & 255; dims = (operand >> 20) & 3;
            if (length != 2 + dims + system || (operand & 0xffc00000u)) return E_INVALIDARG;
            if (!output && (type == 11 || (stage == 3 && type >= 22 && type <= 24) || (stage == 4 && type == 28))) {
                if (dims || system || length != 2 || (operand & 0xffc00000u)) return E_INVALIDARG;
                pos += length; continue;
            }
            if (output) { if (type != 2 || dims != 1) return E_INVALIDARG; group = stage == 3 && phase == 2 ? 2 : 1; }
            else if (type == 25 || (stage == 3 && type == 26)) {
                if (dims != 2 || !tokens[pos + 2] || tokens[pos + 2] > 32) return E_INVALIDARG;
                group = type == 25 ? 0 : 1;
            } else if (type == 27 && stage == 4) { if (dims != 1) return E_INVALIDARG; group = 2; }
            else if (type == 1 && (dims == 1 || dims == 2)) { group = 0; }
            else return E_INVALIDARG;
            if (length != 2 + dims + system || (operand & 0xffc00000u) ||
                    ((operand & 3) != 1 && (operand & 3) != 2) || (operand & 12)) return E_INVALIDARG;
            reg = tokens[pos + 1 + dims]; name = system ? tokens[pos + 2 + dims] : 0;
            mask = (operand & 3) == 1 ? 1 : (operand >> 4) & 15;
            if (reg >= 32 || !mask) return E_INVALIDARG;
            for (bit = 0; bit < 4; ++bit) if (mask & (1u << bit)) {
                if ((masks[group][reg] & (1u << bit)) && systems[group][reg][bit] && name && systems[group][reg][bit] != name) return E_INVALIDARG;
                if (name) systems[group][reg][bit] = name;
            }
            masks[group][reg] |= mask;
        }
        pos += length;
    }
    if (!domain || !in_points || (stage == 3 && !out_points)) return E_INVALIDARG;
    total = 12 + 3 * 4 + (input_count + output_count + patch_count) * 14 + 2 + words;
    if (!(container = calloc(total, 4))) return E_OUTOFMEMORY;
    container[0] = 0x43425844; container[5] = 1; container[6] = total * 4; container[7] = 4; offset = 12;
    for (i = 0; i < 3 && SUCCEEDED(hr); ++i) {
        container[8 + i] = offset * 4; container[offset] = i == 0 ? 0x4e475349 : i == 1 ? 0x4e47534f : 0x47534350;
        container[offset + 1] = (2 + counts[i] * 14) * 4;
        hr = vkdu_tess_signature(container + offset + 2, stage, i, entries[i], counts[i], domain, masks[i], systems[i]);
        offset += 4 + counts[i] * 14;
    }
    if (FAILED(hr)) { free(container); return hr; }
    container[11] = offset * 4; container[offset] = 0x58454853; container[offset + 1] = words * 4;
    memcpy(container + offset + 2, tokens, words * 4);
#ifdef VKDU_TEST_TESS_SWAP_LINE_FACTORS
    /* Change executable declaration wiring as well as PCSG: the embedded
     * compiler derives tessellation builtins directly from token declarations. */
    if (stage == 3 && domain == 1) {
        uint32_t *copied = container + offset + 2;
        for (pos = 2; pos < words;) {
            uint32_t op = copied[pos] & 0x7ff, length = (copied[pos] >> 24) & 0x7f;
            if (op == 53) length = copied[pos + 1];
            if (op == 103 && (copied[pos + length - 1] == 21 || copied[pos + length - 1] == 22))
                copied[pos + length - 1] = copied[pos + length - 1] == 21 ? 22 : 21;
            pos += length;
        }
    }
#endif
    vkd3d_compute_dxbc_checksum(container, total * 4, container + 1);
    *code = container; *size = total * 4; return S_OK;
}
