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
#include "../../tests/shaders/descriptors/headers/update_root_descriptors.h"
#include "../../tests/shaders/resource/headers/cs_large_tbo_load.h"
#include "../../tests/shaders/descriptors/headers/overlapping_bindings.h"
#include "../../tests/shaders/sparse/headers/update_tile_mappings_smem.h"

#define CHECK(expr) do { int32_t r = (expr); if (r < 0) { fprintf(stderr, "%s:%d: %s returned %08x\n", __FILE__, __LINE__, #expr, (unsigned)r); exit(1); } } while (0)
#define REJECT(expr) do { if ((expr) >= 0) { fprintf(stderr, "unexpected success: %s\n", #expr); exit(1); } } while (0)

#ifdef VKDU_GPU_PROBE
static struct vkdu_adapter requested_adapter;

static int parse_hex32(const char *text, uint32_t *out)
{
    size_t length = strlen(text), i;
    uint32_t value = 0;
    if (!length || length > 8) return 0;
    for (i = 0; i < length; ++i) {
        unsigned digit;
        if (text[i] >= '0' && text[i] <= '9') digit = text[i] - '0';
        else if (text[i] >= 'a' && text[i] <= 'f') digit = text[i] - 'a' + 10;
        else if (text[i] >= 'A' && text[i] <= 'F') digit = text[i] - 'A' + 10;
        else return 0;
        value = (value << 4) | digit;
    }
    *out = value;
    return 1;
}

static int32_t validation_device_create(PFN_vkGetInstanceProcAddr loader, vkdu_device **out)
{
    return vkdu_device_create(loader, &requested_adapter, out);
}
#else
static int32_t validation_device_create(PFN_vkGetInstanceProcAddr loader, vkdu_device **out)
{
    return vkdu_test_device_create(loader, out);
}
#endif

static void check_constant_buffers(vkdu_device *device, vkdu_device *other)
{
    vkdu_object *constants = NULL, *foreign = NULL, *upload = NULL, *buffer = NULL, *readback = NULL;
    vkdu_object *cpu = NULL, *gpu = NULL, *queue = NULL, *allocator = NULL, *command = NULL, *fence = NULL;
    vkdu_object *root = NULL, *table = NULL, *pipeline = NULL, *table_pipeline = NULL;
    struct vkdu_descriptor_range range = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 1};
    struct vkdu_root_parameter params[2] = {{D3D12_ROOT_PARAMETER_TYPE_CBV, 0, 0, 0, 0},
        {D3D12_ROOT_PARAMETER_TYPE_UAV, 0, 0, 0, 0}};
    uint32_t *mapped, round, i;
    CHECK(vkdu_buffer_create(device, 131072, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &constants));
    CHECK(vkdu_buffer_create(other, 256, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &foreign));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
    CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, &buffer));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_buffer_map(constants, 0, 0, (void **)&mapped));
    memset(mapped, 0, 131072);
    mapped[64] = 37; mapped[65] = 0x13579bdf;
    mapped[128] = 941; mapped[129] = 0x2468ace0;
    CHECK(vkdu_buffer_unmap(constants, 0, 131072));
    CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
    for (i = 0; i < 1024; ++i) mapped[i] = 0xdeadbeef;
    CHECK(vkdu_buffer_unmap(upload, 0, 4096));
    CHECK(vkdu_heap_create(device, 0, 8, 0, &cpu));
    CHECK(vkdu_heap_create(device, 0, 8, 1, &gpu));
    REJECT(vkdu_buffer_cbv(cpu, 8, constants, 0, 256));
    REJECT(vkdu_buffer_cbv(cpu, 1, constants, 1, 256));
    REJECT(vkdu_buffer_cbv(cpu, 1, constants, 0, 255));
    REJECT(vkdu_buffer_cbv(cpu, 1, constants, 0, 0));
    REJECT(vkdu_buffer_cbv(cpu, 1, constants, 0, 65792));
    REJECT(vkdu_buffer_cbv(cpu, 1, constants, 131072, 256));
    REJECT(vkdu_buffer_cbv(cpu, 1, constants, UINT64_MAX - 255, 256));
    REJECT(vkdu_buffer_cbv(cpu, 1, foreign, 0, 256));
    REJECT(vkdu_buffer_cbv(cpu, 1, NULL, 256, 256));
    CHECK(vkdu_buffer_cbv(cpu, 1, constants, 256, 65536));
    CHECK(vkdu_queue_create(device, 0, &queue));
    CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command));
    CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_root_create(device, params, 2, 0, &root));
    CHECK(vkdu_pipeline_create(device, root, update_root_descriptors_code_dxbc, sizeof(update_root_descriptors_code_dxbc), &pipeline));
    params[0].type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ranges = &range; params[0].range_count = 1;
    CHECK(vkdu_root_create(device, params, 2, 0, &table));
    CHECK(vkdu_pipeline_create(device, table, update_root_descriptors_code_dxbc, sizeof(update_root_descriptors_code_dxbc), &table_pipeline));
    for (round = 0; round < 4; ++round) {
        uint32_t expected_index = round == 0 ? 37 : round == 2 ? 0 : 941;
        uint32_t expected_value = round == 0 ? 0x13579bdf : round == 2 ? 0 : 0x2468ace0;
        int use_table = round == 1 || round == 2;
        fprintf(stderr, "CBV round %u: %s\n", round, use_table ? (round == 2 ? "null table" : "copied table") : "root buffer");
        if (round) {
            CHECK(vkdu_allocator_reset(allocator));
            CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
        }
        /* Reset the entire output so previous writes cannot satisfy a later
         * readback, and check all untouched words as well as the shader write. */
        CHECK(vkdu_command_copy(command, buffer, 0, upload, 0, 4096));
        CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        REJECT(vkdu_command_cbv(command, 0, constants, 256)); /* No root bound after reset. */
        CHECK(vkdu_command_root(command, use_table ? table : root));
        CHECK(vkdu_command_pipeline(command, use_table ? table_pipeline : pipeline));
        CHECK(vkdu_command_uav(command, 1, buffer, 0));
        REJECT(vkdu_command_cbv(command, 1, constants, 256));
        if (use_table) {
            CHECK(vkdu_buffer_cbv(cpu, 1, round == 1 ? constants : NULL, round == 1 ? 512 : 0, 256));
            CHECK(vkdu_descriptor_copy(gpu, 6, cpu, 1, 1));
            CHECK(vkdu_command_heaps(command, 1, &gpu));
            CHECK(vkdu_command_table(command, 0, gpu, 5));
        } else {
            REJECT(vkdu_command_cbv(command, 0, constants, 1));
            REJECT(vkdu_command_cbv(command, 0, constants, 131072));
            REJECT(vkdu_command_cbv(command, 0, foreign, 0));
            CHECK(vkdu_command_cbv(command, 0, constants, round == 0 ? 256 : 512));
            /* Rejected zero-address bindings must not replace the valid CBV.
             * Reading a raw root address zero has no null-descriptor guarantee. */
            REJECT(vkdu_command_cbv(command, 0, NULL, 0));
            REJECT(vkdu_command_cbv(command, 0, NULL, 256));
        }
        CHECK(vkdu_command_dispatch(command, 1, 1, 1));
        CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 0, buffer, 0, 4096));
        CHECK(vkdu_command_close(command));
        CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, round + 1));
        CHECK(vkdu_fence_wait(fence, round + 1, 30000));
        CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 1024; ++i) {
            uint32_t expected = i == expected_index ? expected_value : 0xdeadbeef;
            if (mapped[i] != expected) {
                fprintf(stderr, "CBV round %u word %u: %08x expected %08x\n", round, i, mapped[i], expected);
                exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
    }
    CHECK(vkdu_device_status(device));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(pipeline); vkdu_object_destroy(table_pipeline); vkdu_object_destroy(root); vkdu_object_destroy(table);
    vkdu_object_destroy(cpu); vkdu_object_destroy(gpu); vkdu_object_destroy(constants); vkdu_object_destroy(foreign);
    vkdu_object_destroy(upload); vkdu_object_destroy(buffer); vkdu_object_destroy(readback);
    puts("PASS CBV root/table offsets, copied/null descriptors, invalid root address and alignment/range/device rejection: 4x1024 readbacks");
}

static void check_shader_resources(vkdu_device *device, vkdu_device *other)
{
    vkdu_object *input = NULL, *foreign = NULL, *denied = NULL, *constants = NULL;
    vkdu_object *upload = NULL, *buffer = NULL, *readback = NULL, *cpu = NULL, *gpu = NULL;
    vkdu_object *queue = NULL, *allocator = NULL, *command = NULL, *fence = NULL;
    vkdu_object *root = NULL, *pipeline = NULL;
    uint32_t *mapped, round, i, expected[1024];
    const uint32_t mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    struct vkdu_descriptor_range range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 1};
    struct vkdu_root_parameter params[5] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, 0, 0, 0, 0},
        {D3D12_ROOT_PARAMETER_TYPE_UAV, 0, 0, 0, 0}, {D3D12_ROOT_PARAMETER_TYPE_CBV, 0, 0, 0, 0},
        {D3D12_ROOT_PARAMETER_TYPE_SRV, 0, 4, 0, 0}, {D3D12_ROOT_PARAMETER_TYPE_UAV, 0, 2, 0, 0}};
    CHECK(vkdu_buffer_create(device, 262144, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &input));
    CHECK(vkdu_buffer_create(other, 256, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &foreign));
    CHECK(vkdu_buffer_create(device, 256, 1, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON, &denied));
    CHECK(vkdu_buffer_create(device, 256, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &constants));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
    CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, &buffer));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_buffer_map(input, 0, 0, (void **)&mapped));
    for (i = 0; i < 65536; ++i) mapped[i] = 0x13570000u ^ (i * 37u);
    CHECK(vkdu_buffer_unmap(input, 0, 262144));
    CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
    for (i = 0; i < 1024; ++i) mapped[i] = 0xdeadbeef;
    CHECK(vkdu_buffer_unmap(upload, 0, 4096));
    CHECK(vkdu_heap_create(device, 0, 8, 0, &cpu));
    CHECK(vkdu_heap_create(device, 0, 8, 1, &gpu));
    REJECT(vkdu_buffer_srv(cpu, 8, input, DXGI_FORMAT_R32_UINT, 0, 64, 0, 0, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, foreign, DXGI_FORMAT_R32_UINT, 0, 64, 0, 0, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, denied, DXGI_FORMAT_R32_UINT, 0, 64, 0, 0, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, input, DXGI_FORMAT_R32_UINT, 65536, 1, 0, 0, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, input, DXGI_FORMAT_R32_UINT, 65535, 2, 0, 0, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, NULL, DXGI_FORMAT_R32_UINT, UINT64_MAX, 1, 0, 0, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, input, DXGI_FORMAT_R32_UINT, 0, 0, 0, 0, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, input, DXGI_FORMAT_R32_UINT, 0, 64, 0, 1, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, input, DXGI_FORMAT_R32_TYPELESS, 0, 64, 4, 1, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, input, DXGI_FORMAT_UNKNOWN, 0, 64, 3, 0, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, input, DXGI_FORMAT_UNKNOWN, 0, 64, 2052, 0, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, input, DXGI_FORMAT_R32_UINT, 0, 64, 0, 2, mapping));
    REJECT(vkdu_buffer_srv(cpu, 1, input, DXGI_FORMAT_R32_UINT, 0, 64, 0, 0, 0));
    REJECT(vkdu_buffer_srv(cpu, 1, input, DXGI_FORMAT_R32_UINT, 0, 64, 0, 0, mapping | 0x2000));
    REJECT(vkdu_buffer_srv(cpu, 1, input, DXGI_FORMAT_R32_UINT, 0, 64, 0, 0, mapping | 7));
    CHECK(vkdu_queue_create(device, 0, &queue));
    CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command));
    CHECK(vkdu_fence_create(device, 0, &fence));
    for (round = 0; round < 8; ++round) {
        int typed = round < 3, raw = round == 3 || round == 4;
        int root_binding = round == 4 || round == 6, null_binding = round == 1 || round == 7;
        uint32_t format = typed ? DXGI_FORMAT_R32_UINT : raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
        uint32_t count = typed ? 64 : raw ? 128 : 65528;
        const void *shader = typed ? (const void *)cs_large_tbo_load_code_dxbc : raw ?
            (const void *)overlapping_bindings_code_dxbc : (const void *)update_tile_mappings_smem_code_dxbc;
        size_t shader_bytes = typed ? sizeof(cs_large_tbo_load_code_dxbc) : raw ?
            sizeof(overlapping_bindings_code_dxbc) : sizeof(update_tile_mappings_smem_code_dxbc);
        fprintf(stderr, "SRV round %u: %s %s\n", round, typed ? "typed" : raw ? "raw" : "structured",
                root_binding ? "root offset" : null_binding ? "null table" : round == 2 ? "rejected swizzle preserves view" : "copied table offset");
        if (round) {
            CHECK(vkdu_allocator_reset(allocator));
            CHECK(vkdu_command_reset(command, allocator));
            vkdu_object_destroy(pipeline); vkdu_object_destroy(root); pipeline = root = NULL;
            CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
        }
        CHECK(vkdu_buffer_map(constants, 0, 0, (void **)&mapped));
        memset(mapped, 0, 256); mapped[0] = typed ? 23 : 64; mapped[1] = 32; mapped[2] = 3;
        CHECK(vkdu_buffer_unmap(constants, 0, 256));
        params[0].type = root_binding ? D3D12_ROOT_PARAMETER_TYPE_SRV : D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].ranges = root_binding ? NULL : &range; params[0].range_count = root_binding ? 0 : 1;
        params[1].shader_register = typed ? 1 : 0;
        CHECK(vkdu_root_create(device, params, raw ? 5 : 3, 0, &root));
        CHECK(vkdu_pipeline_create(device, root, shader, shader_bytes, &pipeline));
        CHECK(vkdu_command_copy(command, buffer, 0, upload, 0, 4096));
        CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        REJECT(vkdu_command_srv(command, 0, input, 32));
        CHECK(vkdu_command_root(command, root));
        CHECK(vkdu_command_pipeline(command, pipeline));
        CHECK(vkdu_command_cbv(command, 2, constants, 0));
        CHECK(vkdu_command_uav(command, 1, buffer, 0));
        if (raw) {
            CHECK(vkdu_command_srv(command, 3, input, 256));
            CHECK(vkdu_command_uav(command, 4, buffer, 512));
        }
        REJECT(vkdu_command_srv(command, 1, input, 32));
        if (root_binding) {
            REJECT(vkdu_command_srv(command, 0, foreign, 0));
            REJECT(vkdu_command_srv(command, 0, denied, 0));
            REJECT(vkdu_command_srv(command, 0, input, 1));
            REJECT(vkdu_command_srv(command, 0, input, 262144));
            CHECK(vkdu_command_srv(command, 0, input, 32));
            REJECT(vkdu_command_srv(command, 0, NULL, 0));
        } else {
            CHECK(vkdu_buffer_srv(cpu, 1, null_binding ? NULL : input, format, 8, count,
                    typed || raw ? 0 : 4, raw ? 1 : 0, mapping));
            if (round == 2)
                REJECT(vkdu_buffer_srv(cpu, 1, input, format, 8, count, 0, 0,
                        (mapping & ~7u) | D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1));
            CHECK(vkdu_descriptor_copy(gpu, 6, cpu, 1, 1));
            CHECK(vkdu_command_heaps(command, 1, &gpu));
            CHECK(vkdu_command_table(command, 0, gpu, 5));
        }
        CHECK(vkdu_command_dispatch(command, typed || raw ? 1 : 4, 1, 1));
        CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 0, buffer, 0, 4096));
        CHECK(vkdu_command_close(command));
        CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, round + 1));
        CHECK(vkdu_fence_wait(fence, round + 1, 30000));
        for (i = 0; i < 1024; ++i) expected[i] = 0xdeadbeef;
        if (typed) {
            expected[6] = null_binding ? 0 : count;
            expected[7] = null_binding ? 0 : 0x13570000u ^ (31u * 37u);
        } else if (raw) {
            for (i = 0; i < 64; ++i) expected[i] = 0x13570000u ^ ((8u + i) * 37u);
            for (i = 0; i < 32; ++i) expected[128 + i] = 0x13570000u ^ ((64u + i) * 37u);
        } else {
            for (i = 0; i < 4; ++i) expected[i] = null_binding ? 0 : 0x13570000u ^ ((8u + i * 16384u) * 37u);
        }
        CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 1024; ++i) if (mapped[i] != expected[i]) {
            fprintf(stderr, "SRV round %u word %u: %08x expected %08x\n", round, i, mapped[i], expected[i]); exit(1);
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
    }
    CHECK(vkdu_device_status(device));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(pipeline); vkdu_object_destroy(root); vkdu_object_destroy(cpu); vkdu_object_destroy(gpu);
    vkdu_object_destroy(input); vkdu_object_destroy(foreign); vkdu_object_destroy(denied); vkdu_object_destroy(constants);
    vkdu_object_destroy(upload); vkdu_object_destroy(buffer); vkdu_object_destroy(readback);
    puts("PASS typed/raw/structured SRVs, root/table offsets, copied/null descriptors, unsupported swizzle and ownership/range rejection: 8x1024 readbacks");
}

int main(int argc, char **argv)
{
    PFN_vkGetInstanceProcAddr loader;
    vkdu_device *device = NULL, *wrong = NULL;
    vkdu_object *upload = NULL, *buffer = NULL, *readback = NULL;
    vkdu_object *queue = NULL, *allocator = NULL, *command = NULL, *root = NULL, *pipeline = NULL, *fence = NULL;
    vkdu_object *cpu_heap = NULL, *gpu_heap = NULL, *table_root = NULL, *table_pipeline = NULL, *foreign_heap = NULL;
    struct vkdu_adapter absent = {{0}, 0xffffffff, 0xffffffff};
    struct vkdu_root_parameter parameter = {4, 0, 0, 0, 0};
    uint32_t *mapped, i;
    uint64_t completed;
#ifdef _WIN32
    HMODULE module;
#else
    void *module;
#endif
#ifdef VKDU_GPU_PROBE
    uint32_t luid[2];
    /* The caller supplies the OS adapter LUID and the matching Vulkan IDs.
     * Production selection independently verifies all fields and Turnip's
     * driver ID. Never pick the first device or fall back to CPU Vulkan. */
    if (argc != 6 || strcmp(argv[1], "--adapter") ||
            !parse_hex32(argv[2], &luid[0]) || !parse_hex32(argv[3], &luid[1]) ||
            !parse_hex32(argv[4], &requested_adapter.vendor_id) ||
            !parse_hex32(argv[5], &requested_adapter.device_id)) {
        fprintf(stderr, "Usage: %s --adapter LUID_LOW_HEX LUID_HIGH_HEX VENDOR_HEX DEVICE_HEX\n", argv[0]);
        return 2;
    }
    memcpy(requested_adapter.luid, luid, sizeof(luid));
    printf("Production Turnip backend probe: LUID=%08x:%08x vendor=%08x device=%08x\n",
            luid[1], luid[0], requested_adapter.vendor_id, requested_adapter.device_id);
#else
    (void)argc;
    (void)argv;
#endif
#ifdef _WIN32
    module = LoadLibraryW(L"vulkan-1.dll");
    loader = module ? (PFN_vkGetInstanceProcAddr)GetProcAddress(module, "vkGetInstanceProcAddr") : NULL;
#else
    module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    loader = module ? (PFN_vkGetInstanceProcAddr)dlsym(module, "vkGetInstanceProcAddr") : NULL;
#endif
    if (!loader) { fprintf(stderr, "Vulkan loader is required\n"); return 1; }
    REJECT(vkdu_device_create(loader, &absent, &wrong));
    if (wrong) return 1;
    CHECK(validation_device_create(loader, &device));
#ifdef VKDU_GPU_PROBE
    puts("Embedded backend GPU validation; Windows runtime DDI/Present acceptance remains separate");
#else
    puts("TEST ONLY: CPU Vulkan backend semantics, not VIOGPU or native Windows runtime proof");
#endif
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
    /* Exercise an actual descriptor table with nonzero table and range offsets,
     * and a staging-to-shader-visible copy. Reinitialize data so the earlier
     * root-UAV dispatch cannot make this independent second readback pass. */
    CHECK(vkdu_heap_create(device, 0, 8, 0, &cpu_heap));
    CHECK(vkdu_heap_create(device, 0, 8, 1, &gpu_heap));
    CHECK(validation_device_create(loader, &wrong));
    CHECK(vkdu_heap_create(wrong, 0, 8, 0, &foreign_heap));
    {
        struct vkdu_descriptor_range range = {1, 1, 0, 0, 1};
        struct vkdu_root_parameter table = {0, 0, 0, 0, 0, &range, 1};
        uint32_t resolved = UINT32_MAX, stride = vkdu_descriptor_size(device, 0);
        if (!stride || !vkdu_heap_resolve(gpu_heap, vkdu_heap_start(gpu_heap, 1) + 4 * stride, 1, &resolved) || resolved != 4) return 1;
        if (vkdu_heap_resolve(gpu_heap, vkdu_heap_start(gpu_heap, 1) + 8 * stride, 1, &resolved) ||
            vkdu_heap_resolve(gpu_heap, vkdu_heap_start(gpu_heap, 1) + 1, 1, &resolved) || vkdu_heap_start(cpu_heap, 1)) return 1;
        REJECT(vkdu_descriptor_copy(gpu_heap, 4, foreign_heap, 2, 1));
        REJECT(vkdu_descriptor_copy(cpu_heap, 4, gpu_heap, 2, 1));
        REJECT(vkdu_descriptor_copy(gpu_heap, 8, cpu_heap, 2, 1));
        REJECT(vkdu_buffer_uav(cpu_heap, 2, buffer, DXGI_FORMAT_R32_TYPELESS, UINT64_MAX, 1, 0, 1, NULL, 0));
        REJECT(vkdu_buffer_uav(cpu_heap, 2, buffer, DXGI_FORMAT_R32_TYPELESS, 0, 1025, 0, 1, NULL, 0));
        CHECK(vkdu_buffer_uav(cpu_heap, 2, buffer, DXGI_FORMAT_R32_TYPELESS, 0, 1024, 0, 1, NULL, 0));
        CHECK(vkdu_descriptor_copy(gpu_heap, 4, cpu_heap, 2, 1));
        CHECK(vkdu_root_create(device, &table, 1, 0, &table_root));
        CHECK(vkdu_pipeline_create_tokens(device, table_root, execute_indirect_cs_code_dxbc + 21,
                execute_indirect_cs_code_dxbc[22], &table_pipeline));
    }
    CHECK(vkdu_allocator_reset(allocator));
    CHECK(vkdu_command_reset(command, allocator));
    REJECT(vkdu_command_table(command, 0, gpu_heap, 3)); /* Reset erased binding state. */
    CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
    CHECK(vkdu_command_copy(command, buffer, 0, upload, 0, 4096));
    CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    CHECK(vkdu_command_root(command, table_root));
    CHECK(vkdu_command_pipeline(command, table_pipeline));
    REJECT(vkdu_command_table(command, 0, gpu_heap, 3)); /* Heap must actually be bound. */
    REJECT(vkdu_command_heaps(command, 1, &cpu_heap));
    CHECK(vkdu_command_heaps(command, 1, &gpu_heap));
    REJECT(vkdu_command_table(command, 1, gpu_heap, 3));
    REJECT(vkdu_command_table(command, 0, gpu_heap, 7)); /* Range extends outside heap. */
    CHECK(vkdu_command_table(command, 0, gpu_heap, 3));
    CHECK(vkdu_command_dispatch(command, 1024, 1, 1));
    CHECK(vkdu_command_transition(command, buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
    CHECK(vkdu_command_copy(command, readback, 0, buffer, 0, 4096));
    CHECK(vkdu_command_close(command));
    CHECK(vkdu_queue_execute(queue, 1, &command));
    CHECK(vkdu_queue_signal(queue, fence, 3));
    CHECK(vkdu_fence_wait(fence, 3, 30000));
    CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
    for (i = 0; i < 1024; ++i) if (mapped[i] != i) { fprintf(stderr, "descriptor readback[%u]=%u\n", i, mapped[i]); return 1; }
    CHECK(vkdu_buffer_unmap(readback, 0, 0));
    CHECK(vkdu_device_status(device));
    check_constant_buffers(device, wrong);
    check_shader_resources(device, wrong);
    /* Caller follows D3D12 lifetime rules: reset/destroy only after completion. */
    vkdu_object_destroy(command); vkdu_object_destroy(allocator);
    vkdu_object_destroy(table_pipeline); vkdu_object_destroy(table_root);
    vkdu_object_destroy(gpu_heap); vkdu_object_destroy(cpu_heap);
    vkdu_object_destroy(foreign_heap); vkdu_device_destroy(wrong);
    vkdu_object_destroy(pipeline); vkdu_object_destroy(root);
    vkdu_object_destroy(readback); vkdu_object_destroy(buffer); vkdu_object_destroy(upload);
    vkdu_object_destroy(fence); vkdu_object_destroy(queue); vkdu_device_destroy(device);
#ifdef _WIN32
    FreeLibrary(module);
#else
    dlclose(module);
#endif
    puts("PASS root-UAV and descriptor-table compute, 2x1024 readbacks, nonzero offsets, descriptor copies, bounds/device/visibility rejection, reset, queue/fence and errors");
    return 0;
}
