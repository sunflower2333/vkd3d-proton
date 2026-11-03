/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Provider-owned wire records from the pinned v0 shared KMD ABI. No pointers,
 * no UMD object layout, no cross-DLL C++ ownership. */
#pragma pack(push, 4)
struct ProbeWireHeader { uint32_t magic, version, size, reserved; };
struct ProbeWireContextInfo {
    ProbeWireHeader header;
    uint32_t opcode, flags;
    uint64_t expected_generation, va_start, va_size, generation;
    uint32_t context_id, queue_id;
};
struct ProbeWireAllocationInfo {
    ProbeWireHeader header;
    uint64_t size, alignment, iova, generation;
    uint32_t flags, format, width, height, pitch, refresh_numerator, refresh_denominator, context_id;
};
#pragma pack(pop)
static_assert(sizeof(ProbeWireContextInfo) == 64, "v0 KMT context wire size");
static_assert(sizeof(ProbeWireAllocationInfo) == 80 && offsetof(ProbeWireAllocationInfo, context_id) == 76,
    "v0 KMT allocation wire size/offset");

struct QueryIdentity {
    LUID luid{};
    uint64_t generation = 0;
    bool seen = false, queue_protocol = false;
};
HRESULT parse_query_identity(const void *data, UINT size, QueryIdentity &identity) {
    if (!data || (size != 160 && size != 192)) return E_INVALIDARG;
    const auto *bytes = static_cast<const unsigned char *>(data);
    const auto u32 = [&](size_t offset) { uint32_t value; std::memcpy(&value, bytes + offset, 4); return value; };
    if (u32(0) != 0x504d5644 || u32(4) || u32(8) != 128 || u32(12) || u32(16) || u32(20) ||
        u32(112) || u32(116) || u32(120) || u32(124) || u32(128) != 0x44494c56 ||
        u32(132) != 1 || u32(136) != 32 || u32(140) != 1 || u32(152) != 1 || u32(156)) return DXGI_ERROR_UNSUPPORTED;
    if (size == 192 && (u32(160) != 0x43445156 || u32(164) != 1 || u32(168) != 32 || u32(172) != 1 ||
        u32(176) != 32 || u32(180) != 40 || u32(184) != 64 || u32(188))) return DXGI_ERROR_UNSUPPORTED;
    QueryIdentity result{};
    std::memcpy(&result.luid, bytes + 144, sizeof(LUID));
    result.generation = uint64_t(u32(24)) | (uint64_t(u32(28)) << 32);
    if (!(result.luid.LowPart | static_cast<UINT>(result.luid.HighPart)) || !result.generation) return DXGI_ERROR_UNSUPPORTED;
    result.seen = true; result.queue_protocol = size == 192;
    identity = result; return S_OK;
}

HRESULT nt_result(NTSTATUS status, const char *operation) {
    if (status >= 0) return S_OK;
    std::fprintf(stdout, "KMT %s status=%08x\n", operation, static_cast<unsigned>(status));
    return static_cast<HRESULT>(static_cast<uint32_t>(status) | 0x10000000u);
}
D3DKMT_HANDLE kmt_handle(HANDLE value) { return static_cast<D3DKMT_HANDLE>(reinterpret_cast<uintptr_t>(value)); }
HANDLE runtime_context(D3DKMT_HANDLE value) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)); }
/* Only --validate-provider-controls replaces these functions. Hardware runs
 * always call the actual KMT entrypoints; no CPU event is signalled here. */
static auto probe_kmt_query = D3DKMTQueryAdapterInfo;
static auto probe_kmt_deallocate = D3DKMTDestroyAllocation2;
static auto probe_kmt_signal = D3DKMTSignalSynchronizationObject2;
static auto probe_kmt_destroy_context = D3DKMTDestroyContext;
static auto probe_kmt_make_resident = D3DKMTMakeResident;
static auto probe_kmt_evict = D3DKMTEvict;
static auto probe_kmt_wait_paging = D3DKMTWaitForSynchronizationObjectFromCpu;
static auto probe_wait_paging_event = WaitForSingleObject;
static auto probe_kmt_render = D3DKMTRender;

struct Deadline {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    std::thread worker;
    Deadline() : worker([this]() {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(90), [this]() { return done; })) {
            std::fputs("FAIL shared-backing probe exceeded90s; terminating probe process\n", stdout);
            ExitProcess(124);
        }
    }) {}
    ~Deadline() { { std::lock_guard<std::mutex> lock(mutex); done = true; } condition.notify_one(); worker.join(); }
};

struct ProbeRuntime {
    struct ContextCounts { unsigned renders = 0, targets = 0, events = 0, observed = 0; };
    struct Completion { D3DKMT_HANDLE context; HANDLE event; };
    LUID requested_luid{};
    D3DKMT_HANDLE adapter = 0, device = 0, context_handle = 0;
    uint32_t context_id = 0;
    QueryIdentity identity{};
    std::atomic_uint callback_exceptions{0};
    std::map<D3DKMT_HANDLE, ContextCounts> closed_contexts;
    std::map<D3DKMT_HANDLE, void *> lock_addresses;
    std::mutex mutex;
    std::map<D3DKMT_HANDLE, ProbeWireAllocationInfo> allocations;
    std::map<D3DKMT_HANDLE, ContextCounts> contexts;
    std::vector<Completion> completions;
    std::atomic_uint creates{0}, renders{0}, target_references{0};
    std::atomic_uint target_handle{0};
    std::atomic_bool native_cleanup{false};
    std::atomic_uint cleanup_identity_queries{0};
    std::atomic_uint pageable_deallocations{0}, internal_deallocations{0}, command_errors{0}, device_errors{0};
    std::atomic_bool retirement_sealed{false};
    // This isolated runtime owns one residency reference for every allocation,
    // including ICD scratch BOs. Shared ICD mode delegates residency here.
    // WDDM2 physical engines still require MakeResident; allocation/patch lists
    // alone only describe patching and cannot make a submission resident.
    std::mutex residency_mutex;
    D3DKMT_HANDLE paging_queue = 0, paging_sync = 0;
    bool paging_ready = false;
    volatile UINT64 *paging_cpu_fence = nullptr;
    UINT64 pending_paging_fence = 0;
    std::set<D3DKMT_HANDLE> resident_allocations;
    std::map<D3DKMT_HANDLE, UINT> residency_references;
    unsigned char paging_identity = 0;
    bool residency_uncertain = false;
    D3D12DDI_HRTPAGINGQUEUE runtime_paging_queue() { return {&paging_identity}; }
    HRESULT runtime_make_resident(D3D12DDI_HRTPAGINGQUEUE, D3DDDI_MAKERESIDENT *);
    HRESULT runtime_evict(const D3DDDICB_EVICT *);
    HRESULT ensure_paging_queue() {
        std::lock_guard<std::mutex> lock(residency_mutex);
        if (paging_queue) return paging_ready ? S_OK : E_FAIL;
        D3DKMT_CREATEPAGINGQUEUE request{};
        request.hDevice = device; request.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        TlCall call(TlKind::PagingQueue,{device});
        const NTSTATUS status = D3DKMTCreatePagingQueue(&request);
        call.end({tl_u32(status),request.hPagingQueue,request.hSyncObject,tl_ptr(request.FenceValueCPUVirtualAddress)});
        // Even a malformed or failing reply can transfer a queue to clean up.
        paging_queue = request.hPagingQueue; paging_sync = request.hSyncObject;
        paging_cpu_fence = static_cast<volatile UINT64 *>(request.FenceValueCPUVirtualAddress);
        std::fprintf(stdout, "RESIDENCY CreatePagingQueue device=%u queue=%u sync=%u status=%08x\n",
                device, paging_queue, paging_sync, static_cast<unsigned>(status));
        if (status != 0) return status < 0 ? nt_result(status, "CreatePagingQueue") : E_FAIL;
        paging_ready = paging_queue && paging_sync && paging_cpu_fence;
        return paging_ready ? S_OK : E_FAIL;
    }
    HRESULT make_resident(D3DKMT_HANDLE allocation) {
        std::lock_guard<std::mutex> lock(residency_mutex);
        if (!paging_queue || !paging_sync || !paging_cpu_fence) return E_FAIL;
        if (resident_allocations.count(allocation)) return E_UNEXPECTED;
        for (unsigned attempt = 0; attempt != 2; ++attempt) {
            D3DDDI_MAKERESIDENT request{};
            request.hPagingQueue = paging_queue; request.NumAllocations = 1;
            request.AllocationList = &allocation; request.Flags.CantTrimFurther = attempt != 0;
            TlCall call(TlKind::MakeResident,{allocation,paging_queue,attempt,request.Flags.Value});
            const NTSTATUS status = probe_kmt_make_resident(&request);
            call.end({tl_u32(status),allocation,request.NumAllocations,request.PagingFenceValue,request.NumBytesToTrim,
                status==0 || status==static_cast<NTSTATUS>(0x103),status==static_cast<NTSTATUS>(0x103)});
            std::fprintf(stdout, "RESIDENCY MakeResident allocation=%u status=%08x count=%u fence=%llu trim=%llu\n",
                    allocation, static_cast<unsigned>(status), request.NumAllocations,
                    static_cast<unsigned long long>(request.PagingFenceValue),
                    static_cast<unsigned long long>(request.NumBytesToTrim));
            if (status == 0 || status == static_cast<NTSTATUS>(0x103)) {
                if (request.NumAllocations != 1 || (status == 0x103 && !request.PagingFenceValue)) return E_FAIL;
                resident_allocations.insert(allocation);
                residency_references[allocation] = 1;
                if (status == 0x103 && request.PagingFenceValue > pending_paging_fence)
                    pending_paging_fence = request.PagingFenceValue;
                return S_OK;
            }
            if (status != static_cast<NTSTATUS>(0xc0000017u) || attempt)
                return status < 0 ? nt_result(status, "MakeResident") : E_FAIL;
        }
        return E_OUTOFMEMORY;
    }
    // Caller holds residency_mutex. A queued CPU wait is not completion: only
    // the OS-signalled event (or already advanced monitored fence) permits use.
    HRESULT wait_paging_locked() {
        if (residency_uncertain) return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
        const UINT64 target = pending_paging_fence;
        if (!target) { timeline.emit(0,TlKind::PagingState,0,{0,0,0}); return S_OK; }
        if (!paging_queue || !paging_sync || !paging_cpu_fence) return E_FAIL;
        const UINT64 initial_observed=*paging_cpu_fence;
        timeline.emit(0,TlKind::PagingState,0,{target,initial_observed,initial_observed>=target});
        if (initial_observed < target) {
            HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!event) {
                const DWORD error = GetLastError();
                return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
            }
            D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU request{};
            request.hDevice = device; request.ObjectCount = 1;
            request.ObjectHandleArray = &paging_sync; request.FenceValueArray = &target;
            request.hAsyncEvent = event;
            TlCall call(TlKind::PagingEnqueue,{device,paging_sync,target,tl_ptr(event)});
            const NTSTATUS status = probe_kmt_wait_paging(&request);
            call.end({tl_u32(status)});
            const DWORD waited = status == 0 ? tl_call(TlKind::PagingWait,[&] { return probe_wait_paging_event(event, 5000); },
                {tl_ptr(event),target,5000}) : WAIT_FAILED;
            const DWORD error = waited == WAIT_FAILED ? GetLastError() : waited;
            CloseHandle(event);
            const UINT64 reported_observed=*paging_cpu_fence;
            timeline.emit(call.id,TlKind::PagingState,0,{target,reported_observed,waited,tl_u32(status),error});
            std::fprintf(stdout, "RESIDENCY paging fence=%llu status=%08x wait=%u observed=%llu\n",
                    static_cast<unsigned long long>(target), static_cast<unsigned>(status), static_cast<unsigned>(waited),
                    static_cast<unsigned long long>(reported_observed));
            if (status != 0) return status < 0 ? nt_result(status, "WaitForSynchronizationObjectFromCpu(paging)") : E_FAIL;
            if (waited != WAIT_OBJECT_0) return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
            const UINT64 final_observed=*paging_cpu_fence;
            timeline.emit(call.id,TlKind::PagingState,0,{target,final_observed,final_observed>=target});
            if (final_observed < target) return E_FAIL;
        }
        pending_paging_fence = 0;
        return S_OK;
    }
    // Caller holds residency_mutex through the actual D3DKMTRender return.
    HRESULT wait_render_residency_locked(const D3DDDICB_RENDER &render) {
#ifdef VKDU_PROBE_TEST_RESIDENCY_EARLY_USE
        return S_OK;
#endif
        for (UINT i = 0; i < render.NumAllocations; ++i)
            if (!resident_allocations.count(render.pNewAllocationList[i].hAllocation)) return E_FAIL;
        return wait_paging_locked();
    }
    ~ProbeRuntime() = default; // Session explicitly closes only after confirmed DLL drain.
    HRESULT observe_completions(D3DKMT_HANDLE context) {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto &completion : completions) {
            if (!completion.event || (context && completion.context != context)) continue;
            const DWORD state = tl_call(TlKind::Observe,[&] { return WaitForSingleObject(completion.event, 0); },
                {completion.context,tl_ptr(completion.event)});
            if (state == WAIT_TIMEOUT) return DXGI_ERROR_WAS_STILL_DRAWING;
            if (state != WAIT_OBJECT_0) return E_FAIL;
            ++contexts.at(completion.context).observed;
            CloseHandle(completion.event); completion.event = nullptr;
        }
        return S_OK;
    }
    HRESULT close() {
        // This is the probe's actual final KMT device teardown, performed only
        // after native/backend owners have returned. No reentry is injected.
        if (device) {
            std::lock_guard<std::mutex> residency(residency_mutex);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!allocations.empty() || !contexts.empty()) return DXGI_ERROR_WAS_STILL_DRAWING;
            }
            if (paging_queue) {
                D3DDDI_DESTROYPAGINGQUEUE request{}; request.hPagingQueue = paging_queue;
                const NTSTATUS status = D3DKMTDestroyPagingQueue(&request);
                if (status != 0) return status < 0 ? nt_result(status, "DestroyPagingQueue") : E_FAIL;
                paging_queue = paging_sync = 0; paging_cpu_fence = nullptr; pending_paging_fence = 0;
                paging_ready = false;
            }
            D3DKMT_DESTROYDEVICE args{}; args.hDevice = device;
            HRESULT hr = nt_result(D3DKMTDestroyDevice(&args), "DestroyDevice(final owner)");
            if (FAILED(hr)) return hr;
            device = 0;
        }
        if (adapter) {
            D3DKMT_CLOSEADAPTER args{}; args.hAdapter = adapter;
            HRESULT hr = nt_result(D3DKMTCloseAdapter(&args), "CloseAdapter");
            if (FAILED(hr)) return hr;
            adapter = 0;
        }
        return S_OK;
    }
    void open(LUID luid) {
        requested_luid = luid;
        D3DKMT_OPENADAPTERFROMLUID a{}; a.AdapterLuid = luid;
        HRESULT hr = nt_result(tl_call(TlKind::KmtOpen,[&] { return D3DKMTOpenAdapterFromLuid(&a); },{tl_luid(luid)}), "OpenAdapterFromLuid");
        adapter = a.hAdapter; check(hr, "OpenAdapterFromLuid");
        require(adapter != 0, "nonzero KMT adapter");
        D3DKMT_CREATEDEVICE d{}; d.hAdapter = adapter;
        hr = nt_result(tl_call(TlKind::KmtDevice,[&] { return D3DKMTCreateDevice(&d); },{adapter}), "CreateDevice");
        device = d.hDevice; check(hr, "CreateDevice");
        require(device != 0, "nonzero KMT device");
    }
    bool has(D3DKMT_HANDLE handle) {
        std::lock_guard<std::mutex> lock(mutex);
        return allocations.count(handle) != 0;
    }
    bool has_context(D3DKMT_HANDLE handle) {
        std::lock_guard<std::mutex> lock(mutex);
        return contexts.count(handle) != 0;
    }
    ContextCounts counts(D3DKMT_HANDLE handle) {
        std::lock_guard<std::mutex> lock(mutex);
        require(contexts.count(handle) != 0, "scheduler context is still registered");
        return contexts.at(handle);
    }
};
struct ProbeRuntimeQueue {
    ProbeRuntime *runtime;
    D3DKMT_HANDLE context = 0;
    unsigned creates = 0, destroys = 0;
    D3DKMT_HANDLE last_destroyed = 0;
    ProbeRuntime::ContextCounts closed{};
};
ProbeRuntime *peer(HANDLE value) { return static_cast<ProbeRuntime *>(value); }

HRESULT APIENTRY probe_query(HANDLE h, const D3DDDICB_QUERYADAPTERINFO *args) {
    if (!h || !args) return E_INVALIDARG;
    D3DKMT_QUERYADAPTERINFO q{};
    q.hAdapter = peer(h)->adapter; q.Type = KMTQAITYPE_UMDRIVERPRIVATE;
    q.pPrivateDriverData = args->pPrivateDriverData; q.PrivateDriverDataSize = args->PrivateDriverDataSize;
    TlCall call(TlKind::QueryKmt,{q.hAdapter,q.PrivateDriverDataSize,tl_luid(peer(h)->requested_luid)});
    const NTSTATUS native_status=probe_kmt_query(&q);
    HRESULT hr = nt_result(native_status, "QueryAdapterInfo");
    uint64_t raw_generation=0,raw_luid=0;
    const bool raw_present=q.pPrivateDriverData && (q.PrivateDriverDataSize==160 || q.PrivateDriverDataSize==192);
    if (raw_present) {
        std::memcpy(&raw_generation,static_cast<const uint8_t *>(q.pPrivateDriverData)+24,8);
        std::memcpy(&raw_luid,static_cast<const uint8_t *>(q.pPrivateDriverData)+144,8);
    }
    call.end({tl_u32(native_status),tl_u32(hr),q.PrivateDriverDataSize,raw_present,raw_luid,raw_generation});
    if (args->PrivateDriverDataSize == 192) {
        // Raw discovery evidence, not a substitute for production validation.
        // In particular, old KMDs leave the caller-zeroed trailer absent.
        uint32_t fields[8]{}; uint64_t generation = 0;
        std::memcpy(fields, static_cast<const uint8_t *>(args->pPrivateDriverData) + 160, sizeof(fields));
        std::memcpy(&generation, static_cast<const uint8_t *>(args->pPrivateDriverData) + 24, sizeof(generation));
        std::printf("VQDC query hr=%08x generation=%llu magic=%08x version=%u size=%u flags=%u owner=%u shared=%u context_info=%u reserved=%u\n",
            static_cast<unsigned>(hr), static_cast<unsigned long long>(generation), fields[0], fields[1],
            fields[2], fields[3], fields[4], fields[5], fields[6], fields[7]);
    }
    if (SUCCEEDED(hr) && (args->PrivateDriverDataSize == 160 || args->PrivateDriverDataSize == 192)) {
        QueryIdentity observed{};
        hr = parse_query_identity(args->pPrivateDriverData, args->PrivateDriverDataSize, observed);
        timeline.emit(call.id,TlKind::QueryParse,0,{tl_u32(hr),q.PrivateDriverDataSize,tl_luid(observed.luid),observed.generation});
        if (FAILED(hr)) return hr;
        auto *r = peer(h);
        std::lock_guard<std::mutex> lock(r->mutex);
        timeline.emit(call.id,TlKind::QueryParse,0,{tl_u32(hr),q.PrivateDriverDataSize,tl_luid(observed.luid),observed.generation,
            tl_luid(r->requested_luid),r->identity.seen,tl_luid(r->identity.luid),r->identity.generation});
        if (std::memcmp(&observed.luid, &r->requested_luid, sizeof(LUID)) ||
            (r->identity.seen && (r->identity.generation != observed.generation ||
                std::memcmp(&r->identity.luid, &observed.luid, sizeof(LUID))))) {
            timeline.emit(call.id,TlKind::QueryParse,0,{tl_u32(DXGI_ERROR_DEVICE_REMOVED),q.PrivateDriverDataSize,
                tl_luid(observed.luid),observed.generation,tl_luid(r->requested_luid),r->identity.seen,
                tl_luid(r->identity.luid),r->identity.generation,1});
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        observed.queue_protocol |= r->identity.queue_protocol;
        r->identity = observed;
    }
    // The provider now retires actual context-ordered OS events. Its remaining
    // query during cleanup only proves callback/identity liveness, not GPU idle.
    if (SUCCEEDED(hr) && peer(h)->native_cleanup) ++peer(h)->cleanup_identity_queries;
    return hr;
}
HRESULT probe_create_context(ProbeRuntime &runtime, D3DDDICB_CREATECONTEXT *args) {
    D3DKMT_CREATECONTEXT c{}; c.hDevice = runtime.device;
    c.NodeOrdinal = args->NodeOrdinal; c.EngineAffinity = args->EngineAffinity;
    c.pPrivateDriverData = args->pPrivateDriverData; c.PrivateDriverDataSize = args->PrivateDriverDataSize;
    c.ClientHint = D3DKMT_CLIENTHINT_VULKAN;
    TlCall call(TlKind::CreateContext,{c.hDevice,c.NodeOrdinal,c.EngineAffinity,c.PrivateDriverDataSize});
    const NTSTATUS native_status=D3DKMTCreateContext(&c);
    HRESULT hr = nt_result(native_status, "CreateContext");
    call.end({tl_u32(native_status),tl_u32(hr),c.hDevice,c.hContext,c.CommandBufferSize,c.AllocationListSize,c.PatchLocationListSize});
    args->hContext = runtime_context(c.hContext);
    args->pCommandBuffer = c.pCommandBuffer; args->CommandBufferSize = c.CommandBufferSize;
    args->pAllocationList = c.pAllocationList; args->AllocationListSize = c.AllocationListSize;
    args->pPatchLocationList = c.pPatchLocationList; args->PatchLocationListSize = c.PatchLocationListSize;
    if (c.hContext) {
        std::lock_guard<std::mutex> lock(runtime.mutex);
        runtime.contexts.emplace(c.hContext, ProbeRuntime::ContextCounts{});
    }
    return hr;
}
HRESULT probe_destroy_context(ProbeRuntime &runtime, const D3DDDICB_DESTROYCONTEXT *args) {
    D3DKMT_DESTROYCONTEXT c{}; c.hContext = kmt_handle(args->hContext);
    if (!runtime.has_context(c.hContext)) return E_INVALIDARG;
    HRESULT observed = runtime.observe_completions(c.hContext);
#ifndef VKDU_PROBE_TEST_EARLY_RELEASE
    if (FAILED(observed)) return observed;
#else
    (void)observed;
#endif
    auto destroy_call=[&] { return tl_call(TlKind::DestroyContext,[&] { return probe_kmt_destroy_context(&c); },{c.hContext}); };
    NTSTATUS status = destroy_call();
    for (unsigned attempt = 0; attempt < 100 &&
            (static_cast<uint32_t>(status) == 0x80000011u || static_cast<uint32_t>(status) == 0xc01e0102u); ++attempt) {
        Sleep(1); status = destroy_call();
    }
    if (status >= 0) {
        std::lock_guard<std::mutex> lock(runtime.mutex);
        runtime.closed_contexts[c.hContext] = runtime.contexts.at(c.hContext);
        runtime.contexts.erase(c.hContext);
    }
    return nt_result(status, "DestroyContext");
}
HRESULT APIENTRY probe_context_create(HANDLE h, D3DDDICB_CREATECONTEXT *args) {
    if (!h || !args || peer(h)->context_handle) return E_INVALIDARG;
    HRESULT hr = probe_create_context(*peer(h), args);
    peer(h)->context_handle = kmt_handle(args->hContext);
    return hr;
}
HRESULT APIENTRY probe_context_destroy(HANDLE h, const D3DDDICB_DESTROYCONTEXT *args) {
    if (!h || !args || kmt_handle(args->hContext) != peer(h)->context_handle) return E_INVALIDARG;
    HRESULT hr = probe_destroy_context(*peer(h), args);
    if (SUCCEEDED(hr)) peer(h)->context_handle = 0;
    return hr;
}
HRESULT APIENTRY probe_queue_context_create(D3D12DDI_HRTCOMMANDQUEUE h, D3DDDICB_CREATECONTEXT *args) {
    auto *queue = static_cast<ProbeRuntimeQueue *>(h.handle);
    if (!queue || !queue->runtime || queue->context || !args) return E_INVALIDARG;
    HRESULT hr = probe_create_context(*queue->runtime, args);
    queue->context = kmt_handle(args->hContext);
    if (SUCCEEDED(hr)) ++queue->creates;
    return hr;
}
HRESULT APIENTRY probe_queue_context_destroy(D3D12DDI_HRTCOMMANDQUEUE h, const D3DDDICB_DESTROYCONTEXT *args) {
    auto *queue = static_cast<ProbeRuntimeQueue *>(h.handle);
    if (!queue || !queue->runtime || !args || kmt_handle(args->hContext) != queue->context) return E_INVALIDARG;
    HRESULT hr = probe_destroy_context(*queue->runtime, args);
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(queue->runtime->mutex);
        queue->last_destroyed = queue->context;
        queue->closed = queue->runtime->closed_contexts.at(queue->context);
        ++queue->destroys; queue->context = 0;
    }
    return hr;
}
HRESULT APIENTRY probe_escape(HANDLE h, const D3DDDICB_ESCAPE *args) {
    if (!h || !args || args->hDevice != h) return E_INVALIDARG;
    D3DKMT_ESCAPE e{}; e.hAdapter = peer(h)->adapter; e.hDevice = peer(h)->device;
    e.hContext = kmt_handle(args->hContext); e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = args->pPrivateDriverData; e.PrivateDriverDataSize = args->PrivateDriverDataSize;
    TlCall call(TlKind::Escape,{e.hDevice,e.hContext,e.PrivateDriverDataSize});
    const NTSTATUS native_status=D3DKMTEscape(&e);
    HRESULT hr = nt_result(native_status, "Escape");
    if (SUCCEEDED(hr) && e.PrivateDriverDataSize==sizeof(ProbeWireContextInfo) && e.pPrivateDriverData) {
        const auto &info=*static_cast<const ProbeWireContextInfo *>(e.pPrivateDriverData);
        call.end({tl_u32(native_status),tl_u32(hr),e.hDevice,e.hContext,1,info.opcode,info.context_id,info.queue_id,
            info.generation,info.va_start,info.va_size,info.expected_generation,info.header.magic,info.header.size});
    } else call.end({tl_u32(native_status),tl_u32(hr),e.hDevice,e.hContext,0});
    if (SUCCEEDED(hr) && args->PrivateDriverDataSize == sizeof(ProbeWireContextInfo)) {
        auto *info = static_cast<ProbeWireContextInfo *>(args->pPrivateDriverData);
        if (info->opcode == 1) peer(h)->context_id = info->context_id;
    }
    return hr;
}
#include "runtime_dll_residency.inc"

HRESULT APIENTRY probe_allocate(HANDLE h, D3DDDICB_ALLOCATE *args) {
    if (!h || !args || args->hResource || args->NumAllocations != 1 || !args->pAllocationInfo ||
            args->pAllocationInfo[0].PrivateDriverDataSize != sizeof(ProbeWireAllocationInfo)) return E_INVALIDARG;
    auto *r = peer(h);
    const auto info = *static_cast<const ProbeWireAllocationInfo *>(args->pAllocationInfo[0].pPrivateDriverData);
    if (!r->context_id || info.context_id != r->context_id) return E_INVALIDARG;
    timeline.emit(0,TlKind::AllocationRequest,0,{info.size,info.alignment,info.iova,info.flags,info.context_id,info.generation,
        r->device,r->context_handle,info.header.magic,info.header.size});
    HRESULT paging = r->ensure_paging_queue();
    if (FAILED(paging)) return paging;
    D3DKMT_CREATEALLOCATION a{}; a.hDevice = r->device; a.NumAllocations = 1;
    a.pAllocationInfo = args->pAllocationInfo; a.Flags.NonSecure = 1;
    auto allocate_call=[&] {
        TlCall call(TlKind::CreateAllocation,{a.hDevice,info.size,info.alignment,info.iova,info.flags,info.context_id,info.generation});
        const NTSTATUS result=D3DKMTCreateAllocation(&a);
        call.end({tl_u32(result),args->pAllocationInfo[0].hAllocation}); return result;
    };
    NTSTATUS status = allocate_call();
    for (unsigned attempt = 0; attempt < 100 && static_cast<uint32_t>(status) == 0x80000011u &&
            !args->pAllocationInfo[0].hAllocation; ++attempt) {
        Sleep(1); status = allocate_call();
    }
    if (args->pAllocationInfo[0].hAllocation) {
        std::lock_guard<std::mutex> lock(r->mutex);
        r->allocations[args->pAllocationInfo[0].hAllocation] = info; ++r->creates;
    }
    if (status != 0) return status < 0 ? nt_result(status, "CreateAllocation") : E_FAIL;
    const auto handle = args->pAllocationInfo[0].hAllocation;
    if (!handle) return E_FAIL;
    HRESULT resident = r->make_resident(handle);
    if (FAILED(resident)) {
        // No submission can reference this unpublished allocation. Destroy also
        // removes any residency reference from a malformed accepted reply.
        D3DKMT_DESTROYALLOCATION2 rollback{};
        rollback.hDevice = r->device; rollback.AllocationCount = 1; rollback.phAllocationList = &handle;
        const NTSTATUS released = probe_kmt_deallocate(&rollback);
        if (released == 0) {
            std::lock_guard<std::mutex> residency(r->residency_mutex);
            std::lock_guard<std::mutex> lock(r->mutex);
            r->resident_allocations.erase(handle); r->residency_references.erase(handle); r->allocations.erase(handle);
            args->pAllocationInfo[0].hAllocation = 0;
        } else nt_result(released, "DestroyAllocation2(residency rollback retained)");
        // On failed rollback preserve the handle and owner ledger. The native
        // allocation caller already compensates partial allocations and retries.
    }
    return resident;
}
HRESULT probe_destroy_allocations(HANDLE h, HANDLE resource, UINT count, const D3DKMT_HANDLE *handles, UINT flags) {
    if (!h || resource || !count || count > 1024 || !handles) return E_INVALIDARG;
    const std::vector<D3DKMT_HANDLE> snapshot(handles, handles + count);
    std::lock_guard<std::mutex> residency(peer(h)->residency_mutex);
    for (UINT i = 0; i < count; ++i) {
        if (!peer(h)->has(snapshot[i])) return E_INVALIDARG;
        for (UINT j = 0; j < i; ++j) if (snapshot[i] == snapshot[j]) return E_INVALIDARG;
    }
    HRESULT paged = peer(h)->wait_paging_locked();
    if (FAILED(paged)) return paged;
    for (auto allocation : snapshot) {
        if (!peer(h)->resident_allocations.count(allocation)) continue;
        D3DKMT_EVICT evict{}; evict.hDevice = peer(h)->device;
        evict.NumAllocations = 1; evict.AllocationList = &allocation;
        // Drop every reference, including successful R0 MakeResident calls.
        // A failed decrement preserves its remaining count for retry.
        auto &references = peer(h)->residency_references[allocation];
        if (!references) references = 1; // inherited eager-residency owner
        while (references) {
            const NTSTATUS status = probe_kmt_evict(&evict);
            if (status != 0) return status < 0 ? nt_result(status, "Evict") : E_FAIL;
            --references;
        }
        peer(h)->residency_references.erase(allocation);
        peer(h)->resident_allocations.erase(allocation);
    }
    D3DKMT_DESTROYALLOCATION2 a{}; a.hDevice = peer(h)->device;
    a.AllocationCount = count; a.phAllocationList = snapshot.data(); a.Flags.Value = flags;
#ifdef VKDU_PROBE_TEST_DROP_SYNCHRONOUS
    a.Flags.SynchronousDestroy = 0;
#endif
    NTSTATUS status = probe_kmt_deallocate(&a);
    for (unsigned attempt = 0; attempt < 100 &&
            (static_cast<uint32_t>(status) == 0x80000011u || static_cast<uint32_t>(status) == 0xc01e0102u); ++attempt) {
        Sleep(1); status = probe_kmt_deallocate(&a);
    }
    if (status >= 0) {
        std::lock_guard<std::mutex> lock(peer(h)->mutex);
        for (auto allocation : snapshot) peer(h)->allocations.erase(allocation);
        if (flags) ++peer(h)->pageable_deallocations; else ++peer(h)->internal_deallocations;
    }
    return nt_result(status, "DestroyAllocation2");
}
HRESULT APIENTRY probe_deallocate(HANDLE h, const D3DDDICB_DEALLOCATE *args) {
    return args ? probe_destroy_allocations(h, args->hResource, args->NumAllocations, args->HandleList, 0) : E_INVALIDARG;
}
HRESULT APIENTRY probe_deallocate2(HANDLE h, const D3DDDICB_DEALLOCATE2 *args) {
    // This bounded provider accepts only this port's public pageable contract.
    // Resource associations are deliberately absent in the isolated probe.
    if (!args || args->Flags.Value != 3) return E_INVALIDARG;
    return probe_destroy_allocations(h, args->hResource, args->NumAllocations, args->HandleList, args->Flags.Value);
}
HRESULT APIENTRY probe_lock(HANDLE h, D3DDDICB_LOCK *args) {
    if (!h || !args) return E_INVALIDARG;
    std::lock_guard<std::mutex> residency(peer(h)->residency_mutex);
    if (!peer(h)->has(args->hAllocation)) return E_INVALIDARG;
    if (!peer(h)->resident_allocations.count(args->hAllocation)) return E_FAIL;
    HRESULT paged = peer(h)->wait_paging_locked();
    if (FAILED(paged)) return paged;
    D3DKMT_LOCK l{}; l.hDevice = peer(h)->device; l.hAllocation = args->hAllocation;
    l.Flags.LockEntire = 1;
    HRESULT hr = nt_result(D3DKMTLock(&l), "Lock");
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(peer(h)->mutex);
        if (l.hAllocation && l.hAllocation != args->hAllocation) {
            auto info = peer(h)->allocations.at(args->hAllocation);
            peer(h)->allocations.erase(args->hAllocation); peer(h)->allocations[l.hAllocation] = info;
            if (peer(h)->resident_allocations.erase(args->hAllocation))
                peer(h)->resident_allocations.insert(l.hAllocation);
            const auto reference = peer(h)->residency_references.find(args->hAllocation);
            if (reference != peer(h)->residency_references.end()) {
                const UINT count = reference->second;
                peer(h)->residency_references.erase(reference);
                peer(h)->residency_references.emplace(l.hAllocation, count);
            }
            if (peer(h)->target_handle == args->hAllocation) peer(h)->target_handle = l.hAllocation;
        }
        args->hAllocation = l.hAllocation; args->pData = l.pData;
        peer(h)->lock_addresses[l.hAllocation] = l.pData;
    }
    return hr;
}
HRESULT APIENTRY probe_unlock(HANDLE h, const D3DDDICB_UNLOCK *args) {
    if (!h || !args) return E_INVALIDARG;
    D3DKMT_UNLOCK u{}; u.hDevice = peer(h)->device;
    u.NumAllocations = args->NumAllocations; u.phAllocations = args->phAllocations;
    return nt_result(D3DKMTUnlock(&u), "Unlock");
}
HRESULT APIENTRY probe_render(HANDLE h, D3DDDICB_RENDER *args) {
    if (!h || !args || (args->NumAllocations && !args->pNewAllocationList)) return E_INVALIDARG;
    // Evict, deallocation and lock-renaming take the same exclusion. Keep it
    // across validation, paging completion and the actual KMT submission, so
    // none can invalidate an allocation after its last residency check.
    std::unique_lock<std::mutex> residency(peer(h)->residency_mutex);
    if (!peer(h)->has_context(kmt_handle(args->hContext))) return E_INVALIDARG;
    if (peer(h)->retirement_sealed) {
        std::fputs("FAIL unexpected Render after terminal completion seal\n", stdout); return E_UNEXPECTED;
    }
    bool target = false;
    for (UINT i = 0; i < args->NumAllocations; ++i) {
        if (!peer(h)->has(args->pNewAllocationList[i].hAllocation)) return E_INVALIDARG;
        target |= args->pNewAllocationList[i].hAllocation == peer(h)->target_handle;
    }
    HRESULT paged = peer(h)->wait_render_residency_locked(*args);
    if (FAILED(paged)) return paged;
    D3DKMT_RENDER r{}; r.hContext = kmt_handle(args->hContext);
    r.CommandLength = args->CommandLength; r.AllocationCount = args->NumAllocations;
    r.PatchLocationCount = args->NumPatchLocations;
    r.pNewCommandBuffer = args->pNewCommandBuffer; r.NewCommandBufferSize = args->NewCommandBufferSize;
    r.pNewAllocationList = args->pNewAllocationList; r.NewAllocationListSize = args->NewAllocationListSize;
    r.pNewPatchLocationList = args->pNewPatchLocationList; r.NewPatchLocationListSize = args->NewPatchLocationListSize;
    TlCall call(TlKind::RenderKmt,{r.hContext,r.CommandLength,r.AllocationCount,r.PatchLocationCount,target});
#ifdef VKDU_PROBE_TEST_RESIDENCY_RELEASE_BEFORE_RENDER
    residency.unlock();
#endif
    const NTSTATUS native_status=probe_kmt_render(&r);
    HRESULT hr = nt_result(native_status, "Render");
    call.end({tl_u32(native_status),tl_u32(hr),r.hContext,r.CommandLength,r.AllocationCount,r.PatchLocationCount,target});
    args->pNewCommandBuffer = r.pNewCommandBuffer; args->NewCommandBufferSize = r.NewCommandBufferSize;
    args->pNewAllocationList = r.pNewAllocationList; args->NewAllocationListSize = r.NewAllocationListSize;
    args->pNewPatchLocationList = r.pNewPatchLocationList; args->NewPatchLocationListSize = r.NewPatchLocationListSize;
    if (SUCCEEDED(hr)) {
        ++peer(h)->renders; if (target) ++peer(h)->target_references;
        std::lock_guard<std::mutex> lock(peer(h)->mutex);
        auto &counts = peer(h)->contexts.at(r.hContext);
        ++counts.renders; if (target) ++counts.targets;
    }
    return hr;
}
HRESULT APIENTRY probe_signal(HANDLE h, const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 *args) {
    if (!h || !args || !peer(h)->has_context(kmt_handle(args->hContext)) ||
            args->ObjectCount || args->BroadcastContextCount || args->Flags.Value != 2 || !args->CpuEventHandle)
        return E_INVALIDARG;
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 signal{};
    signal.hContext = kmt_handle(args->hContext);
    signal.Flags = args->Flags;
    signal.CpuEventHandle = args->CpuEventHandle;
    HANDLE observer = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), args->CpuEventHandle, GetCurrentProcess(), &observer,
            0, FALSE, DUPLICATE_SAME_ACCESS)) return HRESULT_FROM_WIN32(GetLastError());
#ifdef VKDU_PROBE_TEST_WRONG_EVENT_CONTEXT
    signal.hContext = peer(h)->context_handle;
#endif
    TlCall call(TlKind::Signal,{signal.hContext,tl_ptr(args->CpuEventHandle),tl_ptr(observer)});
    const NTSTATUS native_status=probe_kmt_signal(&signal);
    HRESULT hr = nt_result(native_status, "SignalSynchronizationObject2(CpuEvent)");
    call.end({tl_u32(native_status),tl_u32(hr),signal.hContext,tl_ptr(args->CpuEventHandle),tl_ptr(observer)});
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(peer(h)->mutex);
        ++peer(h)->contexts.at(signal.hContext).events;
        peer(h)->completions.push_back({signal.hContext, observer});
#ifdef VKDU_PROBE_TEST_CPU_COMPLETION
        SetEvent(observer); /* Only the non-hardware negative binary has this. */
#endif
    } else CloseHandle(observer);
    return hr;
}
void APIENTRY probe_error(D3D10DDI_HRTDEVICE h, HRESULT hr) noexcept {
    timeline.emit(0,TlKind::Error,0,{0,tl_u32(hr)});
    if (h.handle) ++static_cast<ProbeRuntime *>(h.handle)->device_errors;
    std::fprintf(stdout, "native SetErrorCb=%08x\n", static_cast<unsigned>(hr));
}
void APIENTRY probe_command_error(D3D12DDI_HRTCOMMANDLIST h, HRESULT hr) noexcept {
    timeline.emit(0,TlKind::Error,0,{1,tl_u32(hr)});
    if (h.handle) ++static_cast<ProbeRuntime *>(h.handle)->command_errors;
    std::fprintf(stdout, "native SetCommandListErrorCb=%08x\n", static_cast<unsigned>(hr));
}

/* Every DLL-facing callback contains C++ exceptions inside this EXE. A fault
 * keeps the whole session retained; no escaping exception can unwind DLL frames. */
HRESULT APIENTRY safe_query(HANDLE h, const D3DDDICB_QUERYADAPTERINFO *args) noexcept {
    if (!h) return E_INVALIDARG;
    return tl_call(TlKind::QueryFinal,[&] { return callback_boundary(peer(h)->callback_exceptions, [&] { return probe_query(h, args); }); },
        {args ? args->PrivateDriverDataSize : 0});
}
HRESULT APIENTRY safe_context_create(HANDLE h, D3DDDICB_CREATECONTEXT *args) noexcept {
    if (!h) return E_INVALIDARG;
    return callback_boundary(peer(h)->callback_exceptions, [&] { return probe_context_create(h, args); });
}
HRESULT APIENTRY safe_context_destroy(HANDLE h, const D3DDDICB_DESTROYCONTEXT *args) noexcept {
    if (!h) return E_INVALIDARG;
    return callback_boundary(peer(h)->callback_exceptions, [&] { return probe_context_destroy(h, args); });
}
HRESULT APIENTRY safe_escape(HANDLE h, const D3DDDICB_ESCAPE *args) noexcept {
    if (!h) return E_INVALIDARG;
    return callback_boundary(peer(h)->callback_exceptions, [&] { return probe_escape(h, args); });
}
HRESULT APIENTRY safe_allocate(HANDLE h, D3DDDICB_ALLOCATE *args) noexcept {
    if (!h) return E_INVALIDARG;
    return callback_boundary(peer(h)->callback_exceptions, [&] { return probe_allocate(h, args); });
}
HRESULT APIENTRY safe_deallocate(HANDLE h, const D3DDDICB_DEALLOCATE *args) noexcept {
    if (!h) return E_INVALIDARG;
    return callback_boundary(peer(h)->callback_exceptions, [&] { return probe_deallocate(h, args); });
}
HRESULT APIENTRY safe_deallocate2(HANDLE h, const D3DDDICB_DEALLOCATE2 *args) noexcept {
    if (!h) return E_INVALIDARG;
    return callback_boundary(peer(h)->callback_exceptions, [&] { return probe_deallocate2(h, args); });
}
HRESULT APIENTRY safe_lock(HANDLE h, D3DDDICB_LOCK *args) noexcept {
    if (!h) return E_INVALIDARG;
    return callback_boundary(peer(h)->callback_exceptions, [&] { return probe_lock(h, args); });
}
HRESULT APIENTRY safe_unlock(HANDLE h, const D3DDDICB_UNLOCK *args) noexcept {
    if (!h) return E_INVALIDARG;
    return callback_boundary(peer(h)->callback_exceptions, [&] { return probe_unlock(h, args); });
}
HRESULT APIENTRY safe_render(HANDLE h, D3DDDICB_RENDER *args) noexcept {
    if (!h) return E_INVALIDARG;
    return tl_call(TlKind::RenderCallback,[&] { return callback_boundary(peer(h)->callback_exceptions, [&] { return probe_render(h, args); }); },
        {args ? kmt_handle(args->hContext) : 0,args ? args->CommandLength : 0,args ? args->NumAllocations : 0,args ? args->NumPatchLocations : 0});
}
HRESULT APIENTRY safe_signal(HANDLE h, const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 *args) noexcept {
    if (!h) return E_INVALIDARG;
    return callback_boundary(peer(h)->callback_exceptions, [&] { return probe_signal(h, args); });
}
HRESULT APIENTRY safe_queue_context_create(D3D12DDI_HRTCOMMANDQUEUE h, D3DDDICB_CREATECONTEXT *args) noexcept {
    auto *queue = static_cast<ProbeRuntimeQueue *>(h.handle);
    if (!queue || !queue->runtime) return E_INVALIDARG;
    return callback_boundary(queue->runtime->callback_exceptions, [&] { return probe_queue_context_create(h, args); });
}
HRESULT APIENTRY safe_queue_context_destroy(D3D12DDI_HRTCOMMANDQUEUE h, const D3DDDICB_DESTROYCONTEXT *args) noexcept {
    auto *queue = static_cast<ProbeRuntimeQueue *>(h.handle);
    if (!queue || !queue->runtime) return E_INVALIDARG;
    return callback_boundary(queue->runtime->callback_exceptions, [&] { return probe_queue_context_destroy(h, args); });
}
