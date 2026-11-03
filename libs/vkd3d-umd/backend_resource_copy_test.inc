/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_resource_copies(vkdu_device *device, vkdu_device *other)
{
    const uint32_t types[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY};
    const uint32_t formats[] = {0, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM};
    uint32_t ti, fi;
    for (ti = 0; ti < 3; ++ti) for (fi = 0; fi < 5; ++fi) {
        vkdu_object *src = NULL, *dst = NULL, *mismatch = NULL, *foreign = NULL;
        vkdu_object *upload = NULL, *readback = NULL, *queue = NULL, *allocator = NULL, *command = NULL, *fence = NULL;
        const uint32_t format = formats[fi], type = types[ti], pixel_bytes = format == DXGI_FORMAT_D16_UNORM ? 2 : 4;
        const int depth = format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D16_UNORM;
        uint8_t expected[4096], *mapped;
        uint32_t x, y, i;
        CHECK(vkdu_buffer_create(device, 4096, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
        CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
        if (!format) {
            CHECK(vkdu_buffer_create(device, 1024, 1, 0, D3D12_RESOURCE_STATE_COPY_DEST, &src));
            CHECK(vkdu_buffer_create(device, 1024, 1, 0, D3D12_RESOURCE_STATE_COPY_DEST, &dst));
            CHECK(vkdu_buffer_create(device, 512, 1, 0, D3D12_RESOURCE_STATE_COPY_DEST, &mismatch));
            CHECK(vkdu_buffer_create(other, 1024, 1, 0, D3D12_RESOURCE_STATE_COPY_DEST, &foreign));
        } else if (depth) {
            CHECK(vkdu_depth_create(device, 5, 3, format, &src));
            CHECK(vkdu_depth_create(device, 5, 3, format, &dst));
            CHECK(vkdu_depth_create(device, 4, 3, format, &mismatch));
            CHECK(vkdu_depth_create(other, 5, 3, format, &foreign));
        } else {
            CHECK(vkdu_texture2d_create(device, 5, 3, format, &src));
            CHECK(vkdu_texture2d_create(device, 5, 3, format, &dst));
            CHECK(vkdu_texture2d_create(device, 4, 3, format, &mismatch));
            CHECK(vkdu_texture2d_create(other, 5, 3, format, &foreign));
        }
        CHECK(vkdu_queue_create(device, type, &queue)); CHECK(vkdu_allocator_create(device, type, &allocator));
        CHECK(vkdu_command_create(device, allocator, type, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
        memset(expected, 0x5a, sizeof(expected));
        CHECK(vkdu_buffer_map(readback, 0, 0, (void **)&mapped)); memcpy(mapped, expected, sizeof(expected));
        CHECK(vkdu_buffer_unmap(readback, 0, 4096));
        CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped)); memset(mapped, 0, 4096);
        if (!format) {
            for (i = 0; i < 256; ++i) {
                uint32_t value = 0x13570000 + i * 257 + type;
                memcpy(mapped + 512 + i * 4, &value, 4); memcpy(expected + 512 + i * 4, &value, 4);
            }
        } else for (y = 0; y < 3; ++y) for (x = 0; x < 5; ++x) {
            uint32_t value;
            if (format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_D32_FLOAT) {
                float z = (float)(1 + x + y * 5 + type) / 32; memcpy(&value, &z, 4);
            } else value = 0xa1700100 + x * 53 + y * 107 + type;
            memcpy(mapped + 512 + y * 256 + x * pixel_bytes, &value, pixel_bytes);
            memcpy(expected + 512 + y * 256 + x * pixel_bytes, &value, pixel_bytes);
        }
        CHECK(vkdu_buffer_unmap(upload, 0, 4096));
        if (!format) {
            CHECK(vkdu_command_copy(command, src, 0, upload, 512, 1024));
            CHECK(vkdu_command_copy(command, dst, 0, upload, 2048, 1024));
        } else {
            struct vkdu_texture_copy_location input = {upload, 512, 1, format, 5, 3, 1, 256};
            struct vkdu_texture_copy_location image = {src, 0, 0, 0, 0, 0, 0, 0};
            if (depth) {
                CHECK(vkdu_command_transition(command, src, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_DEST));
                CHECK(vkdu_command_transition(command, dst, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_DEST));
            }
            CHECK(vkdu_command_texture_copy(command, &image, 0, 0, 0, &input, NULL));
            image.resource = dst; input.offset = 2048;
            CHECK(vkdu_command_texture_copy(command, &image, 0, 0, 0, &input, NULL));
        }
        CHECK(vkdu_command_transition(command, src, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_resource_copy(command, dst, src));
        /* Invalid calls after the copy must preserve its result. */
        REJECT(vkdu_command_resource_copy(command, dst, dst));
        REJECT(vkdu_command_resource_copy(command, dst, mismatch));
        REJECT(vkdu_command_resource_copy(command, dst, foreign));
        REJECT(vkdu_command_resource_copy(command, dst, NULL));
        REJECT(vkdu_command_resource_copy(command, upload, src));
        REJECT(vkdu_command_resource_copy(command, dst, readback));
        CHECK(vkdu_command_transition(command, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
        if (!format) CHECK(vkdu_command_copy(command, readback, 512, dst, 0, 1024));
        else {
            struct vkdu_texture_copy_location result = {readback, 512, 1, format, 5, 3, 1, 256};
            struct vkdu_texture_copy_location image = {dst, 0, 0, 0, 0, 0, 0, 0};
            CHECK(vkdu_command_texture_copy(command, &result, 0, 0, 0, &image, NULL));
        }
        CHECK(vkdu_command_close(command)); REJECT(vkdu_command_resource_copy(command, dst, src));
        CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, fence, 1));
        CHECK(vkdu_fence_wait(fence, 1, 10000)); CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 4096; ++i) if (mapped[i] != expected[i]) {
            fprintf(stderr, "ResourceCopy queue%u format%u byte%u=%02x expected%02x\n", type, format, i, mapped[i], expected[i]); exit(1);
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0)); CHECK(vkdu_device_status(device));
        printf("PASS actual ResourceCopy queue%u format%u complete copy, rejected updates and4096 byte readback/padding\n", type, format);
        vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
        vkdu_object_destroy(src); vkdu_object_destroy(dst); vkdu_object_destroy(mismatch); vkdu_object_destroy(foreign);
        vkdu_object_destroy(upload); vkdu_object_destroy(readback);
    }
}
