# Ordinary D3D12 runtime queues in physical mode

The earlier direct OS fence-import proposal overstates the role of GPUVA for
ordinary single-adapter execution. Microsoft's physical-mode compute sample
uses runtime-associated physical contexts and runtime external synchronization.
The genuine missing dependency in this implementation is carrying logical
runtime queue identity through the backend and its actual KMD submissions.

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

## Current concrete mismatch

VKD3D's `native_heap_open_context` uses the legacy device-scoped kernel callback
`pKTCallbacks->pfnCreateContextCb(hRTDevice, ...)`, once for the whole VkDevice.
`native_queue_create` remembers `hRTCommandQueue` but never associates it with
that actual scheduler context. `mwd_callbacks.submit/completed` receive only a
device owner; there is no per-submission logical queue identity. Embedded
`ExecuteCommandLists` queues work asynchronously, so a temporary caller-thread
global cannot correctly route later submissions.

KMD baseline `bab3d2ffe87cdee06c9b255506525c8f21d720cf` (58554 source) retains
the same physical memory model as installed 58552. In `wddmddi.cpp:665` and
`:9845`, native allocations must match the submitting native context/domain.
Merely creating extra runtime contexts would either send work to a context that
does not own those allocations, or leave the runtime-associated context idle
while real GPU work goes elsewhere. Neither satisfies runtime fence ordering.

## Coordinated implementation under development

Root authorized isolated local KMD/Mesa worktrees from `bab3d2ff`/`a304f0ff`,
both on `work/d3d12-runtime-queues-20260919`. The existing VKD3D branch owns its
consumer changes. Root active worktrees and pins stay untouched.

The bounded implementation separates a retained native allocation domain from
the scheduler contexts associated with each runtime command queue. Queue-aware
callbacks must route each actual submitted packet and completion to the same
context registered with that runtime queue. Resource imports retain their shared
domain; cross-device, reset-generation and stale-domain requests must reject.
Runtime external signal/wait packets operate on those scheduler contexts, while
embedded Vulkan fences remain an independent completion mechanism.

Required validation includes actual production paths for two independent queue
identities, per-queue ordering/completion, shared allocations, rejected foreign
domains, partial construction rollback, reset, pending destruction and precisely
balanced lifetime. A dropped queue identity or routing to the allocation-owner
context must be detected by negative controls. Do not open ordinary admission
until these paths and mandatory graphics/residency/Present contracts work.
