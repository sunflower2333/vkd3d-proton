/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ddi.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

static void APIENTRY report(void *, HRESULT) {}
template<typename T> struct Guarded { uint64_t before = 0x123456789abcdef0; T value{}; uint64_t after = 0x0fedcba987654321; };
template<typename T> static bool guards(const Guarded<T> &v) { return v.before == 0x123456789abcdef0 && v.after == 0x0fedcba987654321; }
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    HMODULE module = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    REQUIRE(module);
    auto tables = reinterpret_cast<decltype(&VioGpuD3D12BridgeGetTables)>(GetProcAddress(module, "VioGpuD3D12BridgeGetTables"));
    auto size = reinterpret_cast<decltype(&VioGpuD3D12BridgeObjectSize)>(GetProcAddress(module, "VioGpuD3D12BridgeObjectSize"));
    auto create = reinterpret_cast<decltype(&VioGpuD3D12BridgeCreate)>(GetProcAddress(module, "VioGpuD3D12BridgeCreate"));
    REQUIRE(tables && size && create);
    REQUIRE(!GetProcAddress(module, "D3D12CreateDevice"));
    REQUIRE(!GetProcAddress(module, "OpenAdapter12"));
    REQUIRE(!GetProcAddress(module, "vkdu_test_device_create"));
    Guarded<D3D12DDI_DEVICE_FUNCS_CORE_0003> device;
    Guarded<D3D12DDI_COMMAND_LIST_FUNCS_3D_0003> commands;
    Guarded<D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001> queue;
    REQUIRE(tables(&device.value, &commands.value, &queue.value) == S_OK);
    REQUIRE(guards(device) && guards(commands) && guards(queue));
    REQUIRE(tables(nullptr, &commands.value, &queue.value) == E_INVALIDARG);
    REQUIRE(device.value.pfnCreateCommandQueue && device.value.pfnCreateComputeShader && device.value.pfnCreatePipelineState);
    REQUIRE(commands.value.pfnCopyBufferRegion && commands.value.pfnDispatch && queue.value.pfnExecuteCommandLists);
    REQUIRE(device.value.pfnCreateDescriptorHeap && device.value.pfnDestroyDescriptorHeap && device.value.pfnGetDescriptorSizeInBytes);
    REQUIRE(device.value.pfnGetCPUDescriptorHandleForHeapStart && device.value.pfnGetGPUDescriptorHandleForHeapStart);
    REQUIRE(device.value.pfnCreateUnorderedAccessView && device.value.pfnCopyDescriptorsSimple);
    REQUIRE(device.value.pfnCreateConstantBufferView && device.value.pfnCreateShaderResourceView);
    REQUIRE(commands.value.pfnSetComputeRootConstantBufferView && commands.value.pfnSetComputeRootShaderResourceView);
    REQUIRE(commands.value.pfnSetDescriptorHeaps && commands.value.pfnSetComputeRootDescriptorTable);
    REQUIRE(!device.value.pfnCreateHeapAndResource && !device.value.pfnCreateFence && !device.value.pfnMakeResident);
    REQUIRE(!queue.value.pfnSignalFence && !queue.value.pfnWaitForFence);
    D3D12DDI_HDEVICE h{};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 q{};
    REQUIRE(device.value.pfnCalcPrivateCommandQueueSize(h, &q) == size());
    REQUIRE(device.value.pfnCreateCommandQueue(h, &q) == E_INVALIDARG);
    REQUIRE(device.value.pfnCreateCommandAllocator(h, nullptr) == E_INVALIDARG);
    REQUIRE(device.value.pfnCreateCommandList(h, nullptr) == E_INVALIDARG);
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args{};
    D3D12DDI_HDESCRIPTORHEAP heap{};
    REQUIRE(device.value.pfnCalcPrivateDescriptorHeapSize(h, &heap_args) == size());
    REQUIRE(device.value.pfnCreateDescriptorHeap(h, &heap_args, heap) == E_INVALIDARG);
    REQUIRE(device.value.pfnGetDescriptorSizeInBytes(h, D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) == 0);
    REQUIRE(device.value.pfnGetCPUDescriptorHandleForHeapStart(h, heap).ptr == 0);
    REQUIRE(device.value.pfnGetGPUDescriptorHandleForHeapStart(h, heap).ptr == 0);
    device.value.pfnCopyDescriptorsSimple(h, 1, {}, {}, D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device.value.pfnCopyDescriptors(h, 0, nullptr, nullptr, 0, nullptr, nullptr, D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device.value.pfnCreateUnorderedAccessView(h, nullptr, {});
    device.value.pfnCreateShaderResourceView(h, nullptr, {});
    commands.value.pfnSetComputeRootShaderResourceView({}, 0, 0);
    commands.value.pfnSetDescriptorHeaps({}, 0, nullptr);
    commands.value.pfnSetComputeRootDescriptorTable({}, 0, {});
    REQUIRE(create(nullptr, nullptr, report, nullptr, &h) == E_INVALIDARG);
    REQUIRE(!h.pDrvPrivate);
    FreeLibrary(module);
    std::printf("PASS pointer_bits=%zu exact WDK table sizes/calling conventions/exports and invalid-handle errors; no Vulkan instance or GPU work\n", sizeof(void *) * 8);
    return 0;
}
