/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../tests/shaders/descriptors/headers/sampler_rounding.h"

static void check_samplers(vkdu_device *device, vkdu_device *other)
{
    vkdu_object *texture = NULL, *foreign = NULL, *staging = NULL, *init = NULL, *output = NULL, *readback = NULL;
    vkdu_object *cpu = NULL, *gpu = NULL, *srv = NULL, *root = NULL, *static_root = NULL, *pipeline = NULL;
    vkdu_object *static_pipeline = NULL, *queue = NULL, *allocator = NULL, *command = NULL, *fence = NULL, *bad = NULL;
    struct vkdu_descriptor_range ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 1},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 2}};
    struct vkdu_root_parameter params[4] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 0, 0, 0, 3},
        {D3D12_ROOT_PARAMETER_TYPE_UAV, 0, 0, 0, 0},
        {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, 0, 0, 0, 0, &ranges[0], 1},
        {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, 0, 0, 0, 0, &ranges[1], 1}};
    struct vkdu_sampler_desc sampler = {D3D12_FILTER_MIN_MAG_MIP_POINT, 3, 3, 3, 0, 1, 8, {0}, 0, 1000};
    struct vkdu_static_sampler static_desc = {0};
    static const struct { uint32_t filter, address, color; float uv, expected; int fixed; } cases[] = {
        {0x00, 3, 0, 1.25f, 1.0f, 0},  /* Point, clamp. */
        {0x15, 3, 0, 0.5f, 0.5f, 0},   /* Linear blends the two texels. */
        {0x00, 1, 0, 1.25f, 0.0f, 0},  /* Wrap. */
        {0x00, 2, 0, 1.25f, 1.0f, 0},  /* Mirror. */
        {0x00, 4, 0, 1.25f, 0.25f, 0}, /* Custom border. */
        {0x00, 5, 0, -1.25f, 1.0f, 0}, /* Mirror once. */
        {0x00, 4, 2, 1.25f, 1.0f, 1},  /* Static opaque white. */
        {0x00, 4, 0, 1.25f, 0.0f, 1},  /* Static transparent black. */
        {0x15, 3, 0, 0.5f, 0.5f, 1},   /* Static linear. */
    };
    uint32_t *mapped, round, i;
    float *floats;
    CHECK(vkdu_texture2d_create(device, 2, 1, DXGI_FORMAT_R32_FLOAT, &texture));
    CHECK(vkdu_texture2d_create(other, 2, 1, DXGI_FORMAT_R32_FLOAT, &foreign));
    CHECK(vkdu_buffer_create(device, 512, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &staging));
    CHECK(vkdu_buffer_map(staging, 0, 0, (void **)&floats));
    floats[0] = 0.0f; floats[1] = 1.0f;
    CHECK(vkdu_buffer_unmap(staging, 0, 8));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &init));
    CHECK(vkdu_buffer_map(init, 0, 0, (void **)&mapped));
    for (i = 0; i < 1024; ++i) mapped[i] = 0xdeadbeef;
    CHECK(vkdu_buffer_unmap(init, 0, 4096));
    CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST, &output));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_heap_create(device, 1, 8, 0, &cpu));
    CHECK(vkdu_heap_create(device, 1, 8, 1, &gpu));
    CHECK(vkdu_heap_create(device, 0, 8, 1, &srv));
    REJECT(vkdu_texture2d_srv(srv, 8, texture));
    REJECT(vkdu_texture2d_srv(srv, 1, foreign));
    REJECT(vkdu_texture2d_srv(cpu, 1, texture));
    CHECK(vkdu_texture2d_srv(srv, 4, texture));
    REJECT(vkdu_sampler_create(srv, 0, &sampler));
    REJECT(vkdu_sampler_create(cpu, 8, &sampler));
    REJECT(vkdu_sampler_create(cpu, 0, NULL));
    CHECK(vkdu_root_create(device, params, 4, 0, &root));
    CHECK(vkdu_pipeline_create(device, root, sampler_rounding_code_dxbc, sizeof(sampler_rounding_code_dxbc), &pipeline));
    static_desc.desc = sampler;
    REJECT(vkdu_root_create_samplers(device, params, 4, 0, &static_desc, 1, &bad)); /* Dynamic s0 overlap. */
    static_desc.visibility = 6;
    REJECT(vkdu_root_create_samplers(device, params, 3, 0, &static_desc, 1, &bad));
    static_desc.visibility = 0; static_desc.border_color = 3;
    REJECT(vkdu_root_create_samplers(device, params, 3, 0, &static_desc, 1, &bad));
    static_desc.border_color = 0;
    {
        struct vkdu_static_sampler duplicates[2] = {static_desc, static_desc};
        REJECT(vkdu_root_create_samplers(device, params, 3, 0, duplicates, 2, &bad));
    }
    CHECK(vkdu_queue_create(device, 0, &queue));
    CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command));
    CHECK(vkdu_fence_create(device, 0, &fence));
    REJECT(vkdu_command_texture_upload(command, texture, staging, 1, 256));
    REJECT(vkdu_command_texture_upload(command, texture, staging, 0, 4));
    REJECT(vkdu_command_texture_upload(command, texture, staging, 512, 256));
    REJECT(vkdu_command_texture_upload(command, foreign, staging, 0, 256));
    CHECK(vkdu_command_texture_upload(command, texture, staging, 0, 256));
    CHECK(vkdu_command_transition(command, texture, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    for (round = 0; round < sizeof(cases) / sizeof(cases[0]); ++round) {
        vkdu_object *heaps[2] = {srv, gpu};
        float uv[3] = {cases[round].uv, 0.5f, 0.0f};
        uint32_t constants[3], expected;
        memcpy(constants, uv, sizeof(constants)); memcpy(&expected, &cases[round].expected, 4);
        fprintf(stderr, "sampler round %u: %s filter=%x address=%u\n", round,
                cases[round].fixed ? "static" : "dynamic", cases[round].filter, cases[round].address);
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
        }
        sampler.filter = cases[round].filter; sampler.address_u = cases[round].address;
        sampler.border[0] = 0.25f;
        if (cases[round].fixed) {
            static_desc.desc = sampler; static_desc.border_color = cases[round].color;
            CHECK(vkdu_root_create_samplers(device, params, 3, 0, &static_desc, 1, &static_root));
            CHECK(vkdu_pipeline_create(device, static_root, sampler_rounding_code_dxbc,
                    sizeof(sampler_rounding_code_dxbc), &static_pipeline));
        } else {
            struct vkdu_sampler_desc invalid;
            CHECK(vkdu_sampler_create(cpu, 3, &sampler));
            /* Every rejected replacement targets the live descriptor used by
             * this dispatch. A partial update is observable in the readback. */
            invalid = sampler; invalid.address_u = 0; REJECT(vkdu_sampler_create(cpu, 3, &invalid));
            invalid = sampler; invalid.filter = 0x40; REJECT(vkdu_sampler_create(cpu, 3, &invalid));
            invalid = sampler; invalid.filter = 0x100; REJECT(vkdu_sampler_create(cpu, 3, &invalid));
            invalid = sampler; invalid.filter = 0x55; invalid.max_anisotropy = 17;
            REJECT(vkdu_sampler_create(cpu, 3, &invalid));
            invalid = sampler; invalid.min_lod = 3; invalid.max_lod = 2;
            REJECT(vkdu_sampler_create(cpu, 3, &invalid));
            invalid = sampler; invalid.comparison = 0; REJECT(vkdu_sampler_create(cpu, 3, &invalid));
            invalid = sampler; invalid.mip_bias = 16; REJECT(vkdu_sampler_create(cpu, 3, &invalid));
            CHECK(vkdu_descriptor_copy(gpu, 6, cpu, 3, 1));
        }
        CHECK(vkdu_command_copy(command, output, 0, init, 0, 4096));
        CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        CHECK(vkdu_command_root(command, cases[round].fixed ? static_root : root));
        CHECK(vkdu_command_pipeline(command, cases[round].fixed ? static_pipeline : pipeline));
        CHECK(vkdu_command_constants(command, 0, 0, 3, constants));
        CHECK(vkdu_command_uav(command, 1, output, 0));
        CHECK(vkdu_command_heaps(command, cases[round].fixed ? 1 : 2, heaps));
        CHECK(vkdu_command_table(command, 2, srv, 3));
        if (!cases[round].fixed) CHECK(vkdu_command_table(command, 3, gpu, 4));
        CHECK(vkdu_command_dispatch(command, 16, 1, 1));
        CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 0, output, 0, 4096));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, round + 1)); CHECK(vkdu_fence_wait(fence, round + 1, 30000));
        CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 1024; ++i) if (mapped[i] != (i < 16 ? expected : 0xdeadbeef)) {
            fprintf(stderr, "sampler round%u word%u=%08x expected%08x\n", round, i, mapped[i], i < 16 ? expected : 0xdeadbeef);
            exit(1);
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        vkdu_object_destroy(static_pipeline); vkdu_object_destroy(static_root); static_pipeline = static_root = NULL;
    }
    CHECK(vkdu_device_status(device));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(pipeline); vkdu_object_destroy(root); vkdu_object_destroy(cpu); vkdu_object_destroy(gpu);
    vkdu_object_destroy(srv); vkdu_object_destroy(texture); vkdu_object_destroy(foreign); vkdu_object_destroy(staging);
    vkdu_object_destroy(init); vkdu_object_destroy(output); vkdu_object_destroy(readback);
    puts("PASS dynamic/static samplers: 9 rounds x 1024 words, point/linear, wrap/mirror/clamp/border/mirror-once, copied descriptors, invalid replacement preservation");
}
