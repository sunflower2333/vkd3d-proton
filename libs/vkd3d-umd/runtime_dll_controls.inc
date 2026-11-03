void timeline_controls();
/* SPDX-License-Identifier: LGPL-2.1-or-later */
template <typename F> void must_reject(F &&function, const char *message) {
    bool rejected = false;
    try { function(); } catch (const ProbeFailure &) { rejected = true; }
    require(rejected, message);
}

/* Run the actual DllBuffer::identity consumer through typed CPU-only peers.
 * No DLL/KMT/Vulkan entrypoint is loaded or called by these fixtures. */
struct BufferIdentityFixture {
    DllSession session;
    DllBuffer buffer;
    D3D12DDI_RESOURCE_ALLOCATION_INFO info{};
    D3DKMT_HANDLE queried = 0x40000b40, checked = queried;
    UINT64 va = 0x45002a0000ull;
    unsigned status_calls = 0;
    static BufferIdentityFixture &from(D3D12DDI_HDEVICE device) {
        return *static_cast<BufferIdentityFixture *>(device.pDrvPrivate);
    }
    static void APIENTRY allocation_info(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE resource,
            D3D12DDI_RESOURCE_ALLOCATION_INFO *out, D3DKMT_HANDLE *allocation) {
        auto &f = from(device);
        require(resource.pDrvPrivate == f.buffer.resource.pDrvPrivate, "identity control exact resource slot");
        *out = f.info; *allocation = f.queried;
    }
    static D3DKMT_HANDLE APIENTRY allocation_handle(D3D12DDI_HDEVICE device, D3D10DDI_HRESOURCE resource) {
        auto &f = from(device);
        require(resource.pDrvPrivate == f.buffer.resource.pDrvPrivate, "identity control typed handle slot");
        return f.checked;
    }
    static D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY address(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE resource) {
        auto &f = from(device);
        require(resource.pDrvPrivate == f.buffer.resource.pDrvPrivate, "identity control typed GPUVA slot");
        return f.va;
    }
    static HRESULT APIENTRY status(D3D12DDI_HDEVICE device) {
        ++from(device).status_calls; return S_OK;
    }
    BufferIdentityFixture() {
        session.device_live = true; session.create.hDrvDevice = {this};
        session.api.status = status;
        session.api.device.pfnCheckExistingResourceAllocationInfo = allocation_info;
        session.api.device.pfnCheckResourceAllocationHandle = allocation_handle;
        session.api.device.pfnCheckResourceVirtualAddress = address;
        session.runtime.context_id = 36; session.runtime.identity.generation = 2;
        buffer.resource = {this};
        info.ResourceDataSize = info.ResourceDataAlignment = 65536;
        ProbeWireAllocationInfo wire{};
        wire.size = 65536; wire.alignment = 4096; wire.iova = va;
        wire.context_id = 36; wire.generation = 2; wire.flags = 6;
        session.runtime.allocations.emplace(queried, wire);
    }
    ProbeWireAllocationInfo &wire() { return session.runtime.allocations.at(queried); }
    void accept(D3D12DDI_CPU_PAGE_PROPERTY property) {
        buffer.identity(session, property);
        require(buffer.allocation == queried && buffer.address == va && status_calls,
            "accepted identity publishes actual typed handle and GPUVA");
    }
    void reject(D3D12DDI_CPU_PAGE_PROPERTY property, const char *message) {
        const auto prior_allocation = buffer.allocation;
        const auto prior_address = buffer.address;
        must_reject([&] { buffer.identity(session, property); }, message);
        require(buffer.allocation == prior_allocation && buffer.address == prior_address,
            "rejected identity cannot replace published identity");
    }
};

void buffer_identity_controls() {
    unsigned cases = 0;
    std::puts("CONTROL IDENTITY COMPUTE12 property=0 backing_flags=6 handle=40000b40 gpuva=45002a0000 domain=36 generation=2");
    { BufferIdentityFixture f; f.accept(D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE); ++cases; }
    std::puts("PASS IDENTITY COMPUTE12 host-visible default backing does not grant public MapHeap permission");
    { BufferIdentityFixture f; f.wire().flags = 4; f.accept(D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE); ++cases; }
    { BufferIdentityFixture f; f.accept(D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK); ++cases; }
    { BufferIdentityFixture f; f.wire().flags = 4;
      f.reject(D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK, "upload without CPU-visible backing rejected"); ++cases; }
    for (UINT property : {1u, 3u, 0xffffffffu}) {
        BufferIdentityFixture f;
        f.reject(static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(property), "unsupported CPU page property rejected"); ++cases;
    }
    for (unsigned mutation = 0; mutation != 10; ++mutation) {
        BufferIdentityFixture f;
        switch (mutation) {
            case 0: f.session.runtime.allocations.clear(); break;
            case 1: f.wire().size -= 4096; break;
            case 2: f.wire().iova += 65536; break;
            case 3: ++f.wire().context_id; break;
            case 4: ++f.wire().generation; break;
            case 5: f.checked = 0; break;
            case 6: f.queried = f.checked = 0; break;
            case 7: f.va = 0; break;
            case 8: f.info.ResourceDataSize -= 4096; break;
            case 9: f.info.ResourceDataAlignment = 4096; break;
        }
        f.reject(D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE, "bad identity rejected even for host-visible default"); ++cases;
    }
    { BufferIdentityFixture f; f.accept(D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
      f.accept(D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE); ++cases; }
    { BufferIdentityFixture f; f.accept(D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
      const auto wire = f.wire(); f.session.runtime.allocations.clear();
      f.queried = f.checked += 64; f.session.runtime.allocations.emplace(f.queried, wire);
      f.reject(D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE, "changed allocation across batches rejected"); ++cases; }
    { BufferIdentityFixture f; f.accept(D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
      f.va += 65536; f.wire().iova = f.va;
      f.reject(D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE, "changed GPUVA across batches rejected"); ++cases; }
    std::printf("PASS IDENTITY cases=%u actual_DllBuffer_identity=1 controlled_typed_peers=1 no_KMT_Vulkan=1\n", cases);
}

void platform_controls() {
    std::puts("CONTROL ONLY: opaque storage and callback/lifecycle gates; no KMT/Vulkan/GPU work");
    buffer_identity_controls();
    OpaqueStorage storage;
    must_reject([&] { storage.allocate(0, "invalid"); }, "zero size rejected");
    must_reject([&] { storage.allocate(SIZE_MAX, "invalid"); }, "overflow size rejected");
    storage.allocate(513, "control");
    require(reinterpret_cast<uintptr_t>(storage.data()) % 64 == 0 && storage.intact(), "opaque aligned boundaries");
    auto bytes = static_cast<unsigned char *>(storage.data());
    for (size_t i = 0; i < storage.size; ++i) require(bytes[i] == 0, "all supplied private bytes begin zeroed");
    std::memset(bytes, 0x37, storage.size);
    require(storage.intact(), "full advertised opaque extent writable");
    bytes[storage.size] ^= 1;
    require(!storage.intact(), "one-byte overwrite detected outside advertised extent");
    bytes[storage.size] ^= 1;
    must_reject([&] { storage.allocate(64, "duplicate"); }, "live opaque owner not replaced");
    std::atomic_uint faults{0};
    require(callback_boundary(faults, []() -> HRESULT { throw std::bad_alloc(); }) == E_OUTOFMEMORY && faults == 1,
        "bad_alloc cannot cross callback ABI");
    require(callback_boundary(faults, []() -> HRESULT { throw 7; }) == E_FAIL && faults == 2,
        "nonstandard exception cannot cross callback ABI");
    require(callback_boundary(faults, [] { return E_INVALIDARG; }) == E_INVALIDARG && faults == 2,
        "explicit callback failure propagated unchanged");
    BatchGate batch;
    require(!batch.readback_allowed() && !batch.release_allowed(), "uncreated batch cannot permit Map or release");
    batch.begin(22); batch.execute();
    require(!batch.readback_allowed() && !batch.release_allowed(), "successful Execute is not completion");
    must_reject([&] { batch.observed(11); }, "wrong event context rejected");
    batch.observed(22);
    require(!batch.readback_allowed() && !batch.release_allowed(), "event without typed queue drain is insufficient");
    batch.destroy_start();
    must_reject([&] { batch.destroy_finish(11, true, true); }, "wrong DestroyContext cannot certify drain");
    batch.destroy_finish(22, true, true);
    require(batch.readback_allowed() && batch.release_allowed(), "matching observed event and typed drain permit Map");
    must_reject([&] { batch.destroy_start(); }, "duplicate queue destroy rejected");
    BatchGate failed;
    failed.begin(23); failed.execute(); failed.observed(23); failed.destroy_start(); failed.destroy_finish(23, false, true);
    require(!failed.readback_allowed() && !failed.release_allowed(), "residual event prevents readback and storage/module release");
    BatchGate error;
    error.begin(24); error.execute(); error.observed(24); error.destroy_start(); error.destroy_finish(24, true, false);
    require(!error.readback_allowed() && !error.release_allowed(), "callback or status error prevents release");
    require(manifest_library("{\"ICD\":{\"library_path\":\"C:\\\\private\\\\vulkan.dll\"}}") == L"C:\\private\\vulkan.dll",
        "pinned manifest path decodes Windows separators");
    must_reject([] { manifest_library("{\"library_path\":\"a\",\"library_path\":\"b\"}"); }, "ambiguous ICD manifest rejected");
    must_reject([] { canonical_path(L"relative.dll"); }, "implicit CWD DLL resolution rejected");
    std::puts("PASS CONTROL opaque sizing/alignment/zeroing/canary, exception ABI containment, batch event/drain/Map/release ordering");
}

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Executes the real probe provider with intercepted KMT syscalls. These tests
 * prove forwarding, failure transactions and observation gates, never GPU work. */
static UINT control_destroy_flags, control_destroy_count, control_signal_context, control_closed_context;
static unsigned control_destroy_calls, control_signal_calls, control_close_calls;
static NTSTATUS control_destroy_result, control_signal_result;
static NTSTATUS APIENTRY control_deallocate(const D3DKMT_DESTROYALLOCATION2 *args) {
    ++control_destroy_calls; control_destroy_flags = args->Flags.Value; control_destroy_count = args->AllocationCount;
    require(args->hDevice == 42 && args->AllocationCount == 1 && args->phAllocationList[0] == 101 && !args->hResource,
        "provider copied exact allocation request");
    return control_destroy_result;
}
static NTSTATUS APIENTRY control_signal(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 *args) {
    ++control_signal_calls; control_signal_context = args->hContext;
    require(args->Flags.Value == 2 && !args->ObjectCount && !args->BroadcastContextCount && args->CpuEventHandle,
        "provider queued only context-ordered OS completion event");
    return control_signal_result;
}
static NTSTATUS APIENTRY control_destroy_context(const D3DKMT_DESTROYCONTEXT *args) {
    ++control_close_calls; control_closed_context = args->hContext; return 0;
}
void provider_controls() {
    std::puts("CONTROL ONLY: provider forwarding and Windows event state, no KMT/Vulkan/GPU calls");
    probe_kmt_deallocate = control_deallocate; probe_kmt_signal = control_signal;
    probe_kmt_destroy_context = control_destroy_context;
    ProbeRuntime runtime;
    /* Keep destructor away from fake handles on every failed assertion. */
    struct FakeDeviceGuard { ProbeRuntime &r; ~FakeDeviceGuard() { r.device = r.adapter = 0; } } guard{runtime};
    runtime.device = 42; runtime.context_handle = 11;
    runtime.contexts.emplace(11, ProbeRuntime::ContextCounts{});
    runtime.contexts.emplace(22, ProbeRuntime::ContextCounts{});
    runtime.allocations.emplace(101, ProbeWireAllocationInfo{});
    D3DKMT_HANDLE allocation = 101;
    D3DDDICB_DEALLOCATE2 release{}; release.NumAllocations = 1; release.HandleList = &allocation;
    release.Flags.AssumeNotInUse = release.Flags.SynchronousDestroy = 1;
    control_destroy_result = static_cast<NTSTATUS>(0xc0000001u);
    require(FAILED(safe_deallocate2(&runtime, &release)) && runtime.has(101) && runtime.pageable_deallocations == 0,
        "failed typed deallocation retains allocation ownership");
    require(control_destroy_flags == 3 && control_destroy_count == 1,
        "Deallocate2 preserves both flags: AssumeNotInUse and SynchronousDestroy");
    const auto before = control_destroy_calls;
    for (UINT flags : {0u, 1u, 2u, 7u}) {
        release.Flags.Value = flags;
        require(safe_deallocate2(&runtime, &release) == E_INVALIDARG && control_destroy_calls == before,
            "invalid public flags rejected before KMT");
    }
    release.Flags.Value = 3;
    allocation = 999;
    require(safe_deallocate2(&runtime, &release) == E_INVALIDARG && control_destroy_calls == before,
        "foreign allocation rejected before KMT");
    allocation = 101; control_destroy_result = 0;
    check(safe_deallocate2(&runtime, &release), "typed deallocation");
    require(!runtime.has(101) && runtime.pageable_deallocations == 1 && !runtime.internal_deallocations,
        "successful typed close retires exactly one owner");
    runtime.allocations.emplace(101, ProbeWireAllocationInfo{});
    D3DDDICB_DEALLOCATE internal{}; internal.NumAllocations = 1; internal.HandleList = &allocation;
    check(safe_deallocate(&runtime, &internal), "internal deallocation");
    require(control_destroy_flags == 0 && !runtime.has(101) && runtime.internal_deallocations == 1,
        "internal scratch keeps independent legacy flags");
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(event != nullptr, "controlled Windows event");
    struct EventGuard { HANDLE h; ~EventGuard() { CloseHandle(h); } } event_guard{event};
    D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 signal{};
    signal.hContext = runtime_context(22); signal.Flags.EnqueueCpuEvent = 1; signal.CpuEventHandle = event;
    control_signal_result = static_cast<NTSTATUS>(0xc0000001u);
    require(FAILED(safe_signal(&runtime, &signal)) && runtime.completions.empty() && !runtime.counts(22).events,
        "failed event enqueue publishes no completion");
    control_signal_result = 0;
    check(safe_signal(&runtime, &signal), "controlled event enqueue");
    require(control_signal_context == 22 && runtime.counts(22).events == 1 && !runtime.counts(11).events,
        "completion routed to selected queue, not allocation owner");
    require(runtime.observe_completions(22) == DXGI_ERROR_WAS_STILL_DRAWING && !runtime.counts(22).observed,
        "successful enqueue is not GPU completion and must not CPU-signal event");
    D3DDDICB_DESTROYCONTEXT close{}; close.hContext = runtime_context(22);
    require(probe_destroy_context(runtime, &close) == DXGI_ERROR_WAS_STILL_DRAWING && !control_close_calls && runtime.has_context(22),
        "queue context cannot close before its completion event");
    /* This SetEvent is confined to the named CPU control, explicitly models
     * OS completion, and is not reachable from a hardware run. */
    require(SetEvent(event) != FALSE, "simulate OS event completion in control");
    check(runtime.observe_completions(22), "observe simulated completion");
    require(runtime.counts(22).observed == 1 && runtime.completions[0].event == nullptr,
        "completion observed once and duplicate observer handle released");
    check(runtime.observe_completions(22), "idempotent completion observation");
    require(runtime.counts(22).observed == 1, "observation not counted twice");
    check(probe_destroy_context(runtime, &close), "context after simulated completion");
    require(control_close_calls == 1 && control_closed_context == 22 && !runtime.has_context(22) && runtime.has_context(11),
        "exact queue context released once after completion");
    const auto signals = control_signal_calls;
    require(safe_signal(&runtime, &signal) == E_INVALIDARG && control_signal_calls == signals,
        "late completion against destroyed queue rejected");
    signal.hContext = runtime_context(11); signal.CpuEventHandle = nullptr;
    require(safe_signal(&runtime, &signal) == E_INVALIDARG && control_signal_calls == signals,
        "missing completion event rejected before KMT");
    runtime.retirement_sealed = true;
    D3DDDICB_RENDER late{}; late.hContext = runtime_context(11);
    require(safe_render(&runtime, &late) == E_UNEXPECTED && !runtime.renders,
        "terminal seal rejects late Render before real KMT");
    require(runtime.close() == DXGI_ERROR_WAS_STILL_DRAWING && runtime.device == 42,
        "residual active context prevents provider teardown");
    timeline_controls();
    std::puts("PASS provider Deallocate2 flags3/legacy0, failed-close retention, invalid requests, selected-context event route, pending-event retention, observed completion and exact queue release; CONTROL ONLY");
}

struct SyntheticAdapter {
    std::atomic_uint faults{0};
    bool throw_query = false, corrupt_query = false;
};
HRESULT APIENTRY synthetic_query(HANDLE opaque, const D3DDDICB_QUERYADAPTERINFO *request) noexcept {
    auto *owner = static_cast<SyntheticAdapter *>(opaque);
    if (!owner) return E_INVALIDARG;
    return callback_boundary(owner->faults, [&]() -> HRESULT {
        if (owner->throw_query) throw std::bad_alloc();
        if (!request || !request->pPrivateDriverData || request->PrivateDriverDataSize != 160) return E_INVALIDARG;
        std::array<UINT, 40> reply{};
        reply[0] = 0x504d5644; reply[2] = 128; reply[6] = 7;
        reply[32] = 0x44494c56; reply[33] = 1; reply[34] = 32; reply[35] = 1;
        reply[36] = 9; reply[37] = 1; reply[38] = 1;
        if (owner->corrupt_query) reply[39] = 1;
        std::memcpy(request->pPrivateDriverData, reply.data(), sizeof(reply));
        return S_OK;
    });
}

void inspect_dll(const wchar_t *path, const wchar_t *hash, const wchar_t *negative) {
    std::puts("CONTROL ONLY: actual DLL exports/tables/CalcPrivateDeviceSize with synthetic adapter identity; no CreateDevice or KMT/GPU");
    DllApi api;
    if (negative && !std::wcscmp(negative, L"preloaded")) {
        const auto absolute = canonical_path(path);
        require(LoadLibraryExW(absolute.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr,
            "negative control preloads selected basename");
    }
    api.load(path, hash);
    if (negative && !std::wcscmp(negative, L"foreign-pointer")) {
        auto corrupt = api.queues;
        corrupt.pfnExecuteCommandLists = reinterpret_cast<decltype(corrupt.pfnExecuteCommandLists)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId"));
        api.audit_table(corrupt, "injected-foreign-queue");
        throw ProbeFailure("foreign table pointer unexpectedly accepted");
    }
    SyntheticAdapter provider;
    D3DDDI_ADAPTERCALLBACKS callbacks{}; callbacks.pfnQueryAdapterInfoCb = synthetic_query;
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    open.hRTAdapter.handle = &provider; open.pAdapterCallbacks = &callbacks; open.pAdapterFuncs = &adapter;
    provider.throw_query = true;
    require(api.open(&open) == E_OUTOFMEMORY && !open.hAdapter.pDrvPrivate && provider.faults == 1,
        "exception-contained Query callback failure returns through actual DLL OpenAdapter");
    provider.throw_query = false; provider.corrupt_query = true;
    require(api.open(&open) == DXGI_ERROR_UNSUPPORTED && !open.hAdapter.pDrvPrivate,
        "actual DLL rejects malformed synthetic query before adapter publication");
    provider.corrupt_query = false;
    check(api.open(&open), "actual typed OpenAdapter synthetic identity control");
    require(open.hAdapter.pDrvPrivate != nullptr, "actual DLL supplied opaque adapter handle");
    api.audit_table(adapter, "adapter");
    D3D12DDIARG_CALCPRIVATEDEVICESIZE size{};
    size.Interface = D3D12DDI_INTERFACE_VERSION_R0; size.Version = D3D12DDI_BUILD_VERSION << 16;
    const auto bytes = adapter.pfnCalcPrivateDeviceSize(open.hAdapter, &size);
    OpaqueStorage storage; storage.allocate(bytes, "actual-DLL-device-control");
    UINT count = 123;
    check(adapter.pfnGetSupportedVersions(open.hAdapter, &count, nullptr), "actual DLL version query");
    require(count == 0, "public zero-version gate preserved");
    size.Interface ^= 1;
    require(adapter.pfnCalcPrivateDeviceSize(open.hAdapter, &size) == 0,
        "actual typed CalcPrivateDeviceSize rejects unsupported layout");
    check(adapter.pfnCloseAdapter(open.hAdapter), "actual typed CloseAdapter");
    api.unload();
    require(storage.intact(), "actual typed sizing preserves caller canaries");
    std::puts("PASS CONTROL actual DLL path/hash/architecture/export/table ownership, callback error, opaque CalcPrivate sizing, zero public gate and typed adapter cleanup");
}
/* The recorder controls never enter real KMT, a UMD DLL, or Vulkan. */
static std::array<unsigned char,192> timeline_query_reply{};
static NTSTATUS timeline_query_status=0;
static unsigned timeline_query_calls=0,timeline_write_calls=0;
static NTSTATUS APIENTRY timeline_query_stub(const D3DKMT_QUERYADAPTERINFO *q) {
    ++timeline_query_calls;
    require(q && (q->PrivateDriverDataSize==160 || q->PrivateDriverDataSize==192),"controlled query size");
    std::memcpy(q->pPrivateDriverData,timeline_query_reply.data(),q->PrivateDriverDataSize);
    SetLastError(0x7654); return timeline_query_status;
}
static BOOL WINAPI timeline_clock_fail(LARGE_INTEGER *) { SetLastError(0x5432); return FALSE; }
static BOOL WINAPI timeline_write_fail(HANDLE,LPCVOID,DWORD,LPDWORD bytes,LPOVERLAPPED) {
    ++timeline_write_calls; *bytes=0; SetLastError(ERROR_DISK_FULL); return FALSE;
}
static BOOL WINAPI timeline_write_short(HANDLE,LPCVOID,DWORD requested,LPDWORD bytes,LPOVERLAPPED) {
    ++timeline_write_calls; *bytes=requested-1; SetLastError(0x3210); return TRUE;
}
void timeline_controls() {
    std::puts("CONTROL ONLY: fixed timeline recorder and intercepted adapter query, no KMT/Vulkan/GPU work");
    timeline.begin(false);
    unsigned invocations=0;
    SetLastError(0x1234);
    const auto status=tl_call(TlKind::RenderKmt,[&]() -> NTSTATUS { ++invocations; SetLastError(0x2345); return static_cast<NTSTATUS>(0xc0000001u); });
    require(static_cast<uint32_t>(status)==0xc0000001u && invocations==1 && GetLastError()==0x2345,
        "diagnostic wrapper invokes exactly once and preserves raw NTSTATUS/last-error");
    const HRESULT hresult=tl_call(TlKind::CreateHeap,[&]() -> HRESULT { ++invocations; SetLastError(0x3456); return DXGI_ERROR_DEVICE_REMOVED; });
    require(hresult==DXGI_ERROR_DEVICE_REMOVED && invocations==2 && GetLastError()==0x3456,
        "diagnostic wrapper preserves HRESULT/last-error exactly");
    require(timeline.snapshot(1,1)==128+4*192+64 && timeline.output[16+24+6]==0xc0000001u &&
        timeline.output[16+3*24+6]==0x887a0005u,"serialized raw status low32 and terminal failure");
    std::puts("PASS TIMELINE exact HRESULT/NTSTATUS/last-error and single invocation");

    timeline.begin(false);
    auto original_query=probe_kmt_query; probe_kmt_query=timeline_query_stub;
    ProbeRuntime r; r.adapter=11; r.requested_luid={9,1};
    const auto u32=[](size_t offset,uint32_t value) { std::memcpy(timeline_query_reply.data()+offset,&value,4); };
    u32(0,0x504d5644);u32(8,128);u32(24,7);u32(128,0x44494c56);u32(132,1);u32(136,32);u32(140,1);
    u32(144,9);u32(148,1);u32(152,1);u32(160,0x43445156);u32(164,1);u32(168,32);u32(172,1);
    u32(176,32);u32(180,40);u32(184,64);
    for (UINT size : {160u,192u}) {
        std::array<unsigned char,192> bytes{};
        D3DDDICB_QUERYADAPTERINFO q{};q.pPrivateDriverData=bytes.data();q.PrivateDriverDataSize=size;
        require(safe_query(&r,&q)==S_OK && !std::memcmp(bytes.data(),timeline_query_reply.data(),size),
            "timeline query leaves 160/192 byte reply unchanged");
        require(r.identity.generation==7 && tl_luid(r.identity.luid)==0x100000009ull,
            "unchanged provider publishes expected identity");
    }
    u32(24,8);
    std::array<unsigned char,192> bytes{};
    D3DDDICB_QUERYADAPTERINFO q{};q.pPrivateDriverData=bytes.data();q.PrivateDriverDataSize=192;
    require(safe_query(&r,&q)==DXGI_ERROR_DEVICE_REMOVED && r.identity.generation==7 &&
        !std::memcmp(bytes.data(),timeline_query_reply.data(),192),"changed raw generation rejects without changing previous identity or reply");
    probe_kmt_query=original_query;r.adapter=0;
    timeline.snapshot(0,0);
    unsigned seen160=0,seen192=0,seen8=0,removed=0;
    for (uint64_t n=0;n<timeline.published;++n) {
        const auto *p=timeline.output.data()+16+24*n;
        if (p[4]==static_cast<uint64_t>(TlKind::QueryKmt) && p[5]==2) {
            seen160 += p[8]==160 && p[10]==0x100000009ull && p[11]==7;
            seen192 += p[8]==192 && p[10]==0x100000009ull && p[11]==7;
            seen8 += p[8]==192 && p[11]==8;
        }
        if (p[4]==static_cast<uint64_t>(TlKind::QueryFinal) && p[5]==2 && p[6]==0x887a0005u) ++removed;
    }
    require(timeline_query_calls==3 && seen160==1 && seen192==1 && seen8==1 && removed==1,
        "committed query events retain raw sizes/LUID/generation/final HRESULT");
    std::puts("PASS TIMELINE actual provider 160/192 unchanged bytes and raw generation7-to8 rejection");

    timeline.begin(false);
    std::atomic_bool reserved{false},finish{false};
    std::thread unfinished([&] {
        const auto slot=timeline.reserve();reserved.store(true,std::memory_order_release);
        while(!finish.load(std::memory_order_acquire)) std::this_thread::yield();
        timeline.publish(slot,999,TlKind::Run,1,{99});
    });
    while(!reserved.load(std::memory_order_acquire)) std::this_thread::yield();
    std::array<std::thread,4> producers;
    for (unsigned thread=0;thread<producers.size();++thread) producers[thread]=std::thread([thread] {
        for (unsigned n=0;n<64;++n) timeline.emit(thread*64+n+1,TlKind::Observe,0,{thread,n});
    });
    for(auto &thread:producers) thread.join();
    const DWORD bytes_before=timeline.snapshot(1,1);
    require(timeline.reserved==257 && timeline.published==256 && timeline.incomplete==1,
        "close copies only published records and exposes unfinished reservation");
    std::array<bool,257> seen{};
    for(uint64_t n=0;n<timeline.published;++n) {
        const auto *p=timeline.output.data()+16+n*24;
        require(p[0]>0 && p[0]<257 && !seen[p[0]],"concurrent slots unique and unfinished payload absent");seen[p[0]]=true;
    }
    const auto first=timeline.output[16],last=timeline.output[16+255*24];
    finish.store(true,std::memory_order_release);unfinished.join();
    timeline.emit(1234,TlKind::Terminal,0,{7});
    require((timeline.reservation.load()&~Timeline::closed)==257 && timeline.published==256 && timeline.incomplete==1 &&
        timeline.output[16]==first && timeline.output[16+255*24]==last && bytes_before==128+256*192+64,
        "late publication cannot mutate snapshot or reopen reservations");
    std::puts("PASS TIMELINE concurrent publication, incomplete slot and closed immutable snapshot");

    // COPY11 reached 3342 events; the old 2048-slot recorder lost 1294.
    timeline.begin(false);
    for(unsigned n=0;n<3342;++n) timeline.emit(0,TlKind::Terminal,0,{n,0});
    const DWORD copy11_bytes=timeline.snapshot(0,0);
    std::printf("CONTROL TIMELINE copy11 events=3342 reserved=%llu published=%llu overflow=%llu bytes=%u\n",
        static_cast<unsigned long long>(timeline.reserved),static_cast<unsigned long long>(timeline.published),
        static_cast<unsigned long long>(timeline.output[7]),static_cast<unsigned>(copy11_bytes));
    require(timeline.reserved==3342 && timeline.published==3342 && !timeline.output[7] && !timeline.output[8] &&
        copy11_bytes==128+3342*192+64,"COPY11 observed event volume retained without overflow");
    for(uint64_t n=0;n<3342;++n) {
        const auto *p=timeline.output.data()+16+n*24;
        require(p[0]==n && p[4]==static_cast<uint64_t>(TlKind::Terminal) && p[6]==n,
            "COPY11 volume keeps every reservation and payload in order");
    }
    timeline.begin(false);
    for(uint64_t n=0;n<Timeline::capacity;++n) timeline.emit(0,TlKind::Terminal,0,{n,0});
    require(timeline.snapshot(0,0)==sizeof(timeline.output) && timeline.reserved==Timeline::capacity &&
        timeline.published==Timeline::capacity && !timeline.output[7] && !timeline.output[8] &&
        timeline.output[16+(Timeline::capacity-1)*24]==Timeline::capacity-1,
        "exact capacity retains the final slot and writes the full bounded snapshot");
    std::printf("PASS TIMELINE COPY11 volume and exact capacity; slots=%llu slot_bytes=%zu snapshot_bytes=%zu recorder_bytes=%zu\n",
        static_cast<unsigned long long>(Timeline::capacity),sizeof(Timeline::Slot),sizeof(timeline.output),sizeof(Timeline));

    timeline.begin(false);
    for(uint64_t n=0;n<Timeline::capacity+3;++n) timeline.emit(n+1,TlKind::Observe,0,{n});
    timeline.snapshot(0,0);
    require(timeline.reserved==Timeline::capacity && timeline.published==Timeline::capacity && timeline.output[7]==3 && !timeline.output[8],
        "fixed capacity records explicit overflow without silent acceptance");
    auto original_qpc=timeline.qpc;timeline.qpc=timeline_clock_fail;
    timeline.begin(false);SetLastError(0x5678);timeline.emit(1,TlKind::Run,1,{});
    require(GetLastError()==0x5678,"failed diagnostic clock preserves last-error");
    timeline.snapshot(1,1);timeline.qpc=original_qpc;
    require((timeline.output[9]&42)==42 && !timeline.output[7],"clock failure independent from overflow");
    auto original_write=timeline.write_file;timeline.write_file=timeline_write_fail;
    SetLastError(0x6789);auto failed=timeline.write_once(nullptr,timeline.output.data(),192);
    require(!failed.ok && failed.expected==192 && !failed.actual && failed.error==ERROR_DISK_FULL &&
        GetLastError()==0x6789,"one failed write keeps expected/actual/error and last-error");
    timeline.write_file=timeline_write_short;auto short_write=timeline.write_once(nullptr,timeline.output.data(),192);
    timeline.write_file=original_write;
    require(short_write.ok && short_write.actual==191 && short_write.expected==192 && timeline_write_calls==2,
        "successful short write remains distinguishable and is never retried");
    timeline.begin();
    TlCall committed(TlKind::Run,{0,0x100000009ull});committed.end({0});
    timeline.emit(0,TlKind::Terminal,0,{0,0});timeline.finish(0,0);
    require(GetFileAttributesW(timeline.binary_path)!=INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(timeline.status_path)!=INVALID_FILE_ATTRIBUTES,"actual bounded files created outside callbacks");
    std::puts("PASS TIMELINE capacity overflow, clock fault, write failure and short write remain distinct");
}

