/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Test-first fixture. Included by the focused entry after final MRT integration.
 * Expected values are literal D3D stencil outcomes, never production helpers.
 * Combined DS images are observed only through draws; their copy path stays shut. */
struct stencil_test_context {
    vkdu_device *device;
    vkdu_object *root, *depth, *color, *dsv, *rtv, *queue, *allocator, *command, *fence, *readback;
    vkdu_object *qualifier[3];
    void *vs, *ps; size_t vs_size, ps_size;
    uint32_t format, width, height, serial, plane_state[2], plane_sub[2];
};
static const struct vkdu_rect stencil_all = {1, 1, 6, 4};
static const struct vkdu_rect stencil_left = {1, 1, 3, 4};
static const struct vkdu_rect stencil_right = {3, 1, 6, 4};

static struct vkdu_graphics_desc stencil_description(uint32_t format)
{
    struct vkdu_graphics_desc d = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT32_MAX, 3, 1, 0, 1, 15};
    d.depth_format = format;
    d.stencil.enable = d.stencil.front_enable = d.stencil.back_enable = 1;
    d.stencil.read_mask = d.stencil.write_mask = 255;
    d.stencil.front.fail_op = d.stencil.front.depth_fail_op = d.stencil.front.pass_op = D3D12_STENCIL_OP_KEEP;
    d.stencil.front.func = D3D12_COMPARISON_FUNC_ALWAYS;
    d.stencil.back = d.stencil.front;
    return d;
}
static vkdu_object *stencil_pipeline(struct stencil_test_context *t, const struct vkdu_graphics_desc *desc)
{
    vkdu_object *out = NULL;
    CHECK(vkdu_graphics_pipeline_create(t->device, t->root, t->vs, t->vs_size, t->ps, t->ps_size, desc, &out));
    return out;
}
static void stencil_plane(struct stencil_test_context *t, uint32_t plane, uint32_t state)
{
    if (plane && (t->format == DXGI_FORMAT_D32_FLOAT || t->format == DXGI_FORMAT_D16_UNORM)) return;
    struct vkdu_resource_barrier barrier = {VKDU_BARRIER_TRANSITION, t->depth,
        t->plane_state[plane], state, NULL, t->plane_sub[plane] + 1};
    if (t->plane_state[plane] != state) CHECK(vkdu_command_barriers(t->command, 1, &barrier));
    t->plane_state[plane] = state;
}
static void stencil_context_init(struct stencil_test_context *t, vkdu_device *device, uint32_t format)
{
    const struct vkdu_signature vin = {6, 0, 1, 1}, vout = {1, 0, 15, 3}, pout = {0, 0, 15, 3};
    const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc), *p = graphics_tokens(ps_flat_color_code_dxbc);
    struct vkdu_graphics_desc desc = stencil_description(format);
    uint32_t i;
    memset(t, 0, sizeof(*t)); t->device = device; t->format = format; t->width = 7; t->height = 5;
    t->plane_sub[1] = 1; t->plane_state[0] = t->plane_state[1] = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    CHECK(vkdu_shader_dxbc(1, v, v[1], &vin, 1, &vout, 1, &t->vs, &t->vs_size));
    CHECK(vkdu_shader_dxbc(0, p, p[1], NULL, 0, &pout, 1, &t->ps, &t->ps_size));
    CHECK(vkdu_root_create(device, NULL, 0, 0, &t->root));
    CHECK(vkdu_depth_create(device, 7, 5, format, &t->depth));
    CHECK(vkdu_render_target_create(device, 7, 5, &t->color));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 4, 0, &t->dsv));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, 0, &t->rtv));
    for (i = 0; i < 4; ++i) CHECK(vkdu_texture2d_dsv(t->dsv, i, t->depth,
        format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D16_UNORM ? i & 1 : i));
    CHECK(vkdu_texture2d_rtv(t->rtv, 0, t->color));
    CHECK(vkdu_queue_create(device, 0, &t->queue)); CHECK(vkdu_allocator_create(device, 0, &t->allocator));
    CHECK(vkdu_command_create(device, t->allocator, 0, &t->command)); CHECK(vkdu_fence_create(device, 0, &t->fence));
    CHECK(vkdu_buffer_create(device, 4096, D3D12_HEAP_TYPE_READBACK, 0, D3D12_RESOURCE_STATE_COPY_DEST, &t->readback));
    desc.stencil.front.func = desc.stencil.back.func = D3D12_COMPARISON_FUNC_EQUAL;
    desc.stencil.write_mask = 0;
    t->qualifier[0] = stencil_pipeline(t, &desc);
    desc.depth_enable = 1; desc.depth_func = D3D12_COMPARISON_FUNC_LESS;
    t->qualifier[1] = stencil_pipeline(t, &desc);
    desc.depth_func = D3D12_COMPARISON_FUNC_EQUAL;
    t->qualifier[2] = stencil_pipeline(t, &desc);
}
static void stencil_start(struct stencil_test_context *t)
{
    const float red[4] = {1, 0, 0, 1};
    const struct vkdu_viewport viewport = {0, 0, 7, 5, 0, 1};
    uint32_t *mapped, i;
    if (t->serial) {
        CHECK(vkdu_allocator_reset(t->allocator)); CHECK(vkdu_command_reset(t->command, t->allocator));
        CHECK(vkdu_command_transition(t->command, t->color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
    }
    stencil_plane(t, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE); stencil_plane(t, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    CHECK(vkdu_buffer_map(t->readback, 0, 0, (void **)&mapped));
    for (i = 0; i < 1024; ++i) mapped[i] = 0x89abcdef;
    CHECK(vkdu_buffer_unmap(t->readback, 0, 4096));
    CHECK(vkdu_command_clear_rtv(t->command, t->rtv, 0, red, 0, NULL));
    CHECK(vkdu_command_graphics_root(t->command, t->root)); CHECK(vkdu_command_topology(t->command, 4));
    CHECK(vkdu_command_viewports(t->command, 1, &viewport));
}
static void stencil_draw(struct stencil_test_context *t, vkdu_object *pipeline, uint32_t dsv,
        const struct vkdu_rect *rect, int set_reference, uint32_t reference)
{
    CHECK(vkdu_command_targets(t->command, t->rtv, 0, t->dsv, dsv));
    CHECK(vkdu_command_pipeline(t->command, pipeline));
    if (set_reference) CHECK(vkdu_command_stencil_ref(t->command, reference));
    CHECK(vkdu_command_scissors(t->command, 1, rect)); CHECK(vkdu_command_draw(t->command, 3, 1, 0, 0));
}
static void stencil_qualify(struct stencil_test_context *t, uint32_t left_ref, uint32_t right_ref, uint32_t depth_test)
{
    stencil_plane(t, 0, D3D12_RESOURCE_STATE_DEPTH_READ); stencil_plane(t, 1, D3D12_RESOURCE_STATE_DEPTH_READ);
    stencil_draw(t, t->qualifier[depth_test], 3, &stencil_left, 1, left_ref);
    stencil_draw(t, t->qualifier[depth_test], 3, &stencil_right, 1, right_ref);
}
static void stencil_finish(struct stencil_test_context *t, const char *name, uint32_t white_regions)
{
    struct vkdu_texture_copy_location from = {t->color, 0};
    struct vkdu_texture_copy_location to = {t->readback, 512, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 7, 5, 1, 256};
    uint32_t expected[1024], *mapped, i, x, y;
    CHECK(vkdu_command_transition(t->command, t->color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
    CHECK(vkdu_command_texture_copy(t->command, &to, 0, 0, 0, &from, NULL));
    CHECK(vkdu_command_close(t->command)); CHECK(vkdu_queue_execute(t->queue, 1, &t->command));
    CHECK(vkdu_queue_signal(t->queue, t->fence, ++t->serial)); CHECK(vkdu_fence_wait(t->fence, t->serial, 10000));
    for (i = 0; i < 1024; ++i) expected[i] = 0x89abcdef;
    for (y = 0; y < 5; ++y) for (x = 0; x < 7; ++x) {
        int white = y >= 1 && y < 4 && ((x >= 1 && x < 3 && (white_regions & 1)) ||
            (x >= 3 && x < 6 && (white_regions & 2)));
        expected[128 + y * 64 + x] = white ? UINT32_MAX : 0xff0000ff;
    }
    CHECK(vkdu_buffer_map(t->readback, 0, 4096, (void **)&mapped));
    for (i = 0; i < 1024; ++i) if (mapped[i] != expected[i]) {
        fprintf(stderr, "FAIL stencil pixel format%u case=%s word%u got=%08x expected=%08x\n",
            t->format, name, i, mapped[i], expected[i]); exit(1);
    }
    CHECK(vkdu_buffer_unmap(t->readback, 0, 0));
    printf("PASS stencil pixel format%u case=%s words=1024\n", t->format, name);
}
static void stencil_context_destroy(struct stencil_test_context *t)
{
    uint32_t i;
    CHECK(vkdu_device_status(t->device));
    vkdu_object_destroy(t->command); vkdu_object_destroy(t->allocator);
    vkdu_object_destroy(t->queue); vkdu_object_destroy(t->fence); vkdu_object_destroy(t->readback);
    for (i = 0; i < 3; ++i) vkdu_object_destroy(t->qualifier[i]);
    vkdu_object_destroy(t->dsv); vkdu_object_destroy(t->rtv);
    vkdu_object_destroy(t->depth); vkdu_object_destroy(t->color); vkdu_object_destroy(t->root);
    vkdu_shader_dxbc_free(t->vs); vkdu_shader_dxbc_free(t->ps);
}

static void check_stencil_winding(struct stencil_test_context *t)
{
    /* SV_VertexID order projects to (0,5),(14,5),(0,-5), which is
     * counterclockwise on screen. Independently qualify it through culling. */
    static const uint32_t cases[][3] = {{0, 2, 3}, {0, 3, 0}, {1, 2, 0}, {1, 3, 3}};
    uint32_t i;
    for (i = 0; i < 4; ++i) {
        struct vkdu_graphics_desc desc = stencil_description(t->format);
        vkdu_object *pipeline; char name[96];
        desc.stencil.enable = 0; desc.front_ccw = cases[i][0]; desc.cull_mode = cases[i][1];
        pipeline = stencil_pipeline(t, &desc); stencil_start(t);
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, 0x5a, 0, NULL));
        stencil_draw(t, pipeline, 0, &stencil_all, 0, 0);
        snprintf(name, sizeof(name), "winding-frontCCW%u-cull%u", cases[i][0], cases[i][1]);
        stencil_finish(t, name, cases[i][2]); vkdu_object_destroy(pipeline);
    }
    stencil_start(t);
    CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, 0x5a, 0, NULL));
    stencil_qualify(t, 0x5a, 0x5b, 1);
    stencil_finish(t, "clear-and-qualify-without-producer", 1);
}
static void check_stencil_operations(struct stencil_test_context *t)
{
    static const struct { const char *name; uint32_t op, initial, reference, mask, expected; } cases[] = {
        {"keep", 1, 0x5a, 0xa6, 255, 0x5a}, {"zero", 2, 0x5a, 0xa6, 255, 0x00},
        {"replace", 3, 0x5a, 0xa6, 255, 0xa6}, {"incr-sat-limit", 4, 0xff, 0xa6, 255, 0xff},
        {"incr-sat", 4, 0x7f, 0xa6, 255, 0x80}, {"decr-sat-limit", 5, 0x00, 0xa6, 255, 0x00},
        {"decr-sat", 5, 0x80, 0xa6, 255, 0x7f}, {"invert", 6, 0x5a, 0xa6, 255, 0xa5},
        {"incr-wrap", 7, 0xff, 0xa6, 255, 0x00}, {"decr-wrap", 8, 0x00, 0xa6, 255, 0xff},
        {"replace-mask-zero", 3, 0xa5, 0x3c, 0, 0xa5}, {"replace-mask-low", 3, 0xa5, 0x3c, 15, 0xac},
        {"replace-mask-high", 3, 0xa5, 0x3c, 240, 0x35}, {"incr-masked-wrap", 7, 0xff, 0x3c, 15, 0xf0},
    };
    uint32_t n, face, branch;
    for (face = 0; face < 2; ++face) for (branch = 0; branch < 3; ++branch)
    for (n = 0; n < sizeof(cases)/sizeof(cases[0]); ++n) {
        struct vkdu_graphics_desc desc = stencil_description(t->format);
        struct vkdu_stencil_face *active = face ? &desc.stencil.back : &desc.stencil.front;
        vkdu_object *pipeline;
        char name[96];
        /* Culling calibration above independently establishes the winding. */
        desc.front_ccw = !face; desc.write_mask = 0;
        desc.depth_enable = 1; desc.depth_func = branch == 2 ? D3D12_COMPARISON_FUNC_GREATER : D3D12_COMPARISON_FUNC_LESS;
        desc.stencil.write_mask = cases[n].mask;
        desc.stencil.front.func = desc.stencil.back.func = branch == 1 ? D3D12_COMPARISON_FUNC_NEVER : D3D12_COMPARISON_FUNC_ALWAYS;
        desc.stencil.front.fail_op = desc.stencil.front.depth_fail_op = desc.stencil.front.pass_op = D3D12_STENCIL_OP_INVERT;
        desc.stencil.back = desc.stencil.front;
        active->fail_op = active->depth_fail_op = active->pass_op = cases[n].op == 1 ? 2 : 1;
        if (!branch) active->pass_op = cases[n].op;
        else if (branch == 1) active->fail_op = cases[n].op;
        else active->depth_fail_op = cases[n].op;
        pipeline = stencil_pipeline(t, &desc);
        stencil_start(t);
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, cases[n].initial, 0, NULL));
        stencil_draw(t, pipeline, 0, &stencil_all, 1, cases[n].reference);
        stencil_qualify(t, cases[n].expected, cases[n].expected ^ 1, 1);
        snprintf(name, sizeof(name), "%s-face%u-branch%u", cases[n].name, face, branch);
        stencil_finish(t, name, 1); vkdu_object_destroy(pipeline);
    }
}
static void check_stencil_comparisons(struct stencil_test_context *t)
{
    /* Ref compares to stored 0x20. Bits correspond to ref 0x10,0x20,0x30. */
    static const uint32_t pass_bits[] = {0, 1, 2, 3, 4, 5, 6, 7};
    static const uint32_t references[] = {0x10, 0x20, 0x30};
    uint32_t func, r;
    for (func = 1; func <= 8; ++func) {
        struct vkdu_graphics_desc desc = stencil_description(t->format);
        vkdu_object *pipeline;
        desc.stencil.write_mask = 0;
        desc.stencil.front.func = desc.stencil.back.func = func;
        pipeline = stencil_pipeline(t, &desc);
        for (r = 0; r < 3; ++r) {
            char name[64]; stencil_start(t);
            CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, 0x20, 0, NULL));
            stencil_draw(t, pipeline, 0, &stencil_all, 1, references[r]);
            snprintf(name, sizeof(name), "compare%u-ref%02x", func, references[r]);
            stencil_finish(t, name, (pass_bits[func-1] & (1u << r)) ? 3 : 0);
        }
        vkdu_object_destroy(pipeline);
    }
}
static void check_stencil_masks_and_disable(struct stencil_test_context *t)
{
    static const struct {uint32_t read_mask, reference, stored, pass;} reads[] = {
        {255, 0x21, 0x31, 0}, {15, 0x21, 0x31, 1}, {240, 0x21, 0x31, 0}, {0, 0x21, 0x31, 1},
    };
    uint32_t n, face;
    for (n = 0; n < 4; ++n) {
        struct vkdu_graphics_desc desc = stencil_description(t->format);
        vkdu_object *pipeline; char name[64];
        desc.stencil.front.func = desc.stencil.back.func = D3D12_COMPARISON_FUNC_EQUAL;
        desc.stencil.read_mask = reads[n].read_mask; desc.stencil.write_mask = 0;
        pipeline = stencil_pipeline(t, &desc); stencil_start(t);
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, reads[n].stored, 0, NULL));
        stencil_draw(t, pipeline, 0, &stencil_all, 1, reads[n].reference);
        snprintf(name, sizeof(name), "read-mask%u", reads[n].read_mask);
        stencil_finish(t, name, reads[n].pass ? 3 : 0); vkdu_object_destroy(pipeline);
    }
    for (face = 0; face < 3; ++face) {
        struct vkdu_graphics_desc desc = stencil_description(t->format);
        vkdu_object *pipeline; char name[64];
        desc.front_ccw = face != 1;
        desc.stencil.front.func = desc.stencil.back.func = D3D12_COMPARISON_FUNC_NEVER;
        desc.stencil.front.fail_op = desc.stencil.back.fail_op = D3D12_STENCIL_OP_ZERO;
        if (!face) desc.stencil.front_enable = 0;
        else if (face == 1) desc.stencil.back_enable = 0;
        else desc.stencil.enable = 0;
        pipeline = stencil_pipeline(t, &desc); stencil_start(t);
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, 0x5a, 0, NULL));
        stencil_draw(t, pipeline, 0, &stencil_all, 1, 0x33);
        snprintf(name, sizeof(name), "disabled-face%u-passthrough", face);
        stencil_finish(t, name, 3);
        stencil_start(t);
        /* Observe the preceding producer's stored value independently. */
        stencil_qualify(t, 0x5a, 0x5b, 1);
        snprintf(name, sizeof(name), "disabled-face%u", face);
        stencil_finish(t, name, 1); vkdu_object_destroy(pipeline);
    }
}
static void check_stencil_clear_and_readonly(struct stencil_test_context *t)
{
    uint32_t flags;
    for (flags = 0; flags < 4; ++flags) {
        struct vkdu_graphics_desc desc = stencil_description(t->format);
        vkdu_object *pipeline; char name[96];
        desc.depth_enable = desc.depth_write = 1; desc.depth_func = D3D12_COMPARISON_FUNC_ALWAYS;
        desc.write_mask = 0; desc.stencil.front.pass_op = desc.stencil.back.pass_op = D3D12_STENCIL_OP_REPLACE;
        pipeline = stencil_pipeline(t, &desc);
        stencil_start(t);
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, 0x31, 0, NULL));
        stencil_plane(t, 0, flags & 1 ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE);
        stencil_plane(t, 1, flags & 2 ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE);
        stencil_draw(t, pipeline, flags, &stencil_all, 1, 0x6b);
        stencil_qualify(t, flags & 2 ? 0x31 : 0x6b, flags & 2 ? 0x30 : 0x6a, flags & 1 ? 1 : 2);
        snprintf(name, sizeof(name), "readonly%u-draw-both-plane-values", flags);
        stencil_finish(t, name, 1); vkdu_object_destroy(pipeline);

        stencil_start(t);
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, 0x31, 0, NULL));
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, flags, 3, 0.0f, 0x7a, 0, NULL));
        stencil_qualify(t, 0x7a, 0x7b, 2);
        snprintf(name, sizeof(name), "readonly%u-clear-both", flags); stencil_finish(t, name, 1);

        stencil_start(t);
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, 0x31, 0, NULL));
        stencil_plane(t, 0, D3D12_RESOURCE_STATE_DEPTH_READ);
        /* Unused NaN depth is deliberately not a reason to reject STENCIL. */
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, flags, 2, NAN, 0x7a, 1, &stencil_left));
        stencil_qualify(t, 0x7a, 0x31, 1);
        snprintf(name, sizeof(name), "readonly%u-clear-stencil-preserves-depth-and-right", flags);
        stencil_finish(t, name, 3);

        stencil_start(t);
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, 0x31, 0, NULL));
        stencil_plane(t, 1, D3D12_RESOURCE_STATE_DEPTH_READ);
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, flags, 1, 0.0f, 0x99, 1, &stencil_left));
        stencil_qualify(t, 0x31, 0x31, 2);
        snprintf(name, sizeof(name), "readonly%u-clear-depth-preserves-stencil-and-right", flags);
        stencil_finish(t, name, 1);
    }
}
static void check_stencil_reference(struct stencil_test_context *t)
{
    struct vkdu_graphics_desc desc = stencil_description(t->format);
    vkdu_object *pipeline;
    desc.write_mask = 0; desc.stencil.front.pass_op = desc.stencil.back.pass_op = D3D12_STENCIL_OP_REPLACE;
    pipeline = stencil_pipeline(t, &desc);
    stencil_start(t);
    CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, 0, 0, NULL));
    CHECK(vkdu_command_stencil_ref(t->command, 0x123));
    stencil_draw(t, pipeline, 0, &stencil_all, 0, 0);
    stencil_plane(t, 0, D3D12_RESOURCE_STATE_DEPTH_READ); stencil_plane(t, 1, D3D12_RESOURCE_STATE_DEPTH_READ);
    CHECK(vkdu_command_graphics_root(t->command, NULL)); CHECK(vkdu_command_graphics_root(t->command, t->root));
    CHECK(vkdu_command_graphics_root(t->command, t->root));
    stencil_draw(t, t->qualifier[1], 3, &stencil_left, 0, 0);
    stencil_draw(t, t->qualifier[1], 3, &stencil_right, 1, 0x24);
    stencil_finish(t, "reference-UINT-high-bits-PSO-and-root-preservation", 1);
    vkdu_object_destroy(pipeline);

    stencil_start(t);
    CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, 0, 0, NULL));
    stencil_plane(t, 0, D3D12_RESOURCE_STATE_DEPTH_READ); stencil_plane(t, 1, D3D12_RESOURCE_STATE_DEPTH_READ);
    stencil_draw(t, t->qualifier[1], 3, &stencil_left, 0, 0);
    stencil_draw(t, t->qualifier[1], 3, &stencil_right, 1, 1);
    stencil_finish(t, "reference-reset-default-zero", 1);
}
static void check_stencil_array(struct stencil_test_context *t)
{
    const struct vkdu_texture2d_shape shape = {15, 11, t->format, 3, 3, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, 1, 0};
    const uint32_t original[] = {0x10, 0x20, 0x30};
    uint32_t placed, layer, flags;
    for (placed = 0; placed < 2; ++placed) {
        vkdu_object *heap = NULL, *array = NULL; uint64_t bytes, alignment;
        CHECK(vkdu_texture2d_shape_allocation(t->device, &shape, &bytes, &alignment));
        if (placed) {
            CHECK(vkdu_test_attachment_heap_create(t->device, bytes * 2, &heap));
            CHECK(vkdu_texture2d_shape_place(t->device, heap, bytes, &shape, D3D12_RESOURCE_STATE_DEPTH_WRITE, &array));
            vkdu_object_destroy(heap);
        } else CHECK(vkdu_texture2d_shape_create(t->device, &shape, &array));
        vkdu_object_destroy(t->dsv); vkdu_object_destroy(t->depth); t->depth = array;
        CHECK(vkdu_heap_create(t->device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 12, 0, &t->dsv));
        for (layer = 0; layer < 3; ++layer) for (flags = 0; flags < 4; ++flags)
            CHECK(vkdu_texture2d_dsv_range(t->dsv, 4 * layer + flags, array, flags, 1, layer, 1));
        t->plane_sub[0] = 4; t->plane_sub[1] = 13;
        t->plane_state[0] = t->plane_state[1] = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        stencil_start(t);
        {
            /* Last valid plane subresource is 17 (mip2/layer2/stencil).
             * Rejection of a late bad element must leave the first untouched. */
            struct vkdu_resource_barrier barriers[2] = {
                {VKDU_BARRIER_TRANSITION, array, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ, NULL, 18},
                {VKDU_BARRIER_TRANSITION, array, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ, NULL, 19}};
            REJECT(vkdu_command_barriers(t->command, 2, barriers));
            CHECK(vkdu_command_barriers(t->command, 1, barriers));
            barriers[0].before = D3D12_RESOURCE_STATE_DEPTH_READ; barriers[0].after = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            CHECK(vkdu_command_barriers(t->command, 1, barriers));
        }
        for (layer = 0; layer < 3; ++layer)
            CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 4 * layer, 3, 0.5f, original[layer], 0, NULL));
        stencil_plane(t, 0, D3D12_RESOURCE_STATE_DEPTH_READ);
        CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 7, 2, NAN, 0x77, 1, &stencil_left));
        /* Select layer1 by copying its four descriptors into the active slots.
         * Actual descriptor content and MRT metadata must agree. */
        CHECK(vkdu_descriptor_copy(t->dsv, 0, t->dsv, 4, 4));
        stencil_qualify(t, 0x77, 0x20, 1);
        stencil_plane(t, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE); stencil_plane(t, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        stencil_finish(t, placed ? "placed-mip1-layer1-plane13-rect" : "committed-mip1-layer1-plane13-rect", 3);
        for (layer = 0; layer < 3; layer += 2) {
            char name[96];
            for (flags = 0; flags < 4; ++flags)
                CHECK(vkdu_texture2d_dsv_range(t->dsv, flags, array, flags, 1, layer, 1));
            t->plane_sub[0] = 1 + layer * 3; t->plane_sub[1] = 10 + layer * 3;
            t->plane_state[0] = t->plane_state[1] = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            stencil_start(t); stencil_qualify(t, original[layer], original[layer] ^ 1, 1);
            stencil_plane(t, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE); stencil_plane(t, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            snprintf(name, sizeof(name), "%s-mip1-neighbor-layer%u-unchanged", placed ? "placed" : "committed", layer);
            stencil_finish(t, name, 1);
        }
    }
}
static void check_stencil_validation(struct stencil_test_context *t, vkdu_device *other)
{
    vkdu_object *foreign = NULL, *second = NULL, *bad = NULL, *allocator = NULL, *command = NULL;
    struct vkdu_graphics_desc desc = stencil_description(t->format);
    struct vkdu_texture_copy_location a, b;
    uint32_t type, quality, support;
    CHECK(vkdu_format_support(t->device, t->format, &support));
    if (support) { fprintf(stderr, "FAIL stencil exposed unimplemented format capabilities %u\n", support); exit(1); }
    CHECK(vkdu_multisample_quality(t->device, t->format, 1, 0, &quality));
    if (quality != 1) { fprintf(stderr, "FAIL stencil sample1 quality %u\n", quality); exit(1); }
    CHECK(vkdu_multisample_quality(t->device, t->format, 4, 0, &quality));
    if (quality) { fprintf(stderr, "FAIL stencil exposed unsupported MSAA\n"); exit(1); }
    CHECK(vkdu_depth_create(other, 7, 5, t->format, &foreign));
    REJECT(vkdu_texture2d_dsv(t->dsv, 0, foreign, 0)); vkdu_object_destroy(foreign);
    REJECT(vkdu_texture2d_dsv(t->dsv, 0, t->depth, 4));
    desc.stencil.front_enable = desc.stencil.back_enable = 0;
    REJECT(vkdu_graphics_pipeline_create(t->device, t->root, t->vs, t->vs_size, t->ps, t->ps_size, &desc, &bad));
    desc = stencil_description(t->format); desc.stencil.front.fail_op = 9;
    REJECT(vkdu_graphics_pipeline_create(t->device, t->root, t->vs, t->vs_size, t->ps, t->ps_size, &desc, &bad));
    desc = stencil_description(t->format); desc.stencil.read_mask = 256;
    REJECT(vkdu_graphics_pipeline_create(t->device, t->root, t->vs, t->vs_size, t->ps, t->ps_size, &desc, &bad));
    desc = stencil_description(t->format); desc.stencil.front_enable = 0;
    desc.stencil.front = (struct vkdu_stencil_face){9, 9, 9, 9};
    CHECK(vkdu_graphics_pipeline_create(t->device, t->root, t->vs, t->vs_size, t->ps, t->ps_size, &desc, &bad));
    vkdu_object_destroy(bad); bad = NULL;
    for (type = 2; type <= 3; ++type) {
        CHECK(vkdu_allocator_create(t->device, type, &allocator));
        CHECK(vkdu_command_create(t->device, allocator, type, &command));
        REJECT(vkdu_command_stencil_ref(command, 7));
        vkdu_object_destroy(command); vkdu_object_destroy(allocator);
    }
    CHECK(vkdu_depth_create(t->device, 7, 5, t->format, &second));
    stencil_start(t);
    REJECT(vkdu_command_clear_dsv(t->command, t->dsv, 0, 0, 0, 0, 0, NULL));
    REJECT(vkdu_command_clear_dsv(t->command, t->dsv, 0, 4, 0, 0, 0, NULL));
    REJECT(vkdu_command_clear_dsv(t->command, t->dsv, 0, 2, 0, 256, 0, NULL));
    REJECT(vkdu_command_clear_dsv(t->command, t->dsv, 0, 1, NAN, 0, 0, NULL));
    a = (struct vkdu_texture_copy_location){t->depth, 0}; b = (struct vkdu_texture_copy_location){second, 0};
    REJECT(vkdu_command_texture_copy(t->command, &b, 0, 0, 0, &a, NULL));
    REJECT(vkdu_command_resource_copy(t->command, second, t->depth));
    CHECK(vkdu_command_clear_dsv(t->command, t->dsv, 0, 3, 0.5f, 0x40, 0, NULL));
    stencil_qualify(t, 0x40, 0x41, 1); stencil_finish(t, "rejected-inputs-preserve-valid-recording", 1);
    REJECT(vkdu_command_stencil_ref(t->command, 7));
    vkdu_object_destroy(second);
}
static void check_stencil_absent_aspect(vkdu_device *device, uint32_t format)
{
    struct stencil_test_context t;
    uint32_t mode;
    stencil_context_init(&t, device, format);
    for (mode = 0; mode < 3; ++mode) {
        struct vkdu_graphics_desc desc = stencil_description(format);
        vkdu_object *pipeline; char name[96];
        /* Stencil NEVER would suppress all pixels if the absent aspect were
         * erroneously enabled. Depth remains independently LESS/GREATER/off. */
        desc.stencil.front.func = desc.stencil.back.func = D3D12_COMPARISON_FUNC_NEVER;
        desc.stencil.front.fail_op = desc.stencil.back.fail_op = D3D12_STENCIL_OP_REPLACE;
        desc.depth_enable = mode != 2;
        desc.depth_func = mode == 1 ? D3D12_COMPARISON_FUNC_GREATER : D3D12_COMPARISON_FUNC_LESS;
        pipeline = stencil_pipeline(&t, &desc); stencil_start(&t);
        CHECK(vkdu_command_clear_dsv(t.command, t.dsv, 0, 1, 0.5f, 0, 0, NULL));
        REJECT(vkdu_command_clear_dsv(t.command, t.dsv, 0, 2, 0, 5, 0, NULL));
        REJECT(vkdu_texture2d_dsv(t.dsv, 0, t.depth, 2));
        stencil_draw(&t, pipeline, 0, &stencil_all, 1, UINT32_MAX);
        snprintf(name, sizeof(name), "absent-stencil-aspect-NEVER-depth-mode%u", mode);
        stencil_finish(&t, name, mode == 1 ? 0 : 3); vkdu_object_destroy(pipeline);
    }
    stencil_context_destroy(&t);
}
static void check_stencil_pixels(vkdu_device *device, vkdu_device *other)
{
    const uint32_t formats[] = {DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
    uint32_t f;
    for (f = 0; f < 2; ++f) {
        struct stencil_test_context t;
        stencil_context_init(&t, device, formats[f]);
        check_stencil_winding(&t); check_stencil_operations(&t); check_stencil_comparisons(&t);
        check_stencil_masks_and_disable(&t); check_stencil_clear_and_readonly(&t);
        check_stencil_reference(&t); check_stencil_validation(&t, other); check_stencil_array(&t);
        stencil_context_destroy(&t);
    }
    check_stencil_absent_aspect(device, DXGI_FORMAT_D32_FLOAT);
    check_stencil_absent_aspect(device, DXGI_FORMAT_D16_UNORM);
}
