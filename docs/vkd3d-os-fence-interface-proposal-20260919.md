# OS monitored-fence producer and import proposal

Status: interface proposal, not an implemented native fence provider. No KMD,
Mesa, parent-tree or runtime-v1 ABI file is changed by this checkpoint. Ordinary
D3D12 admission remains closed.

## Exact source baseline

Installed driver 58552 source, as identified by the main thread:
`reference/codes/viogpu-shared-read-release-20260919`
at `bc97a4b616aed8a8b57d641ee2d50fd63eb4b2e6`.
The earlier audit of main `gunyah-guest-drivers-windows@e6d3eb09` is superseded
for installed-driver conclusions.

| Producer or consumer | Current source | Evidence |
| --- | --- | --- |
| KMD memory model | `viogpu/viogpudo/viogpudo.cpp:3629` | VirtualAddressingSupported, GpuMmuSupported and IoMmuSupported are all zero |
| KMD DDI registration | `viogpu/viogpuwddm/driver_entry.cpp:282` | Physical CreateContext, Render, Patch and SubmitCommand; no process/page-table/virtual-submit producer |
| Existing completion | `viogpu/viogpuwddm/wddmddi.cpp:4941` | GET_COMPLETED_FENCE returns a context DMA sequence, not application fence storage |
| Existing aperture mapping | `viogpu/viogpuwddm/wddmddi.cpp:7679` | Mapping belongs to a driver-created allocation and its retained MDL |
| Paging dispatcher | `viogpu/viogpuwddm/wddmddi.cpp:8417` | Allocation/aperture operations; no native application fence GPUVA mapping producer |
| Physical submit | `viogpu/viogpuwddm/wddmddi.cpp:10439` | Submission retirement cannot identify an arbitrary application fence GPUVA |
| Turnip transport | `external/mesa/src/freedreno/vulkan/tu_knl_wddm.cc:1351` | Requested IOVA checked against a context-specific host VA range; not VidMm GPUVA |
| VKD3D import ABI | `include/private/mesa_wddm_runtime.h` | Version 1 imports driver-created allocation tokens only |
| VKD3D native R0 | `libs/vkd3d-umd/runtime.inc`, `ddi.cpp` | Fence callbacks are absent, versions zero, tables/caps closed |

Line numbers are for the KMD commit above or this isolated VKD3D branch, as
appropriate. The installed Mesa transport also contains no `mwd_callbacks`
provider; matched runtime-v1 integration is a separate prerequisite already
owned by the main thread.

## What Microsoft actually supplies

R0 `D3D12DDIARG_CREATE_FENCE` gives an array of two GPUVA placements per node:
`FenceValue.BaseAddress` and `FenceMonitoredValue.BaseAddress`, plus flags.
It gives no initial value, CPU pointer or `hSyncObject`. There is no native
GetCompletedValue DDI: the runtime reads the OS-owned CPU mapping itself.

`D3DKMTCreateSynchronizationObject2(MONITORED_FENCE)` returns an actual
`hSyncObject`, CPU mapping and GPUVA **to its caller**. A private object created
by VKD3D is a different object from one created internally by the D3D12 runtime.
That API is not a lookup by GPUVA. The reviewed R0 contract does not expose a
documented reverse lookup from a runtime fence placement to `hSyncObject`.

For direct GPU access, genuine GpuMmu page-table updates provide the process and
OS-owned PTEs. The driver has to implement the GPUVA model and mapping lifetime
before these inputs exist. Some implicit allocations have no KMD allocation
handle. Neither an arbitrary user GPUVA nor a user-supplied PFN is authority to
pin or map kernel memory. This implementation must not reinterpret a GPUVA as
either a CPU pointer or the current Turnip context IOVA.

The WDDM 2.2 `DXGK_OPERATION_SIGNAL_MONITORED_FENCE` and later standalone
`DxgkDdiSignalMonitoredFence` concern paging/kernel-submission fences. They are
not a general application-fence creation notification and cannot populate a
registry for every R0 CreateFence. WDDM 3.2 native GPU fence DDIs are a separate,
newer contract and must not be registered on the current 2.x interface.

## Proposed coordinated boundary

Implement the mapping producer first. Keep runtime-v1 unchanged; negotiate a
separate, size/version-checked in-process extension only after the producer is
real. No numeric capability bit, pNext tag or private escape opcode is reserved
by this document. Allocate those centrally during the coordinated change.

The initial provider is limited to one physical adapter, 64-bit aligned fence
values, and exact known flag values. Its logical interface is:

```c
struct fence_placement_request {
    uint32_t size, version, flags, physical_adapter;
    uint64_t reset_generation;
    uint64_t fence_value_gpuva, fence_monitored_value_gpuva;
};
struct fence_import_result {
    uint32_t size, version, capabilities, reserved;
    uint64_t token, reset_generation, mapping_generation;
};
int32_t import_os_fence(void *device_owner,
    const struct fence_placement_request *, struct fence_import_result *);
int32_t release_os_fence(void *device_owner, uint64_t token);
int32_t enqueue_os_fence(void *queue_owner, uint64_t token,
    uint64_t value, uint32_t operation, uint32_t reserved);
```

These are proposed operations, not callbacks that can already be supplied by
Mesa or KMD. The result exports no CPU pointer, physical address or invented
Windows handle. A token is an unforgeable provider-owned identity, scoped to
the actual process/device and reset epoch. It retains the exact OS storage and
mapping generation until all accepted queue operations finish. Failed import
publishes no token. Failed enqueue transfers no ownership and sets the exact
native device/queue error. Release ends the client's ownership while accepted
work retains its own references; it cannot close a still-used mapping.

The provider must meet all of these before it advertises the extension:

1. Obtain OS mapping authority from a real per-process GpuMmu implementation:
   CreateProcess/DestroyProcess, virtual-context creation, SetRootPageTable,
   page-table update/copy/TLB/paging operations, GPUVA/residency and virtual
   submission. Extend the files listed above and associated KMD types/tests.
   Existing physical-mode caps stay unchanged until that complete alternative
   works. Host-owned page tables alone do not meet the Windows GpuMmu contract.
2. Resolve both placements through that process's actual mapping, pin exactly
   those OS pages through the scheduled use, and bind their storage through the
   existing guest-page/host-resource transport with proven coherency. Importing
   an unrelated allocation or a copy of its current value is not permitted.
3. Implement signal ordered behind every preceding queue operation, with the
   required 64-bit atomic visibility and OS notification semantics. A generic
   two-dword fill does not establish atomicity. Basic monitored-fence progress
   must be visible before the corresponding actual DMA completion notification.
4. Implement wait without blocking the caller or preventing other queues from
   executing the signal. If software OS wait/signal callbacks are selected,
   they require a documented genuine `hSyncObject` producer; none is identified
   in current R0 inputs. That fallback stays unavailable until provenance is
   established. Direct GPU waits need a supported scheduling/preemption design;
   polling the sole non-preemptible host queue can deadlock a producer behind it.
5. Give each logical D3D12 queue a proven ordered execution domain. The current
   single `Context::native_context` callback owner is not sufficient evidence
   that waits on one queue leave another queue able to make progress. Queue
   creation/destruction and submit callbacks need coordinated ownership changes.
6. On reset, paging remap or teardown, revoke imports by generation, terminate
   waits with device loss, and retain OS mappings until accepted work cannot
   access them. Shared/opened fences must resolve to the same OS storage under
   the recipient process's legitimate mapping and access rights.

Changing only `mwd_callbacks` cannot create any of these OS facts. The public
DDI consumer must remain unpublished while any provider operation is a stub.

## Test and integration sequence

1. Run the new owned-object mapping probe on the unchanged installed driver:
   `vkd3d-umd-shared-gpu-probe --luid-low HEX --luid-high HEX --run-os-fence-mapping`.
   It validates the exact KMD-published VIOGPU LUID and records the real KMT
   creation status/handle/CPU/GPU outputs, then tests pending event, CPU signal
   and rewind through OS APIs only. It never loads Vulkan or creates a D3D12
   device in this mode. Run with a 60-second outer deadline. Even a pass proves
   only this independently created object's mappings, not runtime import or GPU
   access. A zero GPUVA or rejected OS creation is a concrete current blocker.
2. Provider unit tests: wrong process/LUID/epoch, unaligned or unmapped GPUVA,
   wraparound/range crossing, implicit allocations without KMD handle, nonresident
   mapping, revocation between import/enqueue, stale token reuse, shared access
   denial, failure rollback and exactly-once final release.
3. Provider semantic negatives: substitute an unrelated page; signal before
   submitted work completes; reuse a token after reset; drop a queue wait;
   retire a mapping before its last operation. Each must fail the verifier.
4. Real OS/KMD tests: consumer queue waits a future value while independent
   producer work completes; CPU signal unblocks the same OS object; GPU signal
   updates the runtime's mapped value and actual event; remap/reset/teardown
   produce no stale success or use-after-free. Do not run deadlock stress until
   independent queue progress and cancellation are implemented.
5. UMD integration: implement exact R0 calc/create/destroy and queue signal/wait
   fields against the provider, preserve flags and output mask semantics, test
   callback reentry, poisoned private slots, native device loss and all ownership
   failures on x86/x64/ARM64. Run existing real backend event regressions.
6. Open ordinary admission only with the remaining mandatory graphics,
   residency, capabilities and Present contracts. Then run the SDK-only system
   D3D12 `--run-fences`/`--run-copy` against the exact signed VIOGPU LUID and
   loaded DLL identity. Explicit WARP results validate only the test harness.

## Microsoft sources reviewed

- `windows-driver-docs/windows-driver-docs-pr/display/context-monitoring.md`
- `display/gpu-virtual-memory-in-wddm-2-0.md`, `gpummu-model.md`,
  `gpu-virtual-address.md`, `per-process-gpu-virtual-address-spaces.md`
- Local DDI `d3d12umddi` CreateFence/CalcPrivateFenceSize/DestroyFence docs and
  exact [R0 header](https://github.com/tpn/winsdk-10/blob/master/Include/10.0.16299.0/um/d3d12umddi.h)
- Local `d3dukmdt` D3DDDI_SYNCHRONIZATIONOBJECTINFO2 and `d3dkmthk` monitored-fence
  creation/signal/wait declarations
- Local `d3dkmddi` DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE,
  DXGKARG_BUILDPAGINGBUFFER, DXGK_BUILDPAGINGBUFFER_OPERATION,
  DxgkDdiSignalMonitoredFence and DXGKARG_SIGNALMONITOREDFENCE
