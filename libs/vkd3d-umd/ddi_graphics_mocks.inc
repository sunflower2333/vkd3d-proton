/* SPDX-License-Identifier: LGPL-2.1-or-later */
static unsigned graphics_calls, graphics_pipelines;
static uint32_t draw_count, draw_instances, draw_first, draw_instance, graphics_index, graphics_format, graphics_size;
static int32_t draw_base;
static uint64_t graphics_offset;
static vkdu_object *graphics_resource;
static vkdu_viewport graphics_view;
static vkdu_rect graphics_rect;
static float graphics_color[4];
static vkdu_graphics_desc graphics_state;
static void (*graphics_hook)();
static void graphics_borrow(vkdu_object *object) {
    if (!object || peer(object)->references < 2) std::abort();
}
static int32_t test_graphics_pipeline(vkdu_device *d, vkdu_object *root, const void *vs, size_t vs_bytes,
        const void *ps, size_t ps_bytes, const vkdu_graphics_desc *state, vkdu_object **out) {
    graphics_borrow(root); ++graphics_pipelines;
    if (graphics_hook) graphics_hook();
    if (vs_bytes < 44 || ps_bytes < 44 || static_cast<const UINT *>(vs)[0] != 0x43425844 ||
            static_cast<const UINT *>(ps)[0] != 0x43425844) std::abort();
    graphics_state = *state;
    *out = SUCCEEDED(next_result) ? make_peer(d, VKDU_PIPELINE) : nullptr;
    return next_result;
}
static int32_t test_graphics_root(vkdu_object *command, vkdu_object *root) {
    graphics_borrow(command); graphics_borrow(root); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    return next_result;
}
static int32_t test_draw(vkdu_object *command, uint32_t count, uint32_t instances, uint32_t first, uint32_t instance) {
    graphics_borrow(command); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    draw_count = count; draw_instances = instances; draw_first = first; draw_instance = instance;
    return next_result;
}
static int32_t test_draw_indexed(vkdu_object *command, uint32_t count, uint32_t instances,
        uint32_t first, int32_t base, uint32_t instance) {
    draw_base = base; return test_draw(command, count, instances, first, instance);
}
static int32_t test_topology(vkdu_object *command, uint32_t topology) {
    graphics_borrow(command); ++graphics_calls; graphics_format = topology; return next_result;
}
static int32_t test_viewports(vkdu_object *command, uint32_t count, const vkdu_viewport *views) {
    graphics_borrow(command); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    graphics_size = count; graphics_view = count ? *views : vkdu_viewport{}; return next_result;
}
static int32_t test_scissors(vkdu_object *command, uint32_t count, const vkdu_rect *rects) {
    graphics_borrow(command); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    graphics_size = count; graphics_rect = count ? *rects : vkdu_rect{}; return next_result;
}
static int32_t test_rtv(vkdu_object *heap, uint32_t index, vkdu_object *texture) {
    graphics_borrow(heap); graphics_borrow(texture); ++graphics_calls;
    graphics_index = index; graphics_resource = texture;
    if (graphics_hook) graphics_hook();
    return next_result;
}
static int32_t test_render_target(vkdu_object *command, vkdu_object *heap, uint32_t index) {
    graphics_borrow(command); if (heap) graphics_borrow(heap); ++graphics_calls;
    graphics_index = index; graphics_resource = heap;
    if (graphics_hook) graphics_hook();
    return next_result;
}
static int32_t test_clear_rtv(vkdu_object *command, vkdu_object *heap, uint32_t index,
        const float *color, uint32_t count, const vkdu_rect *rects) {
    graphics_borrow(command); if (!heap) return E_INVALIDARG;
    graphics_borrow(heap); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    graphics_index = index; graphics_size = count; graphics_rect = count ? *rects : vkdu_rect{};
    for (unsigned i = 0; i < 4; ++i) graphics_color[i] = color[i];
    return next_result;
}
static int32_t test_index_buffer(vkdu_object *command, vkdu_object *buffer, uint64_t offset, uint32_t size, uint32_t format) {
    graphics_borrow(command); if (buffer) graphics_borrow(buffer); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    graphics_resource = buffer; graphics_offset = offset; graphics_size = size; graphics_format = format;
    return next_result;
}
#define vkdu_graphics_pipeline_create test_graphics_pipeline
#define vkdu_command_graphics_root test_graphics_root
#define vkdu_command_draw test_draw
#define vkdu_command_draw_indexed test_draw_indexed
#define vkdu_command_topology test_topology
#define vkdu_command_viewports test_viewports
#define vkdu_command_scissors test_scissors
#define vkdu_texture2d_rtv test_rtv
#define vkdu_command_render_target test_render_target
#define vkdu_command_clear_rtv test_clear_rtv
#define vkdu_command_index_buffer test_index_buffer
