# Fence lifecycle and remaining native contract

## Microsoft contract audit

The local Microsoft `windows-driver-docs/windows-driver-docs-pr/display/context-monitoring.md`
and DDI create/destroy/calc-private-size documents establish that Windows owns
the monitored fence. The exact R0 `d3d12umddi.h` declarations give CreateFence
only an array of `FenceValue` and `FenceMonitoredValue` **GPU virtual address**
placements and flags. They supply neither an initial value, a CPU address nor a
KMT synchronization handle. There is no GetCompletedValue DDI in this table;
the runtime reads the OS-owned CPU mapping.

The CPU mapping is read-only. GPU signal must update the real mapped GPU fence
storage and satisfy the OS notification/order rules. If direct writes are not
supported, Microsoft permits software GPU signal/wait packets through
`SignalSynchronizationObjectFromGpuCb` / `WaitForSynchronizationObjectFromGpuCb`.
Those callbacks require the actual OS `hSyncObject`, and waiting requires prior
pending work to be flushed. Inventing a new embedded fence or reinterpreting a
GPUVA as a CPU pointer does not implement this contract.

The current shared runtime-v1 ABI exposes no monitored-fence import or lookup
from the supplied GPUVA placements to the real OS synchronization object.
Native CreateFence/DestroyFence/queue SignalFence/WaitForFence therefore remain
unpublished. This candidate implements their independently testable backend
event/lifetime prerequisite, not the complete public native fence contract.

## Implemented backend slice

- Completion waits register a real backend fence event, replacing 1-ms value
  polling. CPU Vulkan uses eventfd; Windows uses an event handle.
- The registration owns the embedded fence independently of the caller's
  backend wrapper. Timeout keeps the registration armed; cancel/destroy removes
  it under the same mutex used by actual completion before closing its handle.
- Cancellation wakes a blocked wait with cancellation, never successful GPU
  completion. A completion event stays latched across a later fence rewind.
- Fatal native WDDM errors propagate into the embedded device. Waits check
  device loss between bounded 20-ms event waits, report the removal reason,
  and completed-value queries return UINT64_MAX. New signal/wait work rejects
  a removed device. The public RemoveDevice stub is not changed.
- One waiting thread is supported per event registration. Cancel may run
  concurrently; destroy follows that thread's return. No shared runtime ABI,
  KMD, Mesa, root worktree or target change is required for this slice.

## Validation

`vkd3d-umd-fence-test` runs the real embedded backend on explicitly selected
CPU Vulkan. It checks two-queue deferred signal/wait, pending versus completed
values, rewind, 64 cancellation/handle-reuse rounds, cancellation of a blocked
thread, original fence-wrapper destruction with an outstanding event, and
actual embedded device-loss propagation. A separately compiled negative control
drops the real backend queue wait and must fail at the deferred-completion check.
Existing buffer/texture/sampler/indirect GPU-readback tests use the new event wait.

The SDK-only ordinary application probe adds `--run-fences`:

```text
vkd3d-system-d3d12-probe.exe --list
vkd3d-system-d3d12-probe.exe --luid-low LOWHEX --luid-high HIGHHEX --run-fences
```

After successful device admission it exercises public CreateFence, completed
values, future queue Wait, queue Signal, event completion, CPU rewind and final
fence-release event behavior. Explicit CI WARP validates this harness on all
three Windows architectures; it is never VIOGPU acceptance. The installed native
driver still requires a root-owned target run and the missing OS fence mapping.

### Verified CI checkpoint

Production commit `9ab92ec87535fa3c50c9257c94cb57970066cee2` passed all five jobs
of [run 35443399378](https://github.com/sunflower2333/vkd3d-proton/actions/runs/35443399378).
Actual logs were checked, including the precise positive and negative markers:

| Job | ID | Result |
| --- | --- | --- |
| Linux CPU Vulkan backend | 105898092720 | Real fence event lifecycle and full backend regression pass; dropped queue wait fails as required |
| Windows ARM64 build | 105898092809 | WDK build and package pass |
| Windows x64 | 105898092810 | Runtime fixtures, six semantic negatives and public WARP fences/copy pass |
| Windows x86 | 105898092844 | Runtime fixtures, six semantic negatives and public WARP fences/copy pass |
| Windows ARM64 runtime | 105898905444 | Actual ARM64 runtime fixtures, six semantic negatives and public WARP fences/copy pass |

The Windows fixtures confirm fatal native errors reach the embedded backend
device. All three ordinary WARP runs report
`PASS PUBLIC_FENCE_LIFECYCLE delayed_queue_completion, CPU_rewind, final_release_event`
and four copy rounds with changed data and sentinel verification. These are
CPU harness results, not VIOGPU or OS native monitored-fence validation.

Artifacts below belong to that exact production commit. Archive SHA256 values
come from the GitHub API; no archive was downloaded by this worker.

| Architecture | Artifact ID | Bytes | Archive SHA256 |
| --- | --- | --- | --- |
| ARM64 | 10585190425 | 13026717 | 15ff2d1582a3736e62a902f8bbaff4e918ca0f50fdb3e17c6c0802f5472e769a |
| x64 | 10585275364 | 13498978 | 6257069a03c85b349ce0f9d310f3c80658275530231b34fe6a77fcdc90845e93 |
| x86 | 10585130596 | 13323071 | 1d8385d0049bcc4b2ccec8a3a4616d5694786005d84e1e4ef197ee90c4b8961b |

ARM64 package file hashes from the build log:

- `vkd3d-system-d3d12-probe.exe`: `fb13f846fad3e731c0503cfb862aaf82a9504f39c0bb4faca4565ab42c1a636c`
- `viogpud3d12.dll`: `3da8c73a9662e7fcd0e26f39a8b63dc8a037ec0bdc8abed4a288636a12361346`

ARM64 runtime log artifact `10585150539` has archive SHA256
`e49e8407bed23615e48f2f79eea8631c2472d591c5f4de9fd83e91a0f5e02d3a`.
Shared runtime-v1 header remains unchanged, SHA256
`c072169e380f14fd4f967dd0ea765e4c0ac8f671136e93baedce8cd2dadbed34`.

Root owns signing, package integration and hardware execution. The ordinary
probe may test the installed driver independently of replacing its DLL: obtain
the current hardware LUID with `--list`, then run `--run-device`, `--run-fences`
or `--run-copy` with that exact LUID. Bound execution to 60 seconds. Native
admission remains closed, so successful target D3D12CreateDevice is not expected
from this candidate and has not been claimed.

## Still required before opening ordinary D3D12 admission

1. Authoritative OS fence placement import/identity, real monitored storage and
   signal notification, paging/mapping readiness, and ordered OS queue wait.
2. Native fence lifecycle callbacks integrated against that ownership, including
   shared/opened fences and device-loss/teardown of OS waiters.
3. Residency, GPUVA and paging/DDI resource lifetime contracts.
4. Graphics pipeline, draw, RT/DS views, formats, Present and coherent FL11_0
   capability reporting beyond the current compute/limited-copy subset.
5. Ordinary system D3D12 tests on the matching signed VIOGPU package. Backend or
   WARP tests cannot establish runtime admission, hardware pixels or Present.

References: [Context monitoring](https://learn.microsoft.com/windows-hardware/drivers/display/context-monitoring),
[CreateFence DDI](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_createfence),
[exact R0 header](https://github.com/tpn/winsdk-10/blob/master/Include/10.0.16299.0/um/d3d12umddi.h).
