/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Runs the real R0 provider callbacks with intercepted KMT boundaries. Every
 * simulated completion is confined to this named local control. */
struct ResidencyProviderControl {
    ProbeRuntime runtime;
    UINT64 completed = 0, returned_fence = 0;
    NTSTATUS make_result = 0, evict_result = 0, wait_result = 0, destroy_result = 0;
    bool malformed_count = false, signal_event = false, advance_fence = false;
    std::atomic_uint makes{0}, evictions{0}, waits{0}, destroys{0}, renders{0};
    std::vector<D3DKMT_HANDLE> last_make, last_evict;
    D3DKMT_HANDLE *caller_handles = nullptr;
    UINT *caller_priorities = nullptr;
    std::mutex render_mutex;
    std::condition_variable render_condition;
    bool block_render = false, render_entered = false, release_render = false;
    static ResidencyProviderControl *active;
    decltype(probe_kmt_make_resident) old_make = probe_kmt_make_resident;
    decltype(probe_kmt_evict) old_evict = probe_kmt_evict;
    decltype(probe_kmt_wait_paging) old_wait = probe_kmt_wait_paging;
    decltype(probe_wait_paging_event) old_wait_event = probe_wait_paging_event;
    decltype(probe_kmt_deallocate) old_destroy = probe_kmt_deallocate;
    decltype(probe_kmt_render) old_render = probe_kmt_render;
    static NTSTATUS APIENTRY make(D3DDDI_MAKERESIDENT *args) {
        auto &f = *active; ++f.makes;
        require(args->hPagingQueue == 74 && args->NumAllocations && args->AllocationList,
            "R0 paging token translated to owned KMT queue");
        if (f.caller_handles) {
            require(args->AllocationList != f.caller_handles && args->PriorityList != f.caller_priorities,
                "allocation and priority arrays are snapshotted before KMT");
            f.caller_handles[0] = 999; f.caller_priorities[0] = 0;
            require(args->AllocationList[0] == 101 && args->PriorityList[0] == 0x78000000u &&
                args->PriorityList[1] == 0x50000000u, "poisoned caller arrays cannot alter KMT request");
        }
        f.last_make.assign(args->AllocationList, args->AllocationList + args->NumAllocations);
        args->PagingFenceValue = f.returned_fence; args->NumBytesToTrim = 8192;
        if (f.malformed_count) --args->NumAllocations;
        return f.make_result;
    }
    static NTSTATUS APIENTRY evict(D3DKMT_EVICT *args) {
        auto &f = *active; ++f.evictions;
        require(args->hDevice == 42 && args->NumAllocations && args->AllocationList,
            "R0 evict uses owned device and exact atomic allocation list");
        f.last_evict.assign(args->AllocationList, args->AllocationList + args->NumAllocations);
        return f.evict_result;
    }
    static NTSTATUS APIENTRY wait(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *args) {
        auto &f = *active; ++f.waits;
        require(args->hDevice == 42 && args->ObjectCount == 1 && args->ObjectHandleArray[0] == 75 &&
            args->FenceValueArray[0] == f.runtime.pending_paging_fence && args->hAsyncEvent,
            "paging wait targets exact monitored fence and async event");
        if (f.advance_fence) f.completed = args->FenceValueArray[0];
        if (f.signal_event) require(SetEvent(args->hAsyncEvent) != FALSE, "control-only simulated OS event");
        return f.wait_result;
    }
    static DWORD WINAPI wait_event(HANDLE event, DWORD timeout) {
        require(timeout == 5000, "production bounded paging wait retained");
        return WaitForSingleObject(event, 0); // local peer never sleeps5s
    }
    static NTSTATUS APIENTRY destroy(const D3DKMT_DESTROYALLOCATION2 *args) {
        auto &f = *active; ++f.destroys;
        require(args->hDevice == 42 && args->AllocationCount && args->phAllocationList,
            "cleanup retains exact owned KMT allocations");
        return f.destroy_result;
    }
    static NTSTATUS APIENTRY render(D3DKMT_RENDER *args) {
        auto &f = *active; ++f.renders;
        require(args->hContext == 22 && args->AllocationCount && args->pNewAllocationList,
            "actual provider Render forwards exact owned allocation list");
        std::unique_lock<std::mutex> lock(f.render_mutex);
        f.render_entered = true; f.render_condition.notify_all();
        if (f.block_render && !f.render_condition.wait_for(lock, std::chrono::seconds(2),
                [&] { return f.release_render; })) return static_cast<NTSTATUS>(0xc0000001u);
        return 0;
    }
    ResidencyProviderControl() {
        require(!active, "one isolated residency peer"); active = this;
        probe_kmt_make_resident = make; probe_kmt_evict = evict;
        probe_kmt_wait_paging = wait; probe_wait_paging_event = wait_event;
        probe_kmt_deallocate = destroy;
        probe_kmt_render = render;
        runtime.device = 42; runtime.paging_queue = 74; runtime.paging_sync = 75;
        runtime.paging_ready = true; runtime.paging_cpu_fence = &completed;
        runtime.context_id = 36; runtime.identity.generation = 2;
        runtime.contexts.emplace(22, ProbeRuntime::ContextCounts{});
        for (D3DKMT_HANDLE handle : {101u, 102u, 103u}) {
            ProbeWireAllocationInfo info{};
            info.generation = 2; info.context_id = 36; info.size = 65536;
            // Handles101/102 model distinct opens of one backing identity.
            info.iova = handle == 103 ? 0x20000 : 0x10000;
            runtime.allocations.emplace(handle, info);
        }
    }
    ~ResidencyProviderControl() {
        probe_kmt_make_resident = old_make; probe_kmt_evict = old_evict;
        probe_kmt_wait_paging = old_wait; probe_wait_paging_event = old_wait_event;
        probe_kmt_deallocate = old_destroy;
        probe_kmt_render = old_render;
        runtime.device = runtime.adapter = runtime.paging_queue = runtime.paging_sync = 0;
        active = nullptr;
    }
    HRESULT resident(std::initializer_list<D3DKMT_HANDLE> handles, UINT flags = 0) {
        D3DDDI_MAKERESIDENT args{};
        args.NumAllocations = static_cast<UINT>(handles.size()); args.AllocationList = handles.begin();
        args.Flags.Value = flags; args.PagingFenceValue = 0xa5a5;
        const HRESULT hr = safe_r0_make_resident({&runtime}, runtime.runtime_paging_queue(), &args);
        if (hr == E_PENDING) require(args.PagingFenceValue == returned_fence, "exact pending paging fence returned");
        else if (hr == S_OK) require(!args.PagingFenceValue, "synchronous completion returns no fence");
        else require(args.PagingFenceValue == 0xa5a5, "failed callback preserves fence output");
        return hr;
    }
    HRESULT evicted(std::initializer_list<D3DKMT_HANDLE> handles) {
        D3DDDICB_EVICT args{};
        args.NumAllocations = static_cast<UINT>(handles.size()); args.AllocationList = handles.begin();
        return safe_r0_evict({&runtime}, &args);
    }
    HRESULT render(std::initializer_list<D3DKMT_HANDLE> handles) {
        std::vector<D3DDDI_ALLOCATIONLIST> list(handles.size());
        unsigned i = 0; for (auto h : handles) list[i++].hAllocation = h;
        D3DDDICB_RENDER args{};
        args.hContext = runtime_context(22);
        args.NumAllocations = static_cast<UINT>(list.size()); args.pNewAllocationList = list.data();
        return safe_render(&runtime, &args);
    }
};
ResidencyProviderControl *ResidencyProviderControl::active = nullptr;

void residency_provider_controls() {
    std::puts("CONTROL ONLY R0 residency callbacks: intercepted KMT; no device/DLL/Vulkan/GPU work");
    unsigned groups = 0;
    {
        ResidencyProviderControl f;
        D3DKMT_HANDLE handles[] = {101, 102}; UINT priorities[] = {0x78000000u, 0x50000000u};
        D3DDDI_MAKERESIDENT args{};
        args.NumAllocations = 2; args.AllocationList = handles; args.PriorityList = priorities;
        f.caller_handles = handles; f.caller_priorities = priorities;
        check(safe_r0_make_resident({&f.runtime}, f.runtime.runtime_paging_queue(), &args), "priority snapshot");
        require(handles[0] == 999 && !priorities[0] && f.last_make == std::vector<D3DKMT_HANDLE>({101,102}),
            "caller poison test reached actual callback and retained exact snapshot");
        ++groups;
    }
    for (unsigned destroy = 0; destroy != 2; ++destroy) {
        ResidencyProviderControl f;
        check(f.resident({101}), "concurrent submission resident");
        f.block_render = true;
        HRESULT submitted = E_FAIL, retired = E_FAIL;
        std::thread submitting([&] { submitted = f.render({101}); });
        bool entered;
        {
            std::unique_lock<std::mutex> lock(f.render_mutex);
            entered = f.render_condition.wait_for(lock, std::chrono::seconds(2), [&] { return f.render_entered; });
        }
        std::atomic_bool attempted{false}, was_excluded{false}, retirement_done{false};
        std::thread retiring([&] {
            // A nonblocking attempt while KMT Render is stopped at a barrier
            // deterministically checks the actual gate-to-submit exclusion.
            const bool acquired = f.runtime.residency_mutex.try_lock();
            if (acquired) f.runtime.residency_mutex.unlock();
            was_excluded = !acquired; attempted = true;
            const D3DKMT_HANDLE allocation = 101;
            retired = destroy ? probe_destroy_allocations(&f.runtime, nullptr, 1, &allocation, 0) : f.evicted({101});
            retirement_done = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!attempted && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        const bool excluded = entered && attempted && was_excluded && !retirement_done && !f.evictions && !f.destroys;
        {
            std::lock_guard<std::mutex> lock(f.render_mutex); f.release_render = true;
        }
        f.render_condition.notify_all();
        submitting.join(); retiring.join();
        require(excluded, "Evict and Destroy cannot cross resident-check to actual KMT Render boundary");
        check(submitted, "barrier-controlled actual provider submission");
        check(retired, "retirement follows KMT Render return");
        require(f.renders == 1 && f.evictions == 1 && f.destroys == destroy,
            "one actual controlled submission precedes exact retirement");
        if (destroy) {
            const D3DKMT_HANDLE allocation = 101;
            require(probe_destroy_allocations(&f.runtime, nullptr, 1, &allocation, 0) == E_INVALIDARG && f.destroys == 1,
                "retired ownership is revalidated under residency exclusion");
        }
        ++groups;
    }
    {
        ResidencyProviderControl f;
        check(f.resident({101, 102, 103}), "atomic multi-allocation/alias resident");
        require(f.makes == 1 && f.last_make == std::vector<D3DKMT_HANDLE>({101, 102, 103}) &&
            f.runtime.residency_references.size() == 3, "one exact batch and separate alias handles");
        check(f.resident({101}), "second reference");
        check(f.evicted({101}), "one reference released");
        require(f.runtime.residency_references.count(101) && f.runtime.residency_references.at(101) == 1 &&
            f.runtime.resident_allocations.count(101),
            "evict decrements without dropping another residency owner");
        check(f.evicted({101, 102, 103}), "last atomic residency references released");
        require(f.runtime.resident_allocations.empty() && f.runtime.residency_references.empty() &&
            FAILED(f.render({101})), "evicted allocations cannot render");
        check(f.resident({101, 102, 103}), "evict then resident again");
        check(f.render({101, 102, 103}), "reresident exact batch permits submission");
        ++groups;
    }
    for (unsigned completion = 0; completion != 4; ++completion) {
        ResidencyProviderControl f;
        f.make_result = 0x103; f.returned_fence = 42;
        require(f.resident({101, 102}) == E_PENDING && f.runtime.pending_paging_fence == 42,
            "accepted pending must remain pending");
        if (completion == 0) {
            require(FAILED(f.render({101})) && f.waits == 1 && f.runtime.pending_paging_fence == 42,
                "queued wait without event completion cannot permit rendering");
        } else if (completion == 1) {
            f.signal_event = true;
            require(FAILED(f.render({101})) && f.runtime.pending_paging_fence == 42,
                "event without advanced monitored fence cannot permit rendering");
        } else {
            f.advance_fence = f.signal_event = true;
            if (completion == 3) f.completed = 42;
            check(f.render({101}), "completed paging permits rendering");
            require(!f.runtime.pending_paging_fence && f.waits == (completion == 3 ? 0u : 1u),
                "only exact completed paging fence retires pending target");
        }
        ++groups;
    }
    {
        ResidencyProviderControl f;
        check(f.resident({101, 102}), "failure-control initial resident");
        const auto references = f.runtime.residency_references;
        f.make_result = static_cast<NTSTATUS>(0xc0000017u);
        require(f.resident({101, 102}) == E_OUTOFMEMORY && f.runtime.residency_references == references,
            "atomic MakeResident OOM does not change any residency reference");
        f.evict_result = static_cast<NTSTATUS>(0xc0000001u);
        require(FAILED(f.evicted({101, 102})) && f.runtime.residency_references == references,
            "atomic Evict failure retains all references");
        ++groups;
    }
    {
        ResidencyProviderControl f;
        require(f.resident({101, 101}) == E_INVALIDARG && !f.makes, "duplicate raw allocation refused");
        require(f.resident({999}) == E_INVALIDARG && !f.makes, "foreign allocation refused");
        require(f.resident({101}, 2) == E_INVALIDARG && !f.makes, "invalid flags refused");
        f.runtime.allocations.at(101).generation = 3;
        require(f.resident({101}) == E_INVALIDARG && !f.makes, "stale allocation generation refused");
        D3DKMT_HANDLE allocation = 102; D3DDDI_MAKERESIDENT args{};
        args.NumAllocations = 1; args.AllocationList = &allocation;
        require(safe_r0_make_resident({&f.runtime}, {&allocation}, &args) == E_INVALIDARG && !f.makes,
            "foreign runtime paging token never reinterpreted as KMT handle");
        ++groups;
    }
    for (unsigned malformed = 0; malformed != 2; ++malformed) {
        ResidencyProviderControl f;
        f.make_result = 0x103; f.returned_fence = malformed ? 8 : 0; f.malformed_count = malformed;
        require(f.resident({101, 102}) == DXGI_ERROR_DRIVER_INTERNAL_ERROR && f.runtime.residency_uncertain,
            "malformed accepted reply marks retained owner uncertain");
        require(f.runtime.residency_references.size() == 2 && FAILED(f.render({101})) &&
            f.evicted({101}) == DXGI_ERROR_DRIVER_INTERNAL_ERROR,
            "uncertain residency retains both allocations and blocks use/eviction");
        ++groups;
    }
    {
        ResidencyProviderControl f;
        check(f.resident({101, 102}), "cleanup resident");
        check(f.resident({101}), "cleanup second reference");
        const D3DKMT_HANDLE handles[] = {101, 102};
        f.destroy_result = static_cast<NTSTATUS>(0xc0000001u);
        require(FAILED(probe_destroy_allocations(&f.runtime, nullptr, 2, handles, 0)) &&
            f.runtime.allocations.size() == 3 && f.runtime.residency_references.empty(),
            "destroy failure preserves allocation ownership after successful reference release");
        require(f.evictions == 3 && f.destroys == 1, "cleanup releases exactly every residency reference");
        f.destroy_result = 0;
        check(probe_destroy_allocations(&f.runtime, nullptr, 2, handles, 0), "cleanup retry without duplicate evict");
        require(f.evictions == 3 && f.destroys == 2 && !f.runtime.has(101) && !f.runtime.has(102),
            "cleanup retry retires only exact owned handles once");
        ++groups;
    }
    std::printf("PASS R0 RESIDENCY provider groups=%u actual_callback_code=1 intercepted_KMT=1 GPU=0 SYSTEM=0\n", groups);
}
