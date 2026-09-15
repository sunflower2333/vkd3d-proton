/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* CPU contract test for the exact production entrypoint bodies. The object
 * shell and final COM calls are test doubles, not WDK ABI or GPU validation. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define S_OK ((int32_t)0)
#define E_INVALIDARG ((int32_t)0x80070057u)
enum { VKDU_BUFFER, VKDU_COMMAND_LIST };
enum { D3D12_ROOT_PARAMETER_TYPE_CBV = 2, D3D12_ROOT_PARAMETER_TYPE_SRV = 3,
       D3D12_ROOT_PARAMETER_TYPE_UAV = 4, D3D12_COMMAND_LIST_TYPE_COPY = 3 };
enum { D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS = 4,
       D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE = 8 };
typedef struct { unsigned calls, type, index; uint64_t address; } ID3D12GraphicsCommandList;
typedef struct vkdu_object {
    void *object, *owner;
    unsigned kind, closed, command_type, slot_count, resource_flags;
    struct { unsigned type; } slots[64];
    uint64_t bytes, test_address;
} vkdu_object;
#define OBJ(t, o) ((t *)(o)->object)
#define VALID(o, k) ((o) && (o)->object && (o)->kind == (k))
#define RECORDING(o) (VALID(o, VKDU_COMMAND_LIST) && !(o)->closed)
static unsigned queries, checks;

/* Emulate only address lookup and device ownership, not binding policy. */
static uint64_t vkdu_buffer_address(vkdu_object *buffer) { ++queries; return buffer->test_address; }
static int vkdu_same_device(vkdu_object *a, vkdu_object *b) { return a->owner == b->owner; }
/* Record the precise arguments and distinguish all three final COM setters. */
static void record(ID3D12GraphicsCommandList *list, unsigned index, uint64_t address, unsigned type)
{ ++list->calls; list->index = index; list->address = address; list->type = type; }
#define ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(c, i, a) record(c, i, a, 4)
#define ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(c, i, a) record(c, i, a, 2)
#define ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(c, i, a) record(c, i, a, 3)
#ifndef ROOT_DESCRIPTOR_IMPLEMENTATION
#define ROOT_DESCRIPTOR_IMPLEMENTATION "backend_root_descriptor.inc"
#endif
#include ROOT_DESCRIPTOR_IMPLEMENTATION

/* Keep failures observable in optimized/NDEBUG builds and in mutation runs. */
static void check(int condition, const char *name, unsigned line)
{
    ++checks;
    if (!condition) { fprintf(stderr, "FAIL root descriptor: %s line=%u\n", name, line); exit(1); }
}
#define CHECK(c, n) check(!!(c), n, __LINE__)
typedef int32_t (*bind_fn)(vkdu_object *, uint32_t, vkdu_object *, uint64_t);

/* Exercise every production entry with a common independent test fixture. */
static void run(unsigned type, bind_fn bind)
{
    ID3D12GraphicsCommandList list = {0};
    int owner = 1, other = 2;
    vkdu_object command = {0}, buffer = {0}, bad;
    const uint64_t alignment = type == D3D12_ROOT_PARAMETER_TYPE_CBV ? 256 : 4;
    const uint64_t bases[] = {0, 1, 4, 256, 65536, UINT64_MAX - 511, UINT64_MAX - 255, UINT64_MAX - 3};
    unsigned b, o;
    command.object = &list; command.owner = &owner; command.kind = VKDU_COMMAND_LIST;
    command.slot_count = 1; command.slots[0].type = type;
    buffer.object = &owner; buffer.owner = &owner; buffer.kind = VKDU_BUFFER;
    buffer.bytes = 1024; buffer.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (b = 0; b < sizeof(bases) / sizeof(bases[0]); ++b) {
        for (o = 0; o <= 1024; ++o) {
            int valid = bases[b] != 0 && o < buffer.bytes && o % alignment == 0 &&
                    bases[b] % alignment == 0 && o <= UINT64_MAX - bases[b];
            buffer.test_address = bases[b]; queries = 0; list.calls = 0; list.address = 0x1234;
            CHECK(bind(&command, 0, &buffer, o) == (valid ? S_OK : E_INVALIDARG), "address verdict");
            CHECK(list.calls == (unsigned)valid, "record only on success");
            if (valid) {
                CHECK(list.address == bases[b] + o && list.type == type && list.index == 0, "exact COM arguments");
                CHECK(queries == 1, "one address lookup");
            } else CHECK(list.address == 0x1234, "rejection leaves state untouched");
        }
    }
    buffer.test_address = 65536;
    list.calls = queries = 0;
    CHECK(bind(NULL, 0, &buffer, 0) == E_INVALIDARG, "null command");
    CHECK(bind(&command, 0, NULL, 0) == E_INVALIDARG, "null buffer");
    CHECK(bind(&command, 64, &buffer, 0) == E_INVALIDARG, "slot out of range");
    CHECK(bind(&command, UINT32_MAX, &buffer, 0) == E_INVALIDARG, "overflow slot");
    bad = command; bad.closed = 1;
    CHECK(bind(&bad, 0, &buffer, 0) == E_INVALIDARG, "closed list");
    bad = command; bad.command_type = D3D12_COMMAND_LIST_TYPE_COPY;
    CHECK(bind(&bad, 0, &buffer, 0) == E_INVALIDARG, "copy list cannot bind compute roots");
    bad = command; bad.slots[0].type = 0;
    CHECK(bind(&bad, 0, &buffer, 0) == E_INVALIDARG, "wrong root type");
    bad = buffer; bad.owner = &other;
    CHECK(bind(&command, 0, &bad, 0) == E_INVALIDARG, "foreign buffer");
    bad = buffer; bad.object = NULL;
    CHECK(bind(&command, 0, &bad, 0) == E_INVALIDARG, "missing backend");
    bad = buffer; bad.bytes = 0;
    CHECK(bind(&command, 0, &bad, 0) == E_INVALIDARG, "empty buffer");
    if (type == D3D12_ROOT_PARAMETER_TYPE_UAV) {
        bad = buffer; bad.resource_flags = 0;
        CHECK(bind(&command, 0, &bad, 0) == E_INVALIDARG, "UAV permission required");
    }
    if (type == D3D12_ROOT_PARAMETER_TYPE_SRV) {
        bad = buffer; bad.resource_flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        CHECK(bind(&command, 0, &bad, 0) == E_INVALIDARG, "SRV denied");
    }
    CHECK(!list.calls && !queries, "invalid ownership never reaches backend");
    for (o = 0; o < 2; ++o) {
        command.command_type = o ? 2 : 0;
        CHECK(bind(&command, 0, &buffer, alignment) == S_OK, "direct and compute remain supported");
    }
}

/* No Vulkan loader, device, queue or fence is created by this executable. */
int main(void)
{
    run(D3D12_ROOT_PARAMETER_TYPE_UAV, vkdu_command_uav);
    run(D3D12_ROOT_PARAMETER_TYPE_CBV, vkdu_command_cbv);
    run(D3D12_ROOT_PARAMETER_TYPE_SRV, vkdu_command_srv);
    printf("PASS root descriptors: %u checks\n", checks);
    puts("BACKEND=mock COM; production entrypoint bodies; GPU_ACCEPTANCE=NOT_RUN");
    return 0;
}
