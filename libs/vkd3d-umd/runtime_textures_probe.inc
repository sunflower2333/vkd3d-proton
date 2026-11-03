/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../tests/shaders/descriptors/headers/sampler_rounding.h"

struct ProbeBound {
    Object slot{};
    ~ProbeBound() { reset(); }
    void reset() { VioGpuD3D12BridgeUnbindObject(&slot); }
    void attach(Context *ctx, vkdu_object *value, vkdu_kind kind) {
        require(vkdu_object_retain(value) != 0, "retain probe native binding");
        HRESULT hr = bind(ctx, &slot, value, kind);
        if (FAILED(hr)) vkdu_object_destroy(value);
        check(hr, "bind probe native object");
    }
};
struct ProbeTexture {
    NativeSession &native;
    NativeHeapSlot heap_slot{};
    Object resource_slot{};
    D3D12DDI_HHEAP heap{};
    D3D12DDI_HRESOURCE resource{};
    explicit ProbeTexture(NativeSession &s) : native(s) {}
    ~ProbeTexture() {
        if (heap.pDrvPrivate || resource.pDrvPrivate)
            native.table.pfnDestroyHeapAndResource(native.create.hDrvDevice, heap, resource);
    }
    void create(DXGI_FORMAT format, bool rt = false, UINT width = 4, UINT height = 4) {
        auto *ctx = context(native.create.hDrvDevice);
        uint64_t bytes = 0, alignment = 0;
        check(rt ? vkdu_render_target_allocation(ctx->backend, width, height, &bytes, &alignment) :
            vkdu_texture2d_allocation(ctx->backend, width, height, format, &bytes, &alignment), "actual image allocation requirements");
        D3D12DDIARG_CREATEHEAP_0001 h{};
        h.ByteSize = bytes; h.Alignment = alignment; h.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
        h.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        h.Flags = rt ? D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES : D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
        h.CreationNodeMask = h.VisibleNodeMask = 1;
        D3D12DDIARG_CREATERESOURCE_0003 r{};
        r.ResourceType = D3D12DDI_RT_TEXTURE2D; r.Width = width; r.Height = height; r.Format = format;
        r.DepthOrArraySize = r.MipLevels = r.SampleDesc.Count = 1;
        r.Flags = static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(rt ? 1 : 0);
        r.InitialResourceState = rt ? D3D12DDI_RESOURCE_STATE_RENDER_TARGET : D3D12DDI_RESOURCE_STATE_COPY_DEST;
        check(native.table.pfnCreateHeapAndResource(native.create.hDrvDevice, &h, {&heap_slot}, {}, &r,
                nullptr, {&resource_slot}), "native paired texture/private import");
        heap = {&heap_slot}; resource = {&resource_slot};
    }
};
uint32_t texture_pixel(DXGI_FORMAT format, unsigned round, unsigned x, unsigned y) {
    const unsigned n = (round + x + y * 2) & 3;
    if (format == DXGI_FORMAT_R32_FLOAT) {
        const float value = static_cast<float>(n) * 0.25f;
        uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); return bits;
    }
    return 0xff000000u | ((round + 1) << 16) | ((x + y * 4) << 8) | (n & 1 ? 255 : 0);
}
void check_shared_textures(NativeSession &native, ProbeRuntime &runtime) {
    auto *ctx = context(native.create.hDrvDevice);
    auto h = native.create.hDrvDevice;
    auto *readback = object(native.resource.pDrvPrivate);
    for (auto format : {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM}) {
        ProbeTexture texture(native);
        const unsigned before_texture = runtime.creates;
        texture.create(format);
        auto *token = native_heap_find(ctx, texture.heap.pDrvPrivate);
        require(token && token->allocation && token->users == 2 && !texture.resource_slot.address &&
                runtime.creates == before_texture + 1, "one runtime allocation backs the placed texture");
        const auto target = token->allocation; runtime.target_handle = target;
        Owned alias, upload, output, queue, allocator, command, fence, pipeline;
        const unsigned before_alias = runtime.creates;
        check(vkdu_texture_heap_import(ctx->backend, ctx, token, token->bytes, &alias.value), "second exact texture backing import");
        require(runtime.creates == before_alias, "texture alias imports without allocating replacement storage");
        check(vkdu_buffer_create(ctx->backend, 4096, 2, 0, 0xac3, &upload.value), "texture upload");
        check(vkdu_buffer_create(ctx->backend, 4096, 1, 4, 0x8, &output.value), "sample output");
        check(vkdu_queue_create(ctx->backend, 0, &queue.value), "texture queue");
        check(vkdu_allocator_create(ctx->backend, 0, &allocator.value), "texture allocator");
        check(vkdu_command_create(ctx->backend, allocator.value, 0, &command.value), "texture command");
        check(vkdu_fence_create(ctx->backend, 0, &fence.value), "texture fence");
        ProbeBound upload_slot, command_slot, srv_heap, root;
        upload_slot.attach(ctx, upload.value, VKDU_BUFFER); command_slot.attach(ctx, command.value, VKDU_COMMAND_LIST);
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_desc{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
            D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE | D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 1};
        check(native.table.pfnCreateDescriptorHeap(h, &heap_desc, {&srv_heap.slot}), "native texture descriptor heap");
        const auto stride = native.table.pfnGetDescriptorSizeInBytes(h, D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto descriptor = native.table.pfnGetCPUDescriptorHandleForHeapStart(h, {&srv_heap.slot});
        descriptor.ptr += stride * 3;
        D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 view{};
        view.hDrvResource = texture.resource; view.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
        view.Format = format; view.Shader4ComponentMapping = 5768; view.Tex2D.MipLevels = view.Tex2D.ArraySize = 1;
        native.table.pfnCreateShaderResourceView(h, &view, descriptor);
        check(VioGpuD3D12BridgeStatus(h), "native imported texture SRV");
        D3D12DDI_DESCRIPTOR_RANGE range{D3D12DDI_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 1};
        D3D12DDI_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = 3;
        parameters[1].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_UAV;
        parameters[2].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2].DescriptorTable = {1, &range};
        D3D12DDI_STATIC_SAMPLER sampler{};
        sampler.Filter = D3D12DDI_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12DDI_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12DDI_COMPARISON_FUNC_ALWAYS; sampler.MaxAnisotropy = 1; sampler.MaxLOD = 1000;
        D3D12DDI_ROOT_SIGNATURE signature{};
        signature.NumParameters = 3; signature.pRootParameters = parameters;
        signature.NumStaticSamplers = 1; signature.pStaticSamplers = &sampler;
        D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_desc{&signature, 1};
        check(native.table.pfnCreateRootSignature(h, &root_desc, {&root.slot}), "native sample root/static sampler");
        check(vkdu_pipeline_create(ctx->backend, root.slot.backend, sampler_rounding_code_dxbc,
                sizeof(sampler_rounding_code_dxbc), &pipeline.value), "native texture SampleLevel pipeline");
        std::printf("TEXTURE format=%u context=%u allocation=%u bytes=%llu\n", format,
                ctx->native_context_id, target, static_cast<unsigned long long>(token->bytes));
        for (uint32_t round = 0; round < 4; ++round) {
            const uint32_t sentinel = 0xdead0000u ^ (round * 0x10101u) ^ format;
            void *mapping = nullptr;
            check(native.table.pfnMapHeap(h, native.heap, &mapping), "native texture readback sentinel");
            auto *words = static_cast<uint32_t *>(mapping);
            for (unsigned i = 0; i < 16384; ++i) words[i] = sentinel;
            native.table.pfnUnmapHeap(h, native.heap);
            check(vkdu_buffer_map(upload.value, 0, 0, &mapping), "map changing texture upload");
            words = static_cast<uint32_t *>(mapping);
            for (unsigned i = 0; i < 1024; ++i) words[i] = 0xcccccccc;
            for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
                words[128 + y * 64 + x] = texture_pixel(format, round, x, y);
            check(vkdu_buffer_unmap(upload.value, 0, 4096), "unmap changing texture upload");
            if (round) {
                check(vkdu_allocator_reset(allocator.value), "texture allocator reset");
                check(vkdu_command_reset(command.value, allocator.value), "texture command reset");
                check(vkdu_command_transition(command.value, texture.resource_slot.backend, 0x800, 0x400), "texture copy-dest transition");
                check(vkdu_command_transition(command.value, output.value, 0x800, 0x8), "sample UAV transition");
            }
            D3D12DDIARG_BUFFER_PLACEMENT image{}, source{}, destination{};
            image.BaseAddress.UMD.hResource = texture.resource;
            source.BaseAddress.UMD.hResource = {&upload_slot.slot}; source.BaseAddress.UMD.Offset = 512;
            destination.BaseAddress.UMD.hResource = native.resource; destination.BaseAddress.UMD.Offset = 512;
            D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT pitched{};
            pitched.Format = format; pitched.PhysicalWidth = pitched.PhysicalHeight = 4;
            pitched.PhysicalDepth = 1; pitched.Pitch = 256; pitched.SlicePitch = 1024;
            D3D12DDIARG_PLACED_RESOURCE image_layout{}, buffer_layout{};
            image_layout.Layout = D3D12DDI_RL_SELECT_SUBRESOURCE;
            buffer_layout.Layout = D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED; buffer_layout.pLayout = &pitched;
            const D3D12DDI_HCOMMANDLIST cmd{&command_slot.slot};
            native.commands.pfnCopyTextureRegion(cmd, &image, image_layout, 0, 0, 0, &source, buffer_layout, nullptr);
            D3D12DDI_BOX box{1, 1, 0, 3, 3, 1};
            native.commands.pfnCopyTextureRegion(cmd, &image, image_layout, 2, 2, 0, &source, buffer_layout, &box);
            check(VioGpuD3D12BridgeStatus(h), "native texture upload and boxed replacement");
            check(vkdu_command_transition(command.value, texture.resource_slot.backend, 0x400, 0x40), "texture sampling transition");
            check(vkdu_command_root(command.value, root.slot.backend), "sample root bind");
            check(vkdu_command_pipeline(command.value, pipeline.value), "sample pipeline bind");
            const float uv[3] = {0.625f, 0.625f, 0}; uint32_t constants[3]; std::memcpy(constants, uv, sizeof(constants));
            check(vkdu_command_constants(command.value, 0, 0, 3, constants), "sample coordinates");
            check(vkdu_command_uav(command.value, 1, output.value, 0), "sample output bind");
            check(vkdu_command_heaps(command.value, 1, &srv_heap.slot.backend), "sample SRV heap bind");
            check(vkdu_command_table(command.value, 2, srv_heap.slot.backend, 2), "sample nonzero table offset");
            check(vkdu_command_dispatch(command.value, 16, 1, 1), "sample imported texture");
            check(vkdu_command_transition(command.value, texture.resource_slot.backend, 0x40, 0x800), "texture readback transition");
            check(vkdu_command_transition(command.value, output.value, 0x8, 0x800), "sample readback transition");
            native.commands.pfnCopyTextureRegion(cmd, &destination, buffer_layout, 0, 0, 0, &image, image_layout, nullptr);
            check(vkdu_command_copy(command.value, readback->backend, 4096, output.value, 0, 64), "sample native readback");
            check(VioGpuD3D12BridgeStatus(h), "native CopyTextureRegion readback");
            const unsigned before_render = runtime.target_references;
            check(vkdu_command_close(command.value), "texture command close");
            check(vkdu_queue_execute(queue.value, 1, &command.value), "texture queue execute");
            check(vkdu_queue_signal(queue.value, fence.value, round + 1), "texture queue signal");
            check(vkdu_fence_wait(fence.value, round + 1, 10000), "texture GPU completion");
            require(runtime.target_references > before_render, "RenderCb contains exact native texture allocation");
            check(native.table.pfnMapHeap(h, native.heap, &mapping), "native texture results");
            words = static_cast<uint32_t *>(mapping); unsigned mismatches = 0;
            const uint32_t sample = texture_pixel(format, round, 1, 1);
            const uint32_t expected_sample = format == DXGI_FORMAT_R32_FLOAT ? sample : (sample & 255 ? 0x3f800000 : 0);
            for (unsigned i = 0; i < 16384; ++i) {
                uint32_t expected = sentinel;
                if (i >= 128 && i < 384 && (i - 128) % 64 < 4) {
                    const unsigned y = (i - 128) / 64, x = (i - 128) % 64;
                    expected = texture_pixel(format, round, x >= 2 && y >= 2 ? x - 1 : x, x >= 2 && y >= 2 ? y - 1 : y);
                } else if (i >= 1024 && i < 1040) expected = expected_sample;
                if (words[i] != expected && mismatches++ < 4)
                    std::fprintf(stderr, "texture format%u round%u word%u=%08x expected%08x\n", format, round, i, words[i], expected);
            }
            native.table.pfnUnmapHeap(h, native.heap);
            require(!mismatches, "native imported texture copy, sampled values and sentinel preservation");
            std::printf("PASS native texture format=%u round=%u texels=16 samples=16 checked_words=16384\n", format, round);
        }
        // Release descriptor/recorded-command uses before testing exact import ownership.
        command_slot.reset(); command.reset(); allocator.reset(); queue.reset(); fence.reset();
        pipeline.reset(); root.reset(); srv_heap.reset(); upload_slot.reset(); upload.reset(); output.reset();
        native.table.pfnDestroyHeapAndResource(h, texture.heap, {}); texture.heap = {};
        require(runtime.has(target), "native texture resource retains destroyed heap slot backing");
        native.table.pfnDestroyHeapAndResource(h, {}, texture.resource); texture.resource = {};
        require(runtime.has(target), "texture alias retains destroyed resource backing");
        alias.reset(); require(!runtime.has(target), "last texture import releases actual runtime allocation");
        check(VioGpuD3D12BridgeStatus(h), "native texture final-owner cleanup");
    }
}
void run_texture_probe(LUID luid) {
    Deadline deadline;
    ProbeRuntime runtime; runtime.open(luid);
    NativeSession native; native.initialize(runtime); native.create_readback();
    check_shared_textures(native, runtime);
    runtime.native_cleanup = true; native.finish();
    require(!runtime.context_handle, "texture native context closed");
    { std::lock_guard<std::mutex> lock(runtime.mutex); require(runtime.allocations.empty(), "texture all allocations released"); }
    check(runtime.close(), "texture final KMT owner cleanup");
    std::printf("PASS native texture shared backing R32_FLOAT/RGBA8:8x16384 checked words; renders=%u allocations=0\n", runtime.renders.load());
    std::puts("BOUNDARY emulated runtime callbacks + real KMT/GPU; NOT Microsoft D3D12CreateDevice or graphics acceptance");
}
