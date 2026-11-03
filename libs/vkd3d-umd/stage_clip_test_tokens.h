/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VKDU_STAGE_CLIP_TEST_TOKENS_H
#define VKDU_STAGE_CLIP_TEST_TOKENS_H
/* VS: Position, Clip/CullDistance0.xy=(1.25,2.5),
 * Clip/CullDistance1.xy=(3.75,4.5). Total distance components is only4. */
static void stage_split_clip_tokens(uint32_t system, uint32_t words[40])
{
    const uint32_t code[40] = {
        0x00010050,40,0x0100086a,
        0x04000067,0x001020f2,0,1,
        0x04000067,0x00102032,1,system,
        0x04000067,0x00102032,2,system,
        0x08000036,0x001020f2,0,0x00004002,0,0,0,0x3f800000,
        0x08000036,0x00102032,1,0x00004002,0x3fa00000,0x40200000,0,0,
        0x08000036,0x00102032,2,0x00004002,0x40700000,0x40900000,0,0,
        0x0100003e
    };
    memcpy(words,code,sizeof(code));
}
/* A point VS/GS with two independent distance registers. Declaration order is
 * deliberately reversed; the signature and capture order need not match it. */
static uint32_t stage_distance_tokens(uint32_t stage, const uint32_t systems[2],
        const uint32_t masks[2], const uint32_t values[2][4], uint32_t words[96])
{
    uint32_t n=0, i;
    words[n++]=(stage<<16)|0x50; words[n++]=0; words[n++]=0x0100086a;
    if (stage==2) {
        words[n++]=0x0100085d;
        words[n++]=0x0300008f; words[n++]=0x00110000; words[n++]=0;
        words[n++]=0x0100085c; words[n++]=0x0200005e; words[n++]=1;
    }
    words[n++]=0x04000067; words[n++]=0x001020f2; words[n++]=0; words[n++]=1;
    for (i=2;i;--i) {
        words[n++]=0x04000067; words[n++]=0x00102002|(masks[i-1]<<4); words[n++]=i; words[n++]=systems[i-1];
    }
    words[n++]=0x08000036; words[n++]=0x001020f2; words[n++]=0; words[n++]=0x00004002;
    words[n++]=0xbe000000; words[n++]=0x3e800000; words[n++]=0; words[n++]=0x3f800000;
    for (i=0;i<2;++i) {
        words[n++]=0x08000036; words[n++]=0x00102002|(masks[i]<<4); words[n++]=i+1; words[n++]=0x00004002;
        memcpy(words+n,values[i],16); n+=4;
    }
    if (stage==2) {
        words[n++]=0x03000075; words[n++]=0x00110000; words[n++]=0;
        words[n++]=0x03000076; words[n++]=0x00110000; words[n++]=0;
    }
    words[n++]=0x0100003e; words[1]=n; return n;
}
#endif
