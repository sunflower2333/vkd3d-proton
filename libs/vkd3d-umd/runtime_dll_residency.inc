/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exact R0 runtime paging callbacks. Opaque hRTPagingQueue identifies this
 * session's queue record; only the record supplies the KMT handle. */
HRESULT ProbeRuntime::runtime_make_resident(D3D12DDI_HRTPAGINGQUEUE queue,
        D3DDDI_MAKERESIDENT *args) {
    if (!args || !queue.handle || queue.handle != runtime_paging_queue().handle ||
            args->hPagingQueue || !args->NumAllocations || args->NumAllocations > 192 ||
            !args->AllocationList || (args->Flags.Value & ~3u) ||
            ((args->Flags.Value & 2u) && !(args->Flags.Value & 1u))) return E_INVALIDARG;
    const UINT count = args->NumAllocations;
    std::vector<D3DKMT_HANDLE> handles(args->AllocationList, args->AllocationList + count);
    std::vector<UINT> priorities;
    if (args->PriorityList) priorities.assign(args->PriorityList, args->PriorityList + count);
    std::lock_guard<std::mutex> residency(residency_mutex);
    std::lock_guard<std::mutex> ownership(mutex);
    if (!device || !paging_ready || !paging_queue || !paging_sync || !paging_cpu_fence)
        return DXGI_ERROR_DEVICE_REMOVED;
    if (residency_uncertain) return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    // Allocate every potentially throwing ledger node before the syscall.
    auto references = residency_references;
    auto residents = resident_allocations;
    for (UINT i = 0; i < count; ++i) {
        const auto entry = allocations.find(handles[i]);
        if (!handles[i] || entry == allocations.end() ||
                entry->second.generation != identity.generation ||
                entry->second.context_id != context_id) return E_INVALIDARG;
        for (UINT j = 0; j < i; ++j) if (handles[j] == handles[i]) return E_INVALIDARG;
        auto &number = references[handles[i]];
        if (number == UINT32_MAX) return E_OUTOFMEMORY;
        ++number; residents.insert(handles[i]);
#ifdef VKDU_PROBE_TEST_RESIDENCY_DROP_REF
        number = 1;
#endif
    }
    D3DDDI_MAKERESIDENT request = *args;
    request.hPagingQueue = paging_queue; request.AllocationList = handles.data();
    request.PriorityList = priorities.empty() ? nullptr : priorities.data();
    request.PagingFenceValue = request.NumBytesToTrim = 0;
    TlCall call(TlKind::MakeResident, {device, paging_queue, count, request.Flags.Value});
    const NTSTATUS status = probe_kmt_make_resident(&request);
    call.end({tl_u32(status), count, request.NumAllocations, request.PagingFenceValue, request.NumBytesToTrim});
    if (status != 0 && status != static_cast<NTSTATUS>(0x103)) {
        if (status == static_cast<NTSTATUS>(0xc0000017u)) {
            args->NumBytesToTrim = request.NumBytesToTrim;
            return E_OUTOFMEMORY; // no retry/trim and no ledger change
        }
        return status < 0 ? nt_result(status, "MakeResident(R0)") : E_FAIL;
    }
    // KMT accepted one atomic list. Commit all references, even when a
    // malformed accepted reply requires the session to fail closed.
    residency_references.swap(references); resident_allocations.swap(residents);
    const bool pending = status == static_cast<NTSTATUS>(0x103);
    if (request.NumAllocations != count || (pending && !request.PagingFenceValue)) {
        residency_uncertain = true;
        return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
    if (pending && request.PagingFenceValue > pending_paging_fence)
        pending_paging_fence = request.PagingFenceValue;
    args->PagingFenceValue = pending ? request.PagingFenceValue : 0;
    args->NumBytesToTrim = request.NumBytesToTrim;
    return pending ? E_PENDING : S_OK;
}

HRESULT ProbeRuntime::runtime_evict(const D3DDDICB_EVICT *args) {
    if (!args || !args->NumAllocations || args->NumAllocations > 192 ||
            !args->AllocationList || (args->Flags.Value & ~1u)) return E_INVALIDARG;
    const UINT count = args->NumAllocations;
    std::vector<D3DKMT_HANDLE> handles(args->AllocationList, args->AllocationList + count);
    std::lock_guard<std::mutex> residency(residency_mutex);
    std::lock_guard<std::mutex> ownership(mutex);
    if (!device || !paging_ready) return DXGI_ERROR_DEVICE_REMOVED;
    if (residency_uncertain) return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    for (UINT i = 0; i < count; ++i) {
        const auto owner = allocations.find(handles[i]);
        const auto reference = residency_references.find(handles[i]);
        if (owner == allocations.end() || reference == residency_references.end() || !reference->second ||
                owner->second.generation != identity.generation || owner->second.context_id != context_id)
            return E_INVALIDARG;
        for (UINT j = 0; j < i; ++j) if (handles[j] == handles[i]) return E_INVALIDARG;
    }
    // Do not race an accepted paging request. This is an explicit completion
    // wait, using the OS event and monitored fence, never a CPU SetEvent.
    HRESULT completed = wait_paging_locked();
    if (FAILED(completed)) return completed;
    D3DKMT_EVICT request{};
    request.hDevice = device; request.NumAllocations = count;
    request.AllocationList = handles.data(); request.Flags = args->Flags;
    const NTSTATUS status = probe_kmt_evict(&request);
    if (status != 0) return status < 0 ? nt_result(status, "Evict(R0)") : E_FAIL;
    for (auto handle : handles) {
        auto reference = residency_references.find(handle);
        if (!--reference->second) {
            residency_references.erase(reference); resident_allocations.erase(handle);
        }
    }
    return S_OK;
}

HRESULT APIENTRY safe_r0_make_resident(D3D12DDI_HRTDEVICE device,
        D3D12DDI_HRTPAGINGQUEUE queue, D3DDDI_MAKERESIDENT *args) noexcept {
    if (!device.handle) return E_INVALIDARG;
    auto *runtime = static_cast<ProbeRuntime *>(device.handle);
    return callback_boundary(runtime->callback_exceptions,
        [&] { return runtime->runtime_make_resident(queue, args); });
}
HRESULT APIENTRY safe_r0_evict(D3D12DDI_HRTDEVICE device, const D3DDDICB_EVICT *args) noexcept {
    if (!device.handle) return E_INVALIDARG;
    auto *runtime = static_cast<ProbeRuntime *>(device.handle);
    return callback_boundary(runtime->callback_exceptions, [&] { return runtime->runtime_evict(args); });
}
