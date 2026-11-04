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
void validate_provider_controls() {
    std::puts("CONTROL ONLY: provider forwarding and Windows event state, no KMT/Vulkan/GPU calls");
    probe_kmt_deallocate = control_deallocate; probe_kmt_signal = control_signal;
    probe_kmt_destroy_context = control_destroy_context;
    ProbeRuntime runtime;
    /* Keep destructor away from fake handles on every failed assertion. */
    struct FakeDeviceGuard { ProbeRuntime &r; ~FakeDeviceGuard() { r.device = r.adapter = 0; } } guard{runtime};
    runtime.device = 42; runtime.context_handle = 11;
    runtime.contexts.emplace(11, ProbeRuntime::ContextCounts{});
    runtime.contexts.emplace(22, ProbeRuntime::ContextCounts{});
    runtime.allocations.emplace(101, NativeAllocationInfo{});
    D3DKMT_HANDLE allocation = 101;
    D3DDDICB_DEALLOCATE2 release{}; release.NumAllocations = 1; release.HandleList = &allocation;
    release.Flags.AssumeNotInUse = release.Flags.SynchronousDestroy = 1;
    control_destroy_result = static_cast<NTSTATUS>(0xc0000001u);
    require(FAILED(probe_deallocate2(&runtime, &release)) && runtime.has(101) && runtime.pageable_deallocations == 0,
        "failed typed deallocation retains allocation ownership");
    require(control_destroy_flags == 3 && control_destroy_count == 1,
        "Deallocate2 preserves both flags: AssumeNotInUse and SynchronousDestroy");
    const auto before = control_destroy_calls;
    for (UINT flags : {0u, 1u, 2u, 7u}) {
        release.Flags.Value = flags;
        require(probe_deallocate2(&runtime, &release) == E_INVALIDARG && control_destroy_calls == before,
            "invalid public flags rejected before KMT");
    }
    release.Flags.Value = 3;
    allocation = 999;
    require(probe_deallocate2(&runtime, &release) == E_INVALIDARG && control_destroy_calls == before,
        "foreign allocation rejected before KMT");
    allocation = 101; control_destroy_result = 0;
    check(probe_deallocate2(&runtime, &release), "typed deallocation");
    require(!runtime.has(101) && runtime.pageable_deallocations == 1 && !runtime.internal_deallocations,
        "successful typed close retires exactly one owner");
    runtime.allocations.emplace(101, NativeAllocationInfo{});
    D3DDDICB_DEALLOCATE internal{}; internal.NumAllocations = 1; internal.HandleList = &allocation;
    check(probe_deallocate(&runtime, &internal), "internal deallocation");
    require(control_destroy_flags == 0 && !runtime.has(101) && runtime.internal_deallocations == 1,
        "internal scratch keeps independent legacy flags");
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(event != nullptr, "controlled Windows event");
    struct EventGuard { HANDLE h; ~EventGuard() { CloseHandle(h); } } event_guard{event};
    D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 signal{};
    signal.hContext = runtime_context(22); signal.Flags.EnqueueCpuEvent = 1; signal.CpuEventHandle = event;
    control_signal_result = static_cast<NTSTATUS>(0xc0000001u);
    require(FAILED(probe_signal(&runtime, &signal)) && runtime.completions.empty() && !runtime.counts(22).events,
        "failed event enqueue publishes no completion");
    control_signal_result = 0;
    check(probe_signal(&runtime, &signal), "controlled event enqueue");
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
    require(probe_signal(&runtime, &signal) == E_INVALIDARG && control_signal_calls == signals,
        "late completion against destroyed queue rejected");
    std::puts("PASS provider Deallocate2 flags3/legacy0, failed-close retention, invalid requests, selected-context event route, pending-event retention, observed completion and exact queue release; CONTROL ONLY");
}
