# Native unordered-access barrier ordering

The native command table previously rejected every UAV barrier with
`E_NOTIMPL`. Consecutive draws or dispatches that require earlier UAV accesses
to finish before later accesses could not express that ordering.

`pfnResourceBarrier` now translates both complete buffer transitions and UAV
barriers through a validated batch into the embedded backend's actual
`ID3D12GraphicsCommandList::ResourceBarrier`. A live owned resource maps to
that resource; a null UAV handle maps to the global UAV barrier. An unknown,
stale, wrong-type or foreign-device non-null handle is an error, never a null
global barrier and never an unchecked pointer dereference.

The native bridge resolves the complete batch while retaining the resource
registry lock. The backend validates all entries and prepares its complete
array before one command recording call. A rejected late element cannot record
an earlier transition. Small batches use stack storage. Larger batches are
bounded at 65,536 entries and report allocation failure before recording.
The resource lock is released before reporting any error; the runtime may
synchronously destroy the command/device and overwrite their private storage
inside its command error callback.

UAV barriers require a recording direct/compute list and, when a resource is
specified, a buffer created with unordered-access support. Copy lists and
invalid kinds/foreign resources are rejected by the backend. Split barriers,
alias/ranged barriers and texture subresources remain unsupported; this change
does not expand feature-level or version admission.

The exact native layout is the WDK's
`D3D12DDIARG_RESOURCE_BARRIER_0003` and its `D3D12DDI_RESOURCE_UAV_BARRIER`
union member. Microsoft's local DDI enum documentation describes completion
of all prior UAV reads/writes; the
[public UAV barrier remarks](https://learn.microsoft.com/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_uav_barrier)
explicitly permit a null resource for all UAV accesses.

The production-entry WDK fixture records a mixed transition/resource-UAV/global
batch on an actual native-created command list, including exact resource and
runtime error ownership. It rejects late invalid/stale/foreign/wrong-type
resources without backend recording, checks small and allocated batches, and
injects a backend error whose runtime callback destroys and poisons command and
device storage. A semantic negative control deliberately loses the global UAV
barrier in the controlled peer and must fail the exact ordering oracle.

The real backend test runs four dependent compute dispatches per format. Each
dispatch reads 1,024 input words, counts prior nonzero values and then writes
all input words. Two resource barriers or one global barrier order successive
dispatches. Two independently initialized rounds compare all 2,048 input and
feedback/sentinel words after completion. Rejected backend batches include
late foreign resources, buffers without UAV support, wrong object kinds and
invalid barrier types, followed by a valid copy/readback. A naturally serial
CPU Vulkan implementation may produce correct pixels even when a barrier is
omitted, so readback alone is not a deterministic missing-barrier oracle; the
native recorded-call negative control supplies that distinct evidence.

No Mesa/KMD/private protocol change is needed. The engine and UMD must be
built together from this revision; driver-parent packaging must pin it before
the usual complete signed package build. Root owns any device execution.
Ordinary Microsoft D3D12CreateDevice still requires the closed native version,
capability and complete table contracts, OS-backed monitored fences, residency
and GPUVA, remaining resource/graphics DDIs and presentation. This command
implementation and its controlled tests do not establish native activation.
