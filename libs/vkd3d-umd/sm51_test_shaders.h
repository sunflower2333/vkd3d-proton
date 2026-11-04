/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Original compiler-produced 5.1 containers. Never change their version words. */
#include "../../tests/shaders/root_signature/headers/embedded_rs_vs_space0_naked.h"
#include "../../tests/shaders/root_signature/headers/embedded_rs_vs_space1_naked.h"
#include "../../tests/shaders/root_signature/headers/embedded_rs_ps_space0_naked.h"
#include "../../tests/shaders/root_signature/headers/embedded_rs_ps_space1_naked.h"
#include "../../tests/shaders/command/headers/execute_indirect_state_vs_code_small_cbv.h"
#include "../../tests/shaders/command/headers/execute_indirect_state_vs_code_large_cbv.h"

struct sm51_fixture {
    const char *name;
    const void *container;
    size_t size;
    uint32_t stage, input_count, output_count;
    struct vkdu_signature inputs[3], outputs[1];
};
#define SM51_FIXTURE(name, stage, inputs, outputs) \
    #name, name##_code_dxbc, sizeof(name##_code_dxbc), stage, inputs, outputs
static const struct sm51_fixture sm51_fixtures[] = {
    {SM51_FIXTURE(embedded_rs_vs_space0_naked, 1, 0, 1), {{0}}, {{1,0,15,3}}},
    {SM51_FIXTURE(embedded_rs_vs_space1_naked, 1, 0, 1), {{0}}, {{1,0,15,3}}},
    {SM51_FIXTURE(embedded_rs_ps_space0_naked, 0, 0, 0), {{0}}, {{0}}},
    {SM51_FIXTURE(embedded_rs_ps_space1_naked, 0, 0, 0), {{0}}, {{0}}},
    {SM51_FIXTURE(execute_indirect_state_vs_code_small_cbv, 1, 3, 1),
        {{0,0,1,3},{0,1,1,3},{8,2,1,1}}, {{1,0,15,3}}},
    {SM51_FIXTURE(execute_indirect_state_vs_code_large_cbv, 1, 3, 1),
        {{0,0,1,3},{0,1,1,3},{8,2,1,1}}, {{1,0,15,3}}},
};
#undef SM51_FIXTURE

/* This bounds/chunk parser is test code, independent of the production builder. */
static const uint32_t *sm51_chunk(const void *data, size_t size, uint32_t tag, uint32_t *words)
{
    const uint32_t *d = (const uint32_t *)data, *found = NULL;
    uint32_t i;
    *words = 0;
    if (!d || size < 32 || size % 4 || d[0] != 0x43425844 || d[5] != 1 ||
            d[6] != size || d[7] > (size - 32) / 4) return NULL;
    for (i = 0; i < d[7]; ++i) {
        uint32_t off = d[8+i], len;
        if (off % 4 || off < 32 + d[7] * 4 || off > size - 8) return NULL;
        len = d[off/4+1];
        if (len % 4 || len > size - off - 8) return NULL;
        if (d[off/4] == tag) {
            if (found) return NULL;
            found = d + off/4 + 2; *words = len/4;
        }
    }
    return found;
}
static int sm51_preserved(const struct sm51_fixture *f, const void *data, size_t size)
{
    uint32_t original_words, output_words, signature_words, i, side;
    const uint32_t *original = sm51_chunk(f->container, f->size, 0x58454853, &original_words);
    const uint32_t *output = sm51_chunk(data, size, 0x58454853, &output_words);
    const uint32_t *d = (const uint32_t *)data;
    if (!original || !output || d[7] != 3 || original_words != output_words ||
            original[0] != ((f->stage << 16) | 0x51) || original[1] != original_words ||
            memcmp(original, output, original_words * 4)) return 0;
    for (side = 0; side < 2; ++side) {
        const uint32_t count = side ? f->output_count : f->input_count;
        const struct vkdu_signature *entries = side ? f->outputs : f->inputs;
        const uint32_t *sig = sm51_chunk(data, size, side ? 0x4e47534f : 0x4e475349, &signature_words);
        if (!sig || signature_words != 2 + count * 10 || sig[0] != count || sig[1] != 8) return 0;
        for (i = 0; i < count; ++i) {
            const uint32_t *entry = sig + 2 + i * 6;
            const char *name;
            if (entry[0] >= signature_words * 4 || entry[2] != entries[i].system_value ||
                    entry[3] != entries[i].component_type || entry[4] != entries[i].reg ||
                    (entry[5] & 255) != entries[i].mask) return 0;
            name = (const char *)sig + entry[0];
            if (!memchr(name, 0, signature_words * 4 - entry[0])) return 0;
            if (strcmp(name, entries[i].system_value == 1 ? "SV_Position" :
                    entries[i].system_value == 8 ? "SV_InstanceID" : "ATTR")) return 0;
        }
    }
    return 1;
}
