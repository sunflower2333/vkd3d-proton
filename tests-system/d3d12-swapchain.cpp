// System D3D12 swapchain probe: flip-model swapchain on a real window, clear + Present N frames.
// Usage: d3d12-swapchain.exe <luid-low-hex> [frames] [interval] [width] [height] [fixed-red:0|1]
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "probe_identity.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
using Microsoft::WRL::ComPtr;

static int fail(const char *stage, HRESULT hr) {
    std::printf("FAIL %s hr=%08lx\n", stage, (unsigned long)hr);
    return 1;
}
#define CHECK(expr, stage) do { HRESULT hr_ = (expr); if (FAILED(hr_)) return fail(stage, hr_); } while (0)

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(w, m, wp, lp); }

static void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 7) { std::puts("usage: d3d12-swapchain <luid-low-hex> [frames] [interval] [w] [h] [fixed-red:0|1]"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned low = 0;
    CHECK(probe_parse_uint(argv[1], 16, 0, UINT_MAX, &low), "adapter LUID argument");
    const LUID luid{static_cast<DWORD>(low), 0};
    CHECK(probe_system_module(L"d3d12.dll", "D3D12"), "System32 d3d12 identity");
    CHECK(probe_system_module(L"dxgi.dll", "DXGI"), "System32 dxgi identity");
    unsigned frames = 3, interval = 1, W = 640, H = 480, fixed_value = 0;
    if (argc > 2) CHECK(probe_parse_uint(argv[2], 10, 1, 600, &frames), "frames argument [1,600]");
    if (argc > 3) CHECK(probe_parse_uint(argv[3], 10, 0, 4, &interval), "interval argument [0,4]");
    if (argc > 4) CHECK(probe_parse_uint(argv[4], 10, 65, 4096, &W), "width argument [65,4096]");
    if (argc > 5) CHECK(probe_parse_uint(argv[5], 10, 64, 4096, &H), "height argument [64,4096]");
    if (argc > 6) CHECK(probe_parse_uint(argv[6], 10, 0, 1, &fixed_value), "fixed-red argument [0,1]");
    const bool fixed = fixed_value != 0;

    WNDCLASSW wc{}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"d3d12swapchain"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT rc{0, 0, (LONG)W, (LONG)H}; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"D3D12 swapchain probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100, 100, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return fail("CreateWindow", HRESULT_FROM_WIN32(GetLastError()));
    pump();

    ComPtr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> adapter;
    CHECK(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid");
    CHECK(probe_hardware_adapter(adapter.Get(), luid), "hardware adapter identity");
    ComPtr<ID3D12Device> device;
    CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    CHECK(probe_driver_modules(), "loaded UMD/ICD identity");
    std::puts("PASS device");

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

    const UINT kBuffers = 2;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.Scaling = DXGI_SCALING_STRETCH;
    ComPtr<IDXGISwapChain1> swap1;
    CHECK(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &swap1), "CreateSwapChainForHwnd");
    ComPtr<IDXGISwapChain3> swap; CHECK(swap1.As(&swap), "QI IDXGISwapChain3");
    std::puts("PASS swapchain");

    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kBuffers;
    ComPtr<ID3D12DescriptorHeap> rtvs; CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)), "CreateDescriptorHeap");
    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> buffers[kBuffers];
    for (UINT i = 0; i < kBuffers; ++i) {
        CHECK(swap->GetBuffer(i, IID_PPV_ARGS(&buffers[i])), "GetBuffer");
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvs->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * inc;
        device->CreateRenderTargetView(buffers[i].Get(), nullptr, h);
    }
    std::puts("PASS buffers");

    ComPtr<ID3D12CommandAllocator> allocators[kBuffers];
    for (auto &a : allocators) CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
    CHECK(list->Close(), "Close(initial)");
    ComPtr<ID3D12Fence> fence; CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
    UINT64 values[kBuffers]{}, next = 1;

    LARGE_INTEGER freq, t0, t1; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    double worst = 0, wait_ms = 0, record_ms = 0, present_ms = 0; LARGE_INTEGER prev = t0;
    auto now_ms = [&]() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return 1000.0 * v.QuadPart / freq.QuadPart; };
    for (unsigned f = 0; f < frames; ++f) {
        pump();
        const UINT i = swap->GetCurrentBackBufferIndex();
        const double a = now_ms();
        if (fence->GetCompletedValue() < values[i]) {
            CHECK(fence->SetEventOnCompletion(values[i], event), "SetEventOnCompletion");
            if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) return fail("fence wait timeout", E_FAIL);
        }
        const double tb = now_ms(); wait_ms += tb - a;
        CHECK(allocators[i]->Reset(), "allocator Reset");
        CHECK(list->Reset(allocators[i].Get(), nullptr), "list Reset");
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = buffers[i].Get(); b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvs->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * inc;
        const float phase = (f % 120) / 119.0f;
        const float color[4] = {fixed ? 1.0f : phase, fixed ? 0.0f : 0.3f, fixed ? 0.0f : 1.0f - phase, 1.0f};
        list->ClearRenderTargetView(h, color, 0, nullptr);
        // A white square that moves so tearing/stale frames are visible on the panel.
        D3D12_RECT r{(LONG)((f * 4) % (W - 64)), (LONG)(H / 2 - 32), 0, (LONG)(H / 2 + 32)}; r.right = r.left + 64;
        const float white[4] = {1, 1, 1, 1};
        list->ClearRenderTargetView(h, white, 1, &r);
        { auto s_ = b.Transition.StateBefore; b.Transition.StateBefore = b.Transition.StateAfter; b.Transition.StateAfter = s_; }
        list->ResourceBarrier(1, &b);
        CHECK(list->Close(), "list Close");
        ID3D12CommandList *lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const double c = now_ms(); record_ms += c - tb;
        const HRESULT phr = swap->Present(interval, 0);
        present_ms += now_ms() - c;
        if (FAILED(phr)) {
            std::printf("frame %u ", f);
            if (phr == DXGI_ERROR_DEVICE_REMOVED) std::printf("removed reason=%08lx ", (unsigned long)device->GetDeviceRemovedReason());
            return fail("Present", phr);
        }
        values[i] = next++;
        CHECK(queue->Signal(fence.Get(), values[i]), "Signal");
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const double ms = 1000.0 * (now.QuadPart - prev.QuadPart) / freq.QuadPart; prev = now;
        if (f > 2 && ms > worst) worst = ms;
        if (f == 0) std::puts("PASS first present");
    }
    CHECK(queue->Signal(fence.Get(), next), "final Signal");
    CHECK(fence->SetEventOnCompletion(next, event), "final SetEventOnCompletion");
    const DWORD final_wait = WaitForSingleObject(event, 5000);
    if (final_wait != WAIT_OBJECT_0) {
        const DWORD error = final_wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        CloseHandle(event);
        std::printf("final wait result=%08lx ", static_cast<unsigned long>(final_wait));
        return fail("final fence wait", HRESULT_FROM_WIN32(error));
    }
    CHECK(device->GetDeviceRemovedReason(), "final GetDeviceRemovedReason");
    if (fence->GetCompletedValue() != next) return fail("final completed fence value", E_FAIL);
    CloseHandle(event);
    QueryPerformanceCounter(&t1);
    const double secs = double(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
    DXGI_FRAME_STATISTICS stats{};
    const HRESULT shr = swap->GetFrameStatistics(&stats);
    std::printf("TIMING per frame: fence-wait=%.2fms record+reset+execute=%.2fms present=%.2fms\n",
        wait_ms / frames, record_ms / frames, present_ms / frames);
    std::printf("PASS D3D12 SWAPCHAIN gpu_completion=1 frames=%u secs=%.3f fps=%.1f worst=%.2fms stats_hr=%08lx PresentCount=%u PresentRefreshCount=%u SyncRefreshCount=%u\n",
        frames, secs, frames / secs, worst, (unsigned long)shr, stats.PresentCount, stats.PresentRefreshCount, stats.SyncRefreshCount);
    DestroyWindow(hwnd);
    return 0;
}
