/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Compile the actual DDI adapter with controlled backend peers. The separate
 * backend test executes real Vulkan; this verifies the native WDK boundary. */
#include "ddi.h"
#include <cstdio>
#include <cstdlib>

struct Peer {
    vkdu_kind kind;
    vkdu_device *owner;
    uint32_t type, count;
    bool visible;
    uint64_t cpu, gpu;
};
static Peer *peer(vkdu_object *p) { return reinterpret_cast<Peer *>(p); }
static uint64_t next_base = 0x10000;
static unsigned calls, copies, tables_seen, destroys;
static int passed_visibility = -1;
static HRESULT next_result = S_OK, reported = S_OK;
static uint32_t uav_index, table_index, table_first, root_offset, root_space;
static uint32_t cbv_index, cbv_bytes, root_cbv_index;
static uint64_t cbv_offset;
static vkdu_object *cbv_buffer;
static vkdu_object *make_peer(vkdu_device *owner, vkdu_kind kind, uint32_t type = 0, uint32_t count = 8, bool visible = false) {
    next_base += 0x10000;
    return reinterpret_cast<vkdu_object *>(new Peer{kind, owner, type, count, visible, next_base, next_base + 0x10000000});
}
static int test_is(vkdu_object *p, vkdu_kind kind) { return p && peer(p)->kind == kind; }
static int test_belongs(vkdu_device *d, vkdu_object *p) { return p && peer(p)->owner == d; }
static void test_destroy(vkdu_object *p) { if (p) { ++destroys; delete peer(p); } }
static uint64_t test_address(vkdu_object *p) { return peer(p)->cpu; }
static uint64_t test_size(vkdu_object *) { return 4096; }
static int32_t test_heap_create(vkdu_device *d, uint32_t type, uint32_t count, int visible, vkdu_object **out) {
    ++calls; passed_visibility = visible; *out = nullptr;
    if (FAILED(next_result)) return next_result;
    *out = make_peer(d, VKDU_DESCRIPTOR_HEAP, type, count, visible != 0);
    return S_OK;
}
static uint32_t test_stride(vkdu_device *, uint32_t) { return 32; }
static uint64_t test_start(vkdu_object *p, int gpu) {
    if (!test_is(p, VKDU_DESCRIPTOR_HEAP)) return 0;
    return gpu ? (peer(p)->visible ? peer(p)->gpu : 0) : peer(p)->cpu;
}
static int test_resolve(vkdu_object *p, uint64_t address, int gpu, uint32_t *index) {
    uint64_t start = test_start(p, gpu);
    if (!start || address < start || (address - start) % 32 || (address - start) / 32 >= peer(p)->count) return 0;
    *index = static_cast<uint32_t>((address - start) / 32); return 1;
}
static int32_t test_uav(vkdu_object *, uint32_t index, vkdu_object *, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t, vkdu_object *, uint64_t) {
    ++calls; uav_index = index; return next_result;
}
static int32_t test_copy(vkdu_object *, uint32_t, vkdu_object *, uint32_t, uint32_t) { ++copies; return next_result; }
static int32_t test_cbv(vkdu_object *, uint32_t index, vkdu_object *buffer, uint64_t offset, uint32_t bytes) {
    ++calls; cbv_index = index; cbv_buffer = buffer; cbv_offset = offset; cbv_bytes = bytes; return next_result;
}
static int32_t test_root_cbv(vkdu_object *, uint32_t index, vkdu_object *buffer, uint64_t offset) {
    ++calls; root_cbv_index = index; cbv_buffer = buffer; cbv_offset = offset; return next_result;
}
static int32_t test_heaps(vkdu_object *, uint32_t, vkdu_object *const *) { ++calls; return next_result; }
static int32_t test_table(vkdu_object *, uint32_t index, vkdu_object *, uint32_t first) {
    ++tables_seen; table_index = index; table_first = first; return next_result;
}
static int32_t test_root(vkdu_device *d, const vkdu_root_parameter *p, uint32_t count, uint32_t, vkdu_object **out) {
    ++calls;
    if (count != 1 || p[0].type != 0 || p[0].range_count != 1 || !p[0].ranges) return E_INVALIDARG;
    root_offset = p[0].ranges[0].offset; root_space = p[0].ranges[0].register_space;
    *out = make_peer(d, VKDU_ROOT); return S_OK;
}
#define vkdu_object_is test_is
#define vkdu_object_belongs test_belongs
#define vkdu_object_destroy test_destroy
#define vkdu_buffer_address test_address
#define vkdu_buffer_size test_size
#define vkdu_heap_create test_heap_create
#define vkdu_descriptor_size test_stride
#define vkdu_heap_start test_start
#define vkdu_heap_resolve test_resolve
#define vkdu_buffer_uav test_uav
#define vkdu_buffer_cbv test_cbv
#define vkdu_command_cbv test_root_cbv
#define vkdu_descriptor_copy test_copy
#define vkdu_command_heaps test_heaps
#define vkdu_command_table test_table
#define vkdu_root_create test_root
#include "ddi.cpp"

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL native descriptor line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static void APIENTRY capture(void *, HRESULT hr) { reported = hr; }
int main() {
    Context ctx;
    ctx.backend = reinterpret_cast<vkdu_device *>(&next_base);
    ctx.report = capture;
    D3D12DDI_HDEVICE h{&ctx};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    REQUIRE(VioGpuD3D12BridgeGetTables(&device, &commands, &queue) == S_OK);
    Object cpu{}, gpu{}, failed{}, buffer{}, command{}, root{};
    D3D12DDI_HDESCRIPTORHEAP hc{&cpu}, hg{&gpu}, hf{&failed};
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE, 1};
    REQUIRE(device.pfnCreateDescriptorHeap(h, &args, hc) == S_OK && passed_visibility == 0);
    args.Flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE | D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    REQUIRE(device.pfnCreateDescriptorHeap(h, &args, hg) == S_OK && passed_visibility == 1);
    REQUIRE(device.pfnGetCPUDescriptorHandleForHeapStart(h, hc).ptr == test_start(cpu.backend, 0));
    REQUIRE(device.pfnGetGPUDescriptorHandleForHeapStart(h, hc).ptr == 0);
    REQUIRE(device.pfnGetGPUDescriptorHandleForHeapStart(h, hg).ptr == test_start(gpu.backend, 1));
    next_result = E_OUTOFMEMORY;
    unsigned references = ctx.references;
    REQUIRE(device.pfnCreateDescriptorHeap(h, &args, hf) == E_OUTOFMEMORY && !failed.magic && ctx.references == references);
    next_result = S_OK;
    auto *b = make_peer(ctx.backend, VKDU_BUFFER);
    auto *c = make_peer(ctx.backend, VKDU_COMMAND_LIST);
    REQUIRE(VioGpuD3D12BridgeBindObject(h, &buffer, b, VKDU_BUFFER) == S_OK);
    REQUIRE(VioGpuD3D12BridgeBindObject(h, &command, c, VKDU_COMMAND_LIST) == S_OK);
    D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 view{};
    view.hDrvResource = {&buffer}; view.ResourceDimension = D3D12DDI_RD_BUFFER;
    view.Format = DXGI_FORMAT_R32_TYPELESS; view.Buffer.NumElements = 1024; view.Buffer.Flags = D3D12DDI_BUFFER_UAV_FLAG_RAW;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE destination{static_cast<SIZE_T>(test_start(cpu.backend, 0) + 3 * 32)};
    device.pfnCreateUnorderedAccessView(h, &view, destination);
    REQUIRE(uav_index == 3 && reported == S_OK);
    unsigned before = calls;
    ++destination.ptr;
    device.pfnCreateUnorderedAccessView(h, &view, destination);
    REQUIRE(calls == before && reported == E_INVALIDARG);
    --destination.ptr;
    reported = S_OK;
    D3D12DDI_CONSTANT_BUFFER_VIEW_DESC cbv{buffer.address + 256, 512, 0};
    device.pfnCreateConstantBufferView(h, &cbv, destination);
    REQUIRE(reported == S_OK && cbv_index == 3 && cbv_buffer == b && cbv_offset == 256 && cbv_bytes == 512);
    cbv.BufferLocation = 0;
    device.pfnCreateConstantBufferView(h, &cbv, destination);
    REQUIRE(!cbv_buffer && cbv_offset == 0 && cbv_bytes == 512);
    before = calls; cbv.BufferLocation = buffer.address + buffer.bytes;
    device.pfnCreateConstantBufferView(h, &cbv, destination);
    REQUIRE(calls == before && reported == E_INVALIDARG);
    device.pfnCreateConstantBufferView(h, nullptr, destination);
    REQUIRE(calls == before && reported == E_INVALIDARG);
    cbv.BufferLocation = buffer.address;
    ++destination.ptr;
    device.pfnCreateConstantBufferView(h, &cbv, destination);
    REQUIRE(calls == before && reported == E_INVALIDARG);
    --destination.ptr;
    commands.pfnSetComputeRootConstantBufferView({&command}, 7, buffer.address + 512);
    REQUIRE(root_cbv_index == 7 && cbv_buffer == b && cbv_offset == 512);
    before = calls;
    commands.pfnSetComputeRootConstantBufferView({&command}, 7, 0);
    REQUIRE(calls == before && reported == E_INVALIDARG);
    commands.pfnSetComputeRootConstantBufferView({&command}, 7, buffer.address + buffer.bytes);
    REQUIRE(calls == before && reported == E_INVALIDARG);
    next_result = DXGI_ERROR_DEVICE_REMOVED;
    device.pfnCreateConstantBufferView(h, &cbv, destination);
    REQUIRE(reported == DXGI_ERROR_DEVICE_REMOVED);
    next_result = S_OK;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE target{static_cast<SIZE_T>(test_start(gpu.backend, 0) + 4 * 32)};
    device.pfnCopyDescriptorsSimple(h, 1, target, destination, D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    REQUIRE(copies == 1);
    device.pfnCopyDescriptorsSimple(h, 1, target, destination, D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    REQUIRE(copies == 1 && reported == E_INVALIDARG);
    commands.pfnSetDescriptorHeaps({&command}, 1, &hg);
    commands.pfnSetComputeRootDescriptorTable({&command}, 5, {test_start(gpu.backend, 1) + 2 * 32});
    REQUIRE(tables_seen == 1 && table_index == 5 && table_first == 2);
    next_result = DXGI_ERROR_DEVICE_REMOVED;
    commands.pfnSetComputeRootDescriptorTable({&command}, 5, {test_start(gpu.backend, 1)});
    REQUIRE(reported == DXGI_ERROR_DEVICE_REMOVED);
    next_result = S_OK;
    D3D12DDI_DESCRIPTOR_RANGE range{D3D12DDI_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 7, 3};
    D3D12DDI_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    D3D12DDI_ROOT_SIGNATURE signature{};
    signature.NumParameters = 1; signature.pRootParameters = &parameter;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args{&signature, 1};
    REQUIRE(device.pfnCreateRootSignature(h, &root_args, {&root}) == S_OK && root_offset == 3 && root_space == 7);
    device.pfnDestroyRootSignature(h, {&root});
    VioGpuD3D12BridgeUnbindObject(&command); VioGpuD3D12BridgeUnbindObject(&buffer);
    device.pfnDestroyDescriptorHeap(h, hc); device.pfnDestroyDescriptorHeap(h, hg);
    REQUIRE(!ctx.descriptor_heaps && !ctx.resources && ctx.references == 1 && destroys == 5);
    REQUIRE(!cpu.magic && !gpu.magic && !root.magic);
    std::puts("PASS actual WDK descriptor DDIs: flags, handles, UAV/CBV/copy/table/root translation, null CBV, failed create, device loss and balanced ownership; backend peers only");
    return 0;
}
