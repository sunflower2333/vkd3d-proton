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
