/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_ENABLE_TEST_DEVICE
static void require_unbound_root(vkdu_object *command, int graphics)
{
    struct d3d12_command_list *list = vkdu_test_command_iface(command);
    struct vkd3d_pipeline_bindings sibling = graphics ? list->compute_bindings : list->graphics_bindings;
    struct vkd3d_pipeline_bindings *binding = graphics ? &list->graphics_bindings : &list->compute_bindings;
    const uint32_t value = 0x12345678;
    if (graphics) CHECK(vkdu_command_graphics_root(command, NULL));
    else CHECK(vkdu_command_root(command, NULL));
    if (binding->root_signature || binding->static_sampler_set || memcmp(&sibling,
            graphics ? &list->compute_bindings : &list->graphics_bindings, sizeof(sibling))) {
        fprintf(stderr, "FAIL %s root unbind did not clear embedded root/static sampler or changed sibling\n",
                graphics ? "graphics" : "compute"); exit(1);
    }
    if (graphics) {
        REJECT(vkdu_command_graphics_constants(command, 0, 0, 1, &value));
        CHECK(vkdu_command_graphics_root(command, NULL));
    } else {
        REJECT(vkdu_command_constants(command, 0, 0, 1, &value));
        CHECK(vkdu_command_root(command, NULL));
    }
}
static void check_root_unbind(vkdu_device *device, vkdu_device *other)
{
    vkdu_object *root, *foreign, *allocator, *command, *copy_allocator, *copy, *compute_allocator, *compute;
    const struct vkdu_root_parameter parameter = {1, 0, 0, 0, 4};
    struct vkdu_static_sampler sampler = {0};
    const uint32_t values[] = {0x12345678, 0x87654321, 0xabcdef01, 0x01010101};
    struct d3d12_command_list *list;
    struct vkd3d_pipeline_bindings before;
    unsigned int i;
    sampler.desc.filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.desc.address_u = sampler.desc.address_v = sampler.desc.address_w = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.desc.max_anisotropy = 1; sampler.desc.comparison = D3D12_COMPARISON_FUNC_ALWAYS; sampler.desc.max_lod = 1000;
    CHECK(vkdu_root_create_samplers(device, &parameter, 1, 0, &sampler, 1, &root));
    CHECK(vkdu_root_create(other, &parameter, 1, 0, &foreign));
    CHECK(vkdu_allocator_create(device, 0, &allocator)); CHECK(vkdu_command_create(device, allocator, 0, &command));
    CHECK(vkdu_allocator_create(device, 3, &copy_allocator)); CHECK(vkdu_command_create(device, copy_allocator, 3, &copy));
    CHECK(vkdu_allocator_create(device, 2, &compute_allocator)); CHECK(vkdu_command_create(device, compute_allocator, 2, &compute));
    list = vkdu_test_command_iface(command);
    CHECK(vkdu_command_root(command, root)); CHECK(vkdu_command_graphics_root(command, root));
    CHECK(vkdu_command_constants(command, 0, 0, 4, values)); CHECK(vkdu_command_graphics_constants(command, 0, 0, 4, values));
    before = list->compute_bindings; CHECK(vkdu_command_root(command, root));
    if (memcmp(&before, &list->compute_bindings, sizeof(before))) { fputs("FAIL redundant compute root changed arguments\n", stderr); exit(1); }
    before = list->graphics_bindings; CHECK(vkdu_command_graphics_root(command, root));
    if (memcmp(&before, &list->graphics_bindings, sizeof(before))) { fputs("FAIL redundant graphics root changed arguments\n", stderr); exit(1); }
    for (i = 0; i < 2; ++i) {
        const int graphics = getenv("VKDU_TEST_UNBIND_GRAPHICS_FIRST") ? !i : !!i;
        require_unbound_root(command, graphics);
        if (graphics) CHECK(vkdu_command_graphics_root(command, root));
        else CHECK(vkdu_command_root(command, root));
        const struct vkd3d_pipeline_bindings *b = graphics ? &list->graphics_bindings : &list->compute_bindings;
        if (!b->root_signature || b->static_sampler_set != b->root_signature->vk_sampler_set ||
                (b->root_constant_dirty_mask & b->root_signature->root_constant_mask) != b->root_signature->root_constant_mask ||
                (b->root_signature->vk_sampler_descriptor_layout && !(b->dirty_flags & VKD3D_PIPELINE_DIRTY_STATIC_SAMPLER_SET))) {
            fputs("FAIL rebound root missing actual sampler/dirty state\n", stderr); exit(1);
        }
    }
    REJECT(vkdu_command_root(command, foreign)); REJECT(vkdu_command_graphics_root(command, foreign));
    REJECT(vkdu_command_root(command, allocator)); REJECT(vkdu_command_graphics_root(command, allocator));
    REJECT(vkdu_command_root(NULL, NULL)); REJECT(vkdu_command_graphics_root(NULL, NULL));
    REJECT(vkdu_command_root(copy, NULL)); REJECT(vkdu_command_graphics_root(copy, NULL));
    CHECK(vkdu_command_root(compute, root)); CHECK(vkdu_command_root(compute, NULL));
    REJECT(vkdu_command_graphics_root(compute, NULL));
    CHECK(vkdu_command_close(command));
    REJECT(vkdu_command_root(command, NULL)); REJECT(vkdu_command_graphics_root(command, NULL));
    CHECK(vkdu_allocator_reset(allocator)); CHECK(vkdu_command_reset(command, allocator));
    CHECK(vkdu_command_root(command, NULL)); CHECK(vkdu_command_graphics_root(command, NULL));
    CHECK(vkdu_device_status(device)); CHECK(vkdu_device_status(other));
    vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(copy); vkdu_object_destroy(copy_allocator);
    vkdu_object_destroy(compute); vkdu_object_destroy(compute_allocator); vkdu_object_destroy(root); vkdu_object_destroy(foreign);
    puts("PASS actual root unbind: NULL/repeated NULL, static sampler clear/rebind dirty, sibling independence, same-root arguments, owner/kind/type/closed validation");
}
#endif
