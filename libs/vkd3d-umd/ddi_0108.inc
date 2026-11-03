/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* D3D12 DDI 0108 tables (interface R8) layered on the R0 implementation.
 *
 * The Windows 11 runtime binds command lists to its newest command-list table
 * layout (0108); R0 drivers get a device-removed stub table there. This layer
 * publishes 0108 tables:
 *  - entries whose function type is unchanged since R0 are copied,
 *  - entries whose argument revision changed get adapters that translate the
 *    newer structures into the R0 ones (dropping only data the R0 path cannot
 *    express, which our caps never enable),
 *  - the 0040 command model (pools + recorders instead of allocators) is
 *    implemented on top of per-queue-type R0 allocators,
 *  - entries for features we do not advertise get ABI-correct stubs.
 * Included by runtime.inc inside the anonymous namespace. */

D3D12DDI_DEVICE_FUNCS_CORE_0003 r0_device{};
D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 r0_commands{};

template<typename F> struct DdiStub;
template<typename R, typename... A> struct DdiStub<R (APIENTRY *)(A...)> {
    static R APIENTRY call(A...) {
        native_trace("unimplemented 0108 DDI %s", __PRETTY_FUNCTION__);
        if constexpr (std::is_same_v<R, HRESULT>) return E_NOTIMPL;
        else if constexpr (std::is_void_v<R>) return;
        else return R{};
    }
};
// Command-list entries for features we do not advertise: record the error on
// the list (its Close then fails) instead of silently dropping GPU work.
template<typename F> struct DdiListStub;
template<typename... A> struct DdiListStub<VOID (APIENTRY *)(D3D12DDI_HCOMMANDLIST, A...)> {
    static VOID APIENTRY call(D3D12DDI_HCOMMANDLIST list, A...) {
        native_trace("unimplemented 0108 command-list DDI %s", __PRETTY_FUNCTION__);
        if (auto *value = object(list.pDrvPrivate)) error(value, E_NOTIMPL);
    }
};
template<typename F> struct DdiNoop;
template<typename... A> struct DdiNoop<VOID (APIENTRY *)(A...)> {
    static VOID APIENTRY call(A...) {}
};
#define DDI_STUB(table, field) (table)->field = &DdiStub<decltype((table)->field)>::call
#define DDI_LIST_STUB(table, field) (table)->field = &DdiListStub<decltype((table)->field)>::call
#define DDI_NOOP(table, field) (table)->field = &DdiNoop<decltype((table)->field)>::call

/* ---- pipeline state objects ------------------------------------------- */
SIZE_T APIENTRY d0108_element_layout_size(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEELEMENTLAYOUT_0010 *args) {
    return r0_device.pfnCalcPrivateElementLayoutSize(h, reinterpret_cast<const D3D12DDIARG_CREATEELEMENTLAYOUT *>(args));
}
VOID APIENTRY d0108_element_layout(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEELEMENTLAYOUT_0010 *args, D3D12DDI_HELEMENTLAYOUT slot) {
    // 0010 appends LibraryReference to the R0 layout.
    r0_device.pfnCreateElementLayout(h, reinterpret_cast<const D3D12DDIARG_CREATEELEMENTLAYOUT *>(args), slot);
}
SIZE_T APIENTRY d0108_blend_size(D3D12DDI_HDEVICE h, const D3D12DDI_BLEND_DESC_0010 *desc) {
    return r0_device.pfnCalcPrivateBlendStateSize(h, reinterpret_cast<const D3D12DDI_BLEND_DESC *>(desc));
}
VOID APIENTRY d0108_blend(D3D12DDI_HDEVICE h, const D3D12DDI_BLEND_DESC_0010 *desc, D3D12DDI_HBLENDSTATE slot) {
    r0_device.pfnCreateBlendState(h, reinterpret_cast<const D3D12DDI_BLEND_DESC *>(desc), slot);
}
SIZE_T APIENTRY d0108_depth_size(D3D12DDI_HDEVICE h, const D3D12DDI_DEPTH_STENCIL_DESC_0095 *desc) {
    return r0_device.pfnCalcPrivateDepthStencilStateSize(h, reinterpret_cast<const D3D12DDI_DEPTH_STENCIL_DESC *>(desc));
}
VOID APIENTRY d0108_depth(D3D12DDI_HDEVICE h, const D3D12DDI_DEPTH_STENCIL_DESC_0095 *desc, D3D12DDI_HDEPTHSTENCILSTATE slot) {
    // The R0 fields are a prefix (front masks); separate back-face masks and
    // depth bounds are not advertised.
    r0_device.pfnCreateDepthStencilState(h, reinterpret_cast<const D3D12DDI_DEPTH_STENCIL_DESC *>(desc), slot);
}
D3D12DDI_RASTERIZER_DESC d0108_rasterizer_r0(const D3D12DDI_RASTERIZER_DESC_0102 &d) {
    D3D12DDI_RASTERIZER_DESC r{};
    r.FillMode = d.FillMode; r.CullMode = d.CullMode; r.FrontCounterClockwise = d.FrontCounterClockwise;
    r.DepthBias = static_cast<INT>(d.DepthBias); r.DepthBiasClamp = d.DepthBiasClamp;
    r.SlopeScaledDepthBias = d.SlopeScaledDepthBias; r.DepthClipEnable = d.DepthClipEnable;
    r.ScissorEnable = d.ScissorEnable;
    r.MultisampleEnable = d.LineRasterizationMode == D3D12DDI_LINE_RASTERIZATION_MODE_QUADRILATERAL_WIDE ||
        d.LineRasterizationMode == D3D12DDI_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW;
    r.AntialiasedLineEnable = d.LineRasterizationMode == D3D12DDI_LINE_RASTERIZATION_MODE_ALPHA_ANTIALIASED;
    r.ForcedSampleCount = d.ForcedSampleCount; r.ConservativeRasterizationMode = d.ConservativeRasterizationMode;
    return r;
}
SIZE_T APIENTRY d0108_rasterizer_size(D3D12DDI_HDEVICE h, const D3D12DDI_RASTERIZER_DESC_0102 *desc) {
    if (!desc) return r0_device.pfnCalcPrivateRasterizerStateSize(h, nullptr);
    const auto r = d0108_rasterizer_r0(*desc);
    return r0_device.pfnCalcPrivateRasterizerStateSize(h, &r);
}
VOID APIENTRY d0108_rasterizer(D3D12DDI_HDEVICE h, const D3D12DDI_RASTERIZER_DESC_0102 *desc, D3D12DDI_HRASTERIZERSTATE slot) {
    if (!desc) { r0_device.pfnCreateRasterizerState(h, nullptr, slot); return; }
    const auto r = d0108_rasterizer_r0(*desc);
    r0_device.pfnCreateRasterizerState(h, &r, slot);
}

SIZE_T APIENTRY d0108_shader_size(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SHADER_0026 *args) {
    return args ? r0_device.pfnCalcPrivateShaderSize(h, args->pShaderCode, args->hRootSignature, args->IOSignatures.Standard) : 0;
}
template<int Stage> VOID APIENTRY d0108_shader(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SHADER_0026 *args, D3D12DDI_HSHADER slot) {
    if (!args) { error(context(h), E_INVALIDARG); return; }
    const auto request = *args;
    switch (Stage) {
    case 0: r0_device.pfnCreatePixelShader(h, request.pShaderCode, request.hRootSignature, slot, request.IOSignatures.Standard, request.Flags); break;
    case 1: r0_device.pfnCreateVertexShader(h, request.pShaderCode, request.hRootSignature, slot, request.IOSignatures.Standard, request.Flags); break;
    case 2: r0_device.pfnCreateGeometryShader(h, request.pShaderCode, request.hRootSignature, slot, request.IOSignatures.Standard, request.Flags); break;
    case 3: r0_device.pfnCreateHullShader(h, request.pShaderCode, request.hRootSignature, slot, request.IOSignatures.Tessellation, request.Flags); break;
    case 4: r0_device.pfnCreateDomainShader(h, request.pShaderCode, request.hRootSignature, slot, request.IOSignatures.Tessellation, request.Flags); break;
    case 5: r0_device.pfnCreateComputeShader(h, request.pShaderCode, request.hRootSignature, slot, request.Flags); break;
    }
}
SIZE_T APIENTRY d0108_tess_size(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SHADER_0026 *args) {
    return args ? r0_device.pfnCalcPrivateTessellationShaderSize(h, args->pShaderCode, args->hRootSignature, args->IOSignatures.Tessellation) : 0;
}
D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT d0108_gs_so_r0(const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026 &a) {
    D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT r{};
    r.pShaderCode = a.CreateShader.pShaderCode; r.hRootSignature = a.CreateShader.hRootSignature;
    r.pOutputStreamDecl = a.pOutputStreamDecl; r.NumEntries = a.NumEntries;
    r.BufferStridesInBytes = a.BufferStridesInBytes; r.NumStrides = a.NumStrides; r.RasterizedStream = a.RasterizedStream;
    return r;
}
SIZE_T APIENTRY d0108_gs_so_size(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026 *args) {
    if (!args) return 0;
    const auto r = d0108_gs_so_r0(*args);
    return r0_device.pfnCalcPrivateGeometryShaderWithStreamOutput(h, &r, args->CreateShader.IOSignatures.Standard);
}
VOID APIENTRY d0108_gs_so(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026 *args, D3D12DDI_HSHADER slot) {
    if (!args) { error(context(h), E_INVALIDARG); return; }
    const auto request = *args;
    const auto r = d0108_gs_so_r0(request);
    r0_device.pfnCreateGeometryShaderWithStreamOutput(h, &r, slot, request.CreateShader.IOSignatures.Standard, request.CreateShader.Flags);
}

D3D12DDIARG_CREATE_PIPELINE_STATE_0001 d0108_pso_r0(const D3D12DDIARG_CREATE_PIPELINE_STATE_0099 &a, D3D12DDI_HPIPELINESTATE slot) {
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 r{};
    r.hDrvPipelineState = slot;
    r.hComputeShader = a.hComputeShader; r.hVertexShader = a.hVertexShader; r.hPixelShader = a.hPixelShader;
    r.hDomainShader = a.hDomainShader; r.hHullShader = a.hHullShader; r.hGeometryShader = a.hGeometryShader;
    r.hRootSignature = a.hRootSignature; r.hBlendState = a.hBlendState; r.SampleMask = a.SampleMask;
    r.hRasterizerState = a.hRasterizerState; r.hDepthStencilState = a.hDepthStencilState;
    r.hElementLayout = a.hElementLayout; r.IBStripCutValue = a.IBStripCutValue;
    r.PrimitiveTopologyType = a.PrimitiveTopologyType; r.NumRenderTargets = a.NumRenderTargets;
    std::memcpy(r.RTVFormats, a.RTVFormats, sizeof(r.RTVFormats));
    r.DSVFormat = a.DSVFormat; r.SampleDesc = a.SampleDesc; r.NodeMask = a.NodeMask;
    return r;
}
SIZE_T APIENTRY d0108_pso_size(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_PIPELINE_STATE_0099 *args) {
    if (!args) return 0;
    const auto r = d0108_pso_r0(*args, {});
    return r0_device.pfnCalcPrivatePipelineStateSize(h, &r);
}
HRESULT APIENTRY d0108_pso(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_PIPELINE_STATE_0099 *args,
        D3D12DDI_HPIPELINESTATE slot, D3D12DDI_HRTPIPELINESTATE) {
    if (!args) return E_INVALIDARG;
    if (args->hMeshShader.pDrvPrivate || args->hAmplificationShader.pDrvPrivate) return E_NOTIMPL;
    const auto r = d0108_pso_r0(*args, slot);
    return r0_device.pfnCreatePipelineState(h, &r);
}

/* ---- root signatures and samplers ------------------------------------- */
struct D0108RootSignature {
    D3D12DDI_ROOT_SIGNATURE root{};
    std::vector<D3D12DDI_ROOT_PARAMETER> parameters;
    std::vector<std::vector<D3D12DDI_DESCRIPTOR_RANGE>> ranges;
    std::vector<D3D12DDI_STATIC_SAMPLER> samplers;
};
HRESULT d0108_root_r0(const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100 &a, D0108RootSignature &out) {
    const auto *src = a.pRootSignature_1_2;
    if (!src || (a.Version != D3D12DDI_ROOT_SIGNATURE_VERSION_1_1 && a.Version != D3D12DDI_ROOT_SIGNATURE_VERSION_1_2))
        return E_INVALIDARG;
    if ((src->NumParameters && !src->pRootParameters) || (src->NumStaticSamplers && !src->pStaticSamplers)) return E_INVALIDARG;
    try {
        out.parameters.resize(src->NumParameters);
        out.ranges.resize(src->NumParameters);
        for (UINT i = 0; i < src->NumParameters; ++i) {
            const auto &p = src->pRootParameters[i];
            auto &d = out.parameters[i];
            d.ParameterType = p.ParameterType; d.ShaderVisibility = p.ShaderVisibility;
            if (p.ParameterType == D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                const auto &t = p.DescriptorTable;
                if (t.NumDescriptorRanges && !t.pDescriptorRanges) return E_INVALIDARG;
                auto &r = out.ranges[i];
                r.resize(t.NumDescriptorRanges);
                for (UINT j = 0; j < t.NumDescriptorRanges; ++j) {
                    // Range/descriptor flags (1.1 volatility hints) have no R0
                    // form; R0 semantics are the conservative "volatile" case.
                    r[j].RangeType = t.pDescriptorRanges[j].RangeType;
                    r[j].NumDescriptors = t.pDescriptorRanges[j].NumDescriptors;
                    r[j].BaseShaderRegister = t.pDescriptorRanges[j].BaseShaderRegister;
                    r[j].RegisterSpace = t.pDescriptorRanges[j].RegisterSpace;
                    r[j].OffsetInDescriptorsFromTableStart = t.pDescriptorRanges[j].OffsetInDescriptorsFromTableStart;
                }
                d.DescriptorTable.NumDescriptorRanges = t.NumDescriptorRanges;
                d.DescriptorTable.pDescriptorRanges = r.data();
            } else if (p.ParameterType == D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                d.Constants = p.Constants;
            } else {
                d.Descriptor.ShaderRegister = p.Descriptor.ShaderRegister;
                d.Descriptor.RegisterSpace = p.Descriptor.RegisterSpace;
            }
        }
        out.samplers.resize(src->NumStaticSamplers);
        for (UINT i = 0; i < src->NumStaticSamplers; ++i) {
            const auto &s = src->pStaticSamplers[i];
            if (s.Flags) return E_NOTIMPL; // UINT border colors / unnormalized coordinates not advertised
            auto &d = out.samplers[i];
            d.Filter = s.Filter; d.AddressU = s.AddressU; d.AddressV = s.AddressV; d.AddressW = s.AddressW;
            d.MipLODBias = s.MipLODBias; d.MaxAnisotropy = s.MaxAnisotropy; d.ComparisonFunc = s.ComparisonFunc;
            d.BorderColor = s.BorderColor; d.MinLOD = s.MinLOD; d.MaxLOD = s.MaxLOD;
            d.ShaderRegister = s.ShaderRegister; d.RegisterSpace = s.RegisterSpace; d.ShaderVisibility = s.ShaderVisibility;
        }
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    out.root.NumParameters = src->NumParameters;
    out.root.pRootParameters = out.parameters.data();
    out.root.NumStaticSamplers = src->NumStaticSamplers;
    out.root.pStaticSamplers = out.samplers.data();
    out.root.Flags = src->Flags;
    return S_OK;
}
SIZE_T APIENTRY d0108_root_size(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100 *args) {
    D0108RootSignature root;
    if (!args || FAILED(d0108_root_r0(*args, root))) return sizeof(Object);
    const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 r{&root.root, args->NodeMask};
    return r0_device.pfnCalcPrivateRootSignatureSize(h, &r);
}
HRESULT APIENTRY d0108_root(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100 *args, D3D12DDI_HROOTSIGNATURE slot) {
    if (!args) return E_INVALIDARG;
    D0108RootSignature root;
    HRESULT hr = d0108_root_r0(*args, root);
    if (FAILED(hr)) return hr;
    const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 r{&root.root, args->NodeMask};
    return r0_device.pfnCreateRootSignature(h, &r, slot);
}
VOID APIENTRY d0108_sampler(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SAMPLER_0096 *args, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst) {
    if (!args || !args->pSamplerDesc) { error(context(h), E_INVALIDARG); return; }
    const auto &s = *args->pSamplerDesc;
    if (s.Flags) { error(context(h), E_NOTIMPL); return; }
    D3D12DDI_SAMPLER_DESC d{};
    d.Filter = s.Filter; d.AddressU = s.AddressU; d.AddressV = s.AddressV; d.AddressW = s.AddressW;
    d.MipLODBias = s.MipLODBias; d.MaxAnisotropy = s.MaxAnisotropy; d.ComparisonFunc = s.ComparisonFunc;
    std::memcpy(d.BorderColor, s.FloatBorderColor, sizeof(d.BorderColor));
    d.MinLOD = s.MinLOD; d.MaxLOD = s.MaxLOD;
    const D3D12DDIARG_CREATE_SAMPLER r{&d};
    r0_device.pfnCreateSampler(h, &r, dst);
}

/* ---- queues ----------------------------------------------------------- */
SIZE_T APIENTRY d0108_queue_size(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATECOMMANDQUEUE_0050 *args) {
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 r{};
    if (args) { r.QueueFlags = args->QueueFlags; r.NodeMask = args->NodeMask; }
    return r0_device.pfnCalcPrivateCommandQueueSize(h, &r);
}
HRESULT APIENTRY d0108_queue(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATECOMMANDQUEUE_0050 *args,
        D3D12DDI_HCOMMANDQUEUE slot, D3D12DDI_HRTCOMMANDQUEUE runtime) {
    if (!args) return E_INVALIDARG;
    // Priority/creation flags and scheduling groups are scheduling hints for
    // hardware queues; the KMD exposes one software-scheduled node.
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 r{};
    r.hDrvCommandQueue = slot; r.hRTCommandQueue = runtime; r.QueueFlags = args->QueueFlags; r.NodeMask = args->NodeMask;
    return r0_device.pfnCreateCommandQueue(h, &r);
}

/* ---- resources -------------------------------------------------------- */
D3D12DDI_RESOURCE_STATES d0108_layout_state(D3D12DDI_BARRIER_LAYOUT layout) {
    switch (layout) {
    case D3D12DDI_BARRIER_LAYOUT_RENDER_TARGET: return D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    case D3D12DDI_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE: return D3D12DDI_RESOURCE_STATE_DEPTH_WRITE;
    case D3D12DDI_BARRIER_LAYOUT_DEPTH_STENCIL_READ: return D3D12DDI_RESOURCE_STATE_DEPTH_READ;
    case D3D12DDI_BARRIER_LAYOUT_COPY_SOURCE: case D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_SOURCE:
        return D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    case D3D12DDI_BARRIER_LAYOUT_COPY_DEST: case D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_DEST:
        return D3D12DDI_RESOURCE_STATE_COPY_DEST;
    case D3D12DDI_BARRIER_LAYOUT_RESOLVE_SOURCE: return D3D12DDI_RESOURCE_STATE_RESOLVE_SOURCE;
    case D3D12DDI_BARRIER_LAYOUT_RESOLVE_DEST: return D3D12DDI_RESOURCE_STATE_RESOLVE_DEST;
    case D3D12DDI_BARRIER_LAYOUT_SHADER_RESOURCE: case D3D12DDI_BARRIER_LAYOUT_LEGACY_SHADER_RESOURCE:
        return static_cast<D3D12DDI_RESOURCE_STATES>(D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    default: return D3D12DDI_RESOURCE_STATE_COMMON;
    }
}
D3D12DDIARG_CREATERESOURCE_0003 d0108_resource_r0(const D3D12DDIARG_CREATERESOURCE_0088 &a) {
    D3D12DDIARG_CREATERESOURCE_0003 r{};
    r.ReuseBufferGPUVA = a.ReuseBufferGPUVA; r.ResourceType = a.ResourceType; r.Width = a.Width; r.Height = a.Height;
    r.DepthOrArraySize = a.DepthOrArraySize; r.MipLevels = a.MipLevels; r.Format = a.Format; r.SampleDesc = a.SampleDesc;
    r.Layout = a.Layout; r.Flags = a.Flags; r.InitialResourceState = d0108_layout_state(a.InitialBarrierLayout);
    // 0022 made buffer UAV access explicit; R0 buffers always permit it.
    if (a.ResourceType == D3D12DDI_RT_BUFFER)
        r.Flags = static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(static_cast<UINT>(r.Flags) &
            ~static_cast<UINT>(D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS));
    r.pRowMajorLayout = a.pRowMajorLayout;
    return r;
}
D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY d0108_heap_sizes(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001 *heap,
        const D3D12DDIARG_CREATERESOURCE_0088 *resource, D3D12DDI_HPROTECTEDRESOURCESESSION_0030) {
    if (!resource) return r0_device.pfnCalcPrivateHeapAndResourceSizes(h, heap, nullptr);
    const auto r = d0108_resource_r0(*resource);
    return r0_device.pfnCalcPrivateHeapAndResourceSizes(h, heap, &r);
}
HRESULT APIENTRY d0108_heap(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001 *heap, D3D12DDI_HHEAP slot,
        D3D12DDI_HRTRESOURCE runtime, const D3D12DDIARG_CREATERESOURCE_0088 *resource, const D3D12DDI_CLEAR_VALUES *clear,
        D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session, D3D12DDI_HRESOURCE resource_slot) {
    if (session.pDrvPrivate) return E_NOTIMPL;
    if (resource && resource->NumCastableFormats) return E_NOTIMPL;
    if (!resource) return r0_device.pfnCreateHeapAndResource(h, heap, slot, runtime, nullptr, clear, resource_slot);
    const auto r = d0108_resource_r0(*resource);
    return r0_device.pfnCreateHeapAndResource(h, heap, slot, runtime, &r, clear, resource_slot);
}
D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY d0108_open_sizes(D3D12DDI_HDEVICE h, const D3D12DDIARG_OPENHEAP_0003 *args,
        D3D12DDI_HPROTECTEDRESOURCESESSION_0030) {
    return r0_device.pfnCalcPrivateOpenedHeapAndResourceSizes(h, args);
}
HRESULT APIENTRY d0108_open(D3D12DDI_HDEVICE h, const D3D12DDIARG_OPENHEAP_0003 *args, D3D12DDI_HHEAP heap,
        D3D12DDI_HRTRESOURCE runtime, D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session, D3D12DDI_HRESOURCE resource) {
    if (session.pDrvPrivate) return E_NOTIMPL;
    return r0_device.pfnOpenHeapAndResource(h, args, heap, runtime, resource);
}
void d0108_allocation_info(const D3D12DDI_RESOURCE_ALLOCATION_INFO &a, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *out) {
    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 r{};
    r.ResourceDataSize = a.ResourceDataSize; r.AdditionalDataHeaderSize = a.AdditionalDataHeaderSize;
    r.AdditionalDataSize = a.AdditionalDataSize;
    r.ResourceDataAlignment = a.ResourceDataAlignment > UINT32_MAX ? UINT32_MAX : static_cast<UINT32>(a.ResourceDataAlignment);
    r.AdditionalDataHeaderAlignment = a.AdditionalDataHeaderAlignment1;
    r.AdditionalDataAlignment = a.AdditionalDataAlignment > UINT32_MAX ? UINT32_MAX : static_cast<UINT32>(a.AdditionalDataAlignment);
    r.Layout = a.Layout; r.MipLevelSwizzleTransition[0] = a.MipLevelSwizzleTransition;
    *out = r;
}
VOID APIENTRY d0108_check_allocation(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATERESOURCE_0088 *resource,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags, UINT32 alignment, UINT nodes, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *out) {
    if (!resource || !out) { error(context(h), E_INVALIDARG); return; }
    const auto r = d0108_resource_r0(*resource);
    native_trace("CheckResourceAllocationInfo in type=%u w=%llu h=%u d=%u mips=%u fmt=%u samples=%u/%u layout=%u flags=%x"
        " barrier_layout=%x opt=%x align=%u nodes=%u rowmajor=%p reuse=%p castable=%u", (unsigned)r.ResourceType,
        (unsigned long long)r.Width, r.Height, r.DepthOrArraySize, r.MipLevels, (unsigned)r.Format, r.SampleDesc.Count,
        r.SampleDesc.Quality, (unsigned)r.Layout, (unsigned)r.Flags, (unsigned)resource->InitialBarrierLayout,
        (unsigned)flags, alignment, nodes, (const void *)r.pRowMajorLayout,
        r.ReuseBufferGPUVA.BaseAddress.UMD.hResource.pDrvPrivate, resource->NumCastableFormats);
    D3D12DDI_RESOURCE_ALLOCATION_INFO info{};
    r0_device.pfnCheckResourceAllocationInfo(h, &r, flags, alignment, nodes, &info);
    native_trace("CheckResourceAllocationInfo type=%u w=%llu -> size=%llu align=%llu", (unsigned)r.ResourceType,
        (unsigned long long)r.Width, (unsigned long long)info.ResourceDataSize, (unsigned long long)info.ResourceDataAlignment);
    d0108_allocation_info(info, out);
}
VOID APIENTRY d0108_check_existing(D3D12DDI_HDEVICE h, D3D12DDI_HRESOURCE resource, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *out) {
    if (!out) { error(context(h), E_INVALIDARG); return; }
    D3D12DDI_RESOURCE_ALLOCATION_INFO info{};
    D3DKMT_HANDLE allocation = 0;
    r0_device.pfnCheckExistingResourceAllocationInfo(h, resource, &info, &allocation);
    d0108_allocation_info(info, out);
}

/* ---- 0040 command model: pools and recorders --------------------------- */
constexpr uint32_t d0108_pool_magic = 0x4c4f4f50; // POOL
constexpr uint32_t d0108_recorder_magic = 0x44434552; // RECD
struct D0108Pool {
    uint32_t magic;
    D3D12DDI_HDEVICE device;
    Object *allocators[4]; // indexed by backend command type: 0 direct, 1 bundle, 2 compute, 3 copy
};
struct D0108Recorder {
    uint32_t magic;
    D3D12DDI_COMMAND_QUEUE_FLAGS queue_flags;
    D0108Pool *target;
};
D0108Pool *d0108_pool(void *memory) {
    auto *value = static_cast<D0108Pool *>(memory);
    return value && value->magic == d0108_pool_magic ? value : nullptr;
}
D0108Recorder *d0108_recorder(void *memory) {
    auto *value = static_cast<D0108Recorder *>(memory);
    return value && value->magic == d0108_recorder_magic ? value : nullptr;
}
// Creates an R0 allocator Object in driver-owned memory.
HRESULT d0108_allocator(D3D12DDI_HDEVICE h, D3D12DDI_COMMAND_LIST_TYPE type, D3D12DDI_COMMAND_QUEUE_FLAGS flags, Object **out) {
    *out = nullptr;
    void *memory = ::operator new(sizeof(Object), std::nothrow);
    if (!memory) return E_OUTOFMEMORY;
    std::memset(memory, 0, sizeof(Object));
    D3D12DDIARG_CREATECOMMANDALLOCATOR args{};
    args.hDrvCommandAllocator.pDrvPrivate = memory; args.Type = type; args.QueueFlags = flags;
    HRESULT hr = r0_device.pfnCreateCommandAllocator(h, &args);
    if (FAILED(hr)) { ::operator delete(memory); return hr; }
    *out = static_cast<Object *>(memory);
    return S_OK;
}
void d0108_allocator_free(D3D12DDI_HDEVICE h, Object *allocator) {
    if (!allocator) return;
    r0_device.pfnDestroyCommandAllocator(h, {allocator});
    ::operator delete(static_cast<void *>(allocator));
}
SIZE_T APIENTRY d0108_pool_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_POOL_0040 *) { return sizeof(D0108Pool); }
HRESULT APIENTRY d0108_pool_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_COMMAND_POOL_0040 *args, D3D12DDI_HCOMMANDPOOL_0040 slot) {
    if (!args || !slot.pDrvPrivate || !context(h)) return E_INVALIDARG;
    new (slot.pDrvPrivate) D0108Pool{d0108_pool_magic, h, {}};
    return S_OK;
}
VOID APIENTRY d0108_pool_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDPOOL_0040 slot) {
    auto *pool = d0108_pool(slot.pDrvPrivate);
    if (!pool) { error(context(h), E_INVALIDARG); return; }
    for (auto *&allocator : pool->allocators) { d0108_allocator_free(h, allocator); allocator = nullptr; }
    pool->magic = 0;
}
VOID APIENTRY d0108_pool_reset(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDPOOL_0040 slot) {
    auto *pool = d0108_pool(slot.pDrvPrivate);
    if (!pool) { error(context(h), E_INVALIDARG); return; }
    for (auto *allocator : pool->allocators)
        if (allocator) { NativeTimed timed(NT_ALLOC_RESET); r0_device.pfnResetCommandAllocator({allocator}); }
}
SIZE_T APIENTRY d0108_recorder_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040 *) { return sizeof(D0108Recorder); }
HRESULT APIENTRY d0108_recorder_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040 *args,
        D3D12DDI_HCOMMANDRECORDER_0040 slot) {
    if (!args || !slot.pDrvPrivate || !context(h)) return E_INVALIDARG;
    new (slot.pDrvPrivate) D0108Recorder{d0108_recorder_magic, args->QueueFlags, nullptr};
    return S_OK;
}
VOID APIENTRY d0108_recorder_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDRECORDER_0040 slot) {
    auto *recorder = d0108_recorder(slot.pDrvPrivate);
    if (!recorder) { error(context(h), E_INVALIDARG); return; }
    recorder->magic = 0;
}
VOID APIENTRY d0108_recorder_target(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDRECORDER_0040 recorder_slot, D3D12DDI_HCOMMANDPOOL_0040 pool_slot) {
    auto *recorder = d0108_recorder(recorder_slot.pDrvPrivate);
    auto *pool = d0108_pool(pool_slot.pDrvPrivate);
    if (!recorder || (pool_slot.pDrvPrivate && !pool)) { error(context(h), E_INVALIDARG); return; }
    recorder->target = pool;
}

// Command lists: the R0 backend needs an allocator at creation, so each list
// starts on a private allocator and is closed; ResetCommandList then moves it
// to the recorder's target pool.
struct D0108ListState { Object *bootstrap; D3D12DDI_COMMAND_LIST_TYPE type; D3D12DDI_COMMAND_QUEUE_FLAGS flags; };
std::mutex d0108_lists_lock;
std::unordered_map<void *, D0108ListState> d0108_lists;
SIZE_T APIENTRY d0108_list_size(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_COMMAND_LIST_0040 *) {
    return r0_device.pfnCalcPrivateCommandListSize(h, nullptr);
}
HRESULT APIENTRY d0108_list_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_COMMAND_LIST_0040 *args,
        D3D12DDI_HCOMMANDLIST slot, D3D12DDI_HRTCOMMANDLIST runtime) {
    if (!args || !slot.pDrvPrivate) return E_INVALIDARG;
    const auto request = *args;
    Object *bootstrap = nullptr;
    HRESULT hr = d0108_allocator(h, request.Type, request.QueueFlags, &bootstrap);
    native_trace("CreateCommandList_0040 type=%u flags=%x bootstrap hr=%08x", (unsigned)request.Type,
        (unsigned)request.QueueFlags, (unsigned)hr);
    if (FAILED(hr)) return hr;
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 r{};
    r.hDrvCommandList = slot; r.hRTCommandList = runtime; r.hDrvCommandAllocator = {bootstrap};
    r.Type = request.Type; r.QueueFlags = request.QueueFlags; r.ID = request.ID;
    r.CommandListFlags = request.CommandListFlags; r.NodeMask = request.NodeMask;
    hr = r0_device.pfnCreateCommandList(h, &r);
    if (FAILED(hr)) { d0108_allocator_free(h, bootstrap); return hr; }
    // The 0040 model creates lists closed; recording starts at ResetCommandList.
    r0_commands.pfnCloseCommandList(slot);
    try {
        std::lock_guard<std::mutex> lock(d0108_lists_lock);
        d0108_lists[slot.pDrvPrivate] = {bootstrap, request.Type, request.QueueFlags};
    } catch (...) {
        r0_device.pfnDestroyCommandList(h, slot);
        d0108_allocator_free(h, bootstrap);
        return E_OUTOFMEMORY;
    }
    return S_OK;
}
VOID APIENTRY d0108_list_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDLIST slot) {
    Object *bootstrap = nullptr;
    {
        std::lock_guard<std::mutex> lock(d0108_lists_lock);
        auto found = d0108_lists.find(slot.pDrvPrivate);
        if (found != d0108_lists.end()) { bootstrap = found->second.bootstrap; d0108_lists.erase(found); }
    }
    r0_device.pfnDestroyCommandList(h, slot);
    d0108_allocator_free(h, bootstrap);
}
VOID APIENTRY d0108_list_reset(D3D12DDI_HCOMMANDLIST slot, const D3D12DDIARG_RESETCOMMANDLIST_0040 *args) {
    auto *list = object(slot.pDrvPrivate);
    if (!list) return;
    auto *recorder = args ? d0108_recorder(args->hDrvCommandRecorder.pDrvPrivate) : nullptr;
    D0108ListState state{};
    {
        std::lock_guard<std::mutex> lock(d0108_lists_lock);
        auto found = d0108_lists.find(slot.pDrvPrivate);
        if (found != d0108_lists.end()) state = found->second;
    }
    if (!recorder || !recorder->target || !state.bootstrap) { error(list, E_INVALIDARG); return; }
    const uint32_t type = command_list_type(state.type, state.flags);
    if (type >= 4) { error(list, E_INVALIDARG); return; }
    auto *pool = recorder->target;
    if (!pool->allocators[type]) {
        HRESULT hr = d0108_allocator(pool->device, state.type, state.flags, &pool->allocators[type]);
        if (FAILED(hr)) { error(list, hr); return; }
    }
    D3D12DDIARG_RESETCOMMANDLIST r{};
    r.hDrvCommandAllocator = {pool->allocators[type]}; r.ID = args->ID; r.CommandListFlags = args->CommandListFlags;
    r0_commands.pfnResetCommandList(slot, &r);
}

/* ---- command lists ------------------------------------------------------ */
VOID APIENTRY d0108_barriers(D3D12DDI_HCOMMANDLIST list, UINT count, const D3D12DDIARG_RESOURCE_BARRIER_0022 *barriers) {
    if (count && !barriers) { if (auto *value = object(list.pDrvPrivate)) error(value, E_INVALIDARG); return; }
    std::vector<D3D12DDIARG_RESOURCE_BARRIER_0003> r;
    try { r.resize(count); } catch (...) { if (auto *value = object(list.pDrvPrivate)) error(value, E_OUTOFMEMORY); return; }
    for (UINT i = 0; i < count; ++i) {
        const auto &b = barriers[i];
        r[i].Type = b.Type; r[i].Flags = b.Flags;
        if (b.Type == D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION) r[i].Transition = b.Transition;
        else if (b.Type == D3D12DDI_RESOURCE_BARRIER_TYPE_UAV) r[i].UAV = b.UAV;
        else if (b.Type == D3D12DDI_RESOURCE_BARRIER_TYPE_0022_RANGED) {
            // A ranged (aliasing) barrier orders all access to the resource:
            // express it with the R0 memory dependency on that resource.
            r[i].Type = D3D12DDI_RESOURCE_BARRIER_TYPE_UAV;
            r[i].UAV.hResource = b.Ranged.hResource;
        } else {
            std::memcpy(&r[i].Aliasing, &b.Ranged, sizeof(r[i].Aliasing));
        }
    }
    r0_commands.pfnResourceBarrier(list, count, r.data());
}
VOID APIENTRY d0108_present(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HCOMMANDQUEUE queue, const D3D12DDIARG_PRESENT_0001 *args,
        D3D12DDI_PRESENT_0051 *out, D3D12DDI_PRESENT_CONTEXTS_0051 *contexts, D3D12DDI_PRESENT_HWQUEUES_0051 *queues) {
    D3D12DDI_PRESENT_0003 r{};
    r0_commands.pfnPresent(list, queue, args, &r);
    if (out) {
        *out = {};
        out->BroadcastSrcAllocation[0] = r.hSrcAllocation; out->BroadcastDstAllocation[0] = r.hDstAllocation;
        for (UINT i = 0; i < r.BroadcastContextCount && i < D3DDDI_MAX_BROADCAST_CONTEXT; ++i) {
            out->BroadcastSrcAllocation[i + 1] = r.BroadcastSrcAllocation[i];
            out->BroadcastDstAllocation[i + 1] = r.BroadcastDstAllocation[i];
        }
        out->AddedGpuWork = r.AddedGpuWork; out->BackBufferMultiplicity = r.BackBufferMultiplicity;
    }
    if (contexts) {
        *contexts = {};
        contexts->hContext = r.hContext; contexts->BroadcastContextCount = r.BroadcastContextCount;
        for (UINT i = 0; i < r.BroadcastContextCount && i < D3DDDI_MAX_BROADCAST_CONTEXT; ++i)
            contexts->BroadcastContext[i] = r.BroadcastContext[i];
    }
    if (queues) *queues = {};
}
VOID APIENTRY d0108_atomic_copy(D3D12DDI_HCOMMANDLIST list, D3D12DDIARG_BUFFER_PLACEMENT dst, D3D12DDIARG_BUFFER_PLACEMENT src, UINT64 bytes) {
    r0_commands.pfnCopyBufferRegion(list, dst, src, bytes);
}
VOID APIENTRY d0108_stencil_refs(D3D12DDI_HCOMMANDLIST list, UINT front, UINT back) {
    if (front != back) { if (auto *value = object(list.pDrvPrivate)) error(value, E_NOTIMPL); return; }
    r0_commands.pfnOmSetStencilRef(list, front);
}

/* ---- device entries with output parameters ----------------------------- */
VOID APIENTRY d0108_debug_allocation_info(D3D12DDI_HDEVICE, D3D12DDI_HANDLE_AND_TYPE, UINT *count_va,
        D3D12DDI_DEBUG_VIRTUAL_ADDRESS_ALLOCATION_INFO_0012 *, UINT *count_heap, D3D12DDI_DEBUG_KMT_ALLOCATION_INFO_0014 *) {
    if (count_va) *count_va = 0;
    if (count_heap) *count_heap = 0;
}
void APIENTRY d0108_background_mode(D3D12DDI_HDEVICE, D3D12DDI_BACKGROUND_PROCESSING_MODE_0062,
        D3D12DDI_MEASUREMENTS_ACTION_0062, BOOL *more) {
    if (more) *more = FALSE;
}
HRESULT APIENTRY d0108_meta_commands(D3D12DDI_HDEVICE, UINT *count, D3D12DDIARG_META_COMMAND_DESC *) {
    if (!count) return E_INVALIDARG;
    *count = 0;
    return S_OK;
}
D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS APIENTRY d0108_matching_identifier(D3D12DDI_HDEVICE, D3D12DDI_SERIALIZED_DATA_TYPE,
        const D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054 *) {
    return D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
}

/* ---- 0096 extended features table (type 27): none supported ------------ */
HRESULT APIENTRY d0108_ext_features(D3D12DDI_HDEVICE, D3D12DDI_FEATURE_0020, UINT32 *count, D3D12DDI_FEATURE_0020 *) {
    if (!count) return E_INVALIDARG;
    *count = 0;
    return S_OK;
}
HRESULT APIENTRY d0108_ext_feature_versions(D3D12DDI_HDEVICE, D3D12DDI_FEATURE_0020, UINT32 *count, UINT32 *) {
    if (!count) return E_INVALIDARG;
    *count = 0;
    return S_OK;
}
HRESULT APIENTRY d0108_ext_enable(D3D12DDI_HDEVICE, D3D12DDI_FEATURE_0020, UINT32) { return E_INVALIDARG; }
HRESULT APIENTRY d0108_ext_callbacks(D3D12DDI_HDEVICE, D3D12DDI_TABLE_TYPE, const void *, SIZE_T) { return S_OK; }
void d0108_extended_features(D3D12DDI_EXTENDED_FEATURES_FUNCS_0096 *table) {
    table->pfnGetSupportedExtendedFeatures = d0108_ext_features;
    table->pfnGetSupportedExtendedFeatureVersions = d0108_ext_feature_versions;
    table->pfnEnableExtendedFeature = d0108_ext_enable;
    table->pfnSetExtendedFeatureCallbacks = d0108_ext_callbacks;
}

void d0108_tables(D3D12DDI_DEVICE_FUNCS_CORE_0108 *device, D3D12DDI_COMMAND_LIST_FUNCS_3D_0108 *commands) {
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    VioGpuD3D12BridgeGetTables(&r0_device, &r0_commands, &queue);
    *device = {}; *commands = {};
#define SAME(field) device->field = r0_device.field
    SAME(pfnCheckFormatSupport); SAME(pfnCheckMultisampleQualityLevels); SAME(pfnGetMipPacking);
    device->pfnCalcPrivateElementLayoutSize = d0108_element_layout_size; device->pfnCreateElementLayout = d0108_element_layout;
    SAME(pfnDestroyElementLayout);
    device->pfnCalcPrivateBlendStateSize = d0108_blend_size; device->pfnCreateBlendState = d0108_blend; SAME(pfnDestroyBlendState);
    device->pfnCalcPrivateDepthStencilStateSize = d0108_depth_size; device->pfnCreateDepthStencilState = d0108_depth;
    SAME(pfnDestroyDepthStencilState);
    device->pfnCalcPrivateRasterizerStateSize = d0108_rasterizer_size; device->pfnCreateRasterizerState = d0108_rasterizer;
    SAME(pfnDestroyRasterizerState);
    device->pfnCalcPrivateShaderSize = d0108_shader_size;
    device->pfnCreateVertexShader = d0108_shader<1>; device->pfnCreatePixelShader = d0108_shader<0>;
    device->pfnCreateGeometryShader = d0108_shader<2>; device->pfnCreateComputeShader = d0108_shader<5>;
    device->pfnCalcPrivateGeometryShaderWithStreamOutput = d0108_gs_so_size;
    device->pfnCreateGeometryShaderWithStreamOutput = d0108_gs_so;
    device->pfnCalcPrivateTessellationShaderSize = d0108_tess_size;
    device->pfnCreateHullShader = d0108_shader<3>; device->pfnCreateDomainShader = d0108_shader<4>;
    SAME(pfnDestroyShader);
    device->pfnCalcPrivateCommandQueueSize = d0108_queue_size; device->pfnCreateCommandQueue = d0108_queue;
    SAME(pfnDestroyCommandQueue);
    device->pfnCalcPrivateCommandPoolSize = d0108_pool_size; device->pfnCreateCommandPool = d0108_pool_create;
    device->pfnDestroyCommandPool = d0108_pool_destroy; device->pfnResetCommandPool = d0108_pool_reset;
    device->pfnCalcPrivatePipelineStateSize = d0108_pso_size; device->pfnCreatePipelineState = d0108_pso;
    SAME(pfnDestroyPipelineState);
    device->pfnCalcPrivateCommandListSize = d0108_list_size; device->pfnCreateCommandList = d0108_list_create;
    device->pfnDestroyCommandList = d0108_list_destroy;
    SAME(pfnCalcPrivateFenceSize); SAME(pfnCreateFence); SAME(pfnDestroyFence);
    SAME(pfnCalcPrivateDescriptorHeapSize); SAME(pfnCreateDescriptorHeap); SAME(pfnDestroyDescriptorHeap);
    SAME(pfnGetDescriptorSizeInBytes); SAME(pfnGetCPUDescriptorHandleForHeapStart); SAME(pfnGetGPUDescriptorHandleForHeapStart);
    SAME(pfnCreateShaderResourceView); SAME(pfnCreateConstantBufferView);
    device->pfnCreateSampler = d0108_sampler;
    SAME(pfnCreateUnorderedAccessView); SAME(pfnCreateRenderTargetView); SAME(pfnCreateDepthStencilView);
    device->pfnCalcPrivateRootSignatureSize = d0108_root_size; device->pfnCreateRootSignature = d0108_root;
    SAME(pfnDestroyRootSignature);
    SAME(pfnMapHeap); SAME(pfnUnmapHeap);
    device->pfnCalcPrivateHeapAndResourceSizes = d0108_heap_sizes; device->pfnCreateHeapAndResource = d0108_heap;
    SAME(pfnDestroyHeapAndResource); SAME(pfnMakeResident); SAME(pfnEvict);
    device->pfnCalcPrivateOpenedHeapAndResourceSizes = d0108_open_sizes; device->pfnOpenHeapAndResource = d0108_open;
    SAME(pfnCopyDescriptors); SAME(pfnCopyDescriptorsSimple);
    SAME(pfnCalcPrivateQueryHeapSize); SAME(pfnCreateQueryHeap); SAME(pfnDestroyQueryHeap);
    SAME(pfnCalcPrivateCommandSignatureSize); SAME(pfnCreateCommandSignature); SAME(pfnDestroyCommandSignature);
    SAME(pfnCheckResourceVirtualAddress); SAME(pfnCheckResourceAllocationHandle);
    device->pfnCheckResourceAllocationInfo = d0108_check_allocation;
    SAME(pfnCheckSubresourceInfo);
    device->pfnCheckExistingResourceAllocationInfo = d0108_check_existing;
    SAME(pfnOfferResources); SAME(pfnReclaimResources);
    SAME(pfnGetImplicitPhysicalAdapterMask); SAME(pfnGetPresentPrivateDriverDataSize); SAME(pfnQueryNodeMap);
    SAME(pfnRetrieveShaderComment);
    DDI_STUB(device, pfnCalcPrivatePipelineLibrarySize); DDI_STUB(device, pfnCreatePipelineLibrary);
    DDI_STUB(device, pfnDestroyPipelineLibrary); DDI_STUB(device, pfnAddPipelineStateToLibrary);
    DDI_STUB(device, pfnCalcSerializedLibrarySize); DDI_STUB(device, pfnSerializeLibrary);
    device->pfnGetDebugAllocationInfo = d0108_debug_allocation_info;
    device->pfnCalcPrivateCommandRecorderSize = d0108_recorder_size; device->pfnCreateCommandRecorder = d0108_recorder_create;
    device->pfnDestroyCommandRecorder = d0108_recorder_destroy;
    device->pfnCommandRecorderSetCommandPoolAsTarget = d0108_recorder_target;
    DDI_STUB(device, pfnCalcPrivateSchedulingGroupSize); DDI_STUB(device, pfnCreateSchedulingGroup);
    DDI_STUB(device, pfnDestroySchedulingGroup);
    device->pfnEnumerateMetaCommands = d0108_meta_commands;
    DDI_STUB(device, pfnEnumerateMetaCommandParameters); DDI_STUB(device, pfnCalcPrivateMetaCommandSize);
    DDI_STUB(device, pfnCreateMetaCommand); DDI_STUB(device, pfnDestroyMetaCommand);
    DDI_STUB(device, pfnGetMetaCommandRequiredParameterInfo);
    DDI_STUB(device, pfnCalcPrivateStateObjectSize); DDI_STUB(device, pfnCreateStateObject); DDI_STUB(device, pfnDestroyStateObject);
    DDI_STUB(device, pfnGetRaytracingAccelerationStructurePrebuildInfo);
    device->pfnCheckDriverMatchingIdentifier = d0108_matching_identifier;
    DDI_STUB(device, pfnGetShaderIdentifier); DDI_STUB(device, pfnGetShaderStackSize);
    DDI_STUB(device, pfnGetPipelineStackSize); DDI_STUB(device, pfnSetPipelineStackSize);
    device->pfnSetBackgroundProcessingMode = d0108_background_mode;
    DDI_STUB(device, pfnCalcPrivateAddToStateObjectSize); DDI_STUB(device, pfnAddToStateObject);
    DDI_STUB(device, pfnCreateSamplerFeedbackUnorderedAccessView);
    DDI_STUB(device, pfnCreateAmplificationShader); DDI_STUB(device, pfnCreateMeshShader);
    DDI_STUB(device, pfnCalcPrivateMeshShaderSize);
    DDI_NOOP(device, pfnImplicitShaderCacheControl);
    DDI_STUB(device, pfnGetProgramIdentifier); DDI_STUB(device, pfnGetWorkGraphMemoryRequirements);
#undef SAME
#define SAME(field) commands->field = r0_commands.field
    SAME(pfnCloseCommandList);
    commands->pfnResetCommandList = d0108_list_reset;
    SAME(pfnDrawInstanced); SAME(pfnDrawIndexedInstanced); SAME(pfnDispatch);
    SAME(pfnClearUnorderedAccessViewUint); SAME(pfnClearUnorderedAccessViewFloat);
    SAME(pfnClearRenderTargetView); SAME(pfnClearDepthStencilView); SAME(pfnDiscardResource);
    SAME(pfnCopyTextureRegion); SAME(pfnResourceCopy); SAME(pfnCopyTiles); SAME(pfnCopyBufferRegion);
    SAME(pfnResourceResolveSubresource); SAME(pfnExecuteBundle); SAME(pfnExecuteIndirect);
    commands->pfnResourceBarrier = d0108_barriers;
    SAME(pfnBlt);
    commands->pfnPresent = d0108_present;
    SAME(pfnBeginQuery); SAME(pfnEndQuery); SAME(pfnResolveQueryData); SAME(pfnSetPredication);
    SAME(pfnIaSetTopology); SAME(pfnRsSetViewports); SAME(pfnRsSetScissorRects); SAME(pfnOmSetBlendFactor);
    SAME(pfnOmSetStencilRef); SAME(pfnSetPipelineState); SAME(pfnSetDescriptorHeaps);
    SAME(pfnSetComputeRootSignature); SAME(pfnSetGraphicsRootSignature);
    SAME(pfnSetComputeRootDescriptorTable); SAME(pfnSetGraphicsRootDescriptorTable);
    SAME(pfnSetComputeRoot32BitConstant); SAME(pfnSetGraphicsRoot32BitConstant);
    SAME(pfnSetComputeRoot32BitConstants); SAME(pfnSetGraphicsRoot32BitConstants);
    SAME(pfnSetComputeRootConstantBufferView); SAME(pfnSetGraphicsRootConstantBufferView);
    SAME(pfnSetComputeRootShaderResourceView); SAME(pfnSetGraphicsRootShaderResourceView);
    SAME(pfnSetComputeRootUnorderedAccessView); SAME(pfnSetGraphicsRootUnorderedAccessView);
    SAME(pfnIASetIndexBuffer); SAME(pfnIASetVertexBuffers); SAME(pfnSOSetTargets); SAME(pfnOMSetRenderTargets);
    SAME(pfnSetMarker); SAME(pfnClearRootArguments);
#undef SAME
    commands->pfnAtomicCopyBufferRegion = d0108_atomic_copy;
    DDI_LIST_STUB(commands, pfnOMSetDepthBounds); DDI_LIST_STUB(commands, pfnSetSamplePositions);
    DDI_LIST_STUB(commands, pfnResourceResolveSubresourceRegion);
    DDI_NOOP(commands, pfnSetProtectedResourceSession);
    DDI_LIST_STUB(commands, pfnWriteBufferImmediate);
    DDI_NOOP(commands, pfnSetViewInstanceMask);
    DDI_LIST_STUB(commands, pfnInitializeMetaCommand); DDI_LIST_STUB(commands, pfnExecuteMetaCommand);
    DDI_LIST_STUB(commands, pfnBuildRaytracingAccelerationStructure);
    DDI_LIST_STUB(commands, pfnEmitRaytracingAccelerationStructurePostbuildInfo);
    DDI_LIST_STUB(commands, pfnCopyRaytracingAccelerationStructure);
    DDI_LIST_STUB(commands, pfnSetPipelineState1); DDI_LIST_STUB(commands, pfnDispatchRays);
    DDI_LIST_STUB(commands, pfnRSSetShadingRate); DDI_LIST_STUB(commands, pfnRSSetShadingRateImage);
    DDI_LIST_STUB(commands, pfnDispatchMesh); DDI_LIST_STUB(commands, pfnBarrier);
    DDI_NOOP(commands, pfnOmSetAlphaBlendFactor);
    commands->pfnOmSetFrontAndBackStencilRef = d0108_stencil_refs;
    DDI_NOOP(commands, pfnRSSetDepthBias); DDI_NOOP(commands, pfnIASetIndexBufferStripCutValue);
    DDI_LIST_STUB(commands, pfnSetProgram); DDI_LIST_STUB(commands, pfnDispatchGraph);
}
/* ---- 0109/0110: CREATERESOURCE_0109 = 0088 + CreateAtVirtualAddress ----- */
D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY d0109_heap_sizes(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001 *heap,
        const D3D12DDIARG_CREATERESOURCE_0109 *resource, D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session) {
    return d0108_heap_sizes(h, heap, reinterpret_cast<const D3D12DDIARG_CREATERESOURCE_0088 *>(resource), session);
}
HRESULT APIENTRY d0109_heap(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001 *heap, D3D12DDI_HHEAP slot,
        D3D12DDI_HRTRESOURCE runtime, const D3D12DDIARG_CREATERESOURCE_0109 *resource, const D3D12DDI_CLEAR_VALUES *clear,
        D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session, D3D12DDI_HRESOURCE resource_slot) {
    if (resource && resource->CreateAtVirtualAddress) return E_NOTIMPL; // RecreateAtTier not supported
    return d0108_heap(h, heap, slot, runtime, reinterpret_cast<const D3D12DDIARG_CREATERESOURCE_0088 *>(resource), clear,
        session, resource_slot);
}
VOID APIENTRY d0109_check_allocation(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATERESOURCE_0109 *resource,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags, UINT32 alignment, UINT nodes, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *out) {
    d0108_check_allocation(h, reinterpret_cast<const D3D12DDIARG_CREATERESOURCE_0088 *>(resource), flags, alignment, nodes, out);
}
void d0110_device(D3D12DDI_DEVICE_FUNCS_CORE_0109 *device) {
    static_assert(sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0109) == sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0108), "same layout");
    D3D12DDI_DEVICE_FUNCS_CORE_0108 device8{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0108 commands8{};
    d0108_tables(&device8, &commands8);
    std::memcpy(device, &device8, sizeof(*device));
    device->pfnCalcPrivateHeapAndResourceSizes = d0109_heap_sizes;
    device->pfnCreateHeapAndResource = d0109_heap;
    device->pfnCheckResourceAllocationInfo = d0109_check_allocation;
}

#undef DDI_STUB
#undef DDI_LIST_STUB
#undef DDI_NOOP
