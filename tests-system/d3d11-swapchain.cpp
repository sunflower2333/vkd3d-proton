// D3D11 baseline for d3d12-swapchain: same window, flip-discard swapchain, clear + Present N frames.
// Usage: d3d11-swapchain.exe <luid-low-hex> [frames] [interval] [w] [h]
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
static int fail(const char *s, HRESULT hr) { std::printf("FAIL %s hr=%08lx\n", s, (unsigned long)hr); return 1; }
#define CHECK(e, s) do { HRESULT h_ = (e); if (FAILED(h_)) return fail(s, h_); } while (0)
static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(w, m, wp, lp); }
static void pump() { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);
    const LUID luid{static_cast<DWORD>(std::strtoul(argv[1], nullptr, 16)), 0};
    const int frames = argc > 2 ? std::atoi(argv[2]) : 300;
    const UINT interval = argc > 3 ? std::atoi(argv[3]) : 1;
    const UINT W = argc > 4 ? std::atoi(argv[4]) : 640, H = argc > 5 ? std::atoi(argv[5]) : 480;
    WNDCLASSW wc{}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"d3d11swapchain";
    RegisterClassW(&wc);
    RECT rc{0, 0, (LONG)W, (LONG)H}; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"D3D11 swapchain probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100, 100, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    pump();
    ComPtr<IDXGIFactory4> factory; CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter1> adapter; CHECK(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "adapter");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    CHECK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &device, nullptr, &ctx), "D3D11CreateDevice");
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap; CHECK(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &swap), "swapchain");
    ComPtr<ID3D11Texture2D> back; CHECK(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
    ComPtr<ID3D11RenderTargetView> rtv; CHECK(device->CreateRenderTargetView(back.Get(), nullptr, &rtv), "rtv");
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (int i = 0; i < frames; ++i) {
        pump();
        const float p = (i % 120) / 119.0f; const float c[4] = {p, 0.3f, 1 - p, 1};
        ctx->ClearRenderTargetView(rtv.Get(), c);
        CHECK(swap->Present(interval, 0), "Present");
    }
    QueryPerformanceCounter(&t1);
    const double secs = double(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    std::printf("PASS D3D11 SWAPCHAIN frames=%d secs=%.3f fps=%.1f\n", frames, secs, frames / secs);
    return 0;
}
