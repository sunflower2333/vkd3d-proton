/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Included by ddi.cpp: one Context and one set of production DDI functions. */
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <type_traits>
#include <vector>

#include <cstdarg>
#include <cstdio>

namespace {
// Experimental public admission for bring-up against the real D3D12 runtime.
// Default stays closed; VIOGPU_D3D12_EXPERIMENTAL=1 opens the version/table gate
// and enables OutputDebugString tracing of runtime negotiation.
bool native_env_flag(const char *name) {
    char value[8]{};
    const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
    return n && n < sizeof(value) && value[0] == '1';
}
bool native_experimental() {
    static const bool enabled = native_env_flag("VIOGPU_D3D12_EXPERIMENTAL");
    return enabled;
}
bool native_tracing() {
    // Experimental bring-up traces; VIOGPU_D3D12_TRACE=0 silences them for timing runs.
    static const bool enabled = [] {
        char value[8]{};
        const DWORD n = GetEnvironmentVariableA("VIOGPU_D3D12_TRACE", value, sizeof(value));
        return native_experimental() && !(n && n < sizeof(value) && value[0] == '0');
    }();
    return enabled;
}
/* Opt-in CPU timing (VIOGPU_D3D12_TIMING=1): accumulated QPC per phase,
 * appended to C:\Users\Public\cc\d3d12-timing.txt every 600 frames. */
enum NativeTimer { NT_EXECUTE, NT_SUBMIT, NT_RENDER, NT_HOP_BEFORE, NT_HOP_AFTER, NT_REAP, NT_FACTS, NT_PRESENT,
    NT_ALLOC_RESET, NT_COUNT };
struct NativeTiming {
    std::atomic<uint64_t> ticks[NT_COUNT]{}, calls[NT_COUNT]{};
    std::atomic<uint64_t> frames{0};
    int64_t freq = 0;
};
NativeTiming &native_timing() { static NativeTiming t; return t; }
bool native_timing_on() {
    static const bool enabled = native_env_flag("VIOGPU_D3D12_TIMING");
    return enabled;
}
struct NativeTimed {
    NativeTimer which; int64_t start = 0;
    explicit NativeTimed(NativeTimer w) : which(w) {
        if (native_timing_on()) { LARGE_INTEGER v; QueryPerformanceCounter(&v); start = v.QuadPart; }
    }
    ~NativeTimed() {
        if (!start) return;
        LARGE_INTEGER v; QueryPerformanceCounter(&v);
        auto &t = native_timing();
        t.ticks[which] += uint64_t(v.QuadPart - start); ++t.calls[which];
    }
};
void native_timing_frame() {
    if (!native_timing_on()) return;
    auto &t = native_timing();
    const uint64_t frame = ++t.frames;
    if (frame % 600) return;
    if (!t.freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); t.freq = f.QuadPart; }
    static const char *names[NT_COUNT] = {"execute", "submit", "render", "hop_before", "hop_after", "reap", "facts",
        "present", "alloc_reset"};
    char line[1024]; int n = std::snprintf(line, sizeof(line), "pid=%lu frames=%llu", GetCurrentProcessId(),
        (unsigned long long)frame);
    for (int i = 0; i < NT_COUNT; ++i) {
        const uint64_t ticks = t.ticks[i].exchange(0), calls = t.calls[i].exchange(0);
        n += std::snprintf(line + n, sizeof(line) - n, " %s=%llu/%.3fms", names[i], (unsigned long long)calls,
            calls ? 1000.0 * double(ticks) / double(t.freq) / double(calls) : 0.0);
    }
    std::snprintf(line + n, sizeof(line) - n, "\r\n");
    HANDLE f = CreateFileA("C:\\Users\\Public\\cc\\d3d12-timing.txt", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(f, line, (DWORD)std::strlen(line), &w, nullptr); CloseHandle(f); }
}
void native_trace(const char *format, ...) {
    if (!native_tracing()) return;
    char line[512];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (n < 0) return;
    size_t len = std::strlen(line);
    line[len] = '\n'; line[len + 1] = 0;
    OutputDebugStringA("[viogpud3d12] ");
    OutputDebugStringA(line);
}

struct NativeAdapter : NativeAdapterFacts {
    D3D12DDI_HRTADAPTER runtime{};
    D3DDDI_ADAPTERCALLBACKS callbacks{};
    // Runtime handles of the filled command-list table copies; a driver
    // selects one per command list with pfnSetCommandListDDITableCb.
    std::array<D3D12DDI_HRTTABLE, 4> command_list_tables{};
    // Highest D3D_SHADER_MODEL of the embedded device, learnt at CreateDevice
    // (the runtime asks SHADER_MODELS only after a device exists).
    std::atomic<uint32_t> shader_model{0};
};
std::mutex native_adapters_lock;
std::unordered_map<void *, std::shared_ptr<NativeAdapter>> native_adapters;

std::shared_ptr<NativeAdapter> native_adapter(D3D12DDI_HADAPTER handle) {
    std::lock_guard<std::mutex> lock(native_adapters_lock);
    auto found = native_adapters.find(handle.pDrvPrivate);
    return found == native_adapters.end() ? nullptr : found->second;
}

HRESULT native_query_adapter_facts(const NativeAdapter &adapter, NativeAdapterFacts &facts,
        bool require_queue_protocol = false) {
    // VIOGPU v0 128-byte prefix + independently versioned VLID trailer.
    // Runtime queues additionally require the independently versioned VQDC
    // trailer. Old KMDs write only160 bytes and leave its zeroed tail absent.
    // Never infer shared-domain support from a package or driver version.
    // Output-only callback: never encode a request in its output buffer.
    NativeTimed timed(NT_FACTS);
    std::array<uint8_t, 192> reply{};
    const UINT expected_size = require_queue_protocol ? 192u : 160u;
    D3DDDICB_QUERYADAPTERINFO args{};
    args.pPrivateDriverData = reply.data();
    args.PrivateDriverDataSize = expected_size;
    HRESULT hr = adapter.callbacks.pfnQueryAdapterInfoCb(adapter.runtime.handle, &args);
    if (FAILED(hr)) { native_trace("QueryAdapterInfoCb size=%u hr=%08x", expected_size, (unsigned)hr); return hr; }
    if (args.PrivateDriverDataSize != expected_size || args.pPrivateDriverData != reply.data())
        return DXGI_ERROR_UNSUPPORTED;
    auto u32 = [&](size_t i) {
        return uint32_t(reply[i]) | (uint32_t(reply[i+1]) << 8) |
            (uint32_t(reply[i+2]) << 16) | (uint32_t(reply[i+3]) << 24);
    };
    if (u32(0) != 0x504d5644 || u32(4) || u32(8) != 128 || u32(12) || u32(16) || u32(20) ||
            u32(112) || u32(116) || u32(120) || u32(124) ||
            u32(128) != 0x44494c56 || u32(132) != 1 || u32(136) != 32 ||
            u32(140) != 1 || u32(152) != 1 || u32(156)) return DXGI_ERROR_UNSUPPORTED;
    if (require_queue_protocol && (u32(160) != 0x43445156 || u32(164) != 1 ||
            u32(168) != 32 || u32(172) != 1 || u32(176) != 32 || u32(180) != 40 ||
            u32(184) != 64 || u32(188))) return DXGI_ERROR_UNSUPPORTED;
    // HasCachedCoherentMemory is an existing UINT32 boolean at byte76 of the
    // original prefix. It is not a VQDC flag or a complete ICD memory profile.
    if (u32(76) > 1) return DXGI_ERROR_UNSUPPORTED;
    NativeAdapterFacts current{};
    current.generation = uint64_t(u32(24)) | (uint64_t(u32(28)) << 32);
    std::memcpy(current.luid.data(), reply.data() + 144, current.luid.size());
    if (!current.generation || current.luid == std::array<uint8_t, 8>{}) return DXGI_ERROR_UNSUPPORTED;
    current.has_cached_coherent_memory = {true, u32(76) == 1};
    if (require_queue_protocol) current.owned_allocations_only = {true, true};
    // Commit only after the complete response is validated. Callers use
    // independent storage across runtime callbacks.
    facts = current;
    return S_OK;
}

bool native_same_identity_facts(const NativeAdapterFacts &a, const NativeAdapterFacts &b) {
    return a.luid == b.luid && a.generation == b.generation &&
        a.has_cached_coherent_memory.known == b.has_cached_coherent_memory.known &&
        (!a.has_cached_coherent_memory.known ||
         a.has_cached_coherent_memory.value == b.has_cached_coherent_memory.value);
}

bool native_owned_facts_valid(const NativeAdapterFacts &facts) {
    return facts.has_cached_coherent_memory.known && facts.has_cached_coherent_memory.value &&
        facts.owned_allocations_only.known && facts.owned_allocations_only.value;
}

bool native_experimental();
bool native_version(UINT interface_version, UINT runtime_version, D3D12DDI_CREATE_DEVICE_FLAGS flags) {
    // The only actual implemented table layouts. Never reinterpret newer tables.
    if (static_cast<UINT>(flags) & ~static_cast<UINT>(D3D12DDI_CREATE_DEVICE_FLAG_DISABLE_IMPLICIT_MGPU)) return false;
    if (native_experimental())
        return interface_version == D3D12DDI_INTERFACE_VERSION_R8 && (runtime_version >> 16) >= D3D12DDI_BUILD_VERSION_0110;
    return interface_version == D3D12DDI_INTERFACE_VERSION_R0 && (runtime_version >> 16) >= D3D12DDI_BUILD_VERSION;
}

bool native_contract_complete() {
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    VioGpuD3D12BridgeGetTables(&device, &commands, &queue);
    // These lifecycle entrypoints are wired, but are only prerequisites.
    // Non-null callbacks do not certify their complete feature-level semantics.
    // Full FL11_0 graphics/format/residency/Present validation must replace this
    // gate as one coherent change before any version is advertised.
    const bool prerequisites = device.pfnCreateHeapAndResource && device.pfnCreateFence &&
        device.pfnMakeResident && queue.pfnSignalFence && queue.pfnWaitForFence &&
        commands.pfnDrawInstanced && commands.pfnDrawIndexedInstanced;
    // No complete native feature-level contract yet; only the explicit
    // experimental opt-in may publish the partial tables for bring-up.
    return prerequisites && native_experimental();
}

HRESULT APIENTRY native_versions(D3D12DDI_HADAPTER handle, UINT32 *count, UINT64 *versions) {
    if (!native_adapter(handle) || !count) return E_INVALIDARG;
    const UINT32 capacity = *count;
    *count = native_contract_complete() ? 1u : 0u;
    native_trace("GetSupportedVersions capacity=%u count=%u versions=%p", capacity, *count, (void *)versions);
    if (!versions || !*count) return S_OK;
    if (capacity < *count) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    versions[0] = native_experimental() ? D3D12DDI_SUPPORTED_0110 : D3D12DDI_SUPPORTED_0003;
    return S_OK;
}

HRESULT APIENTRY native_optional_tables(D3D12DDI_HADAPTER handle, UINT32 *count, D3D12DDI_TABLE_REQUEST *) {
    if (!native_adapter(handle) || !count) return E_INVALIDARG;
    native_trace("GetOptionalDDITables capacity=%u", *count);
    *count = 0;
    return S_OK;
}

HRESULT native_adapter_memory_facts(D3D12DDI_HADAPTER handle, vkdu_memory_facts *output);

HRESULT native_caps_impl(D3D12DDI_HADAPTER handle, const D3D12DDIARG_GETCAPS *args);
// Bring-up answers for the R0 base capability families. Conservative: FL11_0,
// binding/heap tier 1, no optional shader features, no tiled resources.
// Returns S_FALSE for types handled by the regular path.
HRESULT native_experimental_caps(const D3D12DDIARG_GETCAPS &request, uint32_t shader_model) {
    auto put = [&](const void *value, size_t size) -> HRESULT {
        if (request.DataSize != size) return E_INVALIDARG;
        std::memcpy(request.pData, value, size);
        return S_OK;
    };
    switch (static_cast<UINT>(request.Type)) {
    case D3D12DDICAPS_TYPE_3DPIPELINESUPPORT: {
        const D3D12DDI_3DPIPELINELEVEL level = D3D12DDI_3DPIPELINELEVEL_11_0;
        return put(&level, sizeof(level));
    }
    case D3D12DDICAPS_TYPE_0081_3DPIPELINESUPPORT1: {
        if (request.DataSize != sizeof(D3D12DDI_3DPIPELINESUPPORT1_DATA_0081)) return E_INVALIDARG;
        D3D12DDI_3DPIPELINESUPPORT1_DATA_0081 data;
        std::memcpy(&data, request.pData, sizeof(data));
        data.MaximumDriverSupportedFeatureLevel = D3D12DDI_3DPIPELINELEVEL_11_0;
        return put(&data, sizeof(data));
    }
    case D3D12DDICAPS_TYPE_SHADER: {
        // Revisions 0003/0012/0015/0042/0082/0084 append to the same prefix.
        if (request.DataSize != 20 && request.DataSize != 36 && request.DataSize != 40 && request.DataSize != 44 &&
                request.DataSize != 60 && request.DataSize != 64) return E_INVALIDARG;
        D3D12DDI_SHADER_CAPS_0084 caps{};
        // No optional shader features; wave intrinsics stay disabled, but the
        // lane geometry is reported truthfully for the Adreno 830 (wave64).
        caps.WaveLaneCountMin = 64; caps.WaveLaneCountMax = 64; caps.TotalLaneCount = 2048;
        std::memcpy(request.pData, &caps, request.DataSize);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_ARCHITECTURE_INFO: {
        D3D12DDI_ARCHITECTURE_INFO_DATA info{};
        info.TileBasedDeferredRenderer = TRUE;
        return put(&info, sizeof(info));
    }
    case D3D12DDICAPS_TYPE_D3D12_OPTIONS: {
        // Every revision (0003 ... 0089) appends to the 0003 prefix. Zero is
        // "not supported" for every later tier/boolean (render passes,
        // raytracing, mesh shaders, VRS, enhanced barriers, ...).
        static const UINT sizes[] = {28, 32, 36, 40, 44, 52, 60, 64, 68, 72, 92, 100, 104, 108, 116, 120, 124};
        bool known = false;
        for (UINT size : sizes) known = known || request.DataSize == size;
        if (!known) return E_INVALIDARG;
        D3D12DDI_D3D12_OPTIONS_DATA_0003 options{};
        options.ResourceBindingTier = D3D12DDI_RESOURCE_BINDING_TIER_1;
        options.ConservativeRasterizationTier = D3D12DDI_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
        options.TiledResourcesTier = D3D12DDI_TILED_RESOURCES_TIER_NOT_SUPPORTED;
        options.CrossNodeSharingTier = D3D12DDI_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
        options.ResourceHeapTier = D3D12DDI_RESOURCE_HEAP_TIER_1;
        std::memset(request.pData, 0, request.DataSize);
        std::memcpy(request.pData, &options, sizeof(options));
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT: {
        // Only the opaque UNDEFINED layout: no device-dependent or standard
        // swizzles exposed to applications, no row-major textures.
        if (request.DataSize == sizeof(D3D12DDI_TEXTURE_LAYOUT_CAPS_0001)) {
            const D3D12DDI_TEXTURE_LAYOUT_CAPS_0001 caps{};
            return put(&caps, sizeof(caps));
        }
        const D3D12DDI_TEXTURE_LAYOUT_CAPS caps{};
        return put(&caps, sizeof(caps));
    }
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS: {
        // Row-major copy footprints: the API's own placement rules
        // (512-byte offset, 256-byte pitch) are what vkCmdCopyBufferToImage
        // paths are built for; report them for every functional unit.
        if (!request.pInfo) return E_INVALIDARG;
        UINT info[2];
        std::memcpy(info, request.pInfo, sizeof(info));
        if (info[0] != D3D12DDI_TL_ROW_MAJOR) return DXGI_ERROR_UNSUPPORTED;
        D3D12DDI_ROW_MAJOR_LAYOUT_CAPS caps{};
        for (auto &sub : caps.SubCaps) {
            sub.MaxElementSize = 16;
            sub.BaseOffsetAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
            sub.PitchAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
            sub.DepthPitchAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        }
        caps.Flags = D3D12DDI_ROW_MAJOR_LAYOUT_FLAG_NONE;
        return put(&caps, sizeof(caps));
    }
    case D3D12DDICAPS_TYPE_0011_SHADER_MODELS: {
        // Native DDI shaders reach the backend as SM4/SM5 token streams; DXIL
        // (SM6) is not admitted through this path yet.
        if (request.DataSize != sizeof(D3D12DDI_D3D12_SHADER_MODELS_DATA_0011)) return E_INVALIDARG;
        D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 data;
        std::memcpy(&data, request.pData, sizeof(data));
        if (!data.pNumShaderModelsSupported) return E_INVALIDARG;
        // DXIL reaches the embedded vkd3d-proton (dxil-spirv) in a container;
        // list every released model up to what that device supports.
        static const D3D12DDI_SHADER_MODEL models[] = {D3D12DDI_SHADER_MODEL_5_1_RELEASE_0011,
            D3D12DDI_SHADER_MODEL_6_0_RELEASE_0011, D3D12DDI_SHADER_MODEL_6_1_RELEASE_0033,
            D3D12DDI_SHADER_MODEL_6_2_RELEASE_0042, D3D12DDI_SHADER_MODEL_6_3_RELEASE_0054,
            D3D12DDI_SHADER_MODEL_6_4_RELEASE_0062, D3D12DDI_SHADER_MODEL_6_5_RELEASE_0071,
            D3D12DDI_SHADER_MODEL_6_6_RELEASE_0082, D3D12DDI_SHADER_MODEL_6_7_RELEASE_0093,
            D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108};
        static const uint32_t api[] = {0x51, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68};
        UINT count = 1;
        while (count < ARRAYSIZE(models) && shader_model >= api[count]) ++count;
        if (data.pShaderModelsSupported) {
            if (*data.pNumShaderModelsSupported < count) return E_INVALIDARG;
            for (UINT i = 0; i < count; ++i) data.pShaderModelsSupported[i] = models[i];
        }
        *data.pNumShaderModelsSupported = count;
        native_trace("SHADER_MODELS backend=%x count=%u", shader_model, count);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0022_TEXTURE_LAYOUT: {
        // Same answer as the R0 query: only the opaque layout, no device
        // dependent or standard swizzles, no row-major textures.
        if (request.DataSize != sizeof(D3D12DDI_TEXTURE_LAYOUT_CAPS_0026) &&
                request.DataSize != sizeof(D3D12DDI_TEXTURE_LAYOUT_CAPS_0001)) return E_INVALIDARG;
        std::memset(request.pData, 0, request.DataSize);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0090:   // no relaxed format casting
    case D3D12DDICAPS_TYPE_OPTIONS_0091:   // no unrestricted pitch/alignment, no inverted viewports
    case D3D12DDICAPS_TYPE_OPTIONS_0093:   // no advanced texture ops / writable MSAA textures
    case D3D12DDICAPS_TYPE_OPTIONS_0098:   // L1 not CPU accessible (UMA L0 only)
    case D3D12DDICAPS_TYPE_OPTIONS_0101:   // no compute-only custom heaps
    case D3D12DDICAPS_TYPE_OPTIONS_0109:   // no recreate-at
    case D3D12DDICAPS_TYPE_OPTIONS_0110:   // execute indirect tier 1.0 (value 0)
        if (request.DataSize > 64) return E_INVALIDARG;
        std::memset(request.pData, 0, request.DataSize);
        return S_OK;
    case D3D12DDICAPS_TYPE_OPTIONS_0102: {
        if (request.DataSize != sizeof(D3D12DDI_OPTIONS_DATA_0102)) return E_INVALIDARG;
        D3D12DDI_OPTIONS_DATA_0102 limits{};
        limits.SupportedSampleCountsWithNoOutputs = 1;
        // The runtime requires >= 4000 at FL11+ (API shader-visible heaps stay <= 2048).
        limits.MaxSamplerDescriptorHeapSize = 4000;
        limits.MaxSamplerDescriptorHeapSizeWithStaticSamplers = 2048;
        limits.MaxViewDescriptorHeapSize = 1000000;
        return put(&limits, sizeof(limits));
    }
    case D3D12DDICAPS_TYPE_0022_CPU_PAGE_TABLE_FALSE_POSITIVES:  // queue flags: none
    case D3D12DDICAPS_TYPE_0023_UMD_BASED_COMMAND_QUEUE_PRIORITY: // no global realtime queues
    case D3D12DDICAPS_TYPE_0033_ADAPTER_COMPUTE_ONLY:             // full 3D adapter
    case D3D12DDICAPS_TYPE_0050_HARDWARE_SCHEDULING_CAPS:         // no scheduling groups
    case D3D12DDICAPS_TYPE_EXECUTECOMMANDLISTS_PARALLELISM:       // serialized execution
    case D3D12DDICAPS_TYPE_0073_SUPPORT_BATCHED_MARKERS: {        // no batched markers
        if (request.DataSize != 4) return E_INVALIDARG;
        const UINT zero = 0;
        return put(&zero, sizeof(zero));
    }
    case D3D12DDICAPS_TYPE_0074_PROTECTED_RESOURCE_SESSION_TYPE_COUNT: {
        if (request.DataSize != sizeof(D3D12DDI_PROTECTED_RESOURCE_SESSION_TYPE_COUNT_DATA_0074)) return E_INVALIDARG;
        auto *data = static_cast<D3D12DDI_PROTECTED_RESOURCE_SESSION_TYPE_COUNT_DATA_0074 *>(request.pData);
        data->Count = 0;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_GPUVA_CAPS: {
        D3D12DDI_GPUVA_CAPS_0004 caps{};
        caps.MaxGPUVirtualAddressBitsPerResource = 40;
        return put(&caps, sizeof(caps));
    }
    default:
        return S_FALSE;
    }
}
HRESULT APIENTRY native_caps(D3D12DDI_HADAPTER handle, const D3D12DDIARG_GETCAPS *args) {
    const HRESULT hr = native_caps_impl(handle, args);
    if (args) {
        UINT first = 0;
        if (args->pInfo) std::memcpy(&first, args->pInfo, sizeof(first));
        UINT second = 0;
        if (args->pInfo && args->Type == D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS)
            std::memcpy(&second, static_cast<const UINT *>(args->pInfo) + 1, sizeof(second));
        native_trace("GetCaps type=%u info=%u,%u size=%u hr=%08x", (unsigned)args->Type, first, second,
            (unsigned)args->DataSize, (unsigned)hr);
    }
    return hr;
}
HRESULT native_caps_impl(D3D12DDI_HADAPTER handle, const D3D12DDIARG_GETCAPS *args) {
    if (!native_adapter(handle) || !args) return E_INVALIDARG;
    // Runtime callbacks may invalidate the request and node storage. Retain
    // only entry snapshots; the facts query separately validates registration.
    const auto request = *args;
    if (!request.pData || !request.DataSize) return E_INVALIDARG;
    if (native_experimental()) {
        HRESULT experimental = native_experimental_caps(request, native_adapter(handle)->shader_model.load());
        if (experimental != S_FALSE) return experimental;
    }
    if (request.Type != D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE) return DXGI_ERROR_UNSUPPORTED;
    static_assert(sizeof(D3D12DDI_MEMORY_ARCHITECTURE_CAPS) == 12, "original R0 memory caps");
    const bool serialization_tiers = native_experimental() &&
        request.DataSize == sizeof(D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041);
    if (!request.pInfo || (request.DataSize != sizeof(D3D12DDI_MEMORY_ARCHITECTURE_CAPS) && !serialization_tiers))
        return E_INVALIDARG;
    UINT node;
    std::memcpy(&node, request.pInfo, sizeof(node));
    if (node) return E_INVALIDARG;
    vkdu_memory_facts facts{};
    HRESULT hr = native_adapter_memory_facts(handle, &facts);
    if (FAILED(hr)) return hr;
    // S_OK can preserve valid raw false/unknown facts. Those facts cannot be
    // represented by publishing three false BOOLs as a different architecture.
    if (!vkdu_memory_facts_qualified(&facts)) return DXGI_ERROR_UNSUPPORTED;
    const D3D12DDI_MEMORY_ARCHITECTURE_CAPS caps{
        facts.uma.value ? TRUE : FALSE,
        facts.io_coherent.value ? TRUE : FALSE,
        facts.cache_coherent.value ? TRUE : FALSE};
    if (serialization_tiers) std::memset(request.pData, 0, request.DataSize); // heap/resource serialization tier 0
    std::memcpy(request.pData, &caps, sizeof(caps));
    // This metadata answer does not certify a complete native feature level.
    // Supported versions and public table publication remain closed.
    return S_OK;
}

void d0108_tables(D3D12DDI_DEVICE_FUNCS_CORE_0108 *, D3D12DDI_COMMAND_LIST_FUNCS_3D_0108 *);
void d0108_extended_features(D3D12DDI_EXTENDED_FEATURES_FUNCS_0096 *);
void d0110_device(D3D12DDI_DEVICE_FUNCS_CORE_0109 *);
HRESULT APIENTRY native_fill_table(D3D12DDI_HADAPTER handle, D3D12DDI_TABLE_TYPE type,
        void *table, SIZE_T bytes, UINT index, D3D12DDI_HRTTABLE runtime_table) {
    if (type == D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D && index < 4) {
        if (auto adapter = native_adapter(handle)) adapter->command_list_tables[index] = runtime_table;
    }
    if (!native_adapter(handle) || !table) return E_INVALIDARG;
    // The runtime fills multi-instance tables (command lists) once per index;
    // every instance receives the same entry points.
    if (index && !(native_experimental() && type == D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D)) {
        native_trace("FillDDITable type=%u index=%u rejected", (unsigned)type, index);
        return E_INVALIDARG;
    }
    SIZE_T expected = 0;
    const bool r8 = native_experimental();
    switch (type) {
        case D3D12DDI_TABLE_TYPE_DEVICE_CORE:
            expected = r8 ? sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0108) : sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0003); break;
        case D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D:
            expected = r8 ? sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0108) : sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003); break;
        case D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D: expected = sizeof(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001); break;
        case D3D12DDI_TABLE_TYPE_0096_EXTENDED_FEATURES:
            if (!r8 || bytes != sizeof(D3D12DDI_EXTENDED_FEATURES_FUNCS_0096)) return DXGI_ERROR_UNSUPPORTED;
            {
                D3D12DDI_EXTENDED_FEATURES_FUNCS_0096 features{};
                d0108_extended_features(&features);
                std::memcpy(table, &features, sizeof(features));
            }
            native_trace("FillDDITable type=27 extended features (none)");
            return S_OK;
        default:
            native_trace("FillDDITable type=%u bytes=%zu unsupported", (unsigned)type, (size_t)bytes);
            return DXGI_ERROR_UNSUPPORTED;
    }
    native_trace("FillDDITable type=%u index=%u bytes=%zu expected=%zu", (unsigned)type, index, (size_t)bytes, (size_t)expected);
    if (bytes != expected) return E_INVALIDARG;
    // Leave caller memory untouched on failure, including tables with a newer
    // layout. Never let the runtime call a partially initialized callback table.
    if (!native_contract_complete()) return DXGI_ERROR_UNSUPPORTED;
    D3D12DDI_DEVICE_FUNCS_CORE_0003 device{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    VioGpuD3D12BridgeGetTables(&device, &commands, &queue);
    if (r8) {
        D3D12DDI_DEVICE_FUNCS_CORE_0108 device8{};
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0108 commands8{};
        d0108_tables(&device8, &commands8);
        D3D12DDI_DEVICE_FUNCS_CORE_0109 device9{};
        d0110_device(&device9);
        std::memcpy(&device8, &device9, sizeof(device8));
        const void *source = type == D3D12DDI_TABLE_TYPE_DEVICE_CORE ? static_cast<const void *>(&device8) :
            type == D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D ? static_cast<const void *>(&commands8) : &queue;
        const auto *slots = static_cast<void *const *>(source);
        char nulls[400] = {};
        size_t used = 0;
        for (size_t i = 0; i < bytes / sizeof(void *) && used + 6 < sizeof(nulls); ++i)
            if (!slots[i]) used += std::snprintf(nulls + used, sizeof(nulls) - used, " %zu", i);
        native_trace("FillDDITable 0108 type=%u null slots:%s", (unsigned)type, nulls);
        std::memcpy(table, source, bytes);
        return S_OK;
    }
    if (native_experimental()) {
        const void *source = type == D3D12DDI_TABLE_TYPE_DEVICE_CORE ? static_cast<const void *>(&device) :
            type == D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D ? static_cast<const void *>(&commands) : &queue;
        const auto *slots = static_cast<void *const *>(source);
        char nulls[400] = {};
        size_t used = 0;
        for (size_t i = 0; i < bytes / sizeof(void *) && used + 6 < sizeof(nulls); ++i)
            if (!slots[i]) used += std::snprintf(nulls + used, sizeof(nulls) - used, " %zu", i);
        native_trace("FillDDITable type=%u null slots:%s", (unsigned)type, nulls);
    }
    if (type == D3D12DDI_TABLE_TYPE_DEVICE_CORE) std::memcpy(table, &device, bytes);
    else if (type == D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D) std::memcpy(table, &commands, bytes);
    else std::memcpy(table, &queue, bytes);
    return S_OK;
}

SIZE_T APIENTRY native_device_size(D3D12DDI_HADAPTER handle, const D3D12DDIARG_CALCPRIVATEDEVICESIZE *args) {
    if (args) native_trace("CalcPrivateDeviceSize interface=%u version=%08x flags=%x", (unsigned)args->Interface,
        (unsigned)args->Version, (unsigned)args->Flags);
    return native_adapter(handle) && args && native_version(args->Interface, args->Version, args->Flags)
        ? sizeof(NativeDevice) : 0;
}

void APIENTRY native_report(void *opaque, HRESULT hr) {
    auto *ctx = static_cast<Context *>(opaque);
    native_trace("SetErrorCb hr=%08x caller=%p", (unsigned)hr, __builtin_return_address(0));
    if (ctx->runtime_device.handle && ctx->runtime_callbacks.pfnSetErrorCb)
        ctx->runtime_callbacks.pfnSetErrorCb(ctx->runtime_device, hr);
}

HRESULT native_load_vulkan(HMODULE *module, PFN_vkGetInstanceProcAddr *loader) {
    *module = nullptr; *loader = nullptr;
    // Match the active process ABI through the normal system Vulkan loader.
    // Do not search the application's working directory or mutate its DLL path.
    HMODULE loaded = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loaded) return HRESULT_FROM_WIN32(GetLastError());
    auto entry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loaded, "vkGetInstanceProcAddr"));
    if (!entry) { FreeLibrary(loaded); return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND); }
    *module = loaded; *loader = entry;
    return S_OK;
}

vkdu_memory_expectation native_memory_expectation(const NativeAdapterFacts &facts) {
    vkdu_memory_expectation expected{};
    std::memcpy(expected.luid, facts.luid.data(), 8);
    expected.generation = facts.generation;
    expected.cached_coherent = {facts.has_cached_coherent_memory.known, facts.has_cached_coherent_memory.value};
    expected.owned_allocations_only = {facts.owned_allocations_only.known, facts.owned_allocations_only.value};
    return expected;
}

// Internal future caps input. The public GetCaps/version gates remain closed.
// Only independent local storage survives callbacks; no VkPhysicalDevice handle
// escapes the instance owned by the backend query.
HRESULT native_adapter_memory_facts(D3D12DDI_HADAPTER handle, vkdu_memory_facts *output) {
    auto adapter = native_adapter(handle);
    if (!adapter || !output) return E_INVALIDARG;
    NativeAdapterFacts before{}, after{};
    HRESULT hr = native_query_adapter_facts(*adapter, before, true);
    if (native_adapter(handle) != adapter) return DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(hr)) return hr;
    if (!native_same_identity_facts(before, *adapter)) return DXGI_ERROR_DEVICE_REMOVED;
    const auto expected = native_memory_expectation(before);
    vkdu_memory_facts result{};
    HMODULE module = nullptr;
    PFN_vkGetInstanceProcAddr loader = nullptr;
    hr = native_load_vulkan(&module, &loader);
    if (native_adapter(handle) != adapter) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) hr = vkdu_query_adapter_memory_facts(loader, &expected, &result);
    if (module) FreeLibrary(module);
    if (native_adapter(handle) != adapter) return DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(hr)) return hr;
    hr = native_query_adapter_facts(*adapter, after, true);
    if (native_adapter(handle) != adapter) return DXGI_ERROR_DEVICE_REMOVED;
    if (hr == DXGI_ERROR_UNSUPPORTED) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(hr)) return hr;
    if (!native_same_identity_facts(before, after) ||
            !after.owned_allocations_only.known || !after.owned_allocations_only.value)
        return DXGI_ERROR_DEVICE_REMOVED;
    *output = result;
    return S_OK;
}

#include "native_heap.inc"
#include "native_open.inc"
#include "native_primary.inc"
#include "native_buffer_child.inc"
#include "native_samplers.inc"
#include "native_textures.inc"
#include "native_graphics.inc"
#include "native_fences.inc"
#include "native_node_metadata.inc"
#include "native_query_heap.inc"
#include "native_residency.inc"
#include "native_present.inc"
#include "native_aux.inc"

// error_mutex owns these lists. Compare opaque slots before dereferencing them.
Object *native_submission_find(Object *head, void *slot) {
    for (auto *entry = head; entry; entry = entry->next)
        if (entry == slot) return entry;
    return nullptr;
}
void native_submission_remove(Object *&head, Object *entry) {
    Object **link = &head;
    while (*link && *link != entry) link = &(*link)->next;
    if (*link) *link = entry->next;
}

#include "native_indirect.inc"

HRESULT native_queue_create(Context *ctx, const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args) {
    NativeHeapOperation op(ctx);
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    // R0 has no priority field. Never reinterpret a later queue revision.
    const auto request = *args;
    const auto type = command_type(request.QueueFlags);
    native_trace("CreateCommandQueue enter flags=%x type=%x node=%x drv=%p rt=%p", (unsigned)request.QueueFlags, type,
        request.NodeMask, request.hDrvCommandQueue.pDrvPrivate, request.hRTCommandQueue.handle);
    if (!request.hDrvCommandQueue.pDrvPrivate || !request.hRTCommandQueue.handle || type == UINT32_MAX ||
        native_submission_find(ctx->native_queue_objects, request.hDrvCommandQueue.pDrvPrivate) ||
        native_submission_find(ctx->native_command_objects, request.hDrvCommandQueue.pDrvPrivate)) return E_INVALIDARG;
    vkdu_object *value = nullptr;
    NativeRuntimeQueue *route = nullptr;
    HRESULT hr = native_runtime_queue_open(ctx, request.hRTCommandQueue, &route);
    native_trace("CreateCommandQueue flags=%x runtime queue open hr=%08x", (unsigned)request.QueueFlags, (unsigned)hr);
    if (FAILED(hr)) return hr;
    {
        NativeBackendCall call(ctx);
        hr = vkdu_queue_create(ctx->backend, type, &value);
        if (SUCCEEDED(hr)) hr = vkdu_queue_bind_runtime(value, ctx, route);
    }
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) hr = bind(ctx, request.hDrvCommandQueue.pDrvPrivate, value, VKDU_QUEUE);
    if (FAILED(hr)) {
        {
            NativeBackendCall call(ctx);
            vkdu_object_destroy(value);
        }
        native_runtime_queue_close(ctx, route);
        native_runtime_queue_release_record(ctx, route);
        return hr;
    }
    auto *queue = object(request.hDrvCommandQueue.pDrvPrivate);
    queue->runtime_queue = request.hRTCommandQueue;
    queue->runtime_route = route;
    queue->next = ctx->native_queue_objects;
    ctx->native_queue_objects = queue;
    return S_OK;
}

void native_queue_destroy(Context *ctx, void *slot) {
    NativeHeapOperation op(ctx, true);
    if (!op.retained) return;
    auto *queue = native_submission_find(ctx->native_queue_objects, slot);
    if (!queue) { error(ctx, E_INVALIDARG); return; }
    auto *value = queue->backend;
    auto *route = queue->runtime_route;
    native_submission_remove(ctx->native_queue_objects, queue);
    // The runtime may release this memory immediately, including reentrantly.
    std::memset(queue, 0, sizeof(*queue));
    {
        NativeBackendCall call(ctx);
        // All translation/Render callbacks must finish while hRTCommandQueue
        // is still valid. The embedded queue destructor drains remaining GPU work.
        vkdu_object_destroy(value);
    }
    HRESULT hr = op.live() ? native_runtime_queue_drain(ctx, route) : DXGI_ERROR_DEVICE_REMOVED;
    const HRESULT close_hr = native_runtime_queue_close(ctx, route);
    if (SUCCEEDED(hr)) hr = close_hr;
    native_runtime_queue_release_record(ctx, route);
    if (op.live() && FAILED(hr)) error(ctx, hr);
    release(ctx);
}

void native_queue_execute(Object *queue, UINT count, const D3D12DDI_HCOMMANDLIST *commands) {
    auto *ctx = queue->context;
    NativeHeapOperation op(ctx);
    if (!op.live()) { error(ctx, DXGI_ERROR_DEVICE_REMOVED); return; }
    if (!native_submission_find(ctx->native_queue_objects, queue) || !commands || !count || count > 64) {
        error(ctx, E_INVALIDARG); return;
    }
    if (queue->runtime_route && queue->runtime_route->preparing_present) {
        error(ctx, DXGI_ERROR_WAS_STILL_DRAWING); return;
    }
    vkdu_object *native[64]{};
    auto *queue_backend = queue->backend;
    if (!vkdu_object_retain(queue_backend)) { error(ctx, E_OUTOFMEMORY); return; }
    UINT retained = 0;
    HRESULT hr = S_OK;
    for (; retained < count; ++retained) {
        auto *command = native_submission_find(ctx->native_command_objects, commands[retained].pDrvPrivate);
        if (!command || command->kind != VKDU_COMMAND_LIST || !vkdu_object_retain(command->backend)) {
            hr = E_INVALIDARG; break;
        }
        native[retained] = command->backend;
    }
    {
        // Copies and independent backend references outlive every runtime slot.
        // A backend callback may destroy queue/list/device and poison all slots.
        NativeBackendCall call(ctx);
        if (SUCCEEDED(hr)) { NativeTimed timed(NT_EXECUTE); hr = vkdu_queue_execute(queue_backend, count, native); }
        for (UINT index = 0; index < retained; ++index) vkdu_object_destroy(native[index]);
        vkdu_object_destroy(queue_backend);
    }
    // Never report through the queue slot after backend execution/cleanup.
    // error(Context) respects device callback detachment during retirement.
    error(ctx, hr);
}

HRESULT native_command_create(Context *ctx, const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args) {
    NativeHeapOperation op(ctx);
    if (!op.live()) return DXGI_ERROR_DEVICE_REMOVED;
    // The runtime binds a new list to its device-removed table until the
    // driver selects one of the command-list tables it filled.
    if (ctx->runtime_callbacks.pfnSetCommandListDDITableCb && ctx->native_adapter &&
            ctx->native_adapter->command_list_tables[0].handle && args->hRTCommandList.handle)
        ctx->runtime_callbacks.pfnSetCommandListDDITableCb(args->hRTCommandList, ctx->native_adapter->command_list_tables[0]);
    if (!args->hDrvCommandList.pDrvPrivate || !args->hRTCommandList.handle ||
            !ctx->runtime_callbacks.pfnSetCommandListErrorCb) return E_INVALIDARG;
    // Copy all runtime-owned input before a backend callback can retire it.
    void *slot = args->hDrvCommandList.pDrvPrivate;
    const auto runtime = args->hRTCommandList;
    auto *allocator = backend(args->hDrvCommandAllocator.pDrvPrivate, VKDU_ALLOCATOR);
    const auto type = command_list_type(args->Type, args->QueueFlags);
    vkdu_object *value = nullptr;
    HRESULT hr;
    {
        NativeBackendCall call(ctx);
        hr = vkdu_command_create(ctx->backend, allocator, type, &value);
    }
    if (!op.live()) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(hr)) hr = bind(ctx, slot, value, VKDU_COMMAND_LIST);
    if (FAILED(hr)) {
        NativeBackendCall call(ctx);
        vkdu_object_destroy(value);
        return hr;
    }
    object(slot)->runtime_command = runtime;
    object(slot)->next = ctx->native_command_objects;
    ctx->native_command_objects = object(slot);
    return S_OK;
}

void native_command_destroy(Context *ctx, Object *cmd) {
    NativeHeapOperation op(ctx, true);
    if (!op.retained) return;
    cmd = native_submission_find(ctx->native_command_objects, cmd);
    if (!cmd) { error(ctx, E_INVALIDARG); return; }
    auto *value = cmd->backend;
    native_submission_remove(ctx->native_command_objects, cmd);
    // The runtime can destroy this slot recursively from its error callback.
    // Invalidate it before any backend callback and never revisit it afterward.
    std::memset(cmd, 0, sizeof(*cmd));
    {
        NativeBackendCall call(ctx);
        vkdu_object_destroy(value);
    }
    release(ctx); // Drop the command-list owner; op still retains Context.
}

// Interface R8 runtimes route allocation through the D3D12 core-layer
// callbacks (0022) and leave the legacy kernel-thunk Allocate/Deallocate
// slots NULL. Shim them so the native heap code keeps one allocation path.
struct NativeUmAllocation {
    PFND3D12DDI_ALLOCATE_CB_0022 allocate; PFND3D12DDI_DEALLOCATE_CB_0022 deallocate;
    // Allocations made through the core-layer callback start non-resident;
    // the native submission path references them directly, so make each one
    // resident on a driver-owned paging queue before first use.
    PFND3DDDI_CREATEPAGINGQUEUECB create_paging_queue;
    PFND3DDDI_MAKERESIDENTCB make_resident;
    PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMCPUCB wait_cpu;
    D3DKMT_HANDLE paging_queue, paging_fence;
};
std::mutex native_um_alloc_lock;
std::unordered_map<HANDLE, NativeUmAllocation> native_um_alloc;
bool native_um_lookup(HANDLE device, NativeUmAllocation &out) {
    std::lock_guard<std::mutex> lock(native_um_alloc_lock);
    auto found = native_um_alloc.find(device);
    if (found == native_um_alloc.end()) return false;
    out = found->second;
    return true;
}
HRESULT APIENTRY native_shim_allocate(HANDLE device, D3DDDICB_ALLOCATE *args) {
    NativeUmAllocation um{};
    if (!args || !native_um_lookup(device, um) || !um.allocate) return E_INVALIDARG;
    std::vector<D3D12DDI_ALLOCATION_INFO_0022> infos;
    try { infos.resize(args->NumAllocations); } catch (...) { return E_OUTOFMEMORY; }
    for (UINT i = 0; i < args->NumAllocations; ++i) {
        const auto &a = args->pAllocationInfo[i];
        infos[i].pSystemMem = a.pSystemMem;
        infos[i].pPrivateDriverData = a.pPrivateDriverData;
        infos[i].PrivateDriverDataSize = a.PrivateDriverDataSize;
        infos[i].VidPnSourceId = a.VidPnSourceId;
    }
    D3D12DDICB_ALLOCATE_0022 request{};
    request.pPrivateDriverData = args->pPrivateDriverData; request.PrivateDriverDataSize = args->PrivateDriverDataSize;
    request.hResource = args->hResource; request.NumAllocations = args->NumAllocations;
    request.pAllocationInfo = infos.data();
    HRESULT hr = um.allocate(D3D12DDI_HRTDEVICE{device}, &request);
    args->hKMResource = request.hKMResource;
    for (UINT i = 0; i < args->NumAllocations; ++i) args->pAllocationInfo[i].hAllocation = infos[i].hAllocation;
    if (FAILED(hr) || !args->NumAllocations || !um.make_resident) return hr;
    if (!um.paging_queue && um.create_paging_queue) {
        D3DDDICB_CREATEPAGINGQUEUE queue{};
        queue.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        HRESULT queue_hr = um.create_paging_queue(device, &queue);
        if (FAILED(queue_hr)) { native_trace("residency: CreatePagingQueue hr=%08x", (unsigned)queue_hr); return hr; }
        std::lock_guard<std::mutex> lock(native_um_alloc_lock);
        auto found = native_um_alloc.find(device);
        if (found != native_um_alloc.end() && !found->second.paging_queue) {
            found->second.paging_queue = queue.hPagingQueue; found->second.paging_fence = queue.hSyncObject;
        }
        um.paging_queue = found != native_um_alloc.end() ? found->second.paging_queue : queue.hPagingQueue;
        um.paging_fence = found != native_um_alloc.end() ? found->second.paging_fence : queue.hSyncObject;
    }
    if (!um.paging_queue) return hr;
    std::vector<D3DKMT_HANDLE> handles;
    try { handles.resize(args->NumAllocations); } catch (...) { return E_OUTOFMEMORY; }
    for (UINT i = 0; i < args->NumAllocations; ++i) handles[i] = infos[i].hAllocation;
    D3DDDI_MAKERESIDENT resident{};
    resident.hPagingQueue = um.paging_queue; resident.NumAllocations = args->NumAllocations;
    resident.AllocationList = handles.data();
    HRESULT resident_hr = um.make_resident(device, &resident);
    if (resident_hr == E_PENDING && um.wait_cpu) {
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait{};
        wait.ObjectCount = 1; wait.ObjectHandleArray = &um.paging_fence; wait.FenceValueArray = &resident.PagingFenceValue;
        resident_hr = um.wait_cpu(device, &wait);
    }
    if (FAILED(resident_hr)) native_trace("residency: MakeResident hr=%08x", (unsigned)resident_hr);
    return hr;
}
HRESULT APIENTRY native_shim_deallocate(HANDLE device, const D3DDDICB_DEALLOCATE *args) {
    NativeUmAllocation um{};
    if (!args || !native_um_lookup(device, um) || !um.deallocate) return E_INVALIDARG;
    D3D12DDICB_DEALLOCATE_0022 request{};
    request.hResource = args->hResource; request.NumAllocations = args->NumAllocations; request.HandleList = args->HandleList;
    return um.deallocate(D3D12DDI_HRTDEVICE{device}, &request);
}
HRESULT APIENTRY native_shim_deallocate2(HANDLE device, const D3DDDICB_DEALLOCATE2 *args) {
    NativeUmAllocation um{};
    if (!args || !native_um_lookup(device, um) || !um.deallocate) return E_INVALIDARG;
    D3D12DDICB_DEALLOCATE_0022 request{};
    request.hResource = args->hResource; request.NumAllocations = args->NumAllocations; request.HandleList = args->HandleList;
    UINT flags = 0;
    if (args->Flags.AssumeNotInUse) flags |= D3D12DDI_DEALLOCATE_FLAGS_0022_ASSUME_NOT_IN_USE;
    if (args->Flags.SynchronousDestroy) flags |= D3D12DDI_DEALLOCATE_FLAGS_0022_SYNCHRONOUS_DESTROY;
    request.Flags = static_cast<D3D12DDI_DEALLOCATE_FLAGS_0022>(flags);
    return um.deallocate(D3D12DDI_HRTDEVICE{device}, &request);
}

HRESULT native_create_device_impl(D3D12DDI_HADAPTER handle, const D3D12DDIARG_CREATEDEVICE_0003 *args);
HRESULT APIENTRY native_create_device(D3D12DDI_HADAPTER handle, const D3D12DDIARG_CREATEDEVICE_0003 *args) {
    if (args) native_trace("CreateDevice interface=%u version=%08x flags=%x", (unsigned)args->Interface,
        (unsigned)args->Version, (unsigned)args->Flags);
    const HRESULT hr = native_create_device_impl(handle, args);
    native_trace("CreateDevice hr=%08x", (unsigned)hr);
    return hr;
}
HRESULT native_create_device_impl(D3D12DDI_HADAPTER handle, const D3D12DDIARG_CREATEDEVICE_0003 *args) {
    auto adapter = native_adapter(handle);
    if (!adapter || !args || !args->hDrvDevice.pDrvPrivate || !args->hRTDevice.handle ||
            !args->pKTCallbacks || !args->p12UMCallbacks || !args->p12UMCallbacks->pfnSetErrorCb)
        return E_INVALIDARG;
    if (!native_version(args->Interface, args->Version, args->Flags)) { native_trace("CreateDevice: version rejected"); return DXGI_ERROR_UNSUPPORTED; }
    // The callback may close the adapter and invalidate the original inputs.
    const auto request = *args;
    auto kernel_callbacks = *args->pKTCallbacks;
    if (args->Interface == D3D12DDI_INTERFACE_VERSION_R8 && args->p12UMCallbacks_0022 &&
            (!kernel_callbacks.pfnAllocateCb || !kernel_callbacks.pfnDeallocateCb || !kernel_callbacks.pfnDeallocate2Cb)) {
        try {
            std::lock_guard<std::mutex> lock(native_um_alloc_lock);
            native_um_alloc[args->hRTDevice.handle] = {args->p12UMCallbacks_0022->pfnAllocateCb,
                args->p12UMCallbacks_0022->pfnDeallocateCb, kernel_callbacks.pfnCreatePagingQueueCb,
                kernel_callbacks.pfnMakeResidentCb, kernel_callbacks.pfnWaitForSynchronizationObjectFromCpuCb, 0, 0};
        } catch (...) { return E_OUTOFMEMORY; }
        if (!kernel_callbacks.pfnAllocateCb) kernel_callbacks.pfnAllocateCb = native_shim_allocate;
        if (!kernel_callbacks.pfnDeallocateCb) kernel_callbacks.pfnDeallocateCb = native_shim_deallocate;
        if (!kernel_callbacks.pfnDeallocate2Cb) kernel_callbacks.pfnDeallocate2Cb = native_shim_deallocate2;
    }
    if (native_experimental()) {
        const auto *slots = reinterpret_cast<void *const *>(args->pKTCallbacks);
        char nulls[600] = {};
        size_t used = 0;
        for (size_t i = 0; i < sizeof(D3DDDI_DEVICECALLBACKS) / sizeof(void *) && used + 6 < sizeof(nulls); ++i)
            if (!slots[i]) used += std::snprintf(nulls + used, sizeof(nulls) - used, " %zu", i);
        native_trace("CreateDevice KT callbacks null slots:%s", nulls);
        const auto *um = reinterpret_cast<void *const *>(args->p12UMCallbacks);
        used = 0; nulls[0] = 0;
        for (size_t i = 0; i < sizeof(D3D12DDI_CORELAYER_DEVICECALLBACKS_0062) / sizeof(void *) && used + 6 < sizeof(nulls); ++i)
            if (!um[i]) used += std::snprintf(nulls + used, sizeof(nulls) - used, " %zu", i);
        native_trace("CreateDevice UM callbacks null slots:%s", nulls);
    }
    const auto runtime_callbacks = *args->p12UMCallbacks;
    // Never inspect uninitialized runtime-owned storage before constructing it.
    NativeAdapterFacts current_facts{};
    HRESULT hr = native_query_adapter_facts(*adapter, current_facts, true);
    if (native_adapter(handle) != adapter) return DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(hr)) { native_trace("CreateDevice: queue-protocol facts hr=%08x", (unsigned)hr); return hr; }
    // The current owned-allocation KMD path requires cached-coherent memory;
    // a valid false fact is not permission to silently use another cache mode.
    if (!native_owned_facts_valid(current_facts)) {
        native_trace("CreateDevice: owned facts invalid coherent=%d/%d owned=%d/%d",
            current_facts.has_cached_coherent_memory.known, current_facts.has_cached_coherent_memory.value,
            current_facts.owned_allocations_only.known, current_facts.owned_allocations_only.value);
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (!native_same_identity_facts(current_facts, *adapter)) return DXGI_ERROR_DEVICE_REMOVED;
    auto *ctx = new (std::nothrow) Context;
    if (!ctx) return E_OUTOFMEMORY;
    PFN_vkGetInstanceProcAddr loader = nullptr;
    hr = native_load_vulkan(&ctx->vulkan_module, &loader);
    if (FAILED(hr)) native_trace("CreateDevice: load vulkan hr=%08x", (unsigned)hr);
    if (native_adapter(handle) != adapter) hr = DXGI_ERROR_DEVICE_REMOVED;
    ctx->native_adapter = adapter;
    ctx->native_facts = current_facts;
    ctx->runtime_device = request.hRTDevice;
    ctx->kernel_callbacks = kernel_callbacks;
    ctx->runtime_callbacks = runtime_callbacks;
    if (SUCCEEDED(hr)) {
        // VQDC Flags1 certifies only owned allocations sharing a domain.
        // Cross-context imported native surfaces require a separate lifetime
        // and ordering contract. ICD image support alone cannot grant that
        // KMD capability: keep native Open/Present/provider opt-in disabled.
        ctx->native_surfaces = native_experimental();
        const auto expected = native_memory_expectation(current_facts);
        hr = ctx->native_surfaces ?
            vkdu_device_create_shared_native_facts(loader, &expected, &native_runtime_callbacks, ctx,
                &ctx->native_memory_facts, &ctx->backend) :
            vkdu_device_create_shared(loader, &expected, &native_runtime_callbacks, ctx,
                &ctx->native_memory_facts, &ctx->backend);
        native_trace("CreateDevice: vkdu_device_create_shared hr=%08x", (unsigned)hr);
        if (SUCCEEDED(hr) && ctx->backend) {
            NativeBackendCall call(ctx);
            adapter->shader_model.store(vkdu_device_shader_model(ctx->backend));
        }
        if (hr != S_OK && SUCCEEDED(hr)) hr = E_FAIL;
    }
    if (native_adapter(handle) != adapter || ctx->native_retiring) hr = DXGI_ERROR_DEVICE_REMOVED;
    // Backend creation may race a KMD reset; revalidate before publishing state.
    if (SUCCEEDED(hr)) {
        hr = native_query_adapter_facts(*adapter, current_facts, true);
        if (hr == DXGI_ERROR_UNSUPPORTED) hr = DXGI_ERROR_DEVICE_REMOVED;
    }
    if (SUCCEEDED(hr) && (!native_owned_facts_valid(current_facts) ||
            !native_same_identity_facts(current_facts, ctx->native_facts)))
        hr = DXGI_ERROR_DEVICE_REMOVED;
    if (native_adapter(handle) != adapter || ctx->native_retiring) hr = DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(hr)) {
        ctx->native_retiring = true;
        native_heap_retire(ctx);
        ctx->runtime_device = {}; ctx->kernel_callbacks = {};
        release(ctx); return hr;
    }
    ctx->native_adapter = std::move(adapter);
    ctx->runtime_device = request.hRTDevice;
    ctx->kernel_callbacks = kernel_callbacks;
    ctx->runtime_callbacks = runtime_callbacks;
    ctx->report = native_report;
    ctx->report_context = ctx;
    new (request.hDrvDevice.pDrvPrivate) NativeDevice{native_device_magic, ctx};
    return S_OK;
}

void APIENTRY native_destroy_device(D3D12DDI_HDEVICE handle) {
    if (!handle.pDrvPrivate) return;
    auto *slot = static_cast<NativeDevice *>(handle.pDrvPrivate);
    if (slot->magic != native_device_magic) return;
    NativeHeapOperation op(slot->context);
    auto *ctx = op.ctx;
    if (!op.retained || ctx->native_retiring) return;
    const bool first_destroy = !ctx->native_destroying;
    ctx->native_destroying = true;
    // The ordinary last owner drains Vulkan while its completion/unmap/release
    // callbacks are still legal. New allocation/map/submit work is closed.
    // This operation retains a separate reference across a possible recursive
    // DestroyDevice, which may invalidate the runtime slot before we return.
    if (first_destroy && ctx->references.load() == 2 && !ctx->native_backend_calls &&
            !ctx->native_context_pending && !ctx->native_submitting) {
        auto *backend = ctx->backend;
        ctx->backend = nullptr;
        {
            NativeBackendCall backend_call(ctx);
            vkdu_device_destroy(backend);
        }
    }
    // A recursive destroy has already detached the slot/callbacks and dropped
    // its owner reference. Do not touch that expired runtime storage again.
    if (ctx->native_retiring) return;
    // Runtime releases this storage immediately after return. Child objects may
    // keep Context alive, but must never call a retired runtime handle/callback.
    ctx->native_retiring = true;
    native_fences_retire(ctx);
    native_heap_retire(ctx);
    ctx->runtime_device = {};
    ctx->runtime_callbacks = {};
    ctx->kernel_callbacks = {};
    ctx->report = nullptr; ctx->report_context = nullptr;
    slot->context = nullptr; slot->magic = 0;
    release(ctx);
}

HRESULT APIENTRY native_close_adapter(D3D12DDI_HADAPTER handle) {
    std::lock_guard<std::mutex> lock(native_adapters_lock);
    return native_adapters.erase(handle.pDrvPrivate) ? S_OK : E_INVALIDARG;
}
#include "ddi_0108.inc"
}

extern "C" HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER *args) {
    if (!args) return E_INVALIDARG;
    args->hAdapter = {};
    if (!args->pAdapterFuncs) return E_INVALIDARG;
    *args->pAdapterFuncs = {};
    if (!args->hRTAdapter.handle || !args->pAdapterCallbacks || !args->pAdapterCallbacks->pfnQueryAdapterInfoCb)
        return E_INVALIDARG;
    try {
        auto adapter = std::make_shared<NativeAdapter>();
        adapter->runtime = args->hRTAdapter;
        adapter->callbacks = *args->pAdapterCallbacks;
        HRESULT hr = native_query_adapter_facts(*adapter, *adapter);
        native_trace("OpenAdapter12 facts hr=%08x", (unsigned)hr);
        if (FAILED(hr)) return hr;
        {
            std::lock_guard<std::mutex> lock(native_adapters_lock);
            native_adapters.emplace(adapter.get(), adapter);
        }
        D3D12DDI_ADAPTERFUNCS table{};
        table.pfnCalcPrivateDeviceSize = native_device_size;
        table.pfnCreateDevice = native_create_device;
        table.pfnCloseAdapter = native_close_adapter;
        table.pfnGetSupportedVersions = native_versions;
        table.pfnGetCaps = native_caps;
        table.pfnGetOptionalDDITables = native_optional_tables;
        table.pfnFillDDITable = native_fill_table;
        table.pfnDestroyDevice = native_destroy_device;
        *args->pAdapterFuncs = table;
        args->hAdapter.pDrvPrivate = adapter.get();
        return S_OK;
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
