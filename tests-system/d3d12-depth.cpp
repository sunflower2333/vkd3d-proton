// System D3D12 depth probe: near then far overlap, exact RGBA readback.
// Usage: d3d12-depth.exe <luid-high:low-hex> <d32|d16>
// The depth resource deliberately permits shader reads, matching the AIO DDI 0x12 path.
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "probe_identity.h"
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
    "VSOut vertex(uint id, float depth, float4 color) {\n"
    "    // Triangle covering the upper-left half of the target (x + y < 1 in uv).\n"
    "    float2 uv = float2(id == 1 ? 1.0 : 0.0, id == 2 ? 1.0 : 0.0);\n"
    "    VSOut o; o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);\n"
    "    o.color = color; return o;\n"
    "}\n"
    "VSOut vs_near(uint id : SV_VertexID) { return vertex(id, 0.25, float4(0, 1, 0, 1)); }\n"
    "VSOut vs_far(uint id : SV_VertexID) { return vertex(id, 0.75, float4(0, 0, 1, 1)); }\n"
    "float4 ps_main(VSOut i) : SV_Target { return i.color; }\n";

int main(int argc, char **argv) {
    if (argc != 3 || std::strlen(argv[1]) != 17 || argv[1][8] != ':' ||
            (std::strcmp(argv[2], "d32") && std::strcmp(argv[2], "d16"))) {
        std::puts("usage: d3d12-depth <8hex-high:8hex-low> <d32|d16>"); return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    for (unsigned i = 0; i < 17; ++i) if (i != 8 &&
            !((argv[1][i] >= '0' && argv[1][i] <= '9') ||
              (argv[1][i] >= 'a' && argv[1][i] <= 'f') ||
              (argv[1][i] >= 'A' && argv[1][i] <= 'F'))) return 2;
    const std::string high_text(argv[1], 8);
    unsigned low = 0, high = 0;
    CHECK(probe_parse_uint(high_text.c_str(), 16, 0, UINT_MAX, &high), "adapter LUID high");
    CHECK(probe_parse_uint(argv[1] + 9, 16, 0, UINT_MAX, &low), "adapter LUID low");
    const LUID luid{static_cast<DWORD>(low), static_cast<LONG>(high)};
    const DXGI_FORMAT depth_format = !std::strcmp(argv[2], "d32") ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D16_UNORM;
    std::printf("DEPTH format=%s shader_reads_allowed=1 near=0.25 far=0.75 clear=1 comparison=LESS write=ALL\n", argv[2]);
    CHECK(probe_system_module(L"d3d12.dll", "D3D12"), "System32 d3d12 identity");
    CHECK(probe_system_module(L"dxgi.dll", "DXGI"), "System32 dxgi identity");
    HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!compiler) return fail("LoadLibrary(d3dcompiler_47)", HRESULT_FROM_WIN32(GetLastError()));
    auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile"));
    if (!compile) return fail("GetProcAddress(D3DCompile)", E_NOINTERFACE);
    CHECK(probe_system_module(L"d3dcompiler_47.dll", "DXBC_COMPILER"), "System32 compiler identity");
    ComPtr<ID3DBlob> vs, vs_far, ps, errors;
    CHECK(compile(kShaders, std::strlen(kShaders), "depth.hlsl", nullptr, nullptr, "vs_near", "vs_5_0", 0, 0, &vs, &errors), "D3DCompile(vs_near)");
    CHECK(compile(kShaders, std::strlen(kShaders), "depth.hlsl", nullptr, nullptr, "vs_far", "vs_5_0", 0, 0, &vs_far, &errors), "D3DCompile(vs_far)");
    CHECK(compile(kShaders, std::strlen(kShaders), "tri.hlsl", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps, &errors), "D3DCompile(ps)");

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
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.DSVFormat = depth_format;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState");
    pd.VS = {vs_far->GetBufferPointer(), vs_far->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso_far;
    CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_far)), "CreateGraphicsPipelineState(far)");
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

    D3D12_RESOURCE_DESC dd = td; dd.Format = depth_format;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const auto allocation = device->GetResourceAllocationInfo(0, 1, &dd);
    std::printf("DEPTH allocation_size=%llu alignment=%llu\n",
        static_cast<unsigned long long>(allocation.SizeInBytes),
        static_cast<unsigned long long>(allocation.Alignment));
    if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX ||
            allocation.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
        return fail("depth allocation info", E_FAIL);
    D3D12_CLEAR_VALUE dc{}; dc.Format = depth_format; dc.DepthStencil.Depth = 1.0f;
    ComPtr<ID3D12Resource> depth;
    CHECK(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc, IID_PPV_ARGS(&depth)), "CreateCommittedResource(depth)");
    D3D12_DESCRIPTOR_HEAP_DESC dh{}; dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dh.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> dsvs;
    CHECK(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&dsvs)), "CreateDescriptorHeap(dsv)");
    const auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
    CHECK(device->GetDeviceRemovedReason(), "depth view creation");

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
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_VIEWPORT vp{0, 0, (float)W, (float)H, 0, 1}; D3D12_RECT sc{0, 0, (LONG)W, (LONG)H};
    list->RSSetViewports(1, &vp); list->RSSetScissorRects(1, &sc);
    list->SetGraphicsRootSignature(root.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0); // green near triangle writes depth
    list->SetPipelineState(pso_far.Get());
    list->DrawInstanced(3, 1, 0, 0); // blue far triangle must fail the depth test
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
    if (!event) return fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
    CHECK(queue->Signal(fence.Get(), 1), "Signal");
    CHECK(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion");
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) return fail("GPU timeout", E_FAIL);
    CHECK(device->GetDeviceRemovedReason(), "GetDeviceRemovedReason");
    if (fence->GetCompletedValue() != 1) return fail("completed fence value", E_FAIL);
    CloseHandle(event);
    std::puts("PASS execution");

    const unsigned char *pixels = nullptr;
    D3D12_RANGE range{0, pitch * H};
    CHECK(readback->Map(0, &range, (void **)&pixels), "Map(readback)");
    auto px = [&](UINT x, UINT y) { const unsigned char *p = pixels + y * pitch + x * 4; return (unsigned)p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; };
    const unsigned drawn = 0u | 255u << 8 | 0u << 16 | 255u << 24, cleared = 255u | 0u << 8 | 0u << 16 | 255u << 24;
    unsigned bad = 0, inside = 0, outside = 0;
    for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) {
        const bool in = x + y + 1 < W;        // clearly inside the triangle
        const bool out = x + y > W;           // clearly outside
        const unsigned v = px(x, y);
        if (in) { ++inside; if (v != drawn) { if (bad++ < 4) std::printf("MISMATCH in (%u,%u)=%08x\n", x, y, v); } }
        if (out) { ++outside; if (v != cleared) { if (bad++ < 8) std::printf("MISMATCH out (%u,%u)=%08x\n", x, y, v); } }
    }
    std::printf("PIXELS corner(0,0)=%08x corner(63,63)=%08x inside=%u outside=%u bad=%u\n", px(0, 0), px(63, 63), inside, outside, bad);
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    if (bad) return fail("pixel verification", E_FAIL);
    std::printf("PASS D3D12 DEPTH format=%s near-then-far+clear+draw+copy checked=%u\n", argv[2], inside + outside);
    return 0;
}
