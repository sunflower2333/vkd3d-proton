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
    std::puts("PASS OS_FENCE_CONFIGURATION_ONLY shared_pair, affinity_0_1, private, NoGPUAccess, original_bug_rejected; no KMT calls");
}

enum class OsFenceResult { rejected, legacy, gpu_mapping, cpu_only, missing_gpu_mapping };
OsFenceResult run_os_fence_case(ProbeRuntime &runtime, const OsFenceCase &test) {
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
    if (status < 0) return OsFenceResult::rejected;
    require(!test.invalid_flags, "OS unexpectedly accepted documented invalid flags");
    if (!test.monitored) {
        require(fence.object != 0, "same-device legacy synchronization object created");
        check(fence.close(), "legacy control destruction");
        std::puts("PASS KMT_LEGACY_FENCE_DEVICE_CONTROL create_destroy_only");
        return OsFenceResult::legacy;
    }
    require(fence.object != 0 && create.Info.MonitoredFence.FenceValueCPUVirtualAddress &&
        !(reinterpret_cast<uintptr_t>(create.Info.MonitoredFence.FenceValueCPUVirtualAddress) & 7),
        "genuine OS handle and aligned CPU mapping");
    // Only the explicit OS CPU output is dereferenced. Never cast the GPUVA.
    // Reads below happen after synchronous CPU signal/event completion, with
    // no concurrent writer, including on x86. No CPU store touches OS memory.
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
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU signal{};
    signal.hDevice = runtime.device; signal.ObjectCount = 1;
    signal.ObjectHandleArray = &fence.object; signal.FenceValueArray = &target;
    check(nt_result(D3DKMTSignalSynchronizationObjectFromCpu(&signal), "SignalFromCpu"), "OS CPU signal");
    require(WaitForSingleObject(fence.event, 2000) == WAIT_OBJECT_0 && *value == target,
        "same OS storage and actual completion event");
    target = 1;
    check(nt_result(D3DKMTSignalSynchronizationObjectFromCpu(&signal), "SignalFromCpu(rewind)"), "OS CPU rewind");
    require(*value == target, "same OS storage observes rewind");
    const UINT64 gpuva = create.Info.MonitoredFence.FenceValueGPUVirtualAddress;
    std::printf("PASS KMT_OWNED_MONITORED_FENCE_CPU_EVENT case=%s initial_value, delayed_event, CPU_signal, rewind\n", test.name);
    check(fence.close(), "explicit OS fence destruction");
    if (test.no_gpu) {
        require(gpuva == 0, "NoGPUAccess must not expose a GPU mapping");
        return OsFenceResult::cpu_only;
    }
    if (!gpuva) return OsFenceResult::missing_gpu_mapping;
    require(!(gpuva & 7), "OS GPUVA alignment");
    std::printf("PASS KMT_OWNED_MONITORED_FENCE_MAPPING_OUTPUTS_ONLY case=%s; no GPU access or ordinary D3D12 fence import proven\n", test.name);
    return OsFenceResult::gpu_mapping;
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
        const auto result = run_os_fence_case(runtime, os_fence_cases[1]);
        check(runtime.close(), "explicit KMT device/adapter destruction");
        require(result == OsFenceResult::gpu_mapping, "corrected shared affinity1 GPU mapping configuration failed");
        return;
    }
    unsigned legacy = 0, mapped = 0, cpu_only = 0, rejected = 0, missing_mapping = 0, behavior_failures = 0;
    for (const auto &test : os_fence_cases) {
        try {
            switch (run_os_fence_case(runtime, test)) {
                case OsFenceResult::legacy: ++legacy; break;
                case OsFenceResult::gpu_mapping: ++mapped; break;
                case OsFenceResult::cpu_only: ++cpu_only; break;
                case OsFenceResult::missing_gpu_mapping: ++missing_mapping; break;
                case OsFenceResult::rejected: ++rejected; break;
            }
        } catch (const ProbeFailure &error) {
            ++behavior_failures;
            std::fprintf(stderr, "KMT_FENCE_CASE_FAILURE case=%s error=%s\n", test.name, error.what());
        }
    }
    check(runtime.close(), "explicit KMT device/adapter destruction");
    std::printf("KMT_FENCE_CONTROLS_COMPLETE legacy=%u GPU_mapped=%u NoGPUAccess_CPU=%u rejected=%u missing_GPUVA=%u behavior_failures=%u\n",
        legacy, mapped, cpu_only, rejected, missing_mapping, behavior_failures);
    require(!behavior_failures, "one or more created OS objects violated their mapping/event contract");
    require(legacy == 1, "same-device legacy fence creation control failed");
    require(mapped != 0, "none of the tested GPU mapping configurations succeeded; inspect CPU-only outcome and statuses");
    std::puts("BOUNDARY independently KMT-owned objects only; ordinary D3D12 fence import and GPU access unproven");
}
