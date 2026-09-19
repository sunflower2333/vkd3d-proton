/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void check_texture_copies(vkdu_device *device, vkdu_device *other)
{
    uint32_t format_index;
    const uint32_t formats[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM};
    uint64_t bytes, alignment;
    CHECK(vkdu_buffer_allocation(device, 65537, &bytes, &alignment));
    if (bytes != 131072 || alignment != 65536) exit(1);
    REJECT(vkdu_buffer_allocation(device, UINT64_MAX, &bytes, &alignment));
    REJECT(vkdu_buffer_allocation(device, 0, &bytes, &alignment));
    for (format_index = 0; format_index < 2; ++format_index) {
        vkdu_object *texture = NULL, *second = NULL, *foreign = NULL, *upload = NULL, *readback = NULL;
        vkdu_object *queue = NULL, *allocator = NULL, *command = NULL, *fence = NULL;
        struct vkdu_texture_copy_location source = {0}, destination = {0}, result = {0}, image = {0};
        struct vkdu_box box = {1, 1, 0, 3, 3, 1};
        uint64_t bytes, alignment;
        uint32_t *mapped, x, y;
        CHECK(vkdu_texture2d_allocation(device, 4, 4, formats[format_index], &bytes, &alignment));
        if (!bytes || alignment != 65536) exit(1);
        CHECK(vkdu_texture2d_create(device, 4, 4, formats[format_index], &texture));
        CHECK(vkdu_texture2d_create(device, 4, 4, formats[format_index], &second));
        CHECK(vkdu_texture2d_create(other, 4, 4, formats[format_index], &foreign));
        CHECK(vkdu_buffer_create(device, 2048, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
        CHECK(vkdu_buffer_create(device, 2048, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
        CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
        for (x = 0; x < 512; ++x) mapped[x] = 0xdeadbeef;
        for (y = 0; y < 4; ++y) for (x = 0; x < 4; ++x) mapped[128 + y * 64 + x] = 0x3f000000 + y * 4 + x;
        CHECK(vkdu_buffer_unmap(upload, 0, 2048));
        CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_allocator_create(device, 0, &allocator));
        CHECK(vkdu_command_create(device, allocator, 0, &command)); CHECK(vkdu_fence_create(device, 0, &fence));
        source = (struct vkdu_texture_copy_location){upload, 512, 1, formats[format_index], 4, 4, 1, 256};
        destination.resource = texture;
        result = source; result.resource = readback;
        image.resource = second;
        CHECK(vkdu_command_copy(command, readback, 0, upload, 0, 2048));
        CHECK(vkdu_command_texture_copy(command, &destination, 0, 0, 0, &source, NULL));
        CHECK(vkdu_command_texture_copy(command, &image, 0, 0, 0, &source, NULL));
        CHECK(vkdu_command_transition(command, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command, &image, 2, 2, 0, &destination, &box));
        /* Invalid copies must not overwrite the valid destination. */
        REJECT(vkdu_command_texture_copy(command, &image, 3, 2, 0, &destination, &box));
        REJECT(vkdu_command_texture_copy(command, &image, 0, 0, 1, &destination, NULL));
        source.offset = 1; REJECT(vkdu_command_texture_copy(command, &image, 0, 0, 0, &source, NULL));
        source.offset = UINT64_MAX - 511; REJECT(vkdu_command_texture_copy(command, &image, 0, 0, 0, &source, NULL));
        source.offset = 512; source.row_pitch = 16; REJECT(vkdu_command_texture_copy(command, &image, 0, 0, 0, &source, NULL));
        source.row_pitch = 256; image.resource = foreign;
        REJECT(vkdu_command_texture_copy(command, &image, 0, 0, 0, &source, NULL));
        image.resource = second;
        CHECK(vkdu_command_transition(command, second, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_texture_copy(command, &result, 0, 0, 0, &image, NULL));
        CHECK(vkdu_command_close(command)); CHECK(vkdu_queue_execute(queue, 1, &command));
        CHECK(vkdu_queue_signal(queue, fence, 1)); CHECK(vkdu_fence_wait(fence, 1, 30000));
        CHECK(vkdu_buffer_map(readback, 0, 2048, (void **)&mapped));
        for (x = 0; x < 512; ++x) {
            uint32_t expected = 0xdeadbeef;
            if (x >= 128 && x < 384 && (x - 128) % 64 < 4) {
                uint32_t row = (x - 128) / 64, col = (x - 128) % 64;
                expected = 0x3f000000 + row * 4 + col;
                if (row >= 2 && col >= 2) expected = 0x3f000000 + (row - 1) * 4 + col - 1;
            }
            if (mapped[x] != expected) {
                fprintf(stderr, "texture copy format%u word%u=%08x expected%08x\n", formats[format_index], x, mapped[x], expected); exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(queue); vkdu_object_destroy(fence);
        vkdu_object_destroy(texture); vkdu_object_destroy(second); vkdu_object_destroy(foreign);
        vkdu_object_destroy(upload); vkdu_object_destroy(readback);
    }
    puts("PASS texture copies: R32_FLOAT/RGBA8, upload/image/image/readback, nonzero offsets/pitches/box and rejected updates");
}
