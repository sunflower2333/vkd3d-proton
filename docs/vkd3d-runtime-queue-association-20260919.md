# Ordinary D3D12 runtime queues in physical mode

The earlier direct OS fence-import proposal overstates the role of GPUVA for
ordinary single-adapter execution. Microsoft's physical-mode compute sample
uses runtime-associated physical contexts and runtime external synchronization.
The coordinated implementation now carries logical runtime queue identity
through the backend and its actual KMD submissions. Production-backed lifecycle
tests and Windows builds pass; target execution of the coordinated set is still
required. Ordinary D3D12 admission remains disabled.

## Evidence

Local `graphics-driver-samples` commit
`de4a2161991eda254013da6c18226f5ea06e4a9c` matches the current Microsoft sample.

- `compute-only-sample/cosumd12/CosUmd12CommandQueue.cpp`, under
  `!COS_GPUVA_SUPPORT`, calls `p12UMCallbacks->pfnCreateContextCb` with the
  actual `hRTCommandQueue`. It submits physical command/allocation/patch lists
  using that returned `hContext`, and destroys it using the runtime queue
  destroy callback.
- `CosUmd12CommandQueueDdi.cpp` explicitly states SignalFence/WaitForFence DDIs
  are only needed for MultiGPU/LDA. Its single-adapter implementations are empty.
- `CosUmd12Fence.h` stores the fence creation description; it does not fabricate
  a GPUVA-to-OS-handle reverse lookup or create a substitute application fence.
- `coskmd/CosKmdAdapter.cpp:1271` enables GpuMmu/virtual addressing only under
  `COS_GPUVA_SUPPORT`. Physical mode is a real alternative in this sample.
- The sample device uses R5. Its queue funcs `CORE_0001` and the relevant
  CreateContext/DestroyContext callback signatures are already present in R0
  `D3D12DDI_CORELAYER_DEVICECALLBACKS_0003`, as checked against the WDK header.
  This source evidence is not yet a VIOGPU ordinary-runtime acceptance result.

## Baseline mismatch addressed by this change

The original `native_heap_open_context` used the legacy device-scoped kernel callback
`pKTCallbacks->pfnCreateContextCb(hRTDevice, ...)`, once for the whole VkDevice.
`native_queue_create` remembered `hRTCommandQueue` but never associated it with
that actual scheduler context. `mwd_callbacks.submit/completed` received only a
device owner; there is no per-submission logical queue identity. Embedded
`ExecuteCommandLists` queues work asynchronously, so a temporary caller-thread
global cannot correctly route later submissions.

KMD baseline `bab3d2ffe87cdee06c9b255506525c8f21d720cf` (58554 source) retains
the same physical memory model as installed 58552. In `wddmddi.cpp:665` and
`:9845`, native allocations must match the submitting native context/domain.
Merely creating extra runtime contexts would either send work to a context that
does not own those allocations, or leave the runtime-associated context idle
while real GPU work goes elsewhere. Neither satisfies runtime fence ordering.

## Coordinated implementation and validated boundary

Root authorized isolated local KMD/Mesa worktrees from `bab3d2ff`/`a304f0ff`,
both on `work/d3d12-runtime-queues-20260919`. The existing VKD3D branch owns its
consumer changes. Root active worktrees and pins stay untouched.

The implementation separates a retained native allocation domain from
the scheduler contexts associated with each runtime command queue. The KMD uses
an exact 40-byte shared create packet with numeric domain ID and generation;
legacy 32-byte creation is unchanged. Child contexts retain the allocation
owner; owner destruction rejects while children exist. Each child owns its
scheduler state and cannot detach the owner's native registration.

VKD3D creates each child with the actual `hRTCommandQueue` runtime callback,
checks its domain identity, and routes each submitted packet and completion
event to the returned context. Separate retained records outlive runtime slots.
Embedded logical queues carry their identity even when they share one `VkQueue`.
The metadata survives asynchronous workers and split/fallback/wait/signal paths.
Mesa retains the token in its real common submission object, forbids merging
submissions carrying metadata and routes Turnip batches through the provider.

Execute drains software translation and kernel enqueue before returning. This
does not wait for GPU idle; it prevents a later runtime external fence packet
from overtaking the actual Render callback. Mesa also observes device loss while
waiting when a failed worker exits without the normal queue-pop notification.
Completion is a domain-wide FIFO watermark advanced by real context-ordered OS
events, not a private-fence query against an idle owner context. Resource imports
retain their shared domain; foreign device, reset and stale token requests reject.
Runtime external signal/wait packets operate on those scheduler contexts, while
embedded Vulkan fences remain an independent completion mechanism.

Private runtime ABI is now version **2**. The paired headers are byte-identical
with SHA256 `facd7a43c42b227b9bc9cc44d184b4801b3e5201168028fb9ae9700f8abd1999`.
Do not pair these artifacts with runtime-v1 Mesa or VKD3D.

- KMD `00d851e5`: retained domains; production lifetime and 500 create/close
  races, three semantic controls, Render/prepatch 54/54 and private ABI pass.
  Inherited Advanced Color fixture repair `23aef2df`, disposable Windows fixture
  cleanup `8b3d09b4`, and default-build guards `89f4061c` are separate commits.
  Contract and all 47 Advanced Color negatives pass locally. ARM64 driver run
  35450586472 passes default, Advanced Color and opt-in variants, all export and
  INF/PE gates, and publishes unsigned default/HDR artifacts with link maps.
  This is not a jointly signed installable package or target acceptance.
- Mesa `709ac5ef`: run 35450196385 passes regression, ARM64 SDK/WDK compilation
  and full Turnip linking. Production queue metadata/drain passes ASan/UBSan;
  reread-token, double-release and skip-drain semantic controls are caught.
- VKD3D `09c70fb`: run 35450195342 passes Linux real CPU Vulkan backend, Windows
  ARM64 build, x86/x64 execution and actual ARM64 lifecycle execution. Production
  logical-worker routing/split/drain tests and drop-route/shared-token/skip-drain
  controls pass. Actual DDI lifecycle tests verify two distinct runtime contexts,
  shared allocation, per-context Render/events, FIFO completion and teardown.
- VKD3D `25d1a6c`: run 35450875839 passes all five jobs again with the new
  controlled target probe below. Its ARM64, x86 and x64 binaries compile, the
  no-argument gate passes, and actual ARM64 lifecycle tests remain green. CI
  does not execute the real VIOGPU/KMT GPU probe on the target.

## Controlled target probe

`vkd3d-umd-shared-gpu-probe --luid-low HEX --luid-high HEX --run-runtime-queues`
requires the coordinated KMD and private-import Mesa set. It emulates the
Microsoft runtime callbacks while forwarding to real KMT; it does **not** call
ordinary Microsoft `D3D12CreateDevice` or validate runtime external fence policy.
It creates two native command queues and two actual shared-domain KMT contexts,
alternates eight changing buffer copies, verifies that Execute has enqueued the
target DMA before returning, checks Render and OS events use the selected queue,
then validates all 16384 readback words including untouched sentinel regions.
Each queue must destroy its own context before allocation-owner cleanup.

The existing backing probe's cleanup observation now checks identity callback
liveness, reflecting the new completion implementation. An identity query is
not a GPU-completion claim; real readback, OS events and production FIFO tests
are the completion evidence. Target execution is pending.

Admission must remain zero until ordinary runtime scheduling plus the mandatory
graphics, residency and Present contracts are implemented and validated. Current
CPU fixtures, cross-compilation and controlled KMT probes cannot satisfy that
acceptance condition on their own.
