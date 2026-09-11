/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "vkd3d.h"
#include "backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include "../../tests/shaders/command/headers/execute_indirect_cs.h"

#define CHECK(expr) do { int32_t r = (expr); if (r < 0) { fprintf(stderr, "%s:%d: %s returned %08x\n", __FILE__, __LINE__, #expr, (unsigned)r); exit(1); } } while (0)
#define REJECT(expr) do { if ((expr) >= 0) { fprintf(stderr, "unexpected success: %s\n", #expr); exit(1); } } while (0)

int main(void)
{
    PFN_vkGetInstanceProcAddr loader;
    vkdu_device *device = NULL, *wrong = NULL;
    vkdu_object *upload = NULL, *buffer = NULL, *readback = NULL;
    vkdu_object *queue = NULL, *allocator = NULL, *command = NULL, *root = NULL, *pipeline = NULL, *fence = NULL;
    struct vkdu_adapter absent = {{0}, 0xffffffff, 0xffffffff};
    struct vkdu_root_parameter parameter = {4, 0, 0, 0, 0};
    uint32_t *mapped, i;
    uint64_t completed;
#ifdef _WIN32
    HMODULE module = LoadLibraryW(L"vulkan-1.dll");
    loader = module ? (PFN_vkGetInstanceProcAddr)GetProcAddress(module, "vkGetInstanceProcAddr") : NULL;
#else
    void *module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    loader = module ? (PFN_vkGetInstanceProcAddr)dlsym(module, "vkGetInstanceProcAddr") : NULL;
#endif
    if (!loader) { fprintf(stderr, "Vulkan loader is required\n"); return 1; }
    REJECT(vkdu_device_create(loader, &absent, &wrong));
    if (wrong) return 1;
    CHECK(vkdu_test_device_create(loader, &device));
    puts("TEST ONLY: CPU Vulkan backend semantics, not VIOGPU or native Windows runtime proof");
    CHECK(vkdu_queue_create(device, 0, &queue));
    CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command));
    CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
    CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, &buffer));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
    for (i = 0; i < 1024; ++i) mapped[i] = 0xdeadbeef;
    CHECK(vkdu_buffer_unmap(upload, 0, 4096));
    REJECT(vkdu_buffer_map(readback, 0, 4097, (void **)&mapped));
    REJECT(vkdu_command_copy(command, buffer, UINT64_MAX, upload, 0, 1));
    REJECT(vkdu_command_copy(command, buffer, 4095, upload, 0, 2));
    REJECT(vkdu_queue_execute(queue, 1, &command)); /* Must close before execution. */
    CHECK(vkdu_root_create(device, &parameter, 1, 0, &root));
    REJECT(vkdu_pipeline_create(device, root, "invalid", 7, &pipeline));
    if (pipeline) return 1;
    /* Use the same token form the native SM5 compute DDI receives. */
    CHECK(vkdu_pipeline_create_tokens(device, root, execute_indirect_cs_code_dxbc + 21,
            execute_indirect_cs_code_dxbc[22], &pipeline));
    CHECK(vkdu_command_copy(command, buffer, 0, upload, 0, 4096));
    CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    CHECK(vkdu_command_root(command, root));
    CHECK(vkdu_command_pipeline(command, pipeline));
    CHECK(vkdu_command_uav(command, 0, buffer, 0));
    CHECK(vkdu_command_dispatch(command, 1024, 1, 1));
    CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
    CHECK(vkdu_command_copy(command, readback, 0, buffer, 0, 4096));
    CHECK(vkdu_command_close(command));
    REJECT(vkdu_command_dispatch(command, 1, 1, 1));
    CHECK(vkdu_queue_execute(queue, 1, &command));
    CHECK(vkdu_queue_signal(queue, fence, 1));
    CHECK(vkdu_fence_wait(fence, 1, 30000));
    CHECK(vkdu_fence_completed(fence, &completed));
    if (completed < 1) return 1;
    REJECT(vkdu_fence_wait(fence, 2, 0));
    CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
    for (i = 0; i < 1024; ++i) if (mapped[i] != i) { fprintf(stderr, "readback[%u]=%u\n", i, mapped[i]); return 1; }
    CHECK(vkdu_buffer_unmap(readback, 0, 0));
    CHECK(vkdu_allocator_reset(allocator));
    CHECK(vkdu_command_reset(command, allocator));
    CHECK(vkdu_command_close(command));
    CHECK(vkdu_queue_execute(queue, 1, &command));
    CHECK(vkdu_queue_signal(queue, fence, 2));
    CHECK(vkdu_queue_wait(queue, fence, 2));
    CHECK(vkdu_fence_wait(fence, 2, 30000));
    CHECK(vkdu_device_status(device));
    /* Caller follows D3D12 lifetime rules: reset/destroy only after completion. */
    vkdu_object_destroy(command); vkdu_object_destroy(allocator);
    vkdu_object_destroy(pipeline); vkdu_object_destroy(root);
    vkdu_object_destroy(readback); vkdu_object_destroy(buffer); vkdu_object_destroy(upload);
    vkdu_object_destroy(fence); vkdu_object_destroy(queue); vkdu_device_destroy(device);
#ifdef _WIN32
    FreeLibrary(module);
#else
    dlclose(module);
#endif
    puts("PASS embedded compute shader, 1024 readbacks, copy, barriers, queue signal/wait, allocator reuse and error propagation");
    return 0;
}
