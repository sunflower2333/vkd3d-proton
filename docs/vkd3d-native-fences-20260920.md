# Single-node native D3D12 fence lifecycle

This slice fills CalcPrivateFenceSize, CreateFence, DestroyFence and the queue
SignalFence/WaitForFence slots for the existing physical single-node model.
It keeps ordinary D3D12 admission disabled. It adds no Mesa or KMD ABI and does
not modify the separate runtime-v2 package or tested graphics implementation
`a823c15`.

## Runtime contract and references

WDK 10.0.26100.0 `d3d12umddi.h` defines `D3D12DDIARG_CREATE_FENCE` as a count
and array of `D3D12DDI_FENCE` descriptions. The descriptions contain GPU virtual
placements and flags, but no CPU pointer or OS synchronization handle. The same
header declares `D3D12DDIARG_FENCE_OPERATION::PhysicalAdapterMask` as an output:
the set of adapters to broadcast the operation to. Its input fence handle and
64-bit value remain runtime-owned.

Microsoft's [CreateFence DDI documentation](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_createfence)
and [DestroyFence DDI documentation](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_destroyfence)
give the actual entrypoint signatures. The matching files are in the local
`windows-driver-docs-ddi/wdk-ddi-src/content/d3d12umddi` checkout.

Microsoft's graphics-driver-samples commit
`de4a2161991eda254013da6c18226f5ea06e4a9c` supplies the physical-mode evidence:

- `compute-only-sample/cosumd12/CosUmd12CommandQueue.cpp`, compiled when
  `!COS_GPUVA_SUPPORT`, associates its scheduler context with
  `pfnCreateContextCb(hRTCommandQueue)`, NodeOrdinal 0 and EngineAffinity 1.
  Render callbacks use that returned context.
- `CosUmd12CommandQueueDdi.cpp` says SignalFence/WaitForFence DDIs are only
  needed for MultiGPU/LDA and leaves their sample implementations empty.
- `CosUmd12Fence.h` stores creation metadata without creating another fence.

The output mask of 1 follows from this implementation's sole physical node 0
and the WDK output-mask definition. It is our explicit single-node policy;
the sample does not itself assign that output. Runtime external synchronization
on the associated context remains distinct from this metadata DDI and from
embedded Vulkan completion. The output mask alone is not proof of actual OS
signal/wait behavior.

## Implementation

CreateFence accepts exactly one description and recognized flags. It snapshots
the description, including opaque placements, into an independent device-owned
record. Runtime private storage is initialized only after validation; rejected
requests preserve it. No creation request pointer is retained, no placement is
dereferenced, and no KMT/Vulkan substitute application fence is created.

SignalFence/WaitForFence validate the live queue, its actual runtime-associated
context and a fence owned by the same device. They return mask 1 without
modifying the value, imposing monotonicity or waiting for GPU completion.
Stale/foreign fences, incomplete contexts, device loss and recursive calls from
an unfinished Render callback reject. These paths clear the output before the
retained device's SetErrorCb; that callback may destroy/overwrite the queue,
fence, device and request without a later access to their runtime storage.

DestroyFence removes only the owned metadata. Device retirement detaches any
remaining records without touching expired runtime slots or pinning the backend
forever. The existing Execute wrapper drains software translation and actual
Render enqueue before returning, so a subsequent runtime external fence packet
cannot overtake that work. This is not a GPU-idle wait.

## Validation boundary

Actual-WDK fixture coverage is added for description ownership, software-only
zero placements, uninitialized/guarded output storage, invalid counts/flags,
opaque 64-bit values and rewinds, foreign/stale fence rejection, callback owner
identity, request unmapping during error reporting, recursive retirement and
absence of substitute allocation/Render/signal calls. Deliberate owner-check
and output-mask mutations must fail semantically.

The production logical-worker regression also extracts the actual
`vkdu_queue_execute` wrapper. A delayed software worker must have enqueued
before the simulated runtime can append its external fence packet. No GPU
completion is provided. A separate skip-Execute-drain mutation must fail; the
existing routing, split and worker-drain negative controls remain required.

At this checkpoint local worker baseline and both drain controls pass. Actual
Windows ARM64/x64/x86 compile and fixture execution are pending. No target
ordinary-runtime fence, D3D12CreateDevice, presentation or scheduling acceptance
is claimed. Full feature-level, graphics, residency and Present prerequisites
continue to gate admission.
