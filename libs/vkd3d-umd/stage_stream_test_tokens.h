/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VKDU_STAGE_STREAM_TEST_TOKENS_H
#define VKDU_STAGE_STREAM_TEST_TOKENS_H
/* Explicit SM5.0 R0 program, assembled from Microsoft token definitions:
 * four point streams reuse o2.xyzw and emit1/2/3/4 distinct vertices. Every
 * output is initialized before every emit (all streams invalidate outputs).
 * MOV/stream declaration/emit encodings match the upstream gs_topology_point
 * fixture at engine e388a662423b1e59d363b464f3ca15c09b24b9a0. */
static uint32_t stage_multistream_tokens(uint32_t words[512])
{
    uint32_t n = 0, s, v, c;
    words[n++] = 0x00020050; words[n++] = 0;
    words[n++] = 0x0100086a; /* dcl_globalFlags refactoringAllowed */
    words[n++] = 0x0100085d; /* dcl_inputprimitive point */
    for (s = 0; s < 4; ++s) {
        words[n++] = 0x0300008f; words[n++] = 0x00110000; words[n++] = s;
        words[n++] = 0x0100085c; /* dcl_outputtopology pointlist */
        words[n++] = 0x03000065; words[n++] = 0x001020f2; words[n++] = 2;
    }
    words[n++] = 0x0200005e; words[n++] = 10; /* max output vertices */
    for (s = 0; s < 4; ++s) for (v = 0; v <= s; ++v) {
        words[n++] = 0x08000036; words[n++] = 0x001020f2; words[n++] = 2;
        words[n++] = 0x00004002;
        for (c = 0; c < 4; ++c) {
            float value = (float)(16 * s + 4 * v + c + 1);
            memcpy(&words[n++], &value, 4);
        }
        words[n++] = 0x03000075; words[n++] = 0x00110000; words[n++] = s;
        words[n++] = 0x03000076; words[n++] = 0x00110000; words[n++] = s;
    }
    words[n++] = 0x0100003e; words[1] = n;
    return n;
}
static uint32_t stage_multistream_raster_tokens(uint32_t words[512])
{
    const float position[4][4] = {{-.625f,.25f,0,1},{-.125f,.25f,0,1},{.375f,.25f,0,1},{.875f,.25f,0,1}};
    const float color[4][4] = {{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,0,1}};
    uint32_t n = 0, s, reg;
    words[n++] = 0x00020050; words[n++] = 0;
    words[n++] = 0x0100086a; words[n++] = 0x0100085d;
    for (s = 0; s < 4; ++s) {
        words[n++] = 0x0300008f; words[n++] = 0x00110000; words[n++] = s;
        words[n++] = 0x0100085c;
        words[n++] = 0x04000067; words[n++] = 0x001020f2; words[n++] = 0; words[n++] = 1;
        words[n++] = 0x03000065; words[n++] = 0x001020f2; words[n++] = 1;
    }
    words[n++] = 0x0200005e; words[n++] = 4;
    for (s = 0; s < 4; ++s) {
        for (reg = 0; reg < 2; ++reg) {
            words[n++] = 0x08000036; words[n++] = 0x001020f2; words[n++] = reg; words[n++] = 0x00004002;
            memcpy(words + n, reg ? color[s] : position[s], 16); n += 4;
        }
        words[n++] = 0x03000075; words[n++] = 0x00110000; words[n++] = s;
        words[n++] = 0x03000076; words[n++] = 0x00110000; words[n++] = s;
    }
    words[n++] = 0x0100003e; words[1] = n;
    return n;
}
#endif
