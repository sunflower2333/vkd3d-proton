/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_ENABLE_TEST_DEVICE
/* Inspect actual allocations only to ensure a fix does not disable pooling.
 * The independent Vulkan copy/readback and VVL checks enforce buffer usages. */
static void check_heap_usage_pair(vkdu_device *device, unsigned test_case)
{
    const unsigned write_combine = test_case / 2, reverse = test_case % 2;
    ID3D12Heap *heaps[2][2] = {{0}};
    ID3D12Resource *resources[2][2] = {{0}}, *upload = NULL, *readbacks[2] = {0};
    ID3D12GraphicsCommandList *list;
    ID3D12Device *raw_device;
    vkdu_object *allocator, *command, *queue, *fence;
    D3D12_HEAP_DESC heap_desc = {0};
    D3D12_RESOURCE_DESC buffer_desc = {0};
    D3D12_HEAP_PROPERTIES props = {0};
    D3D12_RESOURCE_BARRIER barrier = {0};
    D3D12_RANGE all = {0, 4096}, none = {0, 0};
    unsigned first, kind, j, i;
    uint32_t *mapped;

    CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command));
    CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_fence_create(device, 0, &fence));
    list = vkdu_test_command_iface(command);
    CHECK(ID3D12GraphicsCommandList_GetDevice(list, &IID_ID3D12Device, (void **)&raw_device));
    heap_desc.SizeInBytes = 65536; heap_desc.Alignment = 65536;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    heap_desc.Properties.CreationNodeMask = heap_desc.Properties.VisibleNodeMask = 1;
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = 4096; buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (first = 0; first < 2; ++first) {
        kind = first ^ reverse;
        heap_desc.Properties.Type = kind ? D3D12_HEAP_TYPE_CUSTOM :
                write_combine ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
        heap_desc.Properties.CPUPageProperty = kind ?
                write_combine ? D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE : D3D12_CPU_PAGE_PROPERTY_WRITE_BACK :
                D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_desc.Properties.MemoryPoolPreference = kind ? D3D12_MEMORY_POOL_L0 : D3D12_MEMORY_POOL_UNKNOWN;
        for (j = 0; j < 2; ++j) {
            CHECK(ID3D12Device_CreateHeap(raw_device, &heap_desc, &IID_ID3D12Heap, (void **)&heaps[kind][j]));
            CHECK(ID3D12Device_CreatePlacedResource(raw_device, heaps[kind][j], 0, &buffer_desc,
                    !kind && write_combine ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                    NULL, &IID_ID3D12Resource, (void **)&resources[kind][j]));
        }
        {
            /* Both are actual engine ID3D12Resource interfaces, its first member.
             * Do not inspect the new chunk-key field: this oracle checks reuse. */
            const struct d3d12_resource *a = (const struct d3d12_resource *)resources[kind][0];
            const struct d3d12_resource *b = (const struct d3d12_resource *)resources[kind][1];
            if (!a->mem.chunk || a->mem.chunk != b->mem.chunk || a->res.vk_buffer != b->res.vk_buffer ||
                    a->mem.device_allocation.vk_memory != b->mem.device_allocation.vk_memory ||
                    a->mem.offset == b->mem.offset) {
                fprintf(stderr, "FAIL heap usage case%u kind%u: same-type allocations stopped sharing an actual chunk\n", test_case, kind);
                exit(1);
            }
            printf("OBSERVE heap usage case%u kind%u: chunk%p buffer%"PRIx64" memory%"PRIx64" offsets%"PRIu64",%"PRIu64"\n",
                    test_case, kind, (void *)a->mem.chunk, (uint64_t)(uintptr_t)a->res.vk_buffer,
                    (uint64_t)(uintptr_t)a->mem.device_allocation.vk_memory, a->mem.offset, b->mem.offset);
        }
    }
    props.CreationNodeMask = props.VisibleNodeMask = 1;
    if (!write_combine) {
        props.Type = D3D12_HEAP_TYPE_UPLOAD;
        CHECK(ID3D12Device_CreateCommittedResource(raw_device, &props, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&upload));
    } else {
        props.Type = D3D12_HEAP_TYPE_READBACK;
        for (j = 0; j < 2; ++j)
            CHECK(ID3D12Device_CreateCommittedResource(raw_device, &props, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&readbacks[j]));
    }
    for (j = 0; j < 2; ++j) {
        ID3D12Resource *source = write_combine ? resources[0][j] : upload;
        ID3D12Resource *destination = write_combine ? readbacks[j] : resources[0][j];
        CHECK(ID3D12Resource_Map(source, 0, &none, (void **)&mapped));
        for (i = 0; i < 1024; ++i) mapped[i] = (0x93a70000 + test_case * 113) ^ (i * 37);
        ID3D12Resource_Unmap(source, 0, &all);
        CHECK(ID3D12Resource_Map(resources[1][j], 0, &none, (void **)&mapped));
        for (i = 0; i < 1024; ++i) mapped[i] = 0xa1bc0000 + j;
        ID3D12Resource_Unmap(resources[1][j], 0, &all);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, resources[1][j], 256, source, 128, 2048);
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resources[1][j];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, destination, 0, resources[1][j], 0, 4096);
    }
    CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
    CHECK(vkdu_queue_signal(queue, fence, 1)); CHECK(vkdu_fence_wait(fence, 1, 10000));
    for (j = 0; j < 2; ++j) {
        ID3D12Resource *destination = write_combine ? readbacks[j] : resources[0][j];
        CHECK(ID3D12Resource_Map(destination, 0, &all, (void **)&mapped));
        for (i = 0; i < 1024; ++i) {
            uint32_t expected = i >= 64 && i < 576 ?
                    (0x93a70000 + test_case * 113) ^ ((i - 32) * 37) : 0xa1bc0000 + j;
            if (mapped[i] != expected) {
                fprintf(stderr, "FAIL heap usage case%u resource%u word%u=%08x expected%08x\n",
                        test_case, j, i, mapped[i], expected); exit(1);
            }
        }
        ID3D12Resource_Unmap(destination, 0, &none);
    }
    vkdu_object_destroy(command); vkdu_object_destroy(allocator);
    vkdu_object_destroy(queue); vkdu_object_destroy(fence);
    for (kind = 0; kind < 2; ++kind) for (j = 0; j < 2; ++j) {
        ID3D12Resource_Release(resources[kind][j]); ID3D12Heap_Release(heaps[kind][j]);
    }
    if (upload) ID3D12Resource_Release(upload);
    for (j = 0; j < 2; ++j) if (readbacks[j]) ID3D12Resource_Release(readbacks[j]);
    ID3D12Device_Release(raw_device);
    printf("PASS heap usage case%u: %s order%s, same-type pool reuse, 2048 readback/sentinel words\n",
            test_case, write_combine ? "UPLOAD/CUSTOM_WRITE_COMBINE" : "READBACK/CUSTOM_WRITE_BACK",
            reverse ? "CUSTOM-first" : "standard-first");
}
#endif
