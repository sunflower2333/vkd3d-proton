/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <vector>
static unsigned graphics_calls, graphics_pipelines;
static uint32_t draw_count, draw_instances, draw_first, draw_instance, graphics_index, graphics_format, graphics_size;
static int32_t draw_base;
static uint64_t graphics_offset;
static vkdu_object *graphics_resource;
static vkdu_viewport graphics_view;
static vkdu_rect graphics_rect;
static float graphics_color[4];
static vkdu_graphics_desc graphics_state;
static vkdu_render_target_desc graphics_targets[VKDU_MAX_RENDER_TARGETS];
static vkdu_render_target_binding graphics_bindings[VKDU_MAX_RENDER_TARGETS];
static unsigned graphics_target_count;
static vkdu_input_element graphics_elements[32];
static vkdu_vertex_buffer graphics_vertex_buffers[32];
static uint32_t graphics_vertex_first, graphics_vertex_count;
static bool vertex_swap_fields;
static void (*graphics_hook)();
static uint32_t graphics_root_kind, graphics_root_index, graphics_root_count, graphics_root_values[64];
static vkdu_object *depth_resource;
static uint32_t depth_index, depth_flags, depth_stencil;
static uint32_t attachment_view_mip, attachment_view_first_layer, attachment_view_layers;
static float depth_value;
static bool graphics_root_bad_offset;
static vkdu_object *resource_copy_dst, *resource_copy_src;
static unsigned resource_copy_calls;
static uint32_t format_seen, samples_seen, format_flags_seen;
static vkdu_stream_output_desc graphics_so;
static bool graphics_has_so;
static bool graphics_copy_programs;
static bool graphics_link_distances;
static int32_t graphics_link_result;
static std::vector<uint32_t> graphics_linked_code;
static std::vector<uint32_t> graphics_vertex_code, graphics_geometry_code;
static uint32_t geometry_bytes, hull_bytes, domain_bytes, stream_first, stream_count;
static vkdu_stream_output_buffer stream_views[4];
static unsigned stream_calls;
static unsigned format_calls;
static int32_t test_format(vkdu_device *, uint32_t format, uint32_t *out) {
    ++format_calls; format_seen = format;
    if (graphics_hook) graphics_hook();
    *out = VKDU_FORMAT_SAMPLE | VKDU_FORMAT_RENDER_TARGET;
    return next_result;
}
static int32_t test_multisample(vkdu_device *, uint32_t format, uint32_t samples, uint32_t flags, uint32_t *out) {
    ++format_calls; format_seen = format; samples_seen = samples; format_flags_seen = flags;
    if (graphics_hook) graphics_hook();
    *out = 1;
    return next_result;
}
static void graphics_borrow(vkdu_object *object) {
    if (!object || peer(object)->references < 2) std::abort();
}
static unsigned clear_uav_calls;
static uint32_t clear_uav_gpu_index, clear_uav_cpu_index, clear_uav_count, clear_uav_values[4];
static int clear_uav_floating;
static vkdu_rect clear_uav_rects[19];
static int32_t test_clear_uav(vkdu_object *command, vkdu_object *gpu, uint32_t gi,
        vkdu_object *cpu, uint32_t ci, vkdu_object *resource, int floating,
        const void *values, uint32_t count, const vkdu_rect *rects) {
    vkdu_object *owners[] = {command, gpu, cpu, resource};
    for (auto *value : owners) graphics_borrow(value);
    ++clear_uav_calls;
    if (graphics_hook) graphics_hook();
    clear_uav_gpu_index = gi; clear_uav_cpu_index = ci; clear_uav_count = count; clear_uav_floating = floating;
    std::memcpy(clear_uav_values, values, sizeof(clear_uav_values));
    if (count > 19) std::abort();
    if (count) std::memcpy(clear_uav_rects, rects, count * sizeof(*rects));
    for (auto *value : owners) if (!peer(value)->references) std::abort();
    return next_result;
}
#define vkdu_command_clear_uav test_clear_uav
static unsigned predication_calls;
static uint64_t predication_offset;
static uint32_t predication_operation;
static bool predication_disabled;
static int32_t test_predication(vkdu_object *command, vkdu_object *resource, uint64_t offset, uint32_t operation) {
    graphics_borrow(command); if (resource) graphics_borrow(resource);
    ++predication_calls;
    if (graphics_hook) graphics_hook();
    predication_offset = offset; predication_operation = operation; predication_disabled = !resource;
    if (!peer(command)->references || (resource && !peer(resource)->references)) std::abort();
    return next_result;
}
#define vkdu_command_predication test_predication
static int32_t test_resource_copy(vkdu_object *command, vkdu_object *dst, vkdu_object *src) {
    graphics_borrow(command); graphics_borrow(dst); graphics_borrow(src);
    ++resource_copy_calls;
    if (graphics_hook) graphics_hook();
    resource_copy_dst = dst; resource_copy_src = src;
    if (!peer(dst)->references || !peer(src)->references || !peer(command)->references) std::abort();
    return next_result;
}
#define vkdu_command_resource_copy test_resource_copy
static uint32_t resolve_dst_sub, resolve_src_sub, resolve_format, resolve_calls;
static int32_t test_msaa_resolve(vkdu_object *command, vkdu_object *dst, uint32_t dsub,
        vkdu_object *src, uint32_t ssub, uint32_t format) {
    resolve_dst_sub=dsub; resolve_src_sub=ssub; resolve_format=format; ++resolve_calls;
    return test_resource_copy(command,dst,src);
}
#define vkdu_command_resolve test_msaa_resolve

static int32_t test_graphics_pipeline(vkdu_device *d, vkdu_object *root, const void *vs, size_t vs_bytes,
        const void *ps, size_t ps_bytes, const vkdu_graphics_desc *state, vkdu_object **out) {
    graphics_borrow(root); ++graphics_pipelines;
    if (graphics_hook) graphics_hook();
    if (vs_bytes < 44 || static_cast<const UINT *>(vs)[0] != 0x43425844 ||
            (ps_bytes && (ps_bytes < 44 || static_cast<const UINT *>(ps)[0] != 0x43425844)) ||
            (!ps != !ps_bytes) || (state->gs_size &&
            (state->gs_size < 44 || static_cast<const UINT *>(state->gs)[0] != 0x43425844))) std::abort();
    if ((state->hs_size && (state->hs_size < 48 || static_cast<const UINT *>(state->hs)[0] != 0x43425844)) ||
            (state->ds_size && (state->ds_size < 48 || static_cast<const UINT *>(state->ds)[0] != 0x43425844))) std::abort();
    if (graphics_link_distances) {
        void *linked=nullptr;size_t bytes=0;
        graphics_link_result=vkdu_shader_link_distances(vs,vs_bytes,ps,ps_bytes,0,&linked,&bytes);
        graphics_linked_code.clear();
        if (linked) {
            const auto *p=static_cast<const uint32_t *>(linked);
            graphics_linked_code.assign(p,p+bytes/4);vkdu_shader_dxbc_free(linked);
        }
        if (graphics_link_result<0) {*out=nullptr;return graphics_link_result;}
    }
    hull_bytes = static_cast<uint32_t>(state->hs_size); domain_bytes = static_cast<uint32_t>(state->ds_size);
    geometry_bytes = static_cast<uint32_t>(state->gs_size);
    if (graphics_copy_programs) {
        const auto *v=static_cast<const uint32_t *>(vs), *g=static_cast<const uint32_t *>(state->gs);
        graphics_vertex_code.assign(v,v+vs_bytes/4);
        graphics_geometry_code.clear();
        if (state->gs_size) graphics_geometry_code.assign(g,g+state->gs_size/4);
    }
    graphics_has_so = state->stream_output != nullptr;
    if (graphics_has_so) graphics_so = *state->stream_output;
    graphics_state = *state;
    if (state->render_targets) {
        if (state->render_target_count > VKDU_MAX_RENDER_TARGETS) std::abort();
        std::memcpy(graphics_targets, state->render_targets, state->render_target_count * sizeof(*graphics_targets));
        graphics_state.render_targets = graphics_targets;
    }
    if (state->input_count > 32) std::abort();
    if (state->input_count) std::memcpy(graphics_elements, state->inputs, sizeof(*state->inputs) * state->input_count);
    graphics_state.inputs = graphics_elements;
    *out = SUCCEEDED(next_result) ? make_peer(d, VKDU_PIPELINE) : nullptr;
    return next_result;
}
static vkdu_object *root_signature_seen;
static unsigned root_signature_calls, root_signature_domain;
static int32_t test_root_signature(vkdu_object *command, vkdu_object *root, unsigned domain) {
    graphics_borrow(command); if (root) graphics_borrow(root); ++graphics_calls;
    root_signature_seen = root; root_signature_domain = domain; ++root_signature_calls;
    if (graphics_hook) graphics_hook();
    if (!peer(command)->references || (root && !peer(root)->references)) std::abort();
    return next_result;
}
static int32_t test_graphics_root(vkdu_object *command, vkdu_object *root) { return test_root_signature(command, root, 1); }
static int32_t test_compute_root(vkdu_object *command, vkdu_object *root) { return test_root_signature(command, root, 2); }
#define vkdu_command_root test_compute_root
static int32_t test_clear_root(vkdu_object *command) {
    graphics_borrow(command); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    if (!peer(command)->references) std::abort();
    return next_result;
}
#define vkdu_command_clear_root_arguments test_clear_root
static int32_t test_blend_factor(vkdu_object *command, const float *values) {
    graphics_borrow(command); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    std::memcpy(graphics_color, values, sizeof(graphics_color));
    return next_result;
}
static int32_t test_graphics_table(vkdu_object *command, uint32_t index, vkdu_object *heap, uint32_t first) {
    graphics_borrow(command); graphics_borrow(heap); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    graphics_resource = heap; graphics_root_kind = 0; graphics_root_index = index; graphics_offset = first;
    return next_result;
}
template<unsigned kind> static int32_t test_graphics_descriptor(vkdu_object *command, uint32_t index,
        vkdu_object *buffer, uint64_t offset) {
    graphics_borrow(command); graphics_borrow(buffer); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    graphics_resource = buffer; graphics_root_kind = kind; graphics_root_index = index;
    graphics_offset = graphics_root_bad_offset ? 0 : offset;
    return next_result;
}
static int32_t test_graphics_constants(vkdu_object *command, uint32_t index, uint32_t offset,
        uint32_t count, const uint32_t *values) {
    graphics_borrow(command); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    if (count > 64 || (count && !values)) std::abort();
    graphics_root_kind = 1; graphics_root_index = index; graphics_offset = offset; graphics_root_count = count;
    if (count) std::memcpy(graphics_root_values, values, count * sizeof(*values));
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
#include "ddi_topology_backend_test.inc"
static int32_t test_topology(vkdu_object *command, uint32_t topology) {
    graphics_borrow(command); ++graphics_calls; graphics_format = topology;
#ifdef VKDU_TEST_TOPOLOGY_SOURCE_BODY
    if (topology_contract_active) {
        if (graphics_hook) graphics_hook();
        const auto result=topology_contract_apply(&topology_contract_state,topology);
        if (!peer(command)->references) std::abort();
        return result;
    }
#endif
    return next_result;
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
static int32_t test_rtv(vkdu_object *heap, uint32_t index, vkdu_object *texture, uint32_t format) {
    graphics_borrow(heap); graphics_borrow(texture); ++graphics_calls;
    graphics_index = index; graphics_resource = texture; graphics_format = format;
    if (graphics_hook) graphics_hook();
    return next_result;
}
static int32_t test_dsv(vkdu_object *heap, uint32_t index, vkdu_object *texture, uint32_t flags) {
    graphics_borrow(heap); graphics_borrow(texture); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    depth_index = index; depth_resource = texture; depth_flags = flags;
    return next_result;
}
static int32_t test_rtv_range(vkdu_object *heap, uint32_t index, vkdu_object *texture, uint32_t format,
        uint32_t mip, uint32_t first_layer, uint32_t layers) {
    const auto result = test_rtv(heap, index, texture, format);
    attachment_view_mip = mip; attachment_view_first_layer = first_layer; attachment_view_layers = layers;
    return result;
}
static int32_t test_dsv_range(vkdu_object *heap, uint32_t index, vkdu_object *texture, uint32_t flags,
        uint32_t mip, uint32_t first_layer, uint32_t layers) {
    const auto result = test_dsv(heap, index, texture, flags);
    attachment_view_mip = mip; attachment_view_first_layer = first_layer; attachment_view_layers = layers;
    return result;
}
static int32_t test_targets(vkdu_object *command, vkdu_object *color, uint32_t ci, vkdu_object *depth, uint32_t di) {
    graphics_borrow(command); if (color) graphics_borrow(color); if (depth) graphics_borrow(depth); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    graphics_resource = color; graphics_index = ci; depth_resource = depth; depth_index = di;
    return next_result;
}
static int32_t test_render_targets(vkdu_object *command, uint32_t count,
        const vkdu_render_target_binding *targets, vkdu_object *depth, uint32_t di) {
    graphics_borrow(command);
    if (count > VKDU_MAX_RENDER_TARGETS || (count && !targets)) return E_INVALIDARG;
    for (unsigned i = 0; i < count; ++i) {
        graphics_borrow(targets[i].heap);
        if (targets[i].index >= peer(targets[i].heap)->count) return E_INVALIDARG;
    }
    if (depth) graphics_borrow(depth);
    ++graphics_calls;
    if (graphics_hook) graphics_hook();
    graphics_target_count = count;
    for (unsigned i = 0; i < count; ++i) graphics_bindings[i] = targets[i];
    graphics_resource = count ? targets[0].heap : nullptr;
    graphics_index = count ? targets[0].index : 0;
    depth_resource = depth; depth_index = di;
    return next_result;
}
static int32_t test_clear_dsv(vkdu_object *command, vkdu_object *heap, uint32_t index,
        uint32_t flags, float depth, uint32_t stencil, uint32_t count, const vkdu_rect *rects) {
    graphics_borrow(command); if (!heap) return E_INVALIDARG;
    graphics_borrow(heap); ++graphics_calls;
    if (graphics_hook) graphics_hook();
    depth_index = index; depth_resource = heap; depth_flags = flags; depth_value = depth; depth_stencil = stencil;
    graphics_size = count; graphics_rect = count ? *rects : vkdu_rect{};
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
static int32_t test_vertex_buffers(vkdu_object *command, uint32_t first, uint32_t count, const vkdu_vertex_buffer *buffers) {
    graphics_borrow(command); ++graphics_calls;
    if (count > 32) std::abort();
    for (uint32_t i = 0; i < count; i++) if (buffers[i].buffer) graphics_borrow(buffers[i].buffer);
    if (graphics_hook) graphics_hook();
    graphics_vertex_first = vertex_swap_fields ? count : first;
    graphics_vertex_count = vertex_swap_fields ? first : count;
    if (count) std::memcpy(graphics_vertex_buffers, buffers, sizeof(*buffers) * count);
    return next_result;
}
static int32_t test_stream_output(vkdu_object *command, uint32_t first, uint32_t count,
        const vkdu_stream_output_buffer *buffers) {
    graphics_borrow(command); ++stream_calls;
    if (count > 4) std::abort();
    for (uint32_t i = 0; i < count; ++i) if (buffers[i].size) {
        graphics_borrow(buffers[i].buffer); graphics_borrow(buffers[i].counter);
    }
    if (graphics_hook) graphics_hook();
    stream_first = first; stream_count = count;
    if (count) std::memcpy(stream_views, buffers, count * sizeof(*buffers));
    return next_result;
}
#define vkdu_command_stream_output test_stream_output
static uint32_t pso_samples_seen, pso_quality_seen;
static int32_t test_msaa_pipeline(vkdu_device *d, vkdu_object *root, const void *vs, size_t vs_bytes,
        const void *ps, size_t ps_bytes, const vkdu_graphics_desc *state, uint32_t count, uint32_t quality, vkdu_object **out) {
    pso_samples_seen=count; pso_quality_seen=quality;
    return test_graphics_pipeline(d,root,vs,vs_bytes,ps,ps_bytes,state,out);
}
#define vkdu_graphics_pipeline_create_sampled test_msaa_pipeline
#define vkdu_graphics_pipeline_create test_graphics_pipeline
#define vkdu_format_support test_format
#define vkdu_multisample_quality test_multisample
#define vkdu_command_graphics_root test_graphics_root
#define vkdu_command_blend_factor test_blend_factor
#define vkdu_command_graphics_table test_graphics_table
#define vkdu_command_graphics_cbv test_graphics_descriptor<2>
#define vkdu_command_graphics_srv test_graphics_descriptor<3>
#define vkdu_command_graphics_uav test_graphics_descriptor<4>
#define vkdu_command_graphics_constants test_graphics_constants
#define vkdu_command_draw test_draw
#define vkdu_command_draw_indexed test_draw_indexed
#define vkdu_command_topology test_topology
#define vkdu_command_viewports test_viewports
#define vkdu_command_scissors test_scissors
#define vkdu_texture2d_rtv_format test_rtv
#define vkdu_texture2d_dsv test_dsv
#define vkdu_texture2d_rtv_range test_rtv_range
#define vkdu_texture2d_dsv_range test_dsv_range
#define vkdu_command_targets test_targets
#define vkdu_command_render_targets test_render_targets
#define vkdu_command_clear_dsv test_clear_dsv
#define vkdu_command_render_target test_render_target
#define vkdu_command_clear_rtv test_clear_rtv
#define vkdu_command_index_buffer test_index_buffer
#define vkdu_command_vertex_buffers test_vertex_buffers
