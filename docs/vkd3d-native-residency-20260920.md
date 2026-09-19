# Native heap residency callbacks

This bounded slice adds MakeResident and Evict to the native D3D12 table. It
translates owned native heaps to their existing runtime allocation handles;
there is no separate Vulkan allocation, paging queue or CPU fence wait. Ordinary
D3D12 admission remains zero. Descriptor/query heap backing enumeration and
complete residency acceptance remain separate prerequisites.

## Runtime contract

WDK 10.0.26100.0 d3d12umddi.h defines the R0 MakeResident callback with the
runtime device, opaque runtime paging queue and D3DDDI_MAKERESIDENT packet.
The opaque queue is passed intact to the runtime; the kernel hPagingQueue field
is zero. Microsoft's graphics-driver-samples at
de4a2161991eda254013da6c18226f5ea06e4a9c implements the same boundary in
compute-only-sample/cosumd12/CosUmd12DeviceDDI.cpp, lines 787-986.

The local Microsoft documentation for
[D3DDDI_MAKERESIDENT](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_makeresident)
defines NumAllocations as an input/output count, and E_PENDING as an accepted
request with a paging fence. The native callback returns that HRESULT unchanged,
copies the complete 64-bit fence and sets WaitMask=1. S_OK clears the unused
outputs. Rejected calls preserve outputs. A short accepted count or unexpected
positive callback HRESULT is an internal driver error, never whole-batch
success. E_OUTOFMEMORY is propagated without an implicit trim/retry loop.
MustSucceed is accepted only together with CantTrimFurther, as documented in
[D3DDDI_MAKERESIDENT_FLAGS](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_makeresident_flags).

Evict passes the actual allocation list and EvictOnlyIfNecessary flag through
the runtime callback. NotWrittenTo is rejected because this implementation has
not opted into that behavior. Empty batches are local no-ops. This initial
implementation accepts at most 64 unique native heap handles per call and one
physical adapter. Other pageable object types fail explicitly; filling a table
slot does not make native admission complete.

## Ownership and validation

Every object is looked up in the owning device's registry before dereferencing.
The entire request is validated before a paging/eviction callback. A retained
batch prevents allocation rename, submission and cleanup while its copied
allocation list is borrowed by the runtime callback. Eviction rejects mapped
or outstanding submitted heaps. Duplicate, foreign, stale, pending and
unsupported objects reject without forwarding a partial request.

DestroyHeap may detach runtime storage during the callback; a separate retained
record keeps the actual allocation alive until callback return. Cleanup failure
retains an orphaned allocation for later device teardown. Device retirement
inside the operation or cleanup suppresses all output publication and further
runtime calls. This requires no Mesa/KMD ABI change.

## Validation boundary

The actual-WDK fixture exercises copied requests, callback identity, opaque
paging queues, synchronous and pending completion, zero/high-bit fence values,
flags, failure propagation, foreign/stale/duplicate object rejection, rename
and submission exclusion, nested callbacks, device/request unmapping and failed
cleanup retention. Two deliberate mutations misclassify pending completion or
omit eviction; the wrapper requires specific semantic failures.

Windows ARM64/x64/x86 compile and runtime CI validation are pending for this
checkpoint. These fixtures emulate the runtime callbacks and do not prove an
ordinary D3D12 application, physical paging, Present or target acceptance.
