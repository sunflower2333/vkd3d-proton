// System D3D12 DXIL (SM6) graphics probe: same clear + draw + copy check as d3d12-triangle,
// with shaders compiled by DXC (vs_6_0/ps_6_0).
// Usage: d3d12-dxil.exe <luid-low-hex> [dxcompiler.dll path] [shader model, e.g. 6_0]
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <string>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using Microsoft::WRL::ComPtr;

static int fail(const char *stage, HRESULT hr) {
    std::printf("FAIL %s hr=%08lx\n", stage, (unsigned long)hr);
    return 1;
}
#define CHECK(expr, stage) do { HRESULT hr_ = (expr); if (FAILED(hr_)) return fail(stage, hr_); } while (0)

static const char *kShaders =
    "struct VSOut { float4 pos : SV_Position; float4 color : COLOR0; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    // Triangle covering the upper-left half of the target (x + y < 1 in uv).\n"
    "    float2 uv = float2(id == 1 ? 1.0 : 0.0, id == 2 ? 1.0 : 0.0);\n"
    "    VSOut o; o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);\n"
    "    o.color = float4(0.25, 0.5, 0.75, 1.0); return o;\n"
    "}\n"
    "float4 ps_main(VSOut i) : SV_Target { return i.color; }\n";

static HRESULT dxc_compile(const wchar_t *dll, const char *source, const wchar_t *entry, const wchar_t *target,
        ComPtr<IDxcBlob> &out) {
    HMODULE module = LoadLibraryW(dll);
    if (!module) return HRESULT_FROM_WIN32(GetLastError());
    auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
    if (!create) return E_NOINTERFACE;
    ComPtr<IDxcCompiler3> compiler;
    HRESULT hr = create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void **)&compiler);
    if (FAILED(hr)) return hr;
    DxcBuffer buffer{source, std::strlen(source), DXC_CP_UTF8};
    LPCWSTR args[] = {L"-E", entry, L"-T", target};
    ComPtr<IDxcResult> result;
    hr = compiler->Compile(&buffer, args, 4, nullptr, __uuidof(IDxcResult), (void **)&result);
    if (FAILED(hr)) return hr;
    HRESULT status = S_OK; result->GetStatus(&status);
    ComPtr<IDxcBlobEncoding> errors;
    if (SUCCEEDED(result->GetErrorBuffer(&errors)) && errors && errors->GetBufferSize())
        std::printf("DXC: %.*s\n", (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
    if (FAILED(status)) return status;
    return result->GetResult(&out);
}

int main(int argc, char **argv) {
    if (argc < 2) { std::puts("usage: d3d12-dxil <luid-low-hex> [dxcompiler.dll] [6_0]"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const LUID luid{static_cast<DWORD>(std::strtoul(argv[1], nullptr, 16)), 0};
    std::wstring dll = L"C:\\Program Files (x86)\\Microsoft\\EdgeCore\\156.0.4314.58\\dxcompiler.dll";
    if (argc > 2) { std::string a = argv[2]; dll.assign(a.begin(), a.end()); }
    std::wstring model = L"6_0";
    if (argc > 3) { std::string a = argv[3]; model.assign(a.begin(), a.end()); }
    const std::wstring vs_target = L"vs_" + model, ps_target = L"ps_" + model;
    ComPtr<IDxcBlob> vs, ps;
    ComPtr<ID3DBlob> errors;
    CHECK(dxc_compile(dll.c_str(), kShaders, L"vs_main", vs_target.c_str(), vs), "DXC(vs)");
    CHECK(dxc_compile(dll.c_str(), kShaders, L"ps_main", ps_target.c_str(), ps), "DXC(ps)");
    std::printf("PASS dxc vs=%zu ps=%zu bytes\n", (size_t)vs->GetBufferSize(), (size_t)ps->GetBufferSize());

    ComPtr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> adapter;
    CHECK(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid");
    ComPtr<ID3D12Device> device;
    CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    std::puts("PASS device");
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_7};
    const HRESULT smhr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    std::printf("SHADER_MODEL hr=%08lx highest=%x\n", (unsigned long)smhr, (unsigned)sm.HighestShaderModel);

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12CommandAllocator> allocator;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");

    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> rsblob;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsblob, &errors), "D3D12SerializeRootSignature");
    ComPtr<ID3D12RootSignature> root;
    CHECK(device->CreateRootSignature(0, rsblob->GetBufferPointer(), rsblob->GetBufferSize(), IID_PPV_ARGS(&root)), "CreateRootSignature");
    std::puts("PASS root signature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState");
    std::puts("PASS pipeline");

    const UINT W = 64, H = 64;
    D3D12_HEAP_PROPERTIES def{}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = W; td.Height = H;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{}; cv.Format = td.Format;
    ComPtr<ID3D12Resource> target;
    CHECK(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&target)), "CreateCommittedResource(rt)");
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvs; CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)), "CreateDescriptorHeap(rtv)");
    const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    const UINT pitch = 256;
    D3D12_HEAP_PROPERTIES rb{}; rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = pitch * H; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    CHECK(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "CreateCommittedResource(readback)");
    std::puts("PASS resources");

    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)), "CreateCommandList");
    const float clear[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_VIEWPORT vp{0, 0, (float)W, (float)H, 0, 1}; D3D12_RECT sc{0, 0, (LONG)W, (LONG)H};
    list->RSSetViewports(1, &vp); list->RSSetScissorRects(1, &sc);
    list->SetGraphicsRootSignature(root.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = target.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, pitch};
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = target.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    CHECK(list->Close(), "Close");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence; CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(queue->Signal(fence.Get(), 1), "Signal");
    CHECK(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion");
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) return fail("GPU timeout", E_FAIL);
    CHECK(device->GetDeviceRemovedReason(), "GetDeviceRemovedReason");
    std::puts("PASS execution");

    const unsigned char *pixels = nullptr;
    D3D12_RANGE range{0, pitch * H};
    CHECK(readback->Map(0, &range, (void **)&pixels), "Map(readback)");
    auto px = [&](UINT x, UINT y) { const unsigned char *p = pixels + y * pitch + x * 4; return (unsigned)p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; };
    const unsigned drawn = 64u | 128u << 8 | 191u << 16 | 255u << 24, cleared = 255u | 0u << 8 | 0u << 16 | 255u << 24;
    unsigned bad = 0, inside = 0, outside = 0;
    for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) {
        const bool in = x + y + 1 < W;        // clearly inside the triangle
        const bool out = x + y > W;           // clearly outside
        const unsigned v = px(x, y);
        if (in) { ++inside; if (v != drawn && v != (63u | 128u << 8 | 191u << 16 | 255u << 24) && v != (64u | 128u << 8 | 192u << 16 | 255u << 24)) { if (bad++ < 4) std::printf("MISMATCH in (%u,%u)=%08x\n", x, y, v); } }
        if (out) { ++outside; if (v != cleared) { if (bad++ < 8) std::printf("MISMATCH out (%u,%u)=%08x\n", x, y, v); } }
    }
    std::printf("PIXELS corner(0,0)=%08x corner(63,63)=%08x inside=%u outside=%u bad=%u\n", px(0, 0), px(63, 63), inside, outside, bad);
    readback->Unmap(0, nullptr);
    if (bad) return fail("pixel verification", E_FAIL);
    std::puts("PASS D3D12 DXIL TRIANGLE clear+draw+copy");
    return 0;
}
