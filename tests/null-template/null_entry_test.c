/* Reuse the inherited CBV/SRV readback oracles byte-for-byte. */
#define main inherited_full_backend_main
#include "../../libs/vkd3d-umd/backend_test.c"
#undef main
#include "vkd3d_private.h"

static void check_null_entries(ID3D12Device *device)
{
    ID3D12DescriptorHeap *cpu = NULL, *gpu = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, 0, 0};
    D3D12_CPU_DESCRIPTOR_HANDLE src, dst;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {0, 256};
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {0};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {0};
    unsigned i, repeat, stride;
    CHECK(ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&cpu));
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK(ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&gpu));
    src = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu);
    dst = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(gpu);
    stride = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (i = 0; i < 7; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE s = {src.ptr + i * stride}, d = {dst.ptr + i * stride};
        for (repeat = 0; repeat < 2; ++repeat)
        {
            memset(&srv, 0, sizeof(srv)); memset(&uav, 0, sizeof(uav));
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = uav.Format = DXGI_FORMAT_R32_UINT;
            if (i == 0)
                ID3D12Device_CreateConstantBufferView(device, &cbv, s);
            else if (i == 1)
            {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
                ID3D12Device_CreateShaderResourceView(device, NULL, &srv, s);
            }
            else if (i == 2)
            {
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                ID3D12Device_CreateUnorderedAccessView(device, NULL, NULL, &uav, s);
            }
            else if (i == 3 || i == 5)
            {
                srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; srv.Buffer.NumElements = 16;
                if (i == 5) { srv.Format = DXGI_FORMAT_R32_TYPELESS; srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW; }
                ID3D12Device_CreateShaderResourceView(device, NULL, &srv, s);
            }
            else
            {
                uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uav.Buffer.NumElements = 16;
                if (i == 6) { uav.Format = DXGI_FORMAT_R32_TYPELESS; uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW; }
                ID3D12Device_CreateUnorderedAccessView(device, NULL, NULL, &uav, s);
            }
        }
        ID3D12Device_CopyDescriptorsSimple(device, 1, d, s, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    ID3D12DescriptorHeap_Release(gpu); ID3D12DescriptorHeap_Release(cpu);
    puts("PASS actual CBV, sampled/storage image, typed/raw SRV/UAV null writes, repeated writes and CPU to GPU copies: 7 kinds");
}

int main(int argc, char **argv)
{
    void *module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    PFN_vkGetInstanceProcAddr loader = module ? (PFN_vkGetInstanceProcAddr)dlsym(module, "vkGetInstanceProcAddr") : NULL;
    vkdu_device *device = NULL, *other = NULL;
    struct vkdu_adapter absent = {{0}, 0xffffffff, 0xffffffff};
    vkdu_object *allocator = NULL, *command = NULL;
    ID3D12Device *raw_device;
    struct d3d12_device *implementation;
    if (!loader || argc != 2 || (strcmp(argv[1], "entries") && strcmp(argv[1], "readbacks"))) return 2;
    REJECT(vkdu_device_create(loader, &absent, &other));
    if (other) return 1;
    CHECK(vkdu_test_device_create(loader, &device));
    CHECK(vkdu_allocator_create(device, 0, &allocator));
    CHECK(vkdu_command_create(device, allocator, 0, &command));
    CHECK(ID3D12GraphicsCommandList_GetDevice((ID3D12GraphicsCommandList *)vkdu_test_command_iface(command),
            &IID_ID3D12Device, (void **)&raw_device));
    implementation = unsafe_impl_from_ID3D12Device((d3d12_device_iface *)raw_device);
    printf("RUNTIME mutable=%u split=%u descriptor_buffer=%u embedded=%u\n",
            !!(implementation->bindless_state.flags & VKD3D_BINDLESS_MUTABLE_TYPE),
            !!(implementation->bindless_state.flags & VKD3D_BINDLESS_MUTABLE_TYPE_SPLIT_RAW_TYPED),
            d3d12_device_uses_descriptor_buffers(implementation),
            d3d12_device_use_embedded_mutable_descriptors(implementation));
    if (!strcmp(argv[1], "entries"))
        check_null_entries(raw_device);
    else
    {
        CHECK(vkdu_test_device_create(loader, &other));
        check_constant_buffers(device, other);
        check_shader_resources(device, other);
        CHECK(vkdu_device_status(other)); vkdu_device_destroy(other);
    }
    ID3D12Device_Release(raw_device);
    vkdu_object_destroy(command); vkdu_object_destroy(allocator);
    CHECK(vkdu_device_status(device)); vkdu_device_destroy(device);
    dlclose(module);
    return 0;
}
