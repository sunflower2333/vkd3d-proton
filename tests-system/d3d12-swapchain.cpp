// System D3D12 swapchain probe: flip-model swapchain on a real window, clear + Present N frames.
// Usage: d3d12-swapchain.exe <luid-low-hex> [frames] [interval] [width] [height]
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
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
    if (argc < 2) { std::puts("usage: d3d12-swapchain <luid-low-hex> [frames] [interval] [w] [h]"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const LUID luid{static_cast<DWORD>(std::strtoul(argv[1], nullptr, 16)), 0};
    const int frames = argc > 2 ? std::atoi(argv[2]) : 300;
    const UINT interval = argc > 3 ? std::atoi(argv[3]) : 1;
    const UINT W = argc > 4 ? std::atoi(argv[4]) : 640, H = argc > 5 ? std::atoi(argv[5]) : 480;

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
    ComPtr<ID3D12Device> device;
    CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
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
    UINT64 values[kBuffers]{}, next = 1;

    LARGE_INTEGER freq, t0, t1; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    double worst = 0; LARGE_INTEGER prev = t0;
    for (int f = 0; f < frames; ++f) {
        pump();
        const UINT i = swap->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < values[i]) {
            CHECK(fence->SetEventOnCompletion(values[i], event), "SetEventOnCompletion");
            if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) return fail("fence wait timeout", E_FAIL);
        }
        CHECK(allocators[i]->Reset(), "allocator Reset");
        CHECK(list->Reset(allocators[i].Get(), nullptr), "list Reset");
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = buffers[i].Get(); b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvs->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * inc;
        const float phase = (f % 120) / 119.0f;
        const float color[4] = {phase, 0.3f, 1.0f - phase, 1.0f};
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
        const HRESULT phr = swap->Present(interval, 0);
        if (FAILED(phr)) {
            std::printf("frame %d ", f);
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
    WaitForSingleObject(event, 5000);
    QueryPerformanceCounter(&t1);
    const double secs = double(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
    DXGI_FRAME_STATISTICS stats{};
    const HRESULT shr = swap->GetFrameStatistics(&stats);
    std::printf("PASS D3D12 SWAPCHAIN frames=%d secs=%.3f fps=%.1f worst=%.2fms stats_hr=%08lx PresentCount=%u PresentRefreshCount=%u SyncRefreshCount=%u\n",
        frames, secs, frames / secs, worst, (unsigned long)shr, stats.PresentCount, stats.PresentRefreshCount, stats.SyncRefreshCount);
    DestroyWindow(hwnd);
    return 0;
}
