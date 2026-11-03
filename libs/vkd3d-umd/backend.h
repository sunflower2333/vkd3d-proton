/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VKD3D_UMD_BACKEND_H
#define VKD3D_UMD_BACKEND_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "private/mesa_wddm_memory_profile.h"
struct mwd_callbacks;
struct mwd_allocation;
#ifdef __cplusplus
extern "C" {
#endif

/* No Windows SDK or generated COM types cross this internal ABI. */
typedef struct vkdu_device vkdu_device;
typedef struct vkdu_object vkdu_object;
enum vkdu_kind { VKDU_BUFFER, VKDU_QUEUE, VKDU_ALLOCATOR, VKDU_COMMAND_LIST, VKDU_ROOT, VKDU_PIPELINE, VKDU_FENCE, VKDU_DESCRIPTOR_HEAP, VKDU_MEMORY_HEAP, VKDU_COMMAND_SIGNATURE, VKDU_TEXTURE2D, VKDU_QUERY_HEAP };
/* DXGI typed RGBA/BGRA UNORM and sRGB; exact WDK values checked by the DDI. */
static inline int vkdu_color_target_format(uint32_t format)
{ return format == 28 || format == 29 || format == 87 || format == 91; }
/* Typed D32/D16 and combined D24S8/D32S8 only; no typeless DS views. */
static inline int vkdu_stencil_format(uint32_t format)
{ return format == 45 || format == 20; }
static inline int vkdu_depth_format(uint32_t format)
{ return format == 40 || format == 55 || vkdu_stencil_format(format); }
/* Storage and view formats have distinct contracts. No fully typed casting
 * feature is advertised: only typeless storage may select another view type. */
static inline uint32_t vkdu_color_format_family(uint32_t format)
{
    if (format == 27 || format == 28 || format == 29) return 27;
    if (format == 90 || format == 87 || format == 91) return 90;
    return 0;
}
static inline int vkdu_color_resource_format(uint32_t format)
{ return vkdu_color_format_family(format) != 0; }
static inline int vkdu_texture_view_compatible(uint32_t storage, uint32_t view)
{
    if (view == 41) return storage == view; /* R32_FLOAT. */
    return vkdu_color_target_format(view) &&
        (storage == view || storage == vkdu_color_format_family(view));
}
static inline int vkdu_copy_format_compatible(uint32_t a, uint32_t b)
{ return a == b || (vkdu_color_format_family(a) && vkdu_color_format_family(a) == vkdu_color_format_family(b)); }
struct vkdu_adapter { uint8_t luid[8]; uint32_t vendor_id, device_id; };
struct vkdu_boolean_fact { uint32_t known, value; };
struct vkdu_memory_expectation {
    uint8_t luid[8];
    uint64_t generation;
    struct vkdu_boolean_fact cached_coherent, owned_allocations_only;
};
struct vkdu_memory_facts {
    struct mwd_memory_profile_v1 raw;
    VkPhysicalDeviceMemoryProperties properties;
    uint32_t coherent_cached_types;
    struct vkdu_boolean_fact uma, io_coherent, cache_coherent;
    uint32_t gpu_va_bits_known, gpu_va_bits; /* Unknown in this profile. */
};
/* Internal metadata query. A valid raw false/unknown can return S_OK while
 * derived facts remain unknown. No embedded D3D device is created. */
int32_t vkdu_memory_facts_for_device(PFN_vkGetInstanceProcAddr loader, VkInstance instance,
        VkPhysicalDevice physical, const struct vkdu_memory_expectation *expected, struct vkdu_memory_facts *out);
int32_t vkdu_query_adapter_memory_facts(PFN_vkGetInstanceProcAddr loader,
        const struct vkdu_memory_expectation *expected, struct vkdu_memory_facts *out);
static inline int vkdu_memory_facts_qualified(const struct vkdu_memory_facts *facts)
{
    return facts && facts->uma.known && facts->uma.value && facts->io_coherent.known &&
        facts->io_coherent.value && facts->cache_coherent.known && facts->cache_coherent.value;
}
struct vkdu_descriptor_range { uint32_t type, count, shader_register, register_space, offset; };
struct vkdu_descriptor_span { vkdu_object *heap; uint32_t first, count; };
enum vkdu_barrier_type { VKDU_BARRIER_TRANSITION, VKDU_BARRIER_UAV, VKDU_BARRIER_ALIASING };
struct vkdu_resource_barrier {
    enum vkdu_barrier_type type;
    vkdu_object *resource; /* UAV target, transition target or alias before. */
    uint32_t before, after;
    vkdu_object *alias_after; /* Either alias endpoint may be NULL. */
    uint32_t subresource_plus_one; /* Zero selects all; otherwise index + 1. */
};
struct vkdu_root_parameter {
    uint32_t type, visibility, shader_register, register_space, constant_count;
    const struct vkdu_descriptor_range *ranges;
    uint32_t range_count;
};
struct vkdu_sampler_desc {
    uint32_t filter, address_u, address_v, address_w;
    float mip_bias;
    uint32_t max_anisotropy, comparison;
    float border[4], min_lod, max_lod;
};
struct vkdu_static_sampler {
    struct vkdu_sampler_desc desc;
    uint32_t border_color, shader_register, register_space, visibility;
};
struct vkdu_texture_copy_location {
    vkdu_object *resource;
    uint64_t offset; /* Byte offset for a footprint; subresource index otherwise. */
    uint32_t footprint, format, width, height, depth, row_pitch;
};
struct vkdu_box { uint32_t left, top, front, right, bottom, back; };
struct vkdu_signature { uint32_t system_value, reg, mask, component_type; };
struct vkdu_input_element { uint32_t slot, offset, format, per_instance, step_rate, reg; };
struct vkdu_vertex_buffer { vkdu_object *buffer; uint64_t offset; uint32_t size, stride; };
struct vkdu_stream_output_entry { uint32_t stream, slot, reg, mask; };
enum { VKDU_MAX_SO_ENTRIES = 512, VKDU_MAX_SO_ELEMENTS = 2048 };
struct vkdu_stream_output_desc {
    uint32_t entry_count, stride_count, rasterized_stream;
    struct vkdu_stream_output_entry entries[VKDU_MAX_SO_ENTRIES];
    uint32_t strides[4];
};
struct vkdu_stream_output_buffer {
    vkdu_object *buffer, *counter;
    uint64_t offset, size, counter_offset;
};
enum { VKDU_MAX_RENDER_TARGETS = 8 };
struct vkdu_render_target_desc {
    uint32_t format, write_mask;
    uint32_t blend_enable, src_blend, dst_blend, blend_op, src_blend_alpha, dst_blend_alpha, blend_op_alpha;
};
struct vkdu_render_target_binding { vkdu_object *heap; uint32_t index; };
struct vkdu_stencil_face { uint32_t fail_op, depth_fail_op, pass_op, func; };
struct vkdu_stencil_state {
    uint32_t enable, front_enable, back_enable, read_mask, write_mask;
    struct vkdu_stencil_face front, back;
};
struct vkdu_graphics_desc {
    uint32_t format, sample_mask, fill_mode, cull_mode, front_ccw, depth_clip, write_mask;
    uint32_t input_count;
    const struct vkdu_input_element *inputs;
    uint32_t depth_format, depth_enable, depth_write, depth_func;
    uint32_t blend_enable, src_blend, dst_blend, blend_op, src_blend_alpha, dst_blend_alpha, blend_op_alpha;
    /* Zero preserves the original triangle default for internal callers. */
    uint32_t topology_type, strip_cut;
    const void *gs;
    size_t gs_size;
    const struct vkdu_stream_output_desc *stream_output;
    const void *hs, *ds;
    size_t hs_size, ds_size;
    /* Non-NULL selects explicit effective per-target states, including count
     * zero. NULL/count zero preserves the scalar single-target adapter above.
     * The array is consumed synchronously; it is never retained. */
    uint32_t render_target_count;
    const struct vkdu_render_target_desc *render_targets;
    struct vkdu_stencil_state stencil;
};
struct vkdu_viewport { float x, y, width, height, min_depth, max_depth; };
struct vkdu_rect { int32_t left, top, right, bottom; };
/* Native VS/PS tokens need real input/output signatures, not a compute wrapper. */
int32_t vkdu_shader_link_distances(const void *producer, size_t producer_size,
        const void *consumer, size_t consumer_size, uint32_t stream, void **code, size_t *size);
int32_t vkdu_shader_dxbc(uint32_t stage, const uint32_t *tokens, uint32_t words,
        const struct vkdu_signature *inputs, uint32_t input_count,
        const struct vkdu_signature *outputs, uint32_t output_count, void **code, size_t *size);
void vkdu_shader_dxbc_free(void *code);
int32_t vkdu_shader_tess_dxbc(uint32_t stage, const uint32_t *tokens, uint32_t words,
        const struct vkdu_signature *inputs, uint32_t input_count,
        const struct vkdu_signature *outputs, uint32_t output_count,
        const struct vkdu_signature *patch, uint32_t patch_count, void **code, size_t *size);
int32_t vkdu_stream_output_validate(const struct vkdu_stream_output_desc *desc,
        const void *code, size_t size);
int32_t vkdu_graphics_pipeline_create_sampled(vkdu_device *, vkdu_object *, const void *, size_t,
        const void *, size_t, const struct vkdu_graphics_desc *, uint32_t, uint32_t, vkdu_object **);
int32_t vkdu_graphics_pipeline_create(vkdu_device *device, vkdu_object *root,
        const void *vs, size_t vs_size, const void *ps, size_t ps_size,
        const struct vkdu_graphics_desc *desc, vkdu_object **out);
int32_t vkdu_render_target_create(vkdu_device *device, uint32_t width, uint32_t height, vkdu_object **out);
int32_t vkdu_render_target_create_format(vkdu_device *device, uint32_t width, uint32_t height, uint32_t format, vkdu_object **out);
int32_t vkdu_native_image_import(vkdu_device *device, void *owner, void *token,
        uint32_t width, uint32_t height, uint32_t format, uint32_t stride,
        int render_target, int preserve_contents, vkdu_object **out);
int32_t vkdu_device_create_shared_native(PFN_vkGetInstanceProcAddr loader, const uint8_t luid[8],
        const struct mwd_callbacks *callbacks, void *owner, vkdu_device **out);
int32_t vkdu_render_target_allocation(vkdu_device *device, uint32_t width, uint32_t height, uint64_t *bytes, uint64_t *alignment);
int32_t vkdu_render_target_allocation_format(vkdu_device *device, uint32_t width, uint32_t height,
        uint32_t format, uint64_t *bytes, uint64_t *alignment);
int32_t vkdu_render_target_heap_import_aligned(vkdu_device *, void *, void *, uint64_t, uint64_t, vkdu_object **);
int32_t vkdu_render_target_heap_import(vkdu_device *device, void *owner, void *token, uint64_t bytes, vkdu_object **out);
int32_t vkdu_render_target_place(vkdu_device *device, vkdu_object *heap, uint32_t width, uint32_t height, uint32_t state, vkdu_object **out);
int32_t vkdu_render_target_place_format(vkdu_device *device, vkdu_object *heap, uint32_t width, uint32_t height,
        uint32_t format, uint32_t state, vkdu_object **out);
int32_t vkdu_depth_create(vkdu_device *device, uint32_t width, uint32_t height, uint32_t format, vkdu_object **out);
int32_t vkdu_depth_allocation(vkdu_device *device, uint32_t width, uint32_t height, uint32_t format, uint64_t *bytes, uint64_t *alignment);
int32_t vkdu_depth_place(vkdu_device *device, vkdu_object *heap, uint32_t width, uint32_t height, uint32_t format, uint32_t state, vkdu_object **out);
int32_t vkdu_texture2d_dsv(vkdu_object *heap, uint32_t index, vkdu_object *texture, uint32_t flags);
int32_t vkdu_texture2d_dsv_range(vkdu_object *, uint32_t, vkdu_object *, uint32_t, uint32_t, uint32_t, uint32_t);
int32_t vkdu_texture2d_rtv(vkdu_object *heap, uint32_t index, vkdu_object *texture);
int32_t vkdu_texture2d_rtv_format(vkdu_object *heap, uint32_t index, vkdu_object *texture, uint32_t format);
int32_t vkdu_texture2d_rtv_range(vkdu_object *, uint32_t, vkdu_object *, uint32_t, uint32_t, uint32_t, uint32_t);
int32_t vkdu_command_graphics_root(vkdu_object *command, vkdu_object *root);
int32_t vkdu_command_graphics_table(vkdu_object *command, uint32_t index, vkdu_object *heap, uint32_t first);
int32_t vkdu_command_graphics_uav(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset);
int32_t vkdu_command_graphics_cbv(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset);
int32_t vkdu_command_graphics_srv(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset);
int32_t vkdu_command_graphics_constants(vkdu_object *command, uint32_t index, uint32_t offset,
        uint32_t count, const uint32_t *values);
int32_t vkdu_command_topology(vkdu_object *command, uint32_t topology);
int32_t vkdu_command_viewports(vkdu_object *command, uint32_t count, const struct vkdu_viewport *views);
int32_t vkdu_command_scissors(vkdu_object *command, uint32_t count, const struct vkdu_rect *rects);
int32_t vkdu_command_render_target(vkdu_object *command, vkdu_object *heap, uint32_t index);
int32_t vkdu_command_targets(vkdu_object *command, vkdu_object *color_heap, uint32_t color_index,
        vkdu_object *depth_heap, uint32_t depth_index);
int32_t vkdu_command_render_targets(vkdu_object *command, uint32_t count,
        const struct vkdu_render_target_binding *targets, vkdu_object *depth_heap, uint32_t depth_index);
int32_t vkdu_command_clear_dsv(vkdu_object *command, vkdu_object *heap, uint32_t index,
        uint32_t flags, float depth, uint32_t stencil, uint32_t count, const struct vkdu_rect *rects);
int32_t vkdu_command_blend_factor(vkdu_object *command, const float values[4]);
int32_t vkdu_command_stencil_ref(vkdu_object *command, uint32_t reference);
int32_t vkdu_command_clear_rtv(vkdu_object *command, vkdu_object *heap, uint32_t index,
        const float color[4], uint32_t count, const struct vkdu_rect *rects);
int32_t vkdu_command_index_buffer(vkdu_object *command, vkdu_object *buffer, uint64_t offset, uint32_t size, uint32_t format);
int32_t vkdu_input_layout_validate(uint32_t count, const struct vkdu_input_element *elements);
int32_t vkdu_command_vertex_buffers(vkdu_object *command, uint32_t first, uint32_t count,
        const struct vkdu_vertex_buffer *buffers);
int32_t vkdu_command_stream_output(vkdu_object *command, uint32_t first, uint32_t count,
        const struct vkdu_stream_output_buffer *buffers);
int32_t vkdu_command_draw(vkdu_object *command, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t first_instance);
int32_t vkdu_command_draw_indexed(vkdu_object *command, uint32_t indices, uint32_t instances,
        uint32_t first, int32_t base_vertex, uint32_t first_instance);

int32_t vkdu_device_create(PFN_vkGetInstanceProcAddr loader, const struct vkdu_adapter *adapter, vkdu_device **out);
/* Native runtime adapter identity is the KMD-provided LUID. Vulkan vendor and
 * device IDs name the host GPU, not PCI 1AF4:1050; never invent their mapping. */
int32_t vkdu_device_create_runtime(PFN_vkGetInstanceProcAddr loader, const uint8_t luid[8], vkdu_device **out);
int32_t vkdu_device_create_shared(PFN_vkGetInstanceProcAddr loader, const struct vkdu_memory_expectation *expected,
        const struct mwd_callbacks *callbacks, void *owner, struct vkdu_memory_facts *facts, vkdu_device **out);
/* SM6 DDI shader code: the DXIL program part ('DXIL' bitcode header at word 2). */
int vkdu_shader_is_dxil(const uint32_t *tokens, uint32_t words);
/* Highest D3D_SHADER_MODEL the embedded device (dxil-spirv on this ICD) supports. */
uint32_t vkdu_device_shader_model(vkdu_device *device);
int32_t vkdu_device_create_shared_native_facts(PFN_vkGetInstanceProcAddr loader, const struct vkdu_memory_expectation *expected,
        const struct mwd_callbacks *callbacks, void *owner, struct vkdu_memory_facts *facts, vkdu_device **out);
int32_t vkdu_memory_heap_import(vkdu_device *device, void *owner, void *token, uint64_t bytes,
        int cpu_visible, vkdu_object **out);
/* Buffer read states can be combined; write states must remain exclusive. */
static inline int vkdu_buffer_read_state(uint32_t state) { return !(state & ~0xac3u); }
int32_t vkdu_buffer_place(vkdu_device *device, vkdu_object *heap, uint64_t offset,
        uint64_t bytes, uint32_t state, vkdu_object **out);
/* Test-only entrypoint is absent from the production bridge. CPU Vulkan only. */
#ifdef VKDU_ENABLE_TEST_DEVICE
int32_t vkdu_test_device_create(PFN_vkGetInstanceProcAddr loader, vkdu_device **out);
int32_t vkdu_test_memory_heap_create(vkdu_device *device, uint64_t bytes, int cpu_visible, vkdu_object **out);
int32_t vkdu_test_texture_heap_create(vkdu_device *device, uint64_t bytes, vkdu_object **out);
int32_t vkdu_test_attachment_heap_create_aligned(vkdu_device *, uint64_t, uint64_t, vkdu_object **);
int32_t vkdu_test_attachment_heap_create(vkdu_device *device, uint64_t bytes, vkdu_object **out);
int32_t vkdu_test_destroy_counter(vkdu_object *object, uint32_t *counter);
int32_t vkdu_test_retire_hook(vkdu_object *object, void (*hook)(void *), void *context);
void *vkdu_test_command_iface(vkdu_object *command);
uint32_t vkdu_test_counter_load(uint32_t *counter);
#endif
void vkdu_device_destroy(vkdu_device *device);
int32_t vkdu_device_status(vkdu_device *device);
/* These values are the native DDI format bits, checked against the WDK at the
 * boundary. Capabilities are intersected with implemented native operations. */
enum vkdu_format_flags {
    VKDU_FORMAT_SAMPLE = 0x1, VKDU_FORMAT_RENDER_TARGET = 0x2, VKDU_FORMAT_BLENDABLE = 0x4,
    VKDU_FORMAT_VERTEX = 0x100, VKDU_FORMAT_UAV_WRITE = 0x200,
    VKDU_FORMAT_BUFFER = 0x400, VKDU_FORMAT_GATHER = 0x4000, VKDU_FORMAT_UAV_READ = 0x20000,
};
int32_t vkdu_format_support(vkdu_device *device, uint32_t format, uint32_t *flags);
int32_t vkdu_multisample_quality(vkdu_device *device, uint32_t format, uint32_t samples,
        uint32_t flags, uint32_t *levels);
int32_t vkdu_device_remove(vkdu_device *device, int32_t reason);
/* Caller already owns a live reference. A successful retain owns the wrapper
 * and its COM device/object until the matching vkdu_object_destroy. */
int vkdu_object_retain(vkdu_object *object);
void vkdu_object_destroy(vkdu_object *object);
int vkdu_object_is(vkdu_object *object, enum vkdu_kind kind);
int vkdu_same_device(vkdu_object *a, vkdu_object *b);
int vkdu_object_belongs(vkdu_device *device, vkdu_object *object);
int32_t vkdu_buffer_create(vkdu_device *device, uint64_t bytes, uint32_t heap_type, uint32_t flags, uint32_t state, vkdu_object **out);
int32_t vkdu_buffer_allocation(vkdu_device *device, uint64_t width, uint64_t *bytes, uint64_t *alignment);
int32_t vkdu_buffer_map(vkdu_object *buffer, uint64_t begin, uint64_t end, void **out);
int32_t vkdu_buffer_unmap(vkdu_object *buffer, uint64_t begin, uint64_t end);
uint64_t vkdu_buffer_address(vkdu_object *buffer);
uint64_t vkdu_buffer_size(vkdu_object *buffer);
/* Single-mip, single-layer R32_FLOAT / RGBA8_UNORM staging and sampling.
 * Committed backend resources, not native runtime heap/texture admission. */
int32_t vkdu_texture2d_create(vkdu_device *device, uint32_t width, uint32_t height,
        uint32_t format, vkdu_object **out);
/* flags is zero, ALLOW_RENDER_TARGET(1), or ALLOW_DEPTH_STENCIL(2). This is
 * an internal full-resource description, not an external runtime ABI. */
struct vkdu_texture2d_shape { uint32_t width, height, format, mips, layers, flags, sample_count, sample_quality; };
static inline uint64_t vkdu_texture2d_alignment(const struct vkdu_texture2d_shape *s)
{ return s->sample_count > 1 ? 4194304 : 65536; }
static inline int vkdu_texture2d_shape_valid(const struct vkdu_texture2d_shape *s)
{
    uint32_t size, levels = 0;
    if (!s || !s->width || !s->height || s->width > 16384 || s->height > 16384 ||
            !s->mips || !s->layers || s->layers > 2048 || s->sample_quality ||
            (s->sample_count != 1 && s->sample_count != 2 && s->sample_count != 4) ||
            (s->sample_count > 1 && (s->mips != 1 || s->flags != 1 ||
             (s->format != 28 && s->format != 87)))) return 0;
    size = s->width > s->height ? s->width : s->height;
    do { ++levels; size >>= 1; } while (size);
    return s->mips <= levels;
}
/* Private Phase A probe: never used to advertise public quality levels. */
int32_t vkdu_private_color_samples(vkdu_device *, uint32_t, uint32_t, uint32_t);
int32_t vkdu_texture2d_shape_allocation(vkdu_device *, const struct vkdu_texture2d_shape *, uint64_t *, uint64_t *);
int32_t vkdu_texture2d_shape_create(vkdu_device *, const struct vkdu_texture2d_shape *, vkdu_object **);
int32_t vkdu_texture2d_shape_place(vkdu_device *, vkdu_object *, uint64_t,
        const struct vkdu_texture2d_shape *, uint32_t, vkdu_object **);
int32_t vkdu_texture2d_srv_range(vkdu_object *, uint32_t, vkdu_object *, uint32_t,
        uint32_t, uint32_t, uint32_t, uint32_t);
int32_t vkdu_texture2d_allocation(vkdu_device *device, uint32_t width, uint32_t height,
        uint32_t format, uint64_t *bytes, uint64_t *alignment);
int32_t vkdu_texture_heap_import(vkdu_device *device, void *owner, void *token, uint64_t bytes, vkdu_object **out);
int32_t vkdu_texture2d_place(vkdu_device *device, vkdu_object *heap, uint64_t offset,
        uint32_t width, uint32_t height, uint32_t format, uint32_t state, vkdu_object **out);
int32_t vkdu_command_texture_copy(vkdu_object *command, const struct vkdu_texture_copy_location *dst,
        uint32_t x, uint32_t y, uint32_t z, const struct vkdu_texture_copy_location *src,
        const struct vkdu_box *box);
int32_t vkdu_texture2d_srv(vkdu_object *heap, uint32_t index, vkdu_object *texture);
int32_t vkdu_texture2d_srv_format(vkdu_object *heap, uint32_t index, vkdu_object *texture, uint32_t format);
uint32_t vkdu_texture2d_format(vkdu_object *texture);
int32_t vkdu_command_texture_upload(vkdu_object *command, vkdu_object *texture,
        vkdu_object *upload, uint64_t offset, uint32_t row_pitch);
int32_t vkdu_sampler_create(vkdu_object *heap, uint32_t index, const struct vkdu_sampler_desc *desc);
int32_t vkdu_heap_create(vkdu_device *device, uint32_t type, uint32_t count, int shader_visible, vkdu_object **out);
int32_t vkdu_query_heap_create(vkdu_device *device, uint32_t type, uint32_t count, vkdu_object **out);
int32_t vkdu_command_query(vkdu_object *command, vkdu_object *heap, uint32_t type, uint32_t index, int begin);
int32_t vkdu_command_query_resolve(vkdu_object *command, vkdu_object *heap, uint32_t type,
        uint32_t first, uint32_t count, vkdu_object *destination, uint64_t offset);
int32_t vkdu_pageable_backing(vkdu_object *object, uint32_t *count, struct mwd_allocation *allocations);
uint32_t vkdu_descriptor_size(vkdu_device *device, uint32_t type);
uint64_t vkdu_heap_start(vkdu_object *heap, int gpu);
int vkdu_heap_resolve(vkdu_object *heap, uint64_t handle, int gpu, uint32_t *index);
int32_t vkdu_buffer_uav(vkdu_object *heap, uint32_t index, vkdu_object *buffer,
        uint32_t format, uint64_t first, uint32_t count, uint32_t stride, uint32_t flags,
        vkdu_object *counter, uint64_t counter_offset);
int32_t vkdu_buffer_cbv(vkdu_object *heap, uint32_t index, vkdu_object *buffer, uint64_t offset, uint32_t bytes);
int32_t vkdu_buffer_srv(vkdu_object *heap, uint32_t index, vkdu_object *buffer,
        uint32_t format, uint64_t first, uint32_t count, uint32_t stride, uint32_t flags, uint32_t mapping);
int32_t vkdu_descriptor_copy(vkdu_object *dst, uint32_t dst_index, vkdu_object *src, uint32_t src_index, uint32_t count);
int32_t vkdu_descriptor_copy_ranges(vkdu_device *device, uint32_t type,
        uint32_t dst_count, const struct vkdu_descriptor_span *dst,
        uint32_t src_count, const struct vkdu_descriptor_span *src);
int32_t vkdu_command_heaps(vkdu_object *command, uint32_t count, vkdu_object *const *heaps);
int32_t vkdu_command_table(vkdu_object *command, uint32_t index, vkdu_object *heap, uint32_t first);
int32_t vkdu_queue_create(vkdu_device *device, uint32_t type, vkdu_object **out);
int32_t vkdu_queue_bind_runtime(vkdu_object *queue, void *owner, void *token);
int32_t vkdu_queue_drain_enqueue(vkdu_object *queue);
int32_t vkdu_allocator_create(vkdu_device *device, uint32_t type, vkdu_object **out);
int32_t vkdu_allocator_reset(vkdu_object *allocator);
int32_t vkdu_command_create(vkdu_device *device, vkdu_object *allocator, uint32_t type, vkdu_object **out);
int32_t vkdu_command_close(vkdu_object *command);
int32_t vkdu_command_execute_bundle(vkdu_object *command, vkdu_object *bundle);
int32_t vkdu_command_native_present(vkdu_object *command, vkdu_object *queue,
        vkdu_object *source, vkdu_object *destination);
int32_t vkdu_command_reset(vkdu_object *command, vkdu_object *allocator);
int32_t vkdu_command_clear_root_arguments(vkdu_object *command);
int32_t vkdu_command_predication(vkdu_object *command, vkdu_object *buffer, uint64_t offset, uint32_t operation);
int32_t vkdu_command_clear_uav(vkdu_object *command, vkdu_object *gpu_heap, uint32_t gpu_index,
        vkdu_object *cpu_heap, uint32_t cpu_index, vkdu_object *resource, int floating,
        const void *values, uint32_t count, const struct vkdu_rect *rects);
int32_t vkdu_command_copy(vkdu_object *command, vkdu_object *dst, uint64_t dst_offset, vkdu_object *src, uint64_t src_offset, uint64_t bytes);
int32_t vkdu_command_resolve(vkdu_object *, vkdu_object *, uint32_t, vkdu_object *, uint32_t, uint32_t);
int32_t vkdu_command_resource_copy(vkdu_object *command, vkdu_object *dst, vkdu_object *src);
int32_t vkdu_command_transition(vkdu_object *command, vkdu_object *resource, uint32_t before, uint32_t after);
/* Validate the entire batch before changing command-list state. */
int32_t vkdu_command_barriers(vkdu_object *command, uint32_t count, const struct vkdu_resource_barrier *barriers);
int32_t vkdu_root_create(vkdu_device *device, const struct vkdu_root_parameter *parameters, uint32_t count, uint32_t flags, vkdu_object **out);
int32_t vkdu_root_create_samplers(vkdu_device *device, const struct vkdu_root_parameter *parameters,
        uint32_t count, uint32_t flags, const struct vkdu_static_sampler *samplers,
        uint32_t sampler_count, vkdu_object **out);
int32_t vkdu_pipeline_create(vkdu_device *device, vkdu_object *root, const void *code, size_t size, vkdu_object **out);
int32_t vkdu_pipeline_create_tokens(vkdu_device *device, vkdu_object *root, const uint32_t *tokens, uint32_t words, vkdu_object **out);
int32_t vkdu_command_root(vkdu_object *command, vkdu_object *root);
int32_t vkdu_command_pipeline(vkdu_object *command, vkdu_object *pipeline);
int32_t vkdu_command_uav(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset);
int32_t vkdu_command_cbv(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset);
int32_t vkdu_command_srv(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset);
int32_t vkdu_command_constants(vkdu_object *command, uint32_t index, uint32_t offset,
        uint32_t count, const uint32_t *values);
int32_t vkdu_command_dispatch(vkdu_object *command, uint32_t x, uint32_t y, uint32_t z);
/* Dispatch-only signature: one 12-byte DISPATCH record, optionally padded.
 * Arguments and optional count remain GPU buffers; never read them on CPU. */
int32_t vkdu_dispatch_signature_create(vkdu_device *device, uint32_t stride, vkdu_object **out);
int32_t vkdu_command_execute_indirect(vkdu_object *command, vkdu_object *signature, uint32_t maximum,
        vkdu_object *arguments, uint64_t argument_offset, vkdu_object *count, uint64_t count_offset);
int32_t vkdu_queue_execute(vkdu_object *queue, uint32_t count, vkdu_object *const *commands);
int32_t vkdu_fence_create(vkdu_device *device, uint64_t initial, vkdu_object **out);
int32_t vkdu_queue_signal(vkdu_object *queue, vkdu_object *fence, uint64_t value);
int32_t vkdu_queue_wait(vkdu_object *queue, vkdu_object *fence, uint64_t value);
int32_t vkdu_queue_clock(vkdu_object *queue, uint64_t *gpu, uint64_t *cpu);
int32_t vkdu_fence_completed(vkdu_object *fence, uint64_t *value);
int32_t vkdu_fence_signal_cpu(vkdu_object *fence, uint64_t value);
typedef struct vkd3d_wddm_fence_event vkdu_fence_event;
int32_t vkdu_fence_event_create(vkdu_object *fence, uint64_t value, vkdu_fence_event **out);
int32_t vkdu_fence_event_wait(vkdu_fence_event *event, uint32_t timeout_ms);
void vkdu_fence_event_cancel(vkdu_fence_event *event);
void vkdu_fence_event_destroy(vkdu_fence_event *event);
int32_t vkdu_fence_wait(vkdu_object *fence, uint64_t value, uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
