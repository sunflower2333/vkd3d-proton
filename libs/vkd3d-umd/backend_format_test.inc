/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_native_formats(vkdu_device *device)
{
    uint32_t flags, levels, i;
    const uint32_t expected[] = {
        VKDU_FORMAT_SAMPLE | VKDU_FORMAT_RENDER_TARGET | VKDU_FORMAT_BLENDABLE | VKDU_FORMAT_VERTEX | VKDU_FORMAT_GATHER,
        VKDU_FORMAT_SAMPLE | VKDU_FORMAT_VERTEX | VKDU_FORMAT_GATHER | VKDU_FORMAT_BUFFER | VKDU_FORMAT_UAV_READ | VKDU_FORMAT_UAV_WRITE,
        VKDU_FORMAT_VERTEX | VKDU_FORMAT_BUFFER | VKDU_FORMAT_UAV_READ | VKDU_FORMAT_UAV_WRITE,
        VKDU_FORMAT_VERTEX,
        VKDU_FORMAT_SAMPLE | VKDU_FORMAT_RENDER_TARGET | VKDU_FORMAT_BLENDABLE | VKDU_FORMAT_GATHER,
        VKDU_FORMAT_SAMPLE | VKDU_FORMAT_RENDER_TARGET | VKDU_FORMAT_BLENDABLE | VKDU_FORMAT_GATHER,
        VKDU_FORMAT_SAMPLE | VKDU_FORMAT_RENDER_TARGET | VKDU_FORMAT_BLENDABLE | VKDU_FORMAT_GATHER, 0,
    };
    const uint32_t formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8X8_UNORM};
    for (i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        flags = 0xcccccccc;
        CHECK(vkdu_format_support(device, formats[i], &flags));
        if (flags != expected[i]) { fprintf(stderr, "native format%u flags%x expected%x\n", formats[i], flags, expected[i]); exit(1); }
    }
    CHECK(vkdu_multisample_quality(device, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, &levels));
    if (levels != 1) { fprintf(stderr, "single-sample quality=%u\n", levels); exit(1); }
    CHECK(vkdu_multisample_quality(device, DXGI_FORMAT_R32_FLOAT, 1, 0, &levels));
    if (levels != 1) exit(1);
    CHECK(vkdu_multisample_quality(device, DXGI_FORMAT_D32_FLOAT, 1, 0, &levels));
    if (levels != 1) exit(1);
    CHECK(vkdu_multisample_quality(device, DXGI_FORMAT_D16_UNORM, 1, 0, &levels));
    if (levels != 1) exit(1);
    CHECK(vkdu_multisample_quality(device, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 0, &levels));
    if (levels) { fprintf(stderr, "native subset leaked MSAA quality\n"); exit(1); }
    CHECK(vkdu_multisample_quality(device, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0, &levels));
    if (levels != 1) exit(1);
    CHECK(vkdu_multisample_quality(device, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1, 0, &levels));
    if (levels != 1) exit(1);
    CHECK(vkdu_multisample_quality(device, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 1, 0, &levels));
    if (levels != 1) exit(1);
    CHECK(vkdu_multisample_quality(device, DXGI_FORMAT_B8G8R8X8_UNORM, 1, 0, &levels));
    if (levels) { fprintf(stderr, "native subset leaked texture format\n"); exit(1); }
    levels = 0x12345678;
    REJECT(vkdu_multisample_quality(device, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, &levels));
    REJECT(vkdu_multisample_quality(device, DXGI_FORMAT_R8G8B8A8_UNORM, 33, 0, &levels));
    REJECT(vkdu_multisample_quality(device, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 2, &levels));
    if (levels != 0x12345678) exit(1);
    puts("PASS native format/MSAA queries: real backend capabilities intersect native image/blend/buffer/VB paths, no MSAA/display leakage");
}
