# Native context completion ownership

This descendant of09fb9be adds WDDM context-ordered CPU event completion to the
production native Render callback path. It keeps every referenced KMD allocation
alive until the OS scheduler reports completion. Synchronous queue-wrapper
references alone do not cover that interval.

## Microsoft contract

The actual selected WDK types are D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 and
D3DDDICB_SIGNALFLAGS, invoked through the copied D3DDDI_DEVICECALLBACKS table.
The completion packet follows RenderCb on exactly its hContext and runtime
device. Flags.EnqueueCpuEvent=1, SignalAtSubmission=0, ObjectCount=0, no broadcast
contexts, and a noninheritable manual-reset event identify completion rather
than submission. Local Microsoft DDI sources used:

- d3dumddi/nc-d3dumddi-pfnd3dddi_signalsynchronizationobject2cb.md
- d3dumddi/ns-d3dumddi-_d3dddicb_signalsynchronizationobject2.md
- d3dukmdt/ns-d3dukmdt-_d3dddicb_signalflags.md

These callbacks are available to WDDM1.2+ drivers and do not require advertising
GPUVA or MMU support. Both the current physical1.2 path and the independent
WDDM2 physical candidate6f11fd4 have scheduler-completion plumbing. Hardware
acceptance of the new event path must still be established on target.

## Ownership

Submission snapshots commands/references before callbacks. A bounded64-entry
FIFO acquires allocation owners before RenderCb, then preserves them across
backend release and reentrant runtime callbacks. Polling completion/status
retires only the completed prefix. An unavailable event callback rejects work
before Render; a full FIFO returns WAS_STILL_DRAWING before accepting more work.

The private KMD completion counter never advances beyond OS event retirement;
while events are pending, queries preserve the last safely reported value.
Failed Render or event registration keeps conservative pending ownership.
Registration is retried on the same context; this may delay release but never
move completion earlier. Failed resource cleanup retains the final owner for
retry. LockCb cannot rename an allocation still referenced by pending DMA.
No worker waits while holding the native callback lock: event polls use zero
timeout. Recursive completion polls are benign; nested submissions during event
retirement are rejected. DestroyDevice detaches callbacks and leaves pending
KMT backing to the runtime's device teardown; final local cleanup closes only
our event handles and metadata. Accepted kernel event references are independent.

The explicit shared-backing GPU probe forwards this callback to actual
D3DKMTSignalSynchronizationObject2. Probe execution and deployment remain the
parent's responsibility. No Mesa ABI, KMD advertisement or host change is needed.

## Boundaries

This is a driver-owned OS completion event, not an application D3D12 monitored
fence. WDK D3D12DDI_FENCE provides GPU virtual placements only, no CPU mapping or
KMT synchronization handle. The D3D12-specific sync callbacks are Downlevel0054
extensions, not R0 core callbacks. WDDM2 candidate6f11fd4 explicitly leaves
VirtualAddressingSupported/GpuMmuSupported/IoMmuSupported zero. GPUVA/MMU,
application CreateFence/SignalFence/WaitForFence, residency and full graphics
remain prerequisites. GetSupportedVersions remains zero; partial tables and
feature-level admission remain closed.

## Validation

The native WDK production fixture uses real Windows event objects and controlled
kernel peers to test delayed completion, Render/event ordering, temporary
request ownership,64-entry backpressure, cleanup failure/retry, failed Render,
failed event registration, callback reentry and device retirement. The semantic
negative forces a premature completion observation and must fail on early DMA
resource retirement. Multiarch/native ARM64 CI executes the same production
code. This validates ownership and DDI call semantics, not dxgkrnl/Turnip or
ordinary D3D12CreateDevice execution.
