/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real shader/readback controls for per-invocation GetDimensions. These catch
 * broadcasting LOD, shifting array layers, losing view base mip, rounding NPOT
 * sizes up, and replacing a null view's zero dimensions with one. */
static void resinfo_test_shader(uint32_t path, uint32_t *code, size_t *size)
{
    const DWORD *src = tex2d_array_reinterpretation_code_dxbc;
    uint32_t at = src[10] / 4 + 4, end = sizeof(tex2d_array_reinterpretation_code_dxbc) / 4;
    uint32_t out = at, found = 0;
    memcpy(code, src, at * 4);
    while (at < end) {
        uint32_t words = (src[at] >> 24) & 127, op = src[at] & 0x7ff;
        if (!words || words > end - at) exit(1);
        if (op == 61) {
            if (words != 8 || src[at + 3] != 0x0002400a) exit(1);
            if (!found && path != 4) {
                const uint32_t add[] = {0x0600001e, 0x00100012, 4, 0x0002400a, 0x00004001,
                    path == 2 ? 32u : 0xfffffff0u};
                memcpy(code + out, add, sizeof(add)); out += 6;
            }
            memcpy(code + out, src + at, 3 * 4);
            code[out] = (src[at] & 0x80ffffff) | (9u << 24);
            code[out + 3] = path == 4 ? 0x00004001 : 0x0010000a;
            code[out + 4] = path == 4 ? 1 : 4;
            memcpy(code + out + 5, src + at + 4, 4 * 4);
            out += 9; ++found;
        } else {
            memcpy(code + out, src + at, words * 4);
            if (op == 104 && path != 4) {
                if (words != 2 || src[at + 1] != 4) exit(1);
                code[out + 1] = 5;
            }
            out += words;
        }
        at += words;
    }
    if (found != 2) exit(1);
    code[6] = out * 4;
    code[code[10] / 4 + 1] += (out - end) * 4;
    code[code[10] / 4 + 3] += out - end;
    *size = out * 4;
    vkd3d_compute_dxbc_checksum(code, *size, code + 1);
}

static void check_resinfo_dimensions(vkdu_device *device, unsigned scope)
{
    static const struct {
        uint32_t width, height, first_mip, mips, first_layer, layers;
        uint32_t widths[4], heights[4];
        int null_view;
    } cases[] = {
        {9, 7, 0, 4, 0, 3, {9, 4, 2, 1}, {7, 3, 1, 1}, 0},
        {9, 7, 1, 3, 1, 2, {4, 2, 1, 0}, {3, 1, 1, 0}, 0},
        {9, 7, 3, 1, 2, 1, {1, 0, 0, 0}, {1, 0, 0, 0}, 0},
        {1, 9, 0, 2, 1, 1, {1, 1, 0, 0}, {9, 4, 0, 0}, 0},
        {9, 7, 0, 0, 0, 0, {0, 0, 0, 0}, {0, 0, 0, 0}, 1},
    };
    unsigned path, c;
    static const char *names[] = {"DXBC", "DXIL", "DXBC-lod32", "DXBC-lod-fffffff0", "DXBC-constant1"};
    for (path = 0; path < 5; ++path) for (c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        struct vkdu_texture2d_shape shape = {cases[c].width, cases[c].height, DXGI_FORMAT_R32_FLOAT, 4, 3, 0, 1, 0};
        struct vkdu_descriptor_range ranges[] = {{0, 1, 0, 0, 0}, {0, 1, 0, 1, 0}};
        struct vkdu_root_parameter roots[] = {{4, 0, 0, 0, 0}, {0, 0, 0, 0, 0, ranges, 1}, {0, 0, 0, 0, 0, ranges + 1, 1}};
        vkdu_object *image, *upload, *output, *readback, *heap, *root, *pipeline, *allocator, *command, *queue, *fence;
        uint32_t *mapped, sub, i, lane, component;
        uint32_t code[sizeof(tex2d_array_reinterpretation_code_dxbc) / 4 + 8];
        size_t code_size = 0;
        if ((scope == 1 && cases[c].null_view) || (scope == 2 && !cases[c].null_view)) continue;
        CHECK(vkdu_texture2d_shape_create(device, &shape, &image));
        CHECK(vkdu_buffer_create(device, 32768, 2, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
        CHECK(vkdu_buffer_map(upload, 0, 0, (void **)&mapped));
        memset(mapped, 0, 32768);
        for (i = 0; i < 1024; ++i) mapped[i] = 0xacbdcedf;
        CHECK(vkdu_buffer_unmap(upload, 0, 32768));
        CHECK(vkdu_buffer_create(device, 4096, 3, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
        CHECK(vkdu_buffer_create(device, 4096, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, &output));
        CHECK(vkdu_allocator_create(device, 0, &allocator)); CHECK(vkdu_command_create(device, allocator, 0, &command));
        CHECK(vkdu_queue_create(device, 0, &queue)); CHECK(vkdu_fence_create(device, 0, &fence));
        CHECK(vkdu_heap_create(device, 0, 2, 1, &heap));
        if (cases[c].null_view) {
            ID3D12GraphicsCommandList *list = vkdu_test_command_iface(command);
            ID3D12Device *raw_device;
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
            D3D12_CPU_DESCRIPTOR_HANDLE handle = {(SIZE_T)vkdu_heap_start(heap, 0)};
            CHECK(ID3D12GraphicsCommandList_GetDevice(list, &IID_ID3D12Device, (void **)&raw_device));
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipLevels = 1;
            ID3D12Device_CreateShaderResourceView(raw_device, NULL, &desc, handle);
            handle.ptr += ID3D12Device_GetDescriptorHandleIncrementSize(raw_device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.MipLevels = 1; desc.Texture2DArray.ArraySize = 1;
            ID3D12Device_CreateShaderResourceView(raw_device, NULL, &desc, handle);
            ID3D12Device_Release(raw_device);
        } else {
            CHECK(vkdu_texture2d_srv_range(heap, 0, image, DXGI_FORMAT_R32_FLOAT, cases[c].first_mip, cases[c].mips, 0, 1));
            CHECK(vkdu_texture2d_srv_range(heap, 1, image, DXGI_FORMAT_R32_FLOAT, cases[c].first_mip, cases[c].mips,
                    cases[c].first_layer, cases[c].layers));
        }
        CHECK(vkdu_root_create(device, roots, 3, 0, &root));
        if (path >= 2) resinfo_test_shader(path, code, &code_size);
        CHECK(vkdu_pipeline_create(device, root,
                path >= 2 ? (const void *)code : path == 1 ? (const void *)tex2d_array_reinterpretation_code_dxil : (const void *)tex2d_array_reinterpretation_code_dxbc,
                path >= 2 ? code_size : path == 1 ? sizeof(tex2d_array_reinterpretation_code_dxil) : sizeof(tex2d_array_reinterpretation_code_dxbc), &pipeline));
        CHECK(vkdu_command_copy(command, output, 0, upload, 0, 4096));
        for (sub = 0; sub < 12; ++sub) {
            uint32_t mip = sub % 4, w = shape.width >> mip, h = shape.height >> mip;
            struct vkdu_texture_copy_location from = {upload, 4096 + sub * 2048, 1, DXGI_FORMAT_R32_FLOAT,
                w ? w : 1, h ? h : 1, 1, 256}, to = {image, sub};
            CHECK(vkdu_command_texture_copy(command, &to, 0, 0, 0, &from, NULL));
        }
        CHECK(vkdu_command_transition(command, image, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        CHECK(vkdu_command_heaps(command, 1, &heap)); CHECK(vkdu_command_root(command, root));
        CHECK(vkdu_command_pipeline(command, pipeline)); CHECK(vkdu_command_uav(command, 0, output, 0));
        CHECK(vkdu_command_table(command, 1, heap, 0)); CHECK(vkdu_command_table(command, 2, heap, 1));
        CHECK(vkdu_command_dispatch(command, 1, 1, 1));
        CHECK(vkdu_command_transition(command, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        CHECK(vkdu_command_copy(command, readback, 0, output, 0, 4096)); CHECK(vkdu_command_close(command));
        CHECK(vkdu_queue_execute(queue, 1, &command)); CHECK(vkdu_queue_signal(queue, fence, 1)); CHECK(vkdu_fence_wait(fence, 1, 10000));
        CHECK(vkdu_buffer_map(readback, 0, 4096, (void **)&mapped));
        for (i = 0; i < 1024; ++i) {
            uint32_t want = 0xacbdcedf;
            if (i < 16 * 9) {
                uint32_t lod;
                lane = i / 9; component = i % 9; want = 0;
                lod = path < 2 ? lane : path == 4 ? 1 : 32; /* Both large-LOD fixtures are entirely out of range. */
                if (component < 4 && lod < 4) want = component & 1 ? cases[c].heights[lod] : cases[c].widths[lod];
                if (component == 4 && lod < cases[c].mips) want = cases[c].layers;
                if (component == 5 || component == 6) want = cases[c].mips;
            }
            if (mapped[i] != want) {
                fprintf(stderr, "FAIL resinfo %s case%u word%u=%08x expected%08x\n", names[path], c, i, mapped[i], want);
                exit(1);
            }
        }
        CHECK(vkdu_buffer_unmap(readback, 0, 0));
        vkdu_object_destroy(command); vkdu_object_destroy(allocator); vkdu_object_destroy(heap);
        vkdu_object_destroy(root); vkdu_object_destroy(pipeline); vkdu_object_destroy(image);
        vkdu_object_destroy(upload); vkdu_object_destroy(output); vkdu_object_destroy(readback);
        vkdu_object_destroy(queue); vkdu_object_destroy(fence);
        printf("PASS resinfo %s case%u: all 1024 words, per-lane mip, view base/layers, NPOT/tail/bounds/null and unchanged zero Loads/padding\n", names[path], c);
    }
}
