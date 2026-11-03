/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Ordinary application API only. No vkd3d backend, WDK headers or UMD exports. */
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <stdexcept>
using Microsoft::WRL::ComPtr;

namespace {
void check(HRESULT hr, const char *stage) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL PUBLIC_D3D12 stage=%s hr=%08x\n", stage, static_cast<unsigned>(hr));
        throw std::runtime_error(stage);
    }
}
void require(bool value, const char *stage) { if (!value) check(E_FAIL, stage); }
bool hex32(const char *s, uint32_t &value) {
    if (!s || !*s || std::strlen(s) > 8) return false;
    for (const char *p = s; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) return false;
    value = static_cast<uint32_t>(std::strtoul(s, nullptr, 16)); return true;
}
struct Module {
    HMODULE value;
    explicit Module(const wchar_t *name) : value(LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!value) check(HRESULT_FROM_WIN32(GetLastError()), "LoadLibraryExW(System32)");
        wchar_t path[32768]{};
        GetModuleFileNameW(value, path, 32768); std::wprintf(L"SYSTEM_MODULE %ls\n", path);
    }
    ~Module() { FreeLibrary(value); }
};
void modules() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) do {
        wchar_t lower[MAX_PATH]{}; wcsncpy_s(lower, entry.szModule, _TRUNCATE); CharLowerBuffW(lower, static_cast<DWORD>(std::wcslen(lower)));
        if (std::wcsstr(lower, L"viogpu") || std::wcsstr(lower, L"mesa") || std::wcsstr(lower, L"d3d12") ||
                std::wcsstr(lower, L"dxgi") || std::wcsstr(lower, L"vulkan") || std::wcsstr(lower, L"warp") || std::wcsstr(lower, L"dxvk"))
            std::wprintf(L"LOADED_MODULE %ls\n", entry.szExePath);
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
}
void describe(IDXGIAdapter1 *adapter) {
    DXGI_ADAPTER_DESC1 desc{}; check(adapter->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");
    std::wprintf(L"ADAPTER luid=%08x:%08x vendor=%04x device=%04x software=%u name=%ls\n",
        static_cast<unsigned>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
        desc.VendorId, desc.DeviceId, !!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE), desc.Description);
}
void fence_test(ID3D12Device *device) {
    ComPtr<ID3D12CommandQueue> producer, consumer;
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&producer)), "fences: CreateCommandQueue(producer)");
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&consumer)), "fences: CreateCommandQueue(consumer)");
    ComPtr<ID3D12Fence> gate, done;
    check(device->CreateFence(3, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "fences: CreateFence(initial=3)");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "fences: CreateFence(done)");
    require(gate->GetCompletedValue() == 3 && done->GetCompletedValue() == 0, "fences: initial completed values");
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) check(HRESULT_FROM_WIN32(GetLastError()), "fences: CreateEvent");
    struct Close { HANDLE event; ~Close() { CloseHandle(event); } } close{event};
    check(done->SetEventOnCompletion(1, event), "fences: SetEventOnCompletion");
    check(consumer->Wait(gate.Get(), 4), "fences: queue Wait(future)");
    check(consumer->Signal(done.Get(), 1), "fences: queue Signal(behind wait)");
    require(WaitForSingleObject(event, 30) == WAIT_TIMEOUT && done->GetCompletedValue() == 0,
        "fences: future wait must prevent completion");
    check(producer->Signal(gate.Get(), 4), "fences: producer Signal");
    require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "fences: actual completion event");
    check(device->GetDeviceRemovedReason(), "fences: device status");
    require(done->GetCompletedValue() == 1, "fences: actual completed value");
    check(done->Signal(0), "fences: CPU Signal rewind");
    require(done->GetCompletedValue() == 0 && WaitForSingleObject(event, 0) == WAIT_OBJECT_0,
        "fences: prior event stays signaled across rewind");
    ResetEvent(event);
    ComPtr<ID3D12Fence> orphan;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&orphan)), "fences: CreateFence(orphan)");
    check(orphan->SetEventOnCompletion(100, event), "fences: arm orphan event");
    orphan.Reset();
    require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "fences: final fence release unblocks wait");
    std::puts("PASS PUBLIC_FENCE_LIFECYCLE delayed_queue_completion, CPU_rewind, final_release_event");
}
void copy_test(ID3D12Device *device) {
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList> commands;
    check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "CreateCommandList");
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 4096;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto info = device->GetResourceAllocationInfo(0, 1, &desc);
    require(info.SizeInBytes >= desc.Width && info.SizeInBytes != UINT64_MAX && info.Alignment != 0, "GetResourceAllocationInfo");
    std::printf("PUBLIC_ALLOCATION bytes=%llu alignment=%llu\n", static_cast<unsigned long long>(info.SizeInBytes),
        static_cast<unsigned long long>(info.Alignment));
    auto make = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource> &out, const char *stage) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out)), stage);
    };
    ComPtr<ID3D12Resource> upload, gpu, readback;
    make(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upload, "CreateCommittedResource(upload)");
    make(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, gpu, "CreateCommittedResource(default)");
    make(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, readback, "CreateCommittedResource(readback)");
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) check(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent");
    struct Close { HANDLE event; ~Close() { CloseHandle(event); } } close{event};
    for (unsigned round = 0; round < 4; ++round) {
        D3D12_RANGE no_read{0, 0}, written{0, 4096}, no_write{0, 0};
        uint32_t *mapped = nullptr;
        check(upload->Map(0, &no_read, reinterpret_cast<void **>(&mapped)), "Map(upload)");
        for (unsigned i = 0; i < 1024; ++i) mapped[i] = (round + 1) * 0x1020304u ^ i * 0x9e3779b9u;
        upload->Unmap(0, &written);
        const uint32_t sentinel = 0xdeadbeefu ^ round;
        check(readback->Map(0, &no_read, reinterpret_cast<void **>(&mapped)), "Map(readback sentinel)");
        for (unsigned i = 0; i < 1024; ++i) mapped[i] = sentinel;
        readback->Unmap(0, &written);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = gpu.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (round) {
            check(allocator->Reset(), "Reset(allocator)");
            check(commands->Reset(allocator.Get(), nullptr), "Reset(command list)");
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST; commands->ResourceBarrier(1, &barrier);
        }
        commands->CopyBufferRegion(gpu.Get(), 0, upload.Get(), 128, 2048);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; commands->ResourceBarrier(1, &barrier);
        commands->CopyBufferRegion(readback.Get(), 256, gpu.Get(), 0, 2048);
        check(commands->Close(), "Close(command list)");
        ID3D12CommandList *lists[] = {commands.Get()}; queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), round + 1), "Signal(public fence)");
        check(fence->SetEventOnCompletion(round + 1, event), "SetEventOnCompletion");
        require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU completion timeout");
        check(device->GetDeviceRemovedReason(), "GetDeviceRemovedReason");
        check(readback->Map(0, &written, reinterpret_cast<void **>(&mapped)), "Map(readback results)");
        unsigned mismatches = 0;
        for (unsigned i = 0; i < 1024; ++i) {
            const uint32_t expected = i >= 64 && i < 576 ? (round + 1) * 0x1020304u ^ (i - 32) * 0x9e3779b9u : sentinel;
            if (mapped[i] != expected && mismatches++ < 4)
                std::fprintf(stderr, "MISMATCH round=%u word=%u got=%08x expected=%08x\n", round, i, mapped[i], expected);
        }
        readback->Unmap(0, &no_write);
        require(!mismatches, "PUBLIC_COPY_READBACK");
        std::printf("PASS PUBLIC_COPY round=%u changed_words=512 sentinel_words=512\n", round);
    }
}
}

int main(int argc, char **argv) {
    uint32_t low = 0, high = 0;
    const bool list = argc == 2 && !std::strcmp(argv[1], "--list");
    const bool warp = argc == 2 && !std::strcmp(argv[1], "--run-warp-ci");
    const bool selected = argc == 6 && !std::strcmp(argv[1], "--luid-low") && !std::strcmp(argv[3], "--luid-high") &&
        hex32(argv[2], low) && hex32(argv[4], high) && (low | high) &&
        (!std::strcmp(argv[5], "--run-device") || !std::strcmp(argv[5], "--run-copy") || !std::strcmp(argv[5], "--run-fences"));
    if (!list && !warp && !selected) {
        std::fputs("usage: vkd3d-system-d3d12-probe --list\n"
            "       vkd3d-system-d3d12-probe --luid-low HEX --luid-high HEX --run-device|--run-copy|--run-fences\n"
            "       --run-warp-ci is explicit CPU harness validation only; never target GPU acceptance\n", stderr);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("BOUNDARY ordinary system D3D12 application APIs, mode=%s, pointer_bits=%zu\n",
        list ? "list" : warp ? "CPU_WARP_HARNESS_ONLY" : argv[5], sizeof(void *) * 8);
    try {
        Module dxgi(L"dxgi.dll");
        auto create_factory = reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(dxgi.value, "CreateDXGIFactory1"));
        require(create_factory != nullptr, "GetProcAddress(CreateDXGIFactory1)");
        ComPtr<IDXGIFactory4> factory; check(create_factory(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        if (list) {
            for (UINT i = 0; ; ++i) {
                ComPtr<IDXGIAdapter1> adapter; HRESULT hr = factory->EnumAdapters1(i, &adapter);
                if (hr == DXGI_ERROR_NOT_FOUND) break;
                check(hr, "EnumAdapters1"); describe(adapter.Get());
            }
            return 0;
        }
        ComPtr<IDXGIAdapter1> adapter;
        if (warp) check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter(explicit CI only)");
        else check(factory->EnumAdapterByLuid(LUID{low, static_cast<LONG>(high)}, IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid");
        describe(adapter.Get());
        DXGI_ADAPTER_DESC1 desc{}; check(adapter->GetDesc1(&desc), "selected GetDesc1");
        if (!warp) require(!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.AdapterLuid.LowPart == low &&
                static_cast<UINT>(desc.AdapterLuid.HighPart) == high, "exact hardware adapter; no WARP fallback");
        Module d3d12(L"d3d12.dll");
        auto create_device = reinterpret_cast<decltype(&D3D12CreateDevice)>(GetProcAddress(d3d12.value, "D3D12CreateDevice"));
        require(create_device != nullptr, "GetProcAddress(D3D12CreateDevice)");
        ComPtr<ID3D12Device> device;
        HRESULT hr = create_device(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        modules(); check(hr, "D3D12CreateDevice(FL11_0)");
        const LUID actual = device->GetAdapterLuid();
        require(actual.LowPart == desc.AdapterLuid.LowPart && actual.HighPart == desc.AdapterLuid.HighPart, "device adapter LUID");
        std::puts("PASS PUBLIC_D3D12_DEVICE requested_feature_level=11_0");
        if (warp || !std::strcmp(argv[5], "--run-fences")) fence_test(device.Get());
        if (warp || !std::strcmp(argv[5], "--run-copy")) {
            copy_test(device.Get());
            std::puts(warp ? "PASS CPU_WARP_HARNESS_ONLY 4x1024 words; no VIOGPU acceptance" :
                "PASS PUBLIC_D3D12_COPY 4x1024 words; graphics/Present not tested");
        }
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "PUBLIC_D3D12_NOT_PASSED stage=%s\n", e.what());
        return 1;
    }
}
