# Native command queue operation ownership

The R0 native queue path now preserves `hRTCommandQueue` and snapshots creation
arguments before entering the backend. It validates the supported single-node
3D/compute/copy queue flags, rejects missing runtime identity and duplicate live
slots, and cancels construction if a callback retires the device. An unpublished
backend queue is released without touching retired runtime storage.

Native-created queue and command-list slots are registered on their owning
Context. `ExecuteCommandLists` resolves the complete bounded batch against that
registry; foreign, wrong-kind, stale and unknown non-null command handles never
reach the backend or get dereferenced. Rejected batches release any temporary
references without submitting a prefix. Backend validation continues enforcing
closed lists, matching devices and queue types.

The internal backend wrapper now supports temporary ownership. Execution retains
the queue and every resolved list before releasing the native callback lock.
Backend execution and final wrapper cleanup hold no nested callback locks. A
runtime callback can retire queue/list/device and overwrite private slots while
the submitted wrapper references remain alive. Queue/list destruction removes
registry membership and clears the runtime slot before entering backend cleanup.
Returning from execution uses only the retained Context for error delivery;
retired device callbacks remain detached. No command-list error callback is used
for a queue submission error.

This is operation lifetime through the synchronous backend call. It does not
replace GPU-completion/fence ownership or the runtime's synchronization contract
for initial valid handles. It does not implement OS monitored fences, residency,
GPUVA, graphics, presentation or complete Microsoft D3D12 device activation.
`GetSupportedVersions` still returns zero, feature caps remain unsupported, and
partial table negotiation stays closed.

Microsoft contracts consulted in the local WDK 10.0.26100.0 and documentation:

- [CreateCommandQueue R0](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_createcommandqueue_0001)
- [ExecuteCommandLists](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_executecommandlists)
- [DestroyCommandQueue](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_destroycommandqueue)

R0 queue creation contains the driver/runtime handles, QueueFlags and NodeMask;
priority belongs to later revisions. `D3D12DDIARG_CREATE_FENCE` contains an array
of OS FenceValue and FenceMonitoredValue placements. Existing embedded backend
fences cannot substitute for those placements.

The production native entry fixture exercises copied creation inputs, cancellation,
whole-batch rejection, the 64-element boundary, reentrant queue/list destruction,
private memory overwrite, error ownership and full device retirement. Its semantic
negative disables temporary wrapper acquisition only in the critical execution
scenario and must detect a submitted backend owner being released too early.
Existing lifecycle, command-error and UAV negatives remain required.

The separate real backend workload retains queue/list wrappers, releases their
original owners, then records and executes a copy and verifies 1024 words through
a fence/readback. CPU Vulkan and controlled WDK callbacks are source/runtime
fixture validation, not target GPU acceleration or system D3D12 acceptance.
