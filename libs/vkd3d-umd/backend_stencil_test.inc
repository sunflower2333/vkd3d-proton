/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Original-production RED: catches rejection of required typed S8 storage.
 * The known D32 allocation first proves the real device/query path is live. */
static int check_stencil_allocation(vkdu_device *device, uint32_t format)
{
    uint64_t bytes = 0, alignment = 0;
    vkdu_object *texture = NULL;
    int32_t hr;
    CHECK(vkdu_depth_allocation(device, 7, 5, DXGI_FORMAT_D32_FLOAT, &bytes, &alignment));
    if (!bytes || alignment != 65536) return 1;
    hr = vkdu_depth_allocation(device, 7, 5, format, &bytes, &alignment);
    if (hr < 0) {
        fprintf(stderr, "FAIL stencil allocation format%u: HRESULT %08x expected S_OK\n", format, (unsigned)hr);
        return 1;
    }
    if (!bytes || alignment != 65536) return 1;
    CHECK(vkdu_depth_create(device, 7, 5, format, &texture));
    vkdu_object_destroy(texture);
    printf("PASS stencil typed allocation/create format%u\n", format);
    return 0;
}
