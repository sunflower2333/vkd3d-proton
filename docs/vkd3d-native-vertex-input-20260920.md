# Native vertex input checkpoint

This adds owned CORE_0003 input layouts and IASetVertexBuffers to the bounded
native graphics path. Ordinary D3D12 admission remains zero. Runtime ABI2 and
KMD wire ABI0 do not change; no target operation or integrated-package pin
change was performed by this worker.

The WDK contract supplies InputRegister, not application semantic strings.
Each layout register maps to the existing reconstructed shader ATTR/register
signature. Driver private size is sizeof(Object), with a separately owned copy
of at most32 input elements. Registered layout handles and copies survive
recursive destruction while the embedded graphics pipeline is created.

The bounded format set is R32 scalar/vector FLOAT/UINT/SINT and
R8G8B8A8 UNORM/SNORM/UINT/SINT. Other formats are explicitly rejected. Slot and
register limits, duplicate registers, per-slot classification and divisor,
explicit/append offsets and format alignment are validated. Existing graphics
limits (triangle topology, one RGBA8 target, no blending/depth/MSAA) remain.

Vertex views resolve GPU addresses against the owning native resource registry.
The whole input batch is copied, validated and retained before backend entry;
late invalid entries cause no partial binding. Null arrays unbind the requested
slots. Stride0 is supported. Backend code checks the actual COM device, range,
recording state and command type. Native generation checks and copied runtime
error identity protect reset and recursive device/resource/list retirement.

The embedded allocator now owns a deduplicated recording-owner list shared by
queries, query result buffers, vertex buffers and index buffers. Those real COM
references remain until legal allocator Reset or final allocator destruction;
submitted allocators are held by the existing queue timeline until genuine GPU
completion. Unbinding or releasing bridge wrappers does not drop these owners.

## Local validation

- Full production CPU Vulkan backend regression passes.
- Five real1024-word pixel/sentinel rounds cover sparse slots5/7, native input
  register reconstruction, view offsets64/128, element offset4 and append,
  strides20/16/0, StartVertex/StartInstance/BaseVertex/StartIndex, instance
  divisor2, null unbind and rejected update preservation.
- Real destruction notifications verify duplicate binds/unbinds, cancelled
  recording, legal allocator reset and queue submission behind an unsignalled
  GPU fence, including releasing command/allocator wrappers while pending.
- Separately compiled dropped-view-offset and dropped-owner binaries fail at
  the expected pixel mismatch and premature destruction assertions.

## Windows validation

The production DDI fixture adds layout ownership and copied request checks,
atomic native range/foreign-owner rejection, VirtualFree of caller views,
recursive slot destruction, generation change and DestroyDevice cases.
An independent field-order negative must fail on each architecture.
ARM64/x64/x86 compilation and actual WDK execution are pending dedicated CI.

## Remaining ordinary admission blockers

Depth/stencil resources and views, blending and broader rasterization,
graphics descriptor bindings, broad resource placement/format/MRT/MSAA support,
other mandatory core/DDI slots and truthful capability reporting, shared
surfaces/Present, and ordinary Microsoft runtime synchronization/teardown/TDR
acceptance are still incomplete. Filling these input-assembler slots does not
establish FL11_0 or full graphics support.
