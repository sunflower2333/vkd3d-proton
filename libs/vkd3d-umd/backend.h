/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VKD3D_UMD_BACKEND_H
#define VKD3D_UMD_BACKEND_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif

/* No Windows SDK or generated COM types cross this internal ABI. */
typedef struct vkdu_device vkdu_device;
typedef struct vkdu_object vkdu_object;
enum vkdu_kind { VKDU_BUFFER, VKDU_QUEUE, VKDU_ALLOCATOR, VKDU_COMMAND_LIST, VKDU_ROOT, VKDU_PIPELINE, VKDU_FENCE };
struct vkdu_adapter { uint8_t luid[8]; uint32_t vendor_id, device_id; };
struct vkdu_root_parameter { uint32_t type, visibility, shader_register, register_space, constant_count; };

int32_t vkdu_device_create(PFN_vkGetInstanceProcAddr loader, const struct vkdu_adapter *adapter, vkdu_device **out);
/* Test-only entrypoint is absent from the production bridge. CPU Vulkan only. */
#ifdef VKDU_ENABLE_TEST_DEVICE
int32_t vkdu_test_device_create(PFN_vkGetInstanceProcAddr loader, vkdu_device **out);
#endif
void vkdu_device_destroy(vkdu_device *device);
int32_t vkdu_device_status(vkdu_device *device);
void vkdu_object_destroy(vkdu_object *object);
int vkdu_object_is(vkdu_object *object, enum vkdu_kind kind);
int vkdu_same_device(vkdu_object *a, vkdu_object *b);
int vkdu_object_belongs(vkdu_device *device, vkdu_object *object);
int32_t vkdu_buffer_create(vkdu_device *device, uint64_t bytes, uint32_t heap_type, uint32_t flags, uint32_t state, vkdu_object **out);
int32_t vkdu_buffer_map(vkdu_object *buffer, uint64_t begin, uint64_t end, void **out);
int32_t vkdu_buffer_unmap(vkdu_object *buffer, uint64_t begin, uint64_t end);
uint64_t vkdu_buffer_address(vkdu_object *buffer);
uint64_t vkdu_buffer_size(vkdu_object *buffer);
int32_t vkdu_queue_create(vkdu_device *device, uint32_t type, vkdu_object **out);
int32_t vkdu_allocator_create(vkdu_device *device, uint32_t type, vkdu_object **out);
int32_t vkdu_allocator_reset(vkdu_object *allocator);
int32_t vkdu_command_create(vkdu_device *device, vkdu_object *allocator, uint32_t type, vkdu_object **out);
int32_t vkdu_command_close(vkdu_object *command);
int32_t vkdu_command_reset(vkdu_object *command, vkdu_object *allocator);
int32_t vkdu_command_copy(vkdu_object *command, vkdu_object *dst, uint64_t dst_offset, vkdu_object *src, uint64_t src_offset, uint64_t bytes);
int32_t vkdu_command_transition(vkdu_object *command, vkdu_object *resource, uint32_t before, uint32_t after);
int32_t vkdu_root_create(vkdu_device *device, const struct vkdu_root_parameter *parameters, uint32_t count, uint32_t flags, vkdu_object **out);
int32_t vkdu_pipeline_create(vkdu_device *device, vkdu_object *root, const void *code, size_t size, vkdu_object **out);
int32_t vkdu_pipeline_create_tokens(vkdu_device *device, vkdu_object *root, const uint32_t *tokens, uint32_t words, vkdu_object **out);
int32_t vkdu_command_root(vkdu_object *command, vkdu_object *root);
int32_t vkdu_command_pipeline(vkdu_object *command, vkdu_object *pipeline);
int32_t vkdu_command_uav(vkdu_object *command, uint32_t index, vkdu_object *buffer, uint64_t offset);
int32_t vkdu_command_dispatch(vkdu_object *command, uint32_t x, uint32_t y, uint32_t z);
int32_t vkdu_queue_execute(vkdu_object *queue, uint32_t count, vkdu_object *const *commands);
int32_t vkdu_fence_create(vkdu_device *device, uint64_t initial, vkdu_object **out);
int32_t vkdu_queue_signal(vkdu_object *queue, vkdu_object *fence, uint64_t value);
int32_t vkdu_queue_wait(vkdu_object *queue, vkdu_object *fence, uint64_t value);
int32_t vkdu_fence_completed(vkdu_object *fence, uint64_t *value);
int32_t vkdu_fence_wait(vkdu_object *fence, uint64_t value, uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
