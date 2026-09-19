/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Measures genuine OS-owned fence outputs. This creates its own KMT object;
 * it does NOT discover an ordinary D3D12 runtime object's identity. */
struct OwnedOsFence {
    D3DKMT_HANDLE object = 0;
    HANDLE event = nullptr;
    HRESULT close() {
        if (object) {
            D3DKMT_DESTROYSYNCHRONIZATIONOBJECT args{}; args.hSyncObject = object;
            HRESULT hr = nt_result(D3DKMTDestroySynchronizationObject(&args), "DestroySynchronizationObject");
            if (FAILED(hr)) return hr;
            object = 0;
        }
        if (event) { CloseHandle(event); event = nullptr; }
        return S_OK;
    }
    ~OwnedOsFence() { close(); }
};

struct OsFenceCase {
    const char *name;
    bool monitored, shared, no_gpu;
    UINT affinity;
    bool invalid_flags = false;
};
const OsFenceCase os_fence_cases[] = {
    {"legacy_fence_device_control", false, false, false, 0},
    {"monitored_shared_affinity1", true, true, false, 1},
    {"monitored_shared_affinity0", true, true, false, 0},
    {"monitored_private_affinity1", true, false, false, 1},
    {"monitored_private_affinity0", true, false, false, 0},
    {"monitored_no_gpu_access", true, true, true, 0},
    // Deliberately repeat the original invalid pair as an explicit negative.
    {"invalid_nt_without_shared", true, true, false, 1, true},
};

D3DKMT_CREATESYNCHRONIZATIONOBJECT2 os_fence_request(D3DKMT_HANDLE device, const OsFenceCase &test) {
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create{};
    create.hDevice = device;
    create.Info.Type = test.monitored ? D3DDDI_MONITORED_FENCE : D3DDDI_FENCE;
    // Microsoft requires Shared whenever NtSecuritySharing is set. Private
    // objects instead leave both bits zero. Never repeat the old invalid pair.
    create.Info.Flags.Shared = test.shared;
    create.Info.Flags.NtSecuritySharing = test.shared;
    create.Info.Flags.NoGPUAccess = test.no_gpu;
    if (test.invalid_flags) create.Info.Flags.Shared = 0;
    if (test.monitored) {
        create.Info.MonitoredFence.InitialFenceValue = 3;
        create.Info.MonitoredFence.EngineAffinity = test.affinity;
    } else create.Info.Fence.FenceValue = 3;
    return create;
}

bool os_fence_flags_valid(const D3DKMT_CREATESYNCHRONIZATIONOBJECT2 &create) {
    return !(create.Info.Flags.NtSecuritySharing && !create.Info.Flags.Shared) &&
        !(create.Info.Flags.NoSignal && create.Info.Flags.NoWait) &&
        (!create.Info.Flags.NoGPUAccess || create.Info.Type == D3DDDI_MONITORED_FENCE);
}

D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU os_fence_signal_request(
        D3DKMT_HANDLE device, const D3DKMT_HANDLE *object, const UINT64 *target, bool rewind) {
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU signal{};
    signal.hDevice = device; signal.ObjectCount = 1;
    signal.ObjectHandleArray = object; signal.FenceValueArray = target;
    signal.Flags.AllowFenceRewind = rewind;
    return signal;
}

void validate_os_fence_cases() {
    for (const auto &test : os_fence_cases) {
        const auto request = os_fence_request(0x1234, test);
        require(request.hDevice == 0x1234 && !request.hSyncObject, "same-device request and empty output");
        require(os_fence_flags_valid(request) == !test.invalid_flags, "OS fence flags match documented validity");
        if (test.monitored)
            require(request.Info.MonitoredFence.InitialFenceValue == 3 &&
                request.Info.MonitoredFence.EngineAffinity == test.affinity &&
                !request.Info.MonitoredFence.FenceValueCPUVirtualAddress &&
                !request.Info.MonitoredFence.FenceValueGPUVirtualAddress,
                "monitored input and output initialization");
    }
    auto old_bug = os_fence_request(0x1234, os_fence_cases[1]);
    old_bug.Info.Flags.Shared = 0;
    require(!os_fence_flags_valid(old_bug), "missing Shared bit must be detected before a positive probe");
    D3DKMT_HANDLE object = 0x4321;
    UINT64 target = 1;
    const auto monotonic = os_fence_signal_request(0x1234, &object, &target, false);
    const auto rewind = os_fence_signal_request(0x1234, &object, &target, true);
    require(monotonic.Flags.Value == 0 && rewind.Flags.AllowFenceRewind &&
        rewind.Flags.Value == 4 && rewind.hDevice == 0x1234 && rewind.ObjectCount == 1 &&
        rewind.ObjectHandleArray == &object && rewind.FenceValueArray == &target,
        "raw KMT rewind needs explicit AllowFenceRewind without unrelated signal flags");
    std::puts("PASS OS_FENCE_CONFIGURATION_ONLY shared_pair, affinity_0_1, private, NoGPUAccess, original_bug_rejected, explicit_AllowFenceRewind; no KMT calls");
}

enum class OsFenceResult { unclassified, rejected, legacy, gpu_mapping, cpu_only, missing_gpu_mapping };
struct OsFenceObservation {
    OsFenceResult mapping = OsFenceResult::unclassified;
    bool created = false, cpu_mapping = false, cpu_event = false, rewind = false;
};
void run_os_fence_case(ProbeRuntime &runtime, const OsFenceCase &test, OsFenceObservation &observed) {
    OwnedOsFence fence;
    auto create = os_fence_request(runtime.device, test);
    require(os_fence_flags_valid(create) || test.invalid_flags, "valid flags before positive KMT call");
    std::printf("KMT_FENCE_REQUEST case=%s device=%08x type=%u flags=%08x affinity=%u expected_valid=%u\n",
        test.name, runtime.device, static_cast<unsigned>(create.Info.Type), create.Info.Flags.Value,
        test.affinity, !test.invalid_flags);
    const NTSTATUS status = D3DKMTCreateSynchronizationObject2(&create);
    if (status >= 0) fence.object = create.hSyncObject;
    std::printf("KMT_FENCE_CREATE case=%s status=%08x object=%08x cpu=%p gpuva=%016llx\n",
        test.name, static_cast<unsigned>(status), create.hSyncObject,
        test.monitored ? create.Info.MonitoredFence.FenceValueCPUVirtualAddress : nullptr,
        test.monitored ? static_cast<unsigned long long>(create.Info.MonitoredFence.FenceValueGPUVirtualAddress) : 0ull);
    if (status < 0) { observed.mapping = OsFenceResult::rejected; return; }
    observed.created = true;
    require(!test.invalid_flags, "OS unexpectedly accepted documented invalid flags");
    if (!test.monitored) {
        require(fence.object != 0, "same-device legacy synchronization object created");
        observed.mapping = OsFenceResult::legacy;
        check(fence.close(), "legacy control destruction");
        std::puts("PASS KMT_LEGACY_FENCE_DEVICE_CONTROL create_destroy_only");
        return;
    }
    require(fence.object != 0 && create.Info.MonitoredFence.FenceValueCPUVirtualAddress &&
        !(reinterpret_cast<uintptr_t>(create.Info.MonitoredFence.FenceValueCPUVirtualAddress) & 7),
        "genuine OS handle and aligned CPU mapping");
    observed.cpu_mapping = true;
    const UINT64 gpuva = create.Info.MonitoredFence.FenceValueGPUVirtualAddress;
    if (test.no_gpu) {
        require(gpuva == 0, "NoGPUAccess must not expose a GPU mapping");
        observed.mapping = OsFenceResult::cpu_only;
    } else if (!gpuva) observed.mapping = OsFenceResult::missing_gpu_mapping;
    else {
        require(!(gpuva & 7), "OS GPUVA alignment");
        observed.mapping = OsFenceResult::gpu_mapping;
    }
    // Preserve creation/mapping facts even if a later CPU behavior check fails.
    std::printf("KMT_FENCE_MAPPING_OBSERVED case=%s CPU_mapped=1 GPU_mapped=%u NoGPUAccess_CPU=%u missing_GPUVA=%u\n",
        test.name, observed.mapping == OsFenceResult::gpu_mapping,
        observed.mapping == OsFenceResult::cpu_only, observed.mapping == OsFenceResult::missing_gpu_mapping);
    // Only the explicit OS CPU output is dereferenced. Never cast the GPUVA.
    // No CPU store touches OS memory. Values used here fit in the low32 bits,
    // so the optional bounded rewind observation is also safe on x86 even
    // though a general concurrent64-bit read there would require more care.
    auto *value = static_cast<const volatile UINT64 *>(create.Info.MonitoredFence.FenceValueCPUVirtualAddress);
    require(*value == 3, "OS initial value visible through returned CPU mapping");
    fence.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(fence.event != nullptr, "OS fence wait event");
    UINT64 target = 4;
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait{};
    wait.hDevice = runtime.device; wait.ObjectCount = 1;
    wait.ObjectHandleArray = &fence.object; wait.FenceValueArray = &target; wait.hAsyncEvent = fence.event;
    check(nt_result(D3DKMTWaitForSynchronizationObjectFromCpu(&wait), "WaitFromCpu(async)"), "OS asynchronous wait");
    require(WaitForSingleObject(fence.event, 30) == WAIT_TIMEOUT && *value == 3,
        "pending OS fence cannot complete early");
    auto signal = os_fence_signal_request(runtime.device, &fence.object, &target, false);
    check(nt_result(D3DKMTSignalSynchronizationObjectFromCpu(&signal), "SignalFromCpu"), "OS CPU signal");
    require(WaitForSingleObject(fence.event, 2000) == WAIT_OBJECT_0 && *value == target,
        "same OS storage and actual completion event");
    observed.cpu_event = true;
    std::printf("PASS KMT_OWNED_MONITORED_FENCE_CPU_EVENT case=%s initial_value, delayed_event, CPU_signal\n", test.name);

    // Public D3D12 rewind semantics must not be imposed on an unflagged KMT
    // signal. This additional subtest explicitly permits intentional rewind.
    target = 1;
    signal = os_fence_signal_request(runtime.device, &fence.object, &target, true);
    const NTSTATUS rewind_status = D3DKMTSignalSynchronizationObjectFromCpu(&signal);
    std::printf("KMT_FENCE_REWIND case=%s flags=%08x status=%08x target=%llu\n",
        test.name, signal.Flags.Value, static_cast<unsigned>(rewind_status), static_cast<unsigned long long>(target));
    check(nt_result(rewind_status, "SignalFromCpu(AllowFenceRewind)"), "OS explicitly allowed CPU rewind");
    // CPU signal return does not guarantee completion. A threshold wait for1
    // would already pass at4, so observe this probe-owned CPU mapping directly
    // with a deadline; no other writer is submitted during this subtest.
    const ULONGLONG rewind_deadline = GetTickCount64() + 2000;
    while (*value != target && GetTickCount64() < rewind_deadline) Sleep(1);
    require(*value == target, "same OS storage observes explicitly allowed rewind within deadline");
    observed.rewind = true;
    std::printf("PASS KMT_OWNED_MONITORED_FENCE_CPU_REWIND case=%s AllowFenceRewind, observed_same_mapping\n", test.name);
    check(fence.close(), "explicit OS fence destruction");
    if (observed.mapping == OsFenceResult::gpu_mapping)
        std::printf("PASS KMT_OWNED_MONITORED_FENCE_MAPPING_OUTPUTS_ONLY case=%s; no GPU access or ordinary D3D12 fence import proven\n", test.name);
}

void run_os_fence_probe(LUID luid, bool controls) {
    Deadline deadline;
    validate_os_fence_cases();
    ProbeRuntime runtime;
    runtime.open(luid);
    NativeAdapter identity{};
    identity.runtime.handle = &runtime;
    identity.callbacks.pfnQueryAdapterInfoCb = probe_query;
    check(native_query_identity(identity, identity.luid, identity.generation), "KMD adapter identity");
    require(!std::memcmp(identity.luid.data(), &luid, sizeof(luid)), "exact requested VIOGPU LUID");
    std::printf("KMT_FENCE_ADAPTER luid=%08x:%08x generation=%llu device=%08x device_flags=00000000\n",
        static_cast<unsigned>(luid.HighPart), luid.LowPart,
        static_cast<unsigned long long>(identity.generation), runtime.device);
    if (!controls) {
        OsFenceObservation observed;
        run_os_fence_case(runtime, os_fence_cases[1], observed);
        check(runtime.close(), "explicit KMT device/adapter destruction");
        require(observed.mapping == OsFenceResult::gpu_mapping, "corrected shared affinity1 GPU mapping configuration failed");
        return;
    }
    unsigned legacy = 0, mapped = 0, cpu_only = 0, rejected = 0, missing_mapping = 0, behavior_failures = 0;
    unsigned created = 0, cpu_mapped = 0, cpu_event = 0, rewind = 0;
    for (const auto &test : os_fence_cases) {
        OsFenceObservation observed;
        try {
            run_os_fence_case(runtime, test, observed);
        } catch (const ProbeFailure &error) {
            ++behavior_failures;
            std::fprintf(stderr, "KMT_FENCE_CASE_FAILURE case=%s error=%s\n", test.name, error.what());
        }
        created += observed.created; cpu_mapped += observed.cpu_mapping;
        cpu_event += observed.cpu_event; rewind += observed.rewind;
        switch (observed.mapping) {
            case OsFenceResult::legacy: ++legacy; break;
            case OsFenceResult::gpu_mapping: ++mapped; break;
            case OsFenceResult::cpu_only: ++cpu_only; break;
            case OsFenceResult::missing_gpu_mapping: ++missing_mapping; break;
            case OsFenceResult::rejected: ++rejected; break;
            case OsFenceResult::unclassified: break;
        }
    }
    check(runtime.close(), "explicit KMT device/adapter destruction");
    std::printf("KMT_FENCE_CONTROLS_COMPLETE legacy=%u GPU_mapped=%u NoGPUAccess_CPU=%u rejected=%u missing_GPUVA=%u behavior_failures=%u created=%u CPU_mapped=%u CPU_event_pass=%u rewind_pass=%u\n",
        legacy, mapped, cpu_only, rejected, missing_mapping, behavior_failures, created, cpu_mapped, cpu_event, rewind);
    require(!behavior_failures, "one or more created OS objects violated their mapping/event contract");
    require(legacy == 1, "same-device legacy fence creation control failed");
    require(mapped != 0, "none of the tested GPU mapping configurations succeeded; inspect CPU-only outcome and statuses");
    std::puts("BOUNDARY independently KMT-owned objects only; ordinary D3D12 fence import and GPU access unproven");
}
