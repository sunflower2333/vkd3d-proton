/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Standalone diagnostic: embed the unchanged production backend so that the
 * actual allocator can be identified without adding a production debug ABI. */
#include "backend.c"
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "../vkd3d/vkd3d_private.h"
#include <inttypes.h>
#include <stdio.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

struct alignment_results
{
    unsigned checked, misaligned, pooled, direct;
};

#ifndef VKDU_ENABLE_TEST_DEVICE
static int alignment_parse_hex32(const char *text, uint32_t *out)
{
    size_t i, length = strlen(text);
    uint32_t value = 0;
    if (!length || length > 8) return 0;
    for (i = 0; i < length; ++i) {
        unsigned digit;
        if (text[i] >= '0' && text[i] <= '9') digit = text[i] - '0';
        else if (text[i] >= 'a' && text[i] <= 'f') digit = text[i] - 'a' + 10;
        else if (text[i] >= 'A' && text[i] <= 'F') digit = text[i] - 'A' + 10;
        else return 0;
        value = value * 16 + digit;
    }
    *out = value;
    return 1;
}
#endif

static int check_resource(ID3D12Resource *resource, const char *kind,
        const char *heap_name, uint64_t bytes, uint64_t heap_bytes,
        uint64_t heap_offset, unsigned index, struct alignment_results *results)
{
    struct d3d12_resource *impl = impl_from_ID3D12Resource(resource);
    uint64_t address = ID3D12Resource_GetGPUVirtualAddress(resource);
    bool pooled = impl->mem.chunk != NULL;
    bool aligned = address && !(address & UINT64_C(65535));

    printf("%s kind=%s heap=%s bytes=%"PRIu64" heap_bytes=%"PRIu64
            " heap_offset=%"PRIu64" index=%u allocator=%s va=0x%016"PRIx64
            " allocation_va=0x%016"PRIx64" allocation_offset=%"PRIu64
            " realignment_offset=%u required=65536\n",
            aligned ? "ALIGNED" : "MISALIGNED", kind, heap_name, bytes,
            heap_bytes, heap_offset, index, pooled ? "pooled" : "direct",
            address, (uint64_t)impl->mem.resource.va,
            (uint64_t)impl->mem.offset, impl->mem.realignment_offset);
    results->checked++;
    results->misaligned += !aligned;
    results->pooled += pooled;
    results->direct += !pooled;
    return address != 0;
}

static int check_allocations(vkdu_device *device, struct alignment_results *results)
{
    static const struct {
        D3D12_HEAP_TYPE type;
        D3D12_RESOURCE_STATES state;
        const char *name;
    } heaps[] = {
        {D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, "DEFAULT"},
        {D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, "UPLOAD"},
        {D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, "READBACK"},
    };
    static const uint64_t sizes[] = {4096, 65536 + 4096, 2 * 1024 * 1024,
        2 * 1024 * 1024 + 4096, 16 * 1024 * 1024 + 4096};
    unsigned h, s, i, p;
    HRESULT hr = S_OK;
    ID3D12Resource *resources[4] = {0};
    ID3D12Resource *placed = NULL;
    ID3D12Heap *heap = NULL;
    D3D12_RESOURCE_DESC desc = {0};
    D3D12_HEAP_DESC heap_desc = {0};

    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Properties.CreationNodeMask = heap_desc.Properties.VisibleNodeMask = 1;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

    for (h = 0; h < ARRAY_SIZE(heaps); ++h) {
        heap_desc.Properties.Type = heaps[h].type;
        for (s = 0; s < ARRAY_SIZE(sizes); ++s) {
            /* Four simultaneous allocations expose allocator reuse/offsets;
             * at most 64.25 MiB of requested committed storage is live here. */
            desc.Width = sizes[s];
            for (i = 0; i < ARRAY_SIZE(resources); ++i) {
                hr = ID3D12Device_CreateCommittedResource(device->object,
                        &heap_desc.Properties, D3D12_HEAP_FLAG_NONE, &desc,
                        heaps[h].state, NULL, &IID_ID3D12Resource, (void **)&resources[i]);
                if (FAILED(hr)) goto fail;
                if (!check_resource(resources[i], "committed", heaps[h].name,
                        desc.Width, 0, 0, i, results)) { hr = E_FAIL; goto fail; }
            }
            for (i = 0; i < ARRAY_SIZE(resources); ++i) {
                ID3D12Resource_Release(resources[i]);
                resources[i] = NULL;
            }

            /* Placed buffers test both offset zero and a nonzero legal offset.
             * Log real allocator state; requested size alone is not evidence
             * that a particular pool/direct path was taken. */
            desc.Width = 65536;
            heap_desc.SizeInBytes = align(max(sizes[s], UINT64_C(131072)), 65536);
            hr = ID3D12Device_CreateHeap(device->object, &heap_desc,
                    &IID_ID3D12Heap, (void **)&heap);
            if (FAILED(hr)) goto fail;
            for (p = 0; p < 2; ++p) {
                hr = ID3D12Device_CreatePlacedResource(device->object, heap,
                        (uint64_t)p * 65536, &desc, heaps[h].state, NULL,
                        &IID_ID3D12Resource, (void **)&placed);
                if (FAILED(hr)) goto fail;
                if (!check_resource(placed, "placed", heaps[h].name, desc.Width,
                        heap_desc.SizeInBytes, (uint64_t)p * 65536, p, results)) {
                    hr = E_FAIL; goto fail;
                }
                ID3D12Resource_Release(placed);
                placed = NULL;
            }
            ID3D12Heap_Release(heap);
            heap = NULL;
        }
    }
    return 1;

fail:
    fprintf(stderr, "ERROR allocation diagnostic hr=0x%08x heap=%u size_case=%u\n",
            (unsigned)hr, h, s);
    if (placed) ID3D12Resource_Release(placed);
    if (heap) ID3D12Heap_Release(heap);
    for (i = 0; i < ARRAY_SIZE(resources); ++i)
        if (resources[i]) ID3D12Resource_Release(resources[i]);
    return 0;
}

int main(int argc, char **argv)
{
    struct alignment_results results = {0};
    PFN_vkGetInstanceProcAddr loader = NULL;
    vkdu_device *device = NULL;
    HRESULT hr;
    int audit = argc > 1 && !strcmp(argv[argc - 1], "--audit");
    int ret = 1;
#ifdef _WIN32
    HMODULE module;
#else
    void *module;
#endif
#ifndef VKDU_ENABLE_TEST_DEVICE
    struct vkdu_adapter adapter = {{0}, 0, 0};
    uint32_t luid[2];
    if (argc - audit != 6 || strcmp(argv[1], "--adapter") ||
            !alignment_parse_hex32(argv[2], &luid[0]) ||
            !alignment_parse_hex32(argv[3], &luid[1]) ||
            !alignment_parse_hex32(argv[4], &adapter.vendor_id) ||
            !alignment_parse_hex32(argv[5], &adapter.device_id)) {
        fprintf(stderr, "Usage: %s --adapter LUID_LOW_HEX LUID_HIGH_HEX VENDOR_HEX DEVICE_HEX [--audit]\n", argv[0]);
        return 2;
    }
    memcpy(adapter.luid, luid, sizeof(luid));
    printf("Turnip GPUVA diagnostic LUID=%08x:%08x vendor=%08x device=%08x\n",
            luid[1], luid[0], adapter.vendor_id, adapter.device_id);
#else
    if (argc - audit != 1) {
        fprintf(stderr, "Usage: %s [--audit]\n", argv[0]);
        return 2;
    }
    puts("TEST ONLY: CPU Vulkan allocation diagnostic; no target GPU acceptance");
#endif
#ifdef _WIN32
    module = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module) loader = (PFN_vkGetInstanceProcAddr)GetProcAddress(module, "vkGetInstanceProcAddr");
#else
    module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (module) loader = (PFN_vkGetInstanceProcAddr)dlsym(module, "vkGetInstanceProcAddr");
#endif
    if (!loader) { fprintf(stderr, "ERROR system Vulkan loader unavailable\n"); goto done; }
#ifdef VKDU_ENABLE_TEST_DEVICE
    hr = vkdu_test_device_create(loader, &device);
#else
    hr = vkdu_device_create(loader, &adapter, &device);
#endif
    if (FAILED(hr)) { fprintf(stderr, "ERROR device create hr=0x%08x\n", (unsigned)hr); goto done; }
    if (!check_allocations(device, &results)) goto done;
    hr = vkdu_device_status(device);
    if (FAILED(hr)) { fprintf(stderr, "ERROR device lost hr=0x%08x\n", (unsigned)hr); goto done; }
    if (results.checked != 90 || !results.pooled || !results.direct) {
        fprintf(stderr, "ERROR incomplete coverage checked=%u pooled=%u direct=%u\n",
                results.checked, results.pooled, results.direct);
        goto done;
    }
    printf("ALIGNMENT_CONTRACT=%s checked=%u misaligned=%u pooled=%u direct=%u mode=%s\n",
            results.misaligned ? "FAIL" : "PASS", results.checked, results.misaligned,
            results.pooled, results.direct, audit ? "audit" : "strict");
    if (audit && results.misaligned)
        puts("AUDIT COMPLETED WITH CONTRACT FAILURES; exit 0 records the baseline, not alignment acceptance");
    ret = results.misaligned && !audit ? 3 : 0;
done:
    vkdu_device_destroy(device);
#ifdef _WIN32
    if (module) FreeLibrary(module);
#else
    if (module) dlclose(module);
#endif
    return ret;
}
