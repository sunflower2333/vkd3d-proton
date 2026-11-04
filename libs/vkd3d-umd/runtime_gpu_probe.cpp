/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real KMT/GPU probe with explicitly emulated Microsoft runtime callbacks.
 * Uses production native entry and backend. Never system D3D12 acceptance. */
#include "ddi.h"
#include <d3dkmthk.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <map>
#include <set>
#include <vector>
#include <stdexcept>
#include "ddi.cpp"

namespace {
struct ProbeFailure : std::runtime_error { using std::runtime_error::runtime_error; };
void check(HRESULT result, const char *operation) {
    if (FAILED(result)) {
        std::fprintf(stderr, "FAIL %s hr=%08x\n", operation, static_cast<unsigned>(result));
        throw ProbeFailure(operation);
    }
}
void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", message); throw ProbeFailure(message); }
}
HRESULT nt_result(NTSTATUS status, const char *operation) {
    if (status >= 0) return S_OK;
    std::fprintf(stderr, "KMT %s status=%08x\n", operation, static_cast<unsigned>(status));
    return static_cast<HRESULT>(static_cast<uint32_t>(status) | 0x10000000u);
}
D3DKMT_HANDLE kmt_handle(HANDLE value) { return static_cast<D3DKMT_HANDLE>(reinterpret_cast<uintptr_t>(value)); }
HANDLE runtime_context(D3DKMT_HANDLE value) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)); }
/* Only --validate-provider-controls replaces these functions. Hardware runs
 * always call the actual KMT entrypoints; no CPU event is signalled here. */
static auto probe_kmt_deallocate = D3DKMTDestroyAllocation2;
static auto probe_kmt_signal = D3DKMTSignalSynchronizationObject2;
static auto probe_kmt_destroy_context = D3DKMTDestroyContext;

struct Deadline {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    std::thread worker;
    Deadline() : worker([this]() {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(90), [this]() { return done; })) {
            std::fputs("FAIL shared-backing probe exceeded90s; terminating probe process\n", stderr);
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
    std::mutex mutex;
    std::map<D3DKMT_HANDLE, NativeAllocationInfo> allocations;
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
    HRESULT ensure_paging_queue() {
        std::lock_guard<std::mutex> lock(residency_mutex);
        if (paging_queue) return paging_ready ? S_OK : E_FAIL;
        D3DKMT_CREATEPAGINGQUEUE request{};
        request.hDevice = device; request.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        const NTSTATUS status = D3DKMTCreatePagingQueue(&request);
        // Even a malformed or failing reply can transfer a queue to clean up.
        paging_queue = request.hPagingQueue; paging_sync = request.hSyncObject;
        paging_cpu_fence = static_cast<volatile UINT64 *>(request.FenceValueCPUVirtualAddress);
        std::fprintf(stderr, "RESIDENCY CreatePagingQueue device=%u queue=%u sync=%u status=%08x\n",
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
            const NTSTATUS status = D3DKMTMakeResident(&request);
            std::fprintf(stderr, "RESIDENCY MakeResident allocation=%u status=%08x count=%u fence=%llu trim=%llu\n",
                    allocation, static_cast<unsigned>(status), request.NumAllocations,
                    static_cast<unsigned long long>(request.PagingFenceValue),
                    static_cast<unsigned long long>(request.NumBytesToTrim));
            if (status == 0 || status == static_cast<NTSTATUS>(0x103)) {
                if (request.NumAllocations != 1 || (status == 0x103 && !request.PagingFenceValue)) return E_FAIL;
                resident_allocations.insert(allocation);
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
        const UINT64 target = pending_paging_fence;
        if (!target) return S_OK;
        if (!paging_queue || !paging_sync || !paging_cpu_fence) return E_FAIL;
        if (*paging_cpu_fence < target) {
            HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!event) {
                const DWORD error = GetLastError();
                return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
            }
            D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU request{};
            request.hDevice = device; request.ObjectCount = 1;
            request.ObjectHandleArray = &paging_sync; request.FenceValueArray = &target;
            request.hAsyncEvent = event;
            const NTSTATUS status = D3DKMTWaitForSynchronizationObjectFromCpu(&request);
            const DWORD waited = status == 0 ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
            const DWORD error = waited == WAIT_FAILED ? GetLastError() : waited;
            CloseHandle(event);
            std::fprintf(stderr, "RESIDENCY paging fence=%llu status=%08x wait=%u observed=%llu\n",
                    static_cast<unsigned long long>(target), static_cast<unsigned>(status), static_cast<unsigned>(waited),
                    static_cast<unsigned long long>(*paging_cpu_fence));
            if (status != 0) return status < 0 ? nt_result(status, "WaitForSynchronizationObjectFromCpu(paging)") : E_FAIL;
            if (waited != WAIT_OBJECT_0) return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
            if (*paging_cpu_fence < target) return E_FAIL;
        }
        pending_paging_fence = 0;
        return S_OK;
    }
    HRESULT wait_render_residency(const D3DDDICB_RENDER &render) {
        std::lock_guard<std::mutex> lock(residency_mutex);
        for (UINT i = 0; i < render.NumAllocations; ++i)
            if (!resident_allocations.count(render.pNewAllocationList[i].hAllocation)) return E_FAIL;
        return wait_paging_locked();
    }
    ~ProbeRuntime() {
        close();
        for (auto &completion : completions) if (completion.event) CloseHandle(completion.event);
    }
    HRESULT observe_completions(D3DKMT_HANDLE context) {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto &completion : completions) {
            if (!completion.event || (context && completion.context != context)) continue;
            const DWORD state = WaitForSingleObject(completion.event, 0);
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
        HRESULT hr = nt_result(D3DKMTOpenAdapterFromLuid(&a), "OpenAdapterFromLuid");
        adapter = a.hAdapter; check(hr, "OpenAdapterFromLuid");
        require(adapter != 0, "nonzero KMT adapter");
        D3DKMT_CREATEDEVICE d{}; d.hAdapter = adapter;
        hr = nt_result(D3DKMTCreateDevice(&d), "CreateDevice");
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
};
ProbeRuntime *peer(HANDLE value) { return static_cast<ProbeRuntime *>(value); }

HRESULT APIENTRY probe_query(HANDLE h, const D3DDDICB_QUERYADAPTERINFO *args) {
    if (!h || !args) return E_INVALIDARG;
    D3DKMT_QUERYADAPTERINFO q{};
    q.hAdapter = peer(h)->adapter; q.Type = KMTQAITYPE_UMDRIVERPRIVATE;
    q.pPrivateDriverData = args->pPrivateDriverData; q.PrivateDriverDataSize = args->PrivateDriverDataSize;
    HRESULT hr = nt_result(D3DKMTQueryAdapterInfo(&q), "QueryAdapterInfo");
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
    HRESULT hr = nt_result(D3DKMTCreateContext(&c), "CreateContext");
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
#endif
    NTSTATUS status = probe_kmt_destroy_context(&c);
    for (unsigned attempt = 0; attempt < 100 &&
            (static_cast<uint32_t>(status) == 0x80000011u || static_cast<uint32_t>(status) == 0xc01e0102u); ++attempt) {
        Sleep(1); status = probe_kmt_destroy_context(&c);
    }
    if (status >= 0) {
        std::lock_guard<std::mutex> lock(runtime.mutex);
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
    return hr;
}
HRESULT APIENTRY probe_queue_context_destroy(D3D12DDI_HRTCOMMANDQUEUE h, const D3DDDICB_DESTROYCONTEXT *args) {
    auto *queue = static_cast<ProbeRuntimeQueue *>(h.handle);
    if (!queue || !queue->runtime || !args || kmt_handle(args->hContext) != queue->context) return E_INVALIDARG;
    HRESULT hr = probe_destroy_context(*queue->runtime, args);
    if (SUCCEEDED(hr)) queue->context = 0;
    return hr;
}
HRESULT APIENTRY probe_escape(HANDLE h, const D3DDDICB_ESCAPE *args) {
    if (!h || !args || args->hDevice != h) return E_INVALIDARG;
    D3DKMT_ESCAPE e{}; e.hAdapter = peer(h)->adapter; e.hDevice = peer(h)->device;
    e.hContext = kmt_handle(args->hContext); e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = args->pPrivateDriverData; e.PrivateDriverDataSize = args->PrivateDriverDataSize;
    HRESULT hr = nt_result(D3DKMTEscape(&e), "Escape");
    if (SUCCEEDED(hr) && args->PrivateDriverDataSize == sizeof(NativeContextInfo)) {
        auto *info = static_cast<NativeContextInfo *>(args->pPrivateDriverData);
        if (info->opcode == 1) peer(h)->context_id = info->context_id;
    }
    return hr;
}
HRESULT APIENTRY probe_allocate(HANDLE h, D3DDDICB_ALLOCATE *args) {
    if (!h || !args || args->hResource || args->NumAllocations != 1 || !args->pAllocationInfo ||
            args->pAllocationInfo[0].PrivateDriverDataSize != sizeof(NativeAllocationInfo)) return E_INVALIDARG;
    auto *r = peer(h);
    const auto info = *static_cast<const NativeAllocationInfo *>(args->pAllocationInfo[0].pPrivateDriverData);
    if (!r->context_id || info.context_id != r->context_id) return E_INVALIDARG;
    HRESULT paging = r->ensure_paging_queue();
    if (FAILED(paging)) return paging;
    D3DKMT_CREATEALLOCATION a{}; a.hDevice = r->device; a.NumAllocations = 1;
    a.pAllocationInfo = args->pAllocationInfo; a.Flags.NonSecure = 1;
    NTSTATUS status = D3DKMTCreateAllocation(&a);
    for (unsigned attempt = 0; attempt < 100 && static_cast<uint32_t>(status) == 0x80000011u &&
            !args->pAllocationInfo[0].hAllocation; ++attempt) {
        Sleep(1); status = D3DKMTCreateAllocation(&a);
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
            r->resident_allocations.erase(handle); r->allocations.erase(handle);
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
    for (UINT i = 0; i < count; ++i) {
        if (!peer(h)->has(snapshot[i])) return E_INVALIDARG;
        for (UINT j = 0; j < i; ++j) if (snapshot[i] == snapshot[j]) return E_INVALIDARG;
    }
    std::lock_guard<std::mutex> residency(peer(h)->residency_mutex);
    HRESULT paged = peer(h)->wait_paging_locked();
    if (FAILED(paged)) return paged;
    for (auto allocation : snapshot) {
        if (!peer(h)->resident_allocations.count(allocation)) continue;
        D3DKMT_EVICT evict{}; evict.hDevice = peer(h)->device;
        evict.NumAllocations = 1; evict.AllocationList = &allocation;
        const NTSTATUS status = D3DKMTEvict(&evict);
        if (status != 0) return status < 0 ? nt_result(status, "Evict") : E_FAIL;
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
    if (!h || !args || !peer(h)->has(args->hAllocation)) return E_INVALIDARG;
    std::lock_guard<std::mutex> residency(peer(h)->residency_mutex);
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
            if (peer(h)->target_handle == args->hAllocation) peer(h)->target_handle = l.hAllocation;
        }
        args->hAllocation = l.hAllocation; args->pData = l.pData;
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
    if (!h || !args || !peer(h)->has_context(kmt_handle(args->hContext))) return E_INVALIDARG;
    if (peer(h)->retirement_sealed) {
        std::fputs("FAIL unexpected Render after terminal completion seal\n", stderr); return E_UNEXPECTED;
    }
    bool target = false;
    for (UINT i = 0; i < args->NumAllocations; ++i) {
        if (!peer(h)->has(args->pNewAllocationList[i].hAllocation)) return E_INVALIDARG;
        target |= args->pNewAllocationList[i].hAllocation == peer(h)->target_handle;
    }
    HRESULT paged = peer(h)->wait_render_residency(*args);
    if (FAILED(paged)) return paged;
    D3DKMT_RENDER r{}; r.hContext = kmt_handle(args->hContext);
    r.CommandLength = args->CommandLength; r.AllocationCount = args->NumAllocations;
    r.PatchLocationCount = args->NumPatchLocations;
    r.pNewCommandBuffer = args->pNewCommandBuffer; r.NewCommandBufferSize = args->NewCommandBufferSize;
    r.pNewAllocationList = args->pNewAllocationList; r.NewAllocationListSize = args->NewAllocationListSize;
    r.pNewPatchLocationList = args->pNewPatchLocationList; r.NewPatchLocationListSize = args->NewPatchLocationListSize;
    HRESULT hr = nt_result(D3DKMTRender(&r), "Render");
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
    HRESULT hr = nt_result(probe_kmt_signal(&signal), "SignalSynchronizationObject2(CpuEvent)");
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
void APIENTRY probe_error(D3D10DDI_HRTDEVICE h, HRESULT hr) {
    if (h.handle) ++static_cast<ProbeRuntime *>(h.handle)->device_errors;
    std::fprintf(stderr, "native SetErrorCb=%08x\n", static_cast<unsigned>(hr));
}
void APIENTRY probe_command_error(D3D12DDI_HRTCOMMANDLIST h, HRESULT hr) {
    if (h.handle) ++static_cast<ProbeRuntime *>(h.handle)->command_errors;
    std::fprintf(stderr, "native SetCommandListErrorCb=%08x\n", static_cast<unsigned>(hr));
}

struct Owned {
    vkdu_object *value = nullptr;
    ~Owned() { vkdu_object_destroy(value); }
    void reset() { vkdu_object_destroy(value); value = nullptr; }
};
struct NativeSession {
    D3D12DDI_ADAPTERFUNCS adapter{};
    D3D12DDIARG_OPENADAPTER open{};
    D3D12DDIARG_CREATEDEVICE_0003 create{};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 table{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands{};
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queues{};
    alignas(NativeDevice) std::array<unsigned char, sizeof(NativeDevice)> device_memory{};
    alignas(NativeHeapSlot) std::array<unsigned char, sizeof(NativeHeapSlot)> heap_memory{};
    alignas(Object) std::array<unsigned char, sizeof(Object)> resource_memory{};
    D3D12DDI_HHEAP heap{};
    D3D12DDI_HRESOURCE resource{};
    ~NativeSession() { finish(); }
    void finish() {
        if (context(create.hDrvDevice)) {
            if (heap.pDrvPrivate || resource.pDrvPrivate)
                table.pfnDestroyHeapAndResource(create.hDrvDevice, heap, resource);
            adapter.pfnDestroyDevice(create.hDrvDevice);
        }
        if (open.hAdapter.pDrvPrivate) { adapter.pfnCloseAdapter(open.hAdapter); open.hAdapter = {}; }
    }
    void initialize(ProbeRuntime &runtime) {
        D3DDDI_ADAPTERCALLBACKS ac{}; ac.pfnQueryAdapterInfoCb = probe_query;
        open.hRTAdapter.handle = &runtime; open.pAdapterCallbacks = &ac; open.pAdapterFuncs = &adapter;
        check(OpenAdapter12(&open), "native OpenAdapter12");
        auto identity = native_adapter(open.hAdapter);
        require(identity && !std::memcmp(identity->luid.data(), &runtime.requested_luid, sizeof(LUID)),
                "KMD publication matches explicitly requested adapter LUID");
        UINT count = 123; check(adapter.pfnGetSupportedVersions(open.hAdapter, &count, nullptr), "native versions");
        require(count == 0, "native admission remains disabled");
        D3DDDI_DEVICECALLBACKS kt{};
        kt.pfnCreateContextCb = probe_context_create; kt.pfnDestroyContextCb = probe_context_destroy;
        kt.pfnEscapeCb = probe_escape; kt.pfnAllocateCb = probe_allocate; kt.pfnDeallocateCb = probe_deallocate;
        kt.pfnDeallocate2Cb = probe_deallocate2;
        kt.pfnLockCb = probe_lock; kt.pfnUnlockCb = probe_unlock; kt.pfnRenderCb = probe_render;
        kt.pfnSignalSynchronizationObject2Cb = probe_signal;
        D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 um{}; um.pfnSetErrorCb = probe_error;
        um.pfnSetCommandListErrorCb = probe_command_error;
        um.pfnCreateContextCb = probe_queue_context_create; um.pfnDestroyContextCb = probe_queue_context_destroy;
        create.hDrvDevice.pDrvPrivate = device_memory.data(); create.hRTDevice.handle = &runtime;
        create.Interface = D3D12DDI_INTERFACE_VERSION_R0; create.Version = D3D12DDI_BUILD_VERSION << 16;
        create.pKTCallbacks = &kt; create.p12UMCallbacks = &um;
        check(adapter.pfnCreateDevice(open.hAdapter, &create), "native CreateDevice/private Turnip handshake");
        check(VioGpuD3D12BridgeGetTables(&table, &commands, &queues), "controlled native tables");
    }
    void create_readback() {
        D3D12DDIARG_CREATEHEAP_0001 desc{};
        desc.ByteSize = desc.Alignment = 65536; desc.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
        desc.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK; desc.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
        desc.CreationNodeMask = desc.VisibleNodeMask = 1;
        D3D12DDIARG_CREATERESOURCE_0003 buffer{};
        buffer.ResourceType = D3D12DDI_RT_BUFFER; buffer.Width = 65536;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12DDI_TL_ROW_MAJOR; buffer.InitialResourceState = static_cast<D3D12DDI_RESOURCE_STATES>(0x400);
        D3D12DDI_HHEAP h{}; h.pDrvPrivate = heap_memory.data();
        D3D12DDI_HRESOURCE r{}; r.pDrvPrivate = resource_memory.data();
        check(table.pfnCreateHeapAndResource(create.hDrvDevice, &desc, h, {}, &buffer, nullptr, r), "native paired readback/import");
        heap = h; resource = r;
    }
};

struct NativeProbeQueue {
    NativeSession &native;
    ProbeRuntimeQueue runtime;
    Object slot{};
    bool live = false;
    NativeProbeQueue(NativeSession &n, ProbeRuntime &r) : native(n), runtime{&r} {}
    ~NativeProbeQueue() { reset(); }
    void create() {
        D3D12DDIARG_CREATECOMMANDQUEUE_0001 args{};
        args.NodeMask = 1; args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        args.hRTCommandQueue.handle = &runtime; args.hDrvCommandQueue.pDrvPrivate = &slot;
        check(native.table.pfnCreateCommandQueue(native.create.hDrvDevice, &args), "runtime-associated queue");
        live = true;
        require(slot.backend && slot.runtime_route && runtime.context &&
                slot.runtime_route->native_heap_context == runtime_context(runtime.context),
                "native queue routes to its actual runtime-associated context");
    }
    void reset() {
        if (live) {
            native.table.pfnDestroyCommandQueue(native.create.hDrvDevice, {&slot});
            live = false;
        }
    }
};

void run_probe(LUID luid, bool runtime_queues = false) {
    Deadline deadline;
    ProbeRuntime runtime; runtime.open(luid);
    NativeSession native; native.initialize(runtime);
    auto *ctx = context(native.create.hDrvDevice);
    require(ctx && ctx->native_context_id == runtime.context_id, "same actual context id");
    Owned upload, queue, allocator, command, fence, alias;
    NativeProbeQueue first(native, runtime), second(native, runtime);
    check(vkdu_buffer_create(ctx->backend, 8192, 2, 0, 0xac3, &upload.value), "upload buffer");
    if (runtime_queues) {
        first.create(); second.create();
        require(first.runtime.context != second.runtime.context && first.runtime.context != runtime.context_handle &&
                second.runtime.context != runtime.context_handle, "two separate runtime contexts share the owner domain");
        std::printf("QUEUES owner=%u first=%u second=%u domain=%u\n", runtime.context_handle,
                first.runtime.context, second.runtime.context, runtime.context_id);
    } else check(vkdu_queue_create(ctx->backend, 0, &queue.value), "queue");
    uint64_t gpu_clock = UINT64_MAX, cpu_clock = UINT64_MAX;
    if (vkdu_queue_clock(runtime_queues ? first.slot.backend : queue.value, &gpu_clock, &cpu_clock) != DXGI_ERROR_UNSUPPORTED ||
            gpu_clock != UINT64_MAX || cpu_clock != UINT64_MAX)
        throw std::runtime_error("runtime-v1 clock must reject unsupported calibration without fabricated outputs");
    check(vkdu_device_status(ctx->backend), "unsupported shared clock preserves device");
    check(vkdu_allocator_create(ctx->backend, 0, &allocator.value), "allocator");
    check(vkdu_command_create(ctx->backend, allocator.value, 0, &command.value), "command");
    check(vkdu_fence_create(ctx->backend, 0, &fence.value), "fence");
    native.create_readback();
    auto *heap = native_heap_find(ctx, native.heap.pDrvPrivate);
    auto *resource = object(native.resource.pDrvPrivate);
    require(heap && resource && heap->allocation && resource->address == heap->address && heap->users == 2,
            "native heap and imported placed buffer share exact backing/GPUVA");
    runtime.target_handle = heap->allocation;
    const unsigned allocations_before_alias = runtime.creates;
    check(vkdu_memory_heap_import(ctx->backend, ctx, heap, heap->bytes, 1, &alias.value), "duplicate private import");
    require(runtime.creates == allocations_before_alias, "private import issues no new AllocateCb");
    std::printf("IDENTITY luid=%08x:%08x generation=%llu context=%u KMTcontext=%u allocation=%u GPUVA=%llx bytes=%llu\n",
        static_cast<unsigned>(luid.HighPart), luid.LowPart, static_cast<unsigned long long>(ctx->native_adapter->generation),
        ctx->native_context_id, runtime.context_handle, heap->allocation,
        static_cast<unsigned long long>(heap->address), static_cast<unsigned long long>(heap->bytes));
    for (uint32_t round = 0; round < 8; ++round) {
        auto &selected = round & 1 ? second : first;
        auto &other = round & 1 ? first : second;
        auto *backend_queue = runtime_queues ? selected.slot.backend : queue.value;
        const uint32_t sentinel = 0xfedc0000u ^ (round * 0x01010101u);
        const uint32_t first_word = 64 + round * 32;
        void *mapped = nullptr;
        check(native.table.pfnMapHeap(native.create.hDrvDevice, native.heap, &mapped), "native MapHeap sentinel");
        require(mapped != nullptr, "native map pointer");
        auto *words = static_cast<uint32_t *>(mapped);
        for (uint32_t i = 0; i < 16384; ++i) words[i] = sentinel;
        native.table.pfnUnmapHeap(native.create.hDrvDevice, native.heap);
        check(vkdu_buffer_map(upload.value, 0, 0, &mapped), "map upload");
        words = static_cast<uint32_t *>(mapped);
        for (uint32_t i = 0; i < 2048; ++i) words[i] = 0xdeadbeefu;
        for (uint32_t i = 0; i < 1024; ++i) words[32 + i] = (round + 1) * 0x1020304u ^ (i * 0x9e3779b9u);
        check(vkdu_buffer_unmap(upload.value, 0, 8192), "unmap upload");
        if (round) {
            check(vkdu_allocator_reset(allocator.value), "reset allocator");
            check(vkdu_command_reset(command.value, allocator.value), "reset command");
        }
        const unsigned target_before = runtime.target_references;
        const auto selected_before = runtime.counts(runtime_queues ? selected.runtime.context : runtime.context_handle);
        const auto other_before = runtime.counts(runtime_queues ? other.runtime.context : runtime.context_handle);
        const auto owner_before = runtime.counts(runtime.context_handle);
        check(vkdu_command_copy(command.value, resource->backend, first_word * 4, upload.value, 128, 4096), "GPU copy");
        check(vkdu_command_close(command.value), "close command");
        check(vkdu_queue_execute(backend_queue, 1, &command.value), "queue execute");
        // Execute includes the production software-worker drain. The DMA must
        // already be on the selected KMT context before an external OS packet
        // could be queued by a runtime after this call returns.
        if (runtime_queues)
            require(runtime.counts(selected.runtime.context).targets > selected_before.targets,
                    "Execute returns only after the selected scheduler receives target DMA");
        check(vkdu_queue_signal(backend_queue, fence.value, round + 1), "queue signal");
        check(vkdu_fence_wait(fence.value, round + 1, 10000), "GPU completion");
        require(runtime.target_references > target_before, "RenderCb contains actual native allocation");
        if (runtime_queues) {
            const auto after = runtime.counts(selected.runtime.context);
            require(after.events > selected_before.events, "real OS completion events use selected scheduler");
            require(runtime.counts(other.runtime.context).targets == other_before.targets &&
                    runtime.counts(runtime.context_handle).targets == owner_before.targets,
                    "target DMA never bypasses the selected runtime context");
            std::printf("PASS queue_round=%u KMTcontext=%u renders=%u events=%u domain=%u\n",
                    round, selected.runtime.context, after.renders, after.events, runtime.context_id);
        }
        check(native.table.pfnMapHeap(native.create.hDrvDevice, native.heap, &mapped), "native MapHeap readback");
        words = static_cast<uint32_t *>(mapped);
        uint32_t mismatches = 0;
        for (uint32_t i = 0; i < 16384; ++i) {
            const uint32_t expected = i >= first_word && i < first_word + 1024 ?
                ((round + 1) * 0x1020304u ^ ((i - first_word) * 0x9e3779b9u)) : sentinel;
            if (words[i] != expected && mismatches++ < 4)
                std::fprintf(stderr, "round=%u word=%u got=%08x expected=%08x\n", round, i, words[i], expected);
        }
        native.table.pfnUnmapHeap(native.create.hDrvDevice, native.heap);
        require(!mismatches, "changing GPU words and untouched sentinel regions");
        check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native status");
        std::printf("PASS round=%u changed_words=1024 sentinel_words=15360 native_allocation=%u\n", round, heap->allocation);
    }
    // Retire command references before releasing resource owners.
    command.reset(); allocator.reset(); queue.reset(); fence.reset(); upload.reset();
    first.reset(); second.reset();
    if (runtime_queues) {
        require(!first.runtime.context && !second.runtime.context,
                "both runtime callbacks destroyed their own scheduler contexts");
        std::lock_guard<std::mutex> lock(runtime.mutex);
        require(runtime.contexts.size() == 1 && runtime.contexts.count(runtime.context_handle),
                "allocation owner survives destruction of both runtime queues");
    }
    const auto target = runtime.target_handle.load();
    native.table.pfnDestroyHeapAndResource(native.create.hDrvDevice, native.heap, {}); native.heap = {};
    require(runtime.has(target), "resource survives native heap slot destruction");
    native.table.pfnDestroyHeapAndResource(native.create.hDrvDevice, {}, native.resource); native.resource = {};
    require(runtime.has(target), "duplicate imported alias survives resource destruction");
    alias.reset();
    require(!runtime.has(target), "final imported owner releases native allocation");
    check(VioGpuD3D12BridgeStatus(native.create.hDrvDevice), "native teardown status");
    runtime.native_cleanup = true;
    native.finish();
    require(runtime.cleanup_identity_queries != 0, "runtime identity callback remains live during native cleanup");
    require(runtime.context_handle == 0, "native context closed before final runtime device cleanup");
    {
        std::lock_guard<std::mutex> lock(runtime.mutex);
        require(runtime.allocations.empty(), "all native allocations released before final runtime device cleanup");
        require(runtime.contexts.empty(), "all scheduler contexts released before final runtime device cleanup");
    }
    std::printf("PASS cleanup identity_queries=%u native_context=0 allocations=0\n", runtime.cleanup_identity_queries.load());
    check(runtime.close(), "final real KMT owner cleanup");
    std::printf("PASS shared native heap/private Turnip import/real RenderCb/MapHeap:8x16384 words; renders=%u\n",
        runtime.renders.load());
    std::puts("BOUNDARY emulated runtime callbacks + real KMT/GPU; NOT Microsoft D3D12CreateDevice acceptance");
}
#include "runtime_textures_probe.inc"
#include "runtime_graphics_probe.inc"
#include "os_fence_probe.inc"
#include "runtime_provider_controls.inc"
#include "runtime_minimal_probe.inc"
#include "runtime_compute_probe.inc"

bool hex32(const char *text, uint32_t &value) {
    if (!text || !*text || std::strlen(text) > 10 || *text == '-' || *text == '+') return false;
    char *end = nullptr;
    const auto parsed = std::strtoull(text, &end, 16);
    if (*end || parsed > UINT32_MAX) return false;
    value = static_cast<uint32_t>(parsed); return true;
}
}

int main(int argc, char **argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--validate-provider-controls")) {
        try { validate_provider_controls(); return 0; }
        catch (const std::exception &error) { std::fprintf(stderr, "FAIL provider control: %s\n", error.what()); return 1; }
    }
#if defined(VKDU_PROBE_TEST_DROP_SYNCHRONOUS) || defined(VKDU_PROBE_TEST_WRONG_EVENT_CONTEXT) || \
    defined(VKDU_PROBE_TEST_CPU_COMPLETION) || defined(VKDU_PROBE_TEST_EARLY_RELEASE)
    std::fputs("CONTROL-ONLY NEGATIVE: hardware execution is disabled\n", stderr);
    return 2;
#endif
    if (argc == 2 && !std::strcmp(argv[1], "--validate-os-fence-controls")) {
        try { validate_os_fence_cases(); return 0; }
        catch (const std::exception &error) { std::fprintf(stderr, "FAIL OS fence configuration: %s\n", error.what()); return 1; }
    }
    uint32_t low = 0, high = 0;
    if (argc != 6 || std::strcmp(argv[1], "--luid-low") || std::strcmp(argv[3], "--luid-high") ||
            (std::strcmp(argv[5], "--run-shared-backing") && std::strcmp(argv[5], "--run-shared-textures") &&
             std::strcmp(argv[5], "--run-runtime-queues") && std::strcmp(argv[5], "--run-runtime-compute") && std::strcmp(argv[5], "--run-shared-graphics") &&
             std::strcmp(argv[5], "--run-os-fence-mapping") && std::strcmp(argv[5], "--run-os-fence-controls")) ||
            !hex32(argv[2], low) || !hex32(argv[4], high) || !(low | high)) {
        std::fputs("usage: vkd3d-umd-shared-gpu-probe --luid-low HEX --luid-high HEX --run-shared-backing|--run-shared-textures|--run-shared-graphics|--run-runtime-queues|--run-runtime-compute|--run-os-fence-mapping|--run-os-fence-controls\n"
            "       --validate-os-fence-controls checks request construction only, with no KMT calls.\n"
            "Requires exact VIOGPU LUID; shared modes also need matching private-import Turnip. No native runtime admission.\n", stderr);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool os_fence_controls = !std::strcmp(argv[5], "--run-os-fence-controls");
    const bool os_fence = !std::strcmp(argv[5], "--run-os-fence-mapping") || os_fence_controls;
    std::puts(os_fence ? "PROBE real KMT-owned monitored fence mappings; no Vulkan or system D3D12 device creation" :
        "PROBE emulated runtime callbacks -> production native entry -> real KMT -> private Turnip import");
    try {
        const LUID luid{low, static_cast<LONG>(high)};
        if (os_fence) run_os_fence_probe(luid, os_fence_controls);
        else if (!std::strcmp(argv[5], "--run-shared-textures")) run_texture_probe(luid);
        else if (!std::strcmp(argv[5], "--run-shared-graphics")) run_graphics_probe(luid);
        else if (!std::strcmp(argv[5], "--run-runtime-queues")) run_minimal_native_probe(luid);
        else if (!std::strcmp(argv[5], "--run-runtime-compute")) run_compute_native_probe(luid);
        else run_probe(luid);
        return 0;
    }
    catch (const std::exception &error) { std::fprintf(stderr, "FAIL shared-backing probe: %s\n", error.what()); return 1; }
}
