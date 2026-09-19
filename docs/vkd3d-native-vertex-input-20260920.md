# Native vertex input checkpoint

This adds owned CORE_0003 input layouts and IASetVertexBuffers to the bounded
native graphics path. Ordinary D3D12 admission remains zero. Runtime ABI2 and
KMD wire ABI0 do not change; no target operation or integrated-package pin
change was performed by this worker.

Tested source: `8ce43838901713e8c048b7133e2bebe4da54480d`, independently
identifiable after query checkpoint `11ddefd` and its documentation `6bc61e2`.
Dedicated CI [35461834992](https://github.com/sunflower2333/vkd3d-proton/actions/runs/35461834992)
finished successfully in all five jobs.

The WDK contract supplies InputRegister, not application semantic strings.
Each layout register maps to the existing reconstructed shader ATTR/register
signature. Driver private size is sizeof(Object), with a separately owned copy
of at most 32 input elements. Registered layout handles and copies survive
recursive destruction while the embedded graphics pipeline is created.

The bounded format set is R32 scalar/vector FLOAT/UINT/SINT and
R8G8B8A8 UNORM/SNORM/UINT/SINT. Other formats are explicitly rejected. Slot and
register limits, duplicate registers, per-slot classification and divisor,
explicit/append offsets and format alignment are validated. Existing graphics
limits (triangle topology, one RGBA8 target, no blending/depth/MSAA) remain.

Vertex views resolve GPU addresses against the owning native resource registry.
The whole input batch is copied, validated and retained before backend entry;
late invalid entries cause no partial binding. Null arrays unbind the requested
slots. Stride 0 is supported. Backend code checks the actual COM device, range,
recording state and command type. Native generation checks and copied runtime
error identity protect reset and recursive device/resource/list retirement.

The embedded allocator now owns a deduplicated recording-owner list shared by
queries, query result buffers, vertex buffers and index buffers. Those real COM
references remain until legal allocator Reset or final allocator destruction;
submitted allocators are held by the existing queue timeline until genuine GPU
completion. Unbinding or releasing bridge wrappers does not drop these owners.

## Local validation

- Full production CPU Vulkan backend regression passes.
- Five real 1024-word pixel/sentinel rounds cover sparse slots 5/7, native input
  register reconstruction, view offsets 64/128, element offset 4 and append,
  strides 20/16/0, StartVertex/StartInstance/BaseVertex/StartIndex, instance
  divisor 2, null unbind and rejected update preservation.
- Real destruction notifications verify duplicate binds/unbinds, cancelled
  recording, legal allocator reset and queue submission behind an unsignalled
  GPU fence, including releasing command/allocator wrappers while pending.
- Separately compiled dropped-view-offset and dropped-owner binaries fail at
  the expected pixel mismatch and premature destruction assertions.

## Windows validation

The production DDI fixture adds layout ownership and copied request checks,
atomic native range/foreign-owner rejection, VirtualFree of caller views,
recursive slot destruction, generation change and DestroyDevice cases.
The independent field-order negative failed semantically as required on each
architecture, alongside all 11 earlier runtime negatives. ARM64/x64/x86 builds
and actual WDK execution all passed. ARM64 runtime job `105947888912` contains
both new vertex positive assertions and the new negative-control PASS token;
x86 job `105946970629` and x64 job `105946970583` contain the same evidence.
The CPU Vulkan job `105946970468` passed every earlier regression and both new
compiled vertex negatives.

| Artifact | ID | Archive bytes |
| --- | ---: | ---: |
| vkd3d-native-ddi-arm64 | 10590136016 | 13,298,108 |
| vkd3d-native-ddi-x64 | 10589094748 | 13,723,836 |
| vkd3d-native-ddi-x86 | 10590435869 | 13,521,434 |
| vkd3d-runtime-arm64-validation | 10590800083 | 13,469 |

GitHub ARM64 artifact archive SHA256:
`9a0fef3273a261911ff57c5850443c808f7d067906894023d767b480ec97981f`.
These are isolated bridge build/test artifacts; root owns the later full signed
driver package and target acceptance. No new target result is claimed here.

## Remaining ordinary admission blockers

Depth/stencil resources and views, blending and broader rasterization,
graphics descriptor bindings, broad resource placement/format/MRT/MSAA support,
other mandatory core/DDI slots and truthful capability reporting, shared
surfaces/Present, and ordinary Microsoft runtime synchronization/teardown/TDR
acceptance are still incomplete. Filling these input-assembler slots does not
establish FL11_0 or full graphics support.
