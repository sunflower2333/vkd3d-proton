/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_ENABLE_TEST_DEVICE
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "../vkd3d/vkd3d_private.h"
static void check_cleared_bindings(const struct vkd3d_pipeline_bindings *bindings)
{
#ifdef VKDU_TEST_ROOT_CLEAR_DROP
    /* Negative control must reach the real stale-value GPU readback. */
    return;
#endif
    unsigned int i;
    const struct d3d12_root_signature *root = bindings->root_signature;
    for (i = 0; i < D3D12_MAX_ROOT_COST; ++i) {
        const struct vkd3d_root_descriptor_info *view = &bindings->root_descriptors[i];
        if (bindings->root_constants[i] || bindings->descriptor_tables[i] || view->info.va) {
            fprintf(stderr, "FAIL cleared root storage index%u constants%x table%x address%"PRIx64"\n",
                i, bindings->root_constants[i], bindings->descriptor_tables[i], view->info.va); exit(1);
        }
        if (root && (root->root_descriptor_push_mask & (1ull << i))) {
            const unsigned type = root->parameters[i].parameter_type;
            const bool buffer = d3d12_device_use_ssbo_root_descriptors(root->device) || type == D3D12_ROOT_PARAMETER_TYPE_CBV;
            const VkDescriptorType expected = type == D3D12_ROOT_PARAMETER_TYPE_CBV ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
                buffer ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : type == D3D12_ROOT_PARAMETER_TYPE_SRV ?
                    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            if (view->vk_descriptor_type != expected || !(bindings->root_descriptor_active_mask & (1ull << i)) ||
                    (buffer && (view->info.buffer.offset || view->info.buffer.range != VK_WHOLE_SIZE))) {
                fputs("FAIL cleared push descriptor type/range/activation\n", stderr); exit(1);
            }
        }
    }
    if (bindings->wddm_root_descriptor_zero_mask != UINT64_MAX || (root &&
            ((bindings->root_constant_dirty_mask & root->root_constant_mask) != root->root_constant_mask ||
             (bindings->root_descriptor_dirty_mask & (root->root_descriptor_raw_va_mask | root->root_descriptor_push_mask)) !=
                (root->root_descriptor_raw_va_mask | root->root_descriptor_push_mask)))) {
        fputs("FAIL cleared root invalidation\n", stderr); exit(1);
    }
}
static void check_root_argument_clear(vkdu_device *device)
{
    vkdu_object *root, *large, *empty, *compute, *graphics, *target, *rtv, *heap;
    vkdu_object *upload, *output, *readback, *allocator, *command, *queue, *fence;
    const struct vkdu_descriptor_range range = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 2, 0, 0};
    const struct vkdu_root_parameter params[] = {{1, 0, 0, 0, 4}, {4, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, &range, 1}, {2, 0, 3, 0, 0}, {3, 0, 4, 0, 0}};
    const struct vkdu_root_parameter many = {1, 0, 0, 0, 64};
    const struct vkdu_graphics_desc desc = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT32_MAX, 3, 1, 0, 1, 15};
    const struct vkdu_viewport viewport = {0, 0, 4, 4, 0, 1};
    const struct vkdu_rect scissor = {1, 2, 2, 3};
    const struct vkdu_signature vin = {6, 0, 1, 1}, vout = {1, 0, 15, 3};
    const uint32_t *v = graphics_tokens(vs_flat_color_code_dxbc), *p = graphics_tokens(ps_store_buffer_code_dxbc);
    uint32_t round, i, values[64], *mapped;
    void *vs, *ps; size_t vs_bytes, ps_bytes;
    CHECK(vkdu_root_create(device, params, 5, 0, &root));
    CHECK(vkdu_root_create(device, &many, 1, 0, &large));
    CHECK(vkdu_root_create(device, NULL, 0, 0, &empty));
    CHECK(vkdu_pipeline_create(device, root, execute_indirect_multi_dispatch_root_constants_code_dxbc,
        sizeof(execute_indirect_multi_dispatch_root_constants_code_dxbc), &compute));
    CHECK(vkdu_shader_dxbc(1, v, v[1], &vin, 1, &vout, 1, &vs, &vs_bytes));
    CHECK(vkdu_shader_dxbc(0, p, p[1], NULL, 0, NULL, 0, &ps, &ps_bytes));
    CHECK(vkdu_graphics_pipeline_create(device, root, vs, vs_bytes, ps, ps_bytes, &desc, &graphics));
    vkdu_shader_dxbc_free(vs); vkdu_shader_dxbc_free(ps);
    CHECK(vkdu_render_target_create(device, 4, 4, &target));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, 0, &rtv));
    CHECK(vkdu_texture2d_rtv(rtv, 0, target));
    CHECK(vkdu_heap_create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, 1, &heap));
    CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
    CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, &output));
    CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    CHECK(vkdu_buffer_cbv(heap, 3, upload, 0, 256));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
    CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
    for (i = 0; i < 1024; ++i) mapped[i] = 0xdeadbeef;
    CHECK(vkdu_buffer_unmap(upload, 0, 4096));
    REJECT(vkdu_command_clear_root_arguments(NULL)); REJECT(vkdu_command_clear_root_arguments(heap));
    for (round = 0; round < 3; ++round) {
        struct d3d12_command_list *list = vkdu_test_command_iface(command);
        struct vkd3d_dynamic_state dynamic;
        struct d3d12_pipeline_state *pipeline;
        if (round) {
            CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
            CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
        }
        CHECK(vkdu_command_clear_root_arguments(command)); /* Create/Reset before a signature. */
        check_cleared_bindings(&list->graphics_bindings); check_cleared_bindings(&list->compute_bindings);
        CHECK(vkdu_command_copy(command, output, 0, upload, 0, 4096));
        CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        CHECK(vkdu_command_root(command, large)); CHECK(vkdu_command_graphics_root(command, large));
        for (i = 0; i < 64; ++i) values[i] = 0x01010101u * (i + 1);
        CHECK(vkdu_command_constants(command, 0, 0, 64, values)); CHECK(vkdu_command_graphics_constants(command, 0, 0, 64, values));
        CHECK(vkdu_command_root(command, NULL)); CHECK(vkdu_command_graphics_root(command, NULL));
        CHECK(vkdu_command_root(command, root)); CHECK(vkdu_command_graphics_root(command, root));
        values[0] = 20; values[1] = 0x12345678; values[2] = 0x20000000; values[3] = 0x40000000;
        CHECK(vkdu_command_constants(command, 0, 0, 4, values)); CHECK(vkdu_command_graphics_constants(command, 0, 0, 4, values));
        CHECK(vkdu_command_heaps(command, 1, &heap));
        CHECK(vkdu_command_table(command, 2, heap, 3)); CHECK(vkdu_command_graphics_table(command, 2, heap, 3));
        CHECK(vkdu_command_cbv(command, 3, upload, 256)); CHECK(vkdu_command_graphics_cbv(command, 3, upload, 256));
        CHECK(vkdu_command_srv(command, 4, upload, 16)); CHECK(vkdu_command_graphics_srv(command, 4, upload, 16));
        CHECK(vkdu_command_uav(command, 1, output, 128)); CHECK(vkdu_command_graphics_uav(command, 1, output, 0));
        /* Same-signature calls preserve every newly populated argument. */
        CHECK(vkdu_command_root(command, root)); CHECK(vkdu_command_graphics_root(command, root));
        CHECK(vkdu_command_pipeline(command, compute)); CHECK(vkdu_command_dispatch(command, 1, 1, 1));
        CHECK(vkdu_command_pipeline(command, graphics)); CHECK(vkdu_command_render_target(command, rtv, 0));
        CHECK(vkdu_command_topology(command, 4)); CHECK(vkdu_command_viewports(command, 1, &viewport));
        CHECK(vkdu_command_scissors(command, 1, &scissor)); CHECK(vkdu_command_draw(command, 3, 1, 0, 0));
        {
            const struct vkdu_resource_barrier barrier = {VKDU_BARRIER_UAV, output, 0, 0};
            CHECK(vkdu_command_barriers(command, 1, &barrier));
        }
        if (round) {
            if (round == 1) { CHECK(vkdu_command_root(command, empty)); CHECK(vkdu_command_graphics_root(command, empty)); }
            else {
                CHECK(vkdu_command_root(command, NULL));
                CHECK(vkdu_command_graphics_root(command, NULL));
            }
        }
        pipeline = list->state; dynamic = list->dynamic_state;
        CHECK(vkdu_command_clear_root_arguments(command));
        if (pipeline != list->state || memcmp(&dynamic, &list->dynamic_state, sizeof(dynamic))) {
            fputs("FAIL root clear changed pipeline/dynamic state\n", stderr); exit(1);
        }
        check_cleared_bindings(&list->graphics_bindings); check_cleared_bindings(&list->compute_bindings);
        CHECK(vkdu_command_root(command, root)); CHECK(vkdu_command_graphics_root(command, root));
        check_cleared_bindings(&list->graphics_bindings); check_cleared_bindings(&list->compute_bindings);
        /* Descriptor heaps and native wrapper root layouts must still be live.
         * Null raw addresses are inspected, never dereferenced by a shader. */
        CHECK(vkdu_command_table(command, 2, heap, 3)); CHECK(vkdu_command_graphics_table(command, 2, heap, 3));
        CHECK(vkdu_command_graphics_uav(command, 1, output, 64)); CHECK(vkdu_command_draw(command, 3, 1, 0, 0));
        values[0] = 0xaabbccdd;
        CHECK(vkdu_command_graphics_constants(command, 0, 1, 1, values));
        CHECK(vkdu_command_graphics_uav(command, 1, output, 80)); CHECK(vkdu_command_draw(command, 3, 1, 0, 0));
        CHECK(vkdu_command_pipeline(command, compute)); CHECK(vkdu_command_uav(command, 1, output, 256));
        CHECK(vkdu_command_dispatch(command, 1, 1, 1));
        values[0] = 0x00040000;
        CHECK(vkdu_command_constants(command, 0, 2, 1, values)); CHECK(vkdu_command_uav(command, 1, output, 320));
        CHECK(vkdu_command_dispatch(command, 1, 1, 1));
        CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 0, output, 0, 4096)); CHECK(vkdu_command_close(command));
        REJECT(vkdu_command_clear_root_arguments(command));
        CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, fence, round + 1));
        CHECK(vkdu_fence_wait(fence, round + 1, 10000)); CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 1024; ++i) {
            uint32_t expected = i == 5 ? 0x12345678 : i == 16 ? 0 : i == 20 ? 0xaabbccdd : 0xdeadbeef;
            if (i == 32) expected += 20 | 0x12345678 | 0x20000000 | 0x40000000;
            if (i == 80) expected += 0x00040000;
            if (mapped[i] != expected) {
                fprintf(stderr, "FAIL root clear round%u word%u=%08x expected%08x\n", round, i, mapped[i], expected); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        printf("PASS root clear round%u graphics/compute,64 constants/views/tables,empty/null/same signature,partial rebind,other state retained:1024 words\n", round);
    }
    CHECK(vkdu_device_status(device));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    vkdu_object_destroy(graphics); vkdu_object_destroy(compute); vkdu_object_destroy(root); vkdu_object_destroy(large);
    vkdu_object_destroy(empty); vkdu_object_destroy(heap); vkdu_object_destroy(rtv); vkdu_object_destroy(target);
    vkdu_object_destroy(output); vkdu_object_destroy(upload); vkdu_object_destroy(readback);
}
#endif
