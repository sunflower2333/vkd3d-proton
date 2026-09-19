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

void run_os_fence_probe(LUID luid) {
    Deadline deadline;
    ProbeRuntime runtime;
    runtime.open(luid);
    NativeAdapter identity{};
    identity.runtime.handle = &runtime;
    identity.callbacks.pfnQueryAdapterInfoCb = probe_query;
    check(native_query_identity(identity, identity.luid, identity.generation), "KMD adapter identity");
    require(!std::memcmp(identity.luid.data(), &luid, sizeof(luid)), "exact requested VIOGPU LUID");
    std::printf("KMT_FENCE_ADAPTER luid=%08x:%08x generation=%llu\n",
        static_cast<unsigned>(luid.HighPart), luid.LowPart,
        static_cast<unsigned long long>(identity.generation));

    OwnedOsFence fence;
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create{};
    create.hDevice = runtime.device;
    create.Info.Type = D3DDDI_MONITORED_FENCE;
    create.Info.Flags.NtSecuritySharing = 1;
    create.Info.MonitoredFence.InitialFenceValue = 3;
    create.Info.MonitoredFence.EngineAffinity = 1;
    const NTSTATUS status = D3DKMTCreateSynchronizationObject2(&create);
    fence.object = create.hSyncObject;
    std::printf("KMT_MONITORED_FENCE_CREATE status=%08x object=%08x cpu=%p gpuva=%016llx\n",
        static_cast<unsigned>(status), fence.object, create.Info.MonitoredFence.FenceValueCPUVirtualAddress,
        static_cast<unsigned long long>(create.Info.MonitoredFence.FenceValueGPUVirtualAddress));
    check(nt_result(status, "CreateSynchronizationObject2(MONITORED_FENCE)"), "OS monitored fence creation");
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
    std::puts("PASS KMT_OWNED_MONITORED_FENCE_CPU_EVENT initial_value, delayed_event, CPU_signal, rewind");
    check(fence.close(), "explicit OS fence destruction");
    check(runtime.close(), "explicit KMT device/adapter destruction");
    require(gpuva != 0 && !(gpuva & 7), "OS must return a nonzero aligned GPUVA for GPU import");
    std::puts("PASS KMT_OWNED_MONITORED_FENCE_MAPPING_OUTPUTS_ONLY; no GPU access or ordinary D3D12 fence import proven");
}
