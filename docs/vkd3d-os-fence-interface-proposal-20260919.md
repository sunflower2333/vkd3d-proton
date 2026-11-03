# OS monitored-fence producer and import proposal

Status: direct GPU fence-import proposal and OS probe record. **The proposed
GPUVA producer is not a universal prerequisite for ordinary single-adapter
D3D12.** Microsoft's physical-mode compute sample associates command queues
through the runtime's CreateContext callback and leaves external fence packet
scheduling to the runtime. The later queue audit supersedes that broad earlier
assumption; see `vkd3d-runtime-queue-association-20260919.md`. Ordinary admission
remains closed for actual incomplete runtime/graphics/residency/Present paths.

The corrected target matrix creates all five valid monitored-fence variants
with real CPU mappings, but all return GPUVA zero. The initial invalid sharing
pair is confirmed as a probe error. A second probe error (missing explicit KMT
rewind permission) is also corrected below. These probe defects do not change
the independently observed source gaps in the GPUVA producer.

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

1. Run the owned-object control matrix on the unchanged installed driver:
   `vkd3d-umd-shared-gpu-probe --luid-low HEX --luid-high HEX --run-os-fence-controls`.
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

## Historical initial probe build (superseded)

Commit `3ffcd2b06060e0b0786165daa09c65b5936dfb0c` passed all five jobs of
[CI 35444359358](https://github.com/sunflower2333/vkd3d-proton/actions/runs/35444359358).
Actual x86/x64 logs show the new probe compiled, linked, and rejected an
unspecified mode/LUID before device access. ARM64 compiled and linked as well.
Native runtime fixtures, semantic negative controls and the explicitly CPU WARP
public D3D12 fence/copy harness still pass on all three execution architectures.
The new OS mapping mode itself is **not run** in CI or on the target by this
worker: it requires the actual VIOGPU adapter. No native-import runtime result
is inferred from compilation.

| Architecture | Artifact ID | Archive SHA256 |
| --- | --- | --- |
| ARM64 | 10584882172 | 729e6e707380414858f7d40f4916c34e7864a1ecf01a67a3b55cfcbda4e78e7e |
| x64 | 10585361633 | af8646aa643d9569aad9fc0628e5dbeb10b911138793602e3d7f50b4fc02fab0 |
| x86 | 10584992067 | 6e36b052b1343198e974c16b3c19412a06f885d94beafda1f860359e5c03ab92 |

ARM64 `package/vkd3d-umd-shared-gpu-probe.exe` SHA256, from the build log:
`76a5161ebe51b2548a984b283ad6a80dcb97d22b890b76421d198296b9e40105`.
Jobs: Linux `105900630403`, x86 `105900630504`, ARM64 build `105900630519`,
x64 `105900630554`, ARM64 runtime `105901454077`.
Archive identities are from the GitHub API; this worker downloaded nothing.
Root can stage only this executable for the OS mapping mode; replacing
`viogpud3d12.dll`, installing a matched ICD or modifying 58552 is unnecessary.

## Target result, probe correction and bounded controls

Main-thread evidence for `candidate58552-os-fence-20260919a`: unchanged 58552,
probe SHA256 `76a5161ebe51b2548a984b283ad6a80dcb97d22b890b76421d198296b9e40105`,
LUID `00000000:01a9f9e3`, reset generation 2. Monitored-fence creation returned
`c000000d`, object/CPU/GPUVA all zero; process exit 1 after 349 ms. The main thread
reported retained DWM/Explorer and no new application faults. This worker did
not run remote commands.

The probe incorrectly set `NtSecuritySharing=1, Shared=0`. Microsoft's local
`d3dukmdt/ns-d3dukmdt-_d3dddi_synchronizationobject_flags.md` explicitly says:
"If NtSecuritySharing is set to 1, Shared must be set to 1."
The same document specifies both bits zero for a nonshared object. The probe
now constructs the correct pair and checks it before a positive OS call.

The new `--run-os-fence-controls` mode uses the **same real KMT device** for
seven bounded creation attempts; successful monitored objects also run the
existing OS CPU mapping/event/signal/rewind test and are destroyed immediately.

| Case | Shared / NT | EngineAffinity | NoGPUAccess | Purpose |
| --- | --- | --- | --- | --- |
| Legacy D3DDDI_FENCE | 0 / 0 | Not applicable | 0 | Check ordinary sync-object creation on this device |
| Corrected monitored fence | 1 / 1 | 1 | 0 | Repeat initial request with the invalid pair fixed |
| All-adapter monitored fence | 1 / 1 | 0 | 0 | Isolate affinity selection |
| Private monitored fence | 0 / 0 | 1 | 0 | Isolate sharing policy |
| Private all-adapter fence | 0 / 0 | 0 | 0 | Combine documented private/default-affinity form |
| Packet/CPU-only monitored fence | 1 / 1 | 0 | 1 | Check monitored-fence support without GPU mapping |
| Explicit historical negative | 0 / 1 | 1 | 0 | Record OS rejection of the original invalid flag pair |

EngineAffinity 0 means all physical adapters; 1 selects physical adapter 0.
NoGPUAccess explicitly prevents a GPUVA mapping and keeps a 64-bit CPU fence;
zero GPUVA is expected and accepted for that control. It proves no GPU import.
The real device creation result/handle and zero device flags are printed.
LegacyMode is left unchanged: its documented meaning is legacy DirectDraw/D3D9
residency/primary behavior, not selection of physical or virtual GPU mode.

Interpret actual case results before drawing a capability conclusion. If the
corrected shared request passes while the historical negative fails, the old
failure is explained by flags. If only affinity 0 passes, investigate adapter
affinity. If CPU-only passes while all four tested GPU-mapped variants fail,
monitored CPU/packet support is demonstrated and the GPU mapping path remains
unresolved. If the legacy object also fails, first investigate the device/OS
sync-object path. Failure of these tested combinations is not proof that every
possible monitored-fence configuration is unsupported.

The matrix prints every creation status and a final category count. It exits
nonzero if the same-device legacy control fails, no tested GPU-mapped variant
succeeds, or an actual mapping/event contract fails. Thus CPU-only success is
recorded without counting it as GPU mapping success.

`--validate-os-fence-controls` constructs all requests without opening any
device. CI executes it on x86/x64/ARM64, verifies documented flag combinations,
input/output initialization and detects the original missing Shared-bit defect.
This validation is a request-construction regression test, not OS acceptance.
The first corrected real-device run is recorded below; it exposed a separate
rewind-permission defect in the behavior subtest.

## Corrected creation results and raw KMT rewind fix

Probe commit `b5515ff61a2dec9638a05cbe134d90f484aa41ab` passed all five jobs of
[CI 35445525811](https://github.com/sunflower2333/vkd3d-proton/actions/runs/35445525811).
Actual x86/x64/ARM64 runtime logs confirm request-construction controls, existing
native semantic negatives and explicitly CPU-WARP public fence/copy validation.
This CI performs no target KMT calls.

Main-thread target run `candidate58552-os-fence-controls-20260919a` used unchanged
58552, LUID `00000000:01a9f9e3`, reset generation 2, device `40000040`, flags 0.
Probe SHA256 `ee8ad96aae49fb7f2d8fc6ca8a1bc75e84b6a954712e808a77927ae0ad276dd6`.
It exited 1 after 1118 ms without timeout; the main thread reported retained
driver/desktop and no faults.

- The same-device legacy fence creation control passed.
- All five valid monitored-fence requests returned status 0 and real CPU
  mappings, with GPUVA 0 in every case. For the four requests allowing GPU
  access this is a missing mapping; for NoGPUAccess it is the expected result.
- The historical invalid Shared/NT pair returned `c000000d`.
- Every valid object reached the rewind subtest, after checking the initial
  value, pending event and CPU signal/event completion. Unflagged rewind then
  returned `c000000d` in all five cases.

The former summary counters were inaccurate: the rewind exception prevented
earlier mapping results from being counted. Raw per-case creation output is the
authority for those mappings, not the zero totals of that old probe.

Microsoft's `D3DDDICB_SIGNALFLAGS.AllowFenceRewind` explicitly permits intentional
rewind. `D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU.Flags` has this exact type.
The optional rewind subtest now sets that flag, logs its exact status, and waits
up to two seconds for the same CPU mapping to show the new value. CPU-signal
documentation says completion must not be assumed merely because the call
returned. A threshold wait at 1 would already pass while the value is 4, so
the probe observes its own mapping directly without submitting another writer.
NoSignal is unrelated: that object flag would deny every signal, including the
already successful signal to 4.

Creation/mapping observations are now retained before any behavior subtest.
The initial/pending/CPU signal-event result and rewind result are recorded
separately. A later failure cannot erase GPUVA-zero, CPU-only or CPU-event
evidence. A behavior error still prevents the whole probe from passing, and
the GPU mapping gate stays closed when all four GPU-access variants return 0.
CI's no-device request check now rejects omission of explicit rewind permission.

Additional exact Microsoft sources:

- `d3dukmdt/ns-d3dukmdt-_d3dddicb_signalflags.md`
- `d3dkmthk/ns-d3dkmthk-_d3dkmt_signalsynchronizationobjectfromcpu.md`
- `d3dkmthk/nf-d3dkmthk-d3dkmtsignalsynchronizationobjectfromcpu.md`
- [WDK KMT header](https://github.com/tpn/winsdk-10/blob/master/Include/10.0.16299.0/km/d3dkmthk.h)

## Current flagged-rewind probe validation and handoff

Source commit `6fdbcede1d12954de0b5d0b40cabcbaea26bf90c` passed all five jobs of
[CI 35446236355](https://github.com/sunflower2333/vkd3d-proton/actions/runs/35446236355).
Actual x86/x64/native ARM64 execution logs show
`PASS OS_FENCE_CONFIGURATION_ONLY ... explicit_AllowFenceRewind; no KMT calls`.
Existing native ownership/negative controls and explicit CPU-WARP public fence
and copy regressions pass on all three Windows architectures. Linux real CPU
Vulkan backend and semantic negative controls pass as well.

| Architecture | Artifact ID | Archive SHA256 |
| --- | --- | --- |
| ARM64 | 10584414800 | 4d076bbca99a20c7e7c5da872a41bd332609ad1a8ba717316700b0a14559f373 |
| x64 | 10584904892 | ccb87fdcf3e6fe3396ff4299945fc217c2ad2fa5b7ea492cdae5c1501596e25e |
| x86 | 10584864835 | 2a4a1ecee7f6855c2e6ad6ccbd1c6f49d1dc55e57a043fae87a9a51c5ffa05d6 |

ARM64 `package/vkd3d-umd-shared-gpu-probe.exe` SHA256 from actual build log:
`11509ae3be0a0540376f5f3cb2e8aaa4a97e3ca451e0d8021d57a4b0b89f0e6f`.
Actual jobs: Linux `105905523327`, ARM64 build `105905523421`,
x64 `105905523436`, x86 `105905523660`, ARM64 runtime `105906249617`.
ARM64 runtime log artifact `10584934845` archive SHA256
`0bee5966c56a1399c32d52ad0d348d62b2343b1c13df093c435f69d6725ebb35`.

Root can transfer only this EXE and use the same fresh-LUID
`--run-os-fence-controls` command with a 60-second outer deadline. A nonzero
exit is still required if the GPU-access variants return GPUVA zero; successful
CPU events or explicitly allowed rewind do not satisfy the GPU mapping gate.
Root executed `candidate58552-os-fence-rewind-20260919a` on unchanged 58552:
1802 ms, exit 1 only at the absent GPU mapping gate, no timeout or new faults,
DWM/driver retained, task cleaned. LUID `00000000:01a9f9e3`, generation 2.

```text
legacy=1 GPU_mapped=0 NoGPUAccess_CPU=1 rejected=1 missing_GPUVA=4
behavior_failures=0 created=6 CPU_mapped=5 CPU_event_pass=5 rewind_pass=5
```

All five valid requests have successful CPU events and explicitly allowed
rewinds (`flags=4, status=0`). Four GPU-access requests still return GPUVA 0;
NoGPUAccess intentionally returns 0; the invalid flag pair returns `c000000d`.
This proves these CPU operations on probe-owned OS fences. It does not prove
ordinary runtime queue submission, and it does not prove that runtime software
fence scheduling requires GPUVA.

No production UMD, KMD, Mesa or shared runtime ABI changed in the probe commit; no artifact download
or remote action by this worker. The shared runtime-v1 header SHA256 remains
`c072169e380f14fd4f967dd0ea765e4c0ac8f671136e93baedce8cd2dadbed34`.
