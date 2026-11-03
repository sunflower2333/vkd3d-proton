# Native command-list runtime error ownership

Native command creation previously discarded `hRTCommandList`. Every recording
error reached the device's `SetErrorCb`, so an invalid recording could poison the
entire device while the actual command-list runtime never received its error.

The native path now retains the original opaque command-list runtime handle and
requires the copied `pfnSetCommandListErrorCb`. Errors from command recording,
including backend Close failure, reach that exact handle without changing the
device error for ordinary failures. Device removal/reset/hang/internal-driver
failure additionally reaches the device callback if the device is still live.
Successful commands and sibling command lists produce no error callback.

Native creation rejects missing runtime ownership before backend work. It copies
runtime input before calling the backend, retains Context and drops its callback
lock across backend creation/destruction. If creation reenters DestroyDevice,
the unpublishable backend object is released without touching expired runtime
input or private storage. Command destruction invalidates its private slot before
calling the backend and retains Context until backend callbacks return.

Error reporting copies the runtime handle and retains Context across a callback
that may synchronously destroy both command list and device. It never reads the
command slot after the callback. The barrier loop stops at the first failed
backend transition so it cannot continue reading a command destroyed by that
error callback. Legacy internal bridge error reporting remains unchanged.

The exact WDK contracts are `D3D12DDIARG_CREATE_COMMAND_LIST_0001` and
`PFND3D12DDI_SETCOMMANDLISTERROR_CB`, with the callback stored in
[`D3D12DDI_CORELAYER_DEVICECALLBACKS_0003`](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/ns-d3d12umddi-d3d12ddi_corelayer_devicecallbacks_0003).
The local WDK26100 header confirms the callback takes the runtime command-list
handle, not the driver command handle or runtime device handle. The local
Microsoft DDI documentation contains the same callback member.

The production-entry fixture creates two native lists with distinct runtime
handles, verifies validation and backend errors reach the correct list, and
checks genuine device loss reaches both owners. It mutates the caller's callback
table/request, rejects missing ownership and failed creation with untouched
storage, recursively destroys and poisons command/device storage inside the
error callback, and cancels creation while overwriting runtime arguments. Backend
worker callbacks during command creation/destruction must remain unblocked.
The negative-control invocation deliberately clears a list's runtime identity
and must fail specifically because the error escaped to the device owner.
All cases use production native entry/DDIs and controlled backend peers; they
are not a Microsoft-runtime or GPU acceptance test.

The prior4bcc712 cleanup candidate remains frozen with its independently
confirmed8round realGPU/cleanup pass. This change does not alter its Mesa/KMD
protocol. Native admission still advertises zero versions and rejects caps and
partial table negotiation. It does not register partial command tables, provide
OS-backed monitored fences, graphics, residency, presentation, or full native
D3D12CreateDevice support. In particular, native fences receive OS GPU value and
monitored-value placements, so an embedded backend fence is not a substitute.
