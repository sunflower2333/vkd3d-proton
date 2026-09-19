# Embedded native D3D12 DDI bridge

This target statically embeds vkd3d-proton in `viogpud3d12.dll`. It does not
import or replace the application `d3d12.dll` or export D3D12CreateDevice.
It exports the genuine WDK OpenAdapter12 entrypoint, but advertises no complete
DDI version or feature level yet and is not registered as the active D3D12 UMD.
The bounded texture continuation supports single-layer/mip/sample R32_FLOAT
and RGBA8 non-render-target images backed by the native runtime's allocation.
It adds native texture SRVs and physical-pitched CopyTextureRegion, validates
actual embedded image allocation requirements, and rejects separate committed
or staging-memory fallbacks. It does not advertise native D3D12 admission,
depth views, residency or Present. See
`docs/vkd3d-native-textures-20260919.md` for exact tests and limits.

The bounded graphics continuation adds native SM5.0 vertex/pixel shaders,
one RGBA8 render target, raster/blend/depth state handles, viewport/scissor,
RTV clear/bind, and direct/indexed instanced triangle draws. Shader signatures
are rebuilt from the native register contract and actual token declarations;
unused entries in a runtime signature do not become required vertex attributes.
Render targets use the same retained native allocation domain with no committed
fallback. Depth, blending, input layouts, graphics descriptor bindings, MRT,
multisampling and full feature-level/runtime acceptance remain unfinished.
The controlled `--run-shared-graphics` probe requires runtime-v2 Mesa/KMD pairing
and validates native DDI pixels, exact allocation references and runtime-context
OS completion. It uses emulated runtime callbacks; it is not an ordinary
`D3D12CreateDevice` test. See `docs/vkd3d-native-graphics-20260919.md`.

The single-node fence continuation owns copied native fence descriptions and
implements CreateFence/DestroyFence plus SignalFence/WaitForFence broadcast
selection. Windows runtime owns and emits the actual external synchronization
on the associated physical queue context. These DDIs reject foreign/stale
fences and preserve callback ownership during recursive device retirement;
they neither create replacement Vulkan fences nor interpret GPU virtual
placements as CPU addresses. This remains a partial contract with ordinary
admission disabled. See `docs/vkd3d-native-fences-20260920.md` for evidence and
the explicit runtime/target acceptance boundary.

The native heap residency continuation forwards MakeResident/Evict through the
runtime's device and opaque paging queue, using existing owned allocation
handles. E_PENDING preserves the complete paging fence and WaitMask; callbacks
cannot rename, submit or free the borrowed handles. Device retirement suppresses
later output writes. Only native heap objects are supported in this slice;
descriptor/query backing, physical paging and ordinary runtime acceptance remain
unproven. See `docs/vkd3d-native-residency-20260920.md`.

The engine revision
and every submodule are pinned by the driver parent repository.

Implemented backend operations create a device on the exact supplied Vulkan
LUID/vendor/device/Turnip driver, then create buffers, map/unmap, command queues,
allocators/lists, root signatures, compute pipelines, copy and transition
buffers, dispatch, signal/wait fences, and read completion/device-loss status.
Native SM5 compute token streams are wrapped into a checksummed DXBC container
for the translation engine. No system D3D device creation or nested swapchain
is used. Invalid adapter identity never falls back to another adapter.

The Windows bridge assigns real WDK callback types for queue/allocator/list
creation and destruction, command close/reset/copy/transition/dispatch/execute,
root signatures, compute shaders and pipelines. Private objects preserve their
device context; errors from void DDIs reach the supplied error callback. Buffer
copy placements use `BaseAddress.UMD.hResource` and `Offset`, with overflow-safe
range checks. Root UAV, CBV and SRV addresses resolve against the bridge's owned buffer
registry; this does not implement runtime GPUVA allocation. Unsupported table fields remain null; these partial tables must
not yet be advertised to the Windows runtime.

The descriptor continuation adds real descriptor heap creation/destruction,
native CPU/GPU handle and stride queries, buffer UAV creation, simple descriptor
copy, descriptor heap binding and compute root descriptor tables. WDK shader
visibility flags are explicitly converted to the embedded API's different
bit value. CPU descriptor handles are resolved against live heaps belonging to
the device; shader table handles must resolve into the currently bound heap.
Root-table ranges preserve register spaces, explicit offsets and APPEND, with
overflow-safe heap bounds. Command reset clears table and heap binding state.
Buffer UAVs support raw, structured (including counters) and R32 typed views;
other typed formats, textures and samplers still require their native view adapters.

UAV creation resolves both data and optional counter handles in the locked live
buffer registry, matching SRV ownership rules. Unknown non-null handles never
get dereferenced or converted to null descriptors. Rejected late creation leaves
the existing descriptor unchanged; actual WDK fixtures exercise invalid pointers,
wrong object types, removed registry entries and valid counter forwarding.

Native CopyDescriptors now resolves independently sized source/destination
ranges across live owned heaps, including omitted size arrays (one descriptor
per range), empty ranges and repeated source ranges. The backend validates all
ranges, matching total counts, heap type, source visibility, device ownership
and cross-range source/destination overlap before copying. It walks the two
flattened streams in contiguous chunks without requiring their boundaries to
match. A rejected late range cannot partially replace an earlier destination.
This follows the Microsoft [copying descriptor contract](https://learn.microsoft.com/windows/win32/direct3d12/copying-descriptors).

Constant buffers support native CreateConstantBufferView and compute root
CBV callbacks, including 256-byte alignment, a 64-KiB descriptor-size bound,
buffer ranges and same-device ownership. Heap CBVs with a zero BufferLocation
create real null descriptors. Root CBVs instead require a live owned buffer
address: root descriptors have no descriptor bounds/null-read guarantee.
Unknown or zero root addresses reach the error callback before backend state
changes. See the Microsoft [root descriptor contract](https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html#root-descriptors).

Native SetComputeRoot32BitConstant and SetComputeRoot32BitConstants now map
to the embedded command list. The bound root layout retains each constants
slot's DWORD count; updates validate the slot type, range, recording state
and source pointer before changing any value. Partial updates preserve all
other constants, and caller storage is copied during command recording.
Root creation enforces the total64-DWORD budget, including table/root-descriptor
costs. Command reset clears the bound layout so stale constants cannot be set
without rebinding. This follows Microsoft's [root constants contract](https://learn.microsoft.com/windows/win32/direct3d12/using-constants-directly-in-the-root-signature).

Root-constant checkpointbc61de9 passed all four standalone CI34609416636
jobs and parent targetseven-vkd3dbc-constants-05:17workloads/17408correct
words in1167ms, including both root32 partial-update rounds. Exact native
adapter LUID581B/ICD9AA5/KMD58386, original DWM2140/Explorer5972 retained.
Parent owns correlated host trace closure. This does not prove Microsoft
runtime activation, graphics or Present support.

Buffer SRVs support the native CreateShaderResourceView and compute root SRV
callbacks, raw/structured/R32 typed views, same-device ownership, bounded
element ranges and valid null table descriptors. The DDI resolves resource
handles from its live registry before reading them; unknown non-null handles
are errors, never converted to null. Root SRVs require aligned owned addresses.
Non-default buffer component mappings return E_NOTIMPL because the embedded
engine currently implements swizzles only for textures. This limitation must
be resolved before advertising complete shader-resource support.

Descriptor reuse tests exposed stale buffer-range metadata when replacing a
live SRV with a null descriptor. The engine now clears auxiliary ranges and
preserves their copy flag, so direct and copied null descriptors cannot retain
the previous buffer's dimensions. This is exercised by shader GetDimensions
and readback after first seeding the same source/destination slots with a live
view, rather than testing only initially zeroed descriptor heaps.

The native entry retains the original runtime adapter callback and validates the
paired KMD's v0 prefix, VLID trailer and reset generation. Native CreateDevice
uses runtime-owned private memory, separately retains the shared backend Context,
copies the original device callbacks, and uses the system Vulkan loader for the
process architecture. Selection requires exactly one Turnip physical device with
the exact KMD LUID; guest PCI IDs are not assumed to equal host Vulkan IDs.
Destruction detaches runtime callbacks before releasing the Context and unloads
Vulkan only after the final backend reference is destroyed. Failure never deletes
runtime-owned storage or publishes an incompletely initialized private slot.

GetSupportedVersions deliberately returns zero entries, GetCaps refuses unsupported
contracts, and FillDDITable refuses partial tables. This is real adapter/device
lifecycle code with a WDK fixture, not successful Microsoft D3D12CreateDevice
activation. The fixture does not enable an alternate production admission path.

The next native slice implements R0 buffer-only heaps through the runtime's real
CreateContext/Escape/Allocate/Lock/Unlock/Deallocate callbacks. It preserves the
original adapter/device/resource handles, validates the v0 KMD context's reset
generation and GPUVA range, and reserves non-overlapping 64-KiB heap ranges.
Only L0 buffers with CPU access unavailable or WRITE_BACK are accepted; current
KMD native allocations must fit its 32-bit allocation-size field. WRITE_COMBINE,
L1, textures and primary/coherent-systemwide heaps remain unsupported. Paired
buffer resources accept offset0, row-major/unknown-format, one mip/sample and
COMMON/COPY_SOURCE/COPY_DEST; WRITE_BACK paired resources require COPY_DEST.
Nonzero ReuseBufferGPUVA and other placement semantics are rejected.
Runtime-owned heap storage need not be pre-zeroed. Failed cleanup
keeps the allocation and VA reservation until device teardown; no AssumeNotInUse
flag or synthetic KMT handle is used. Each non-null runtime resource belongs to
one heap; deallocation releases both its kernel resource and allocation. Null
runtime resources create device-owned allocations released by exact handle.
Nested CPU mappings share one kernel lock,
including ownership of handles renamed by LockCb. Synchronous device destruction
detaches callbacks safely and delegates residual kernel handles to runtime device
teardown, without stale callbacks or touching expired private slots.

The private shared-runtime protocol now initializes matching Turnip against the
same actual native context and allocator before its internal BO creation. A
native heap token is imported into Vulkan without a second allocation, and its
placed buffer GPUVA must match the heap's IOVA. Separate heap/resource/alias
owners retain the actual backing. Direct KMT Vulkan clients keep their existing
path. Public Win32 external memory and WDDM2 GPUVA mapping remain unsupported.
All native DDI versions stay unadvertised, and no system D3D12 acceptance is
claimed. The WDK fixture checks heap address alignment/exhaustion, runtime handle
identity, foreign-device rejection, nested/renamed/null-pointer maps, failed
context and allocation cleanup, mid-allocation reset and callback reentrancy.

Backend calls release all recursive callback lock levels and retain Context
metadata. Terminal callbacks cannot resurrect a zero reference count. Device
retirement skips driver unlock/deallocate/context-close during active backend
windows, and import cancellation is checked before placement starts. This does
not guarantee backing survival after the real Microsoft runtime's final KMT
teardown; its destruction serialization and worker quiescence remain admission
gates. Controlled retirement fixtures do not access a map after DestroyDevice.

Still required for a native system driver: complete negotiated feature levels;
full device/core and graphics DDIs; remaining resource/heap types and placement,
residency and remaining descriptor views; the runtime's actual synchronization
contract; shared surfaces, presentation and device-removal/TDR recovery. The
runtime-associated physical contexts implemented here are a valid single-adapter
direction; GPUVA/application-fence import is not a universal requirement for
that mode. The backend fences used by the tests do not validate the Microsoft
runtime's external scheduling and synchronization. WDDM2 GPUVA remains absent
and must be implemented if a negotiated mode or feature actually requires it.
Caller must keep resources, shaders and command allocators alive until submitted
work has retired and reset allocators only after completion, as required by D3D12.

Native ResourceBarrier now supports complete buffer transitions and both
resource-specific and global UAV ordering. Opaque resource handles resolve
through the live owned registry, and the complete native/backend batch is
validated before any barrier is recorded. UAV barriers reject copy lists and
buffers without unordered-access support. Two dependent four-dispatch
readbacks and a WDK recorded-call negative control cover this boundary; see
`docs/vkd3d-native-uav-barriers-20260912.md`. Split, alias/ranged and texture
barriers remain unsupported. Native version admission remains closed.
R0 default-buffer placement now keeps UAV-capable embedded usage and accepts
initial UAV state; readback buffers remain COPY_DEST-only without UAV usage.
No later-version native flag is treated as an R0 resource flag.

Native command signatures now support one DISPATCH argument with a DWORD-aligned
stride of at least12bytes. ExecuteIndirect resolves owned argument/count buffers,
preserves nonzero offsets and forwards an optional GPU count buffer to the real
embedded command list. Count clamping happens on GPU. There is no CPU readback
of indirect arguments. Signatures modifying root arguments and draw signatures
remain unsupported. Error reporting and backend references survive reentrant
runtime object/device retirement. See
`docs/vkd3d-native-indirect-dispatch-20260913.md` for verification and boundaries.

The CPU-Vulkan test compiles and executes a real SM5 compute shader, compares all
1024 readbacks after copy/barrier operations, checks fence signal/wait, timeout,
allocator reuse, and invalid shader/range/state propagation. Its CPU-only device
entrypoint is built separately and absent from production. This verifies backend
semantics, not VIOGPU acceleration, Windows-runtime loading or Display+Render.
Windows x86/x64 ABI tests load the actual DLL, check guarded WDK table sizes,
stdcall callbacks, exact exports and invalid-handle errors without GPU activity.
ARM64 initially receives compile/link/PE validation until a separate runtime test.

The descriptor backend test executes a second independent compute dispatch,
reinitializing the output before dispatch and checking all 1024 words. It uses
nonzero descriptor-table and range offsets, staging-to-visible descriptor copy,
and rejects wrong-device heaps, invisible heaps, misaligned/out-of-range handles,
invalid root indices and stale post-reset bindings. Local CPU Vulkan passes
both the original root UAV and new descriptor-table readbacks (2048 words total).
Windows three-architecture compile/ABI CI34593634076 passed at7e50d05;
the complete paired parent CI34594763298 also passed at9e42361.

The CBV continuation adds four independently reset 1024-word readbacks:
root CBV at a nonzero buffer offset, copied table CBV at different buffer
and heap offsets, a null table CBV, then valid root rebinding after reset.
Rejected zero root bindings must preserve the preceding valid binding.
Local CPU Vulkan passes all six workloads (6144 words). The original test
incorrectly dispatched root address zero and faulted reading address4 in the
CPU shader; the core placed the caller in round3/fence4 after the first three
CBV readbacks succeeded. The updated test uses a valid allocation for that
round and checks zero-root rejection. Three-architecture CBV CI34598879880
and paired parent f2bd055/CI34599103361 passed.

The SRV continuation adds eight independently reset 1024-word readbacks:
typed tables and null replacement, preservation after rejected swizzle,
raw tables/root offsets with two independent bindings, and structured
tables/root/null views. All fourteen workloads (14336 words) pass local CPU
Vulkan. This includes the null-range repair; its first negative run returned
the old size64 instead of0. The b2c510d checkpoint passed all four standalone
WDK/CPU jobs and all seven paired743acf9 jobs, then all fourteen target GPU
workloads in1015ms with14336 correct readbacks. Parent's matched host trace
reported no lost events/faults and retired18/18 submissions for that context;
original DWM/Explorer retained and postcheck3/3 passed. These are backend,
callback and backing results, not native Microsoft runtime acceptance.

The ranged-copy continuation adds a fifth CBV workload, gathering from two
storage heaps and scattering across different destination boundaries. It then
tries invalid late ranges, foreign heaps, visible sources, overlap and count
mismatches before executing the retained valid descriptor. All1024 words must
still match that valid CBV. Local CPU Vulkan passes fifteen workloads/15360
words. Standalone0447a76 CI34606533835 and paired72cef21 CI34607682895 both
passed all jobs. Parent targetseven-vkd3d044-ranges-03 passed in1039ms with
all15360words correct, retained DWM2088/Explorer5820 and correlated host trace
coverage. Native runtime and Present remain unproven.

The root-constants continuation adds two independent1024-word readbacks with
a single HLSL uint4 cbuffer. Disjoint bit lanes verify bulk updates, partial
offset updates and preservation when rebinding the same root signature. The
caller arrays are overwritten immediately after recording, proving commands
do not retain their addresses. Rejected late ranges and null sources must
preserve the valid result; each round resets and initializes the full output.
Local CPU Vulkan passes all17workloads/17408words. Native Windows callback
validation and target root-constant execution are pending.

`vkd3d-umd-gpu-probe --adapter LUID_LOW_HEX LUID_HIGH_HEX VENDOR_HEX DEVICE_HEX`
executes the same compute/readback workloads through the production backend.
Its build excludes the CPU test-device entrypoint. Supply the actual OS adapter
LUID and matching Vulkan IDs; device selection independently requires all of
them and Mesa Turnip's driver ID. Missing/malformed identity fails before Vulkan
loading. This test diagnoses real hardware backend integration without changing
system registration. It is not a Windows runtime DDI/Present acceptance test;
The earlier09146e1 two-workload checkpoint passed on the real ARM64 VIOGPU
in1005ms with process-local Mesa56bd30c and the exact matched OS/Vulkan identity.
The84d6bba CBV checkpoint also passed the ARM64 target in806ms with6144
correct GPU readbacks; this does not validate the later SRV extension.

`vkd3d-umd-shared-gpu-probe --luid-low HEX --luid-high HEX --run-shared-backing`
tests the newer shared runtime allocation path. It embeds the production native
entry/backend and emulates only the Microsoft runtime callbacks, forwarding
allocations, maps, escapes and RenderCb to real D3DKMT. It requires the exact
VIOGPU LUID and a matching private-import Turnip selected through the system
Vulkan loader (for example with a process-local VK_DRIVER_FILES manifest).
An old Mesa without the exact protocol is rejected; there is no CPU fallback.

The probe verifies duplicate import creates no additional allocation, actual
native allocation identity in RenderCb, and8 changing copies of1024 words into
a native READBACK buffer with15360 untouched sentinel words per round. The
result is read through native MapHeap. Native heap, resource and Vulkan alias
destruction is checked, and final KMT cleanup must succeed. Each GPU fence wait
is bounded by10s; an overall90s deadline terminates a hung probe process.
Exit0 means all probe checks passed,1 failure,2 missing/invalid explicit identity,
and124 timeout. CI compiles the probe and checks its no-argument gate; target
GPU execution remains a separate required test. This is emulated-runtime plus
actual KMT/GPU proof only, never ordinary Microsoft D3D12CreateDevice acceptance.

The `--run-runtime-queues` mode adds two native command queues with separate
runtime-associated KMT scheduling contexts sharing the allocation domain.
Eight alternating GPU copies must appear on the selected context before Execute
returns, and completion events must use that context. Readback checks all16384
words per round; both child contexts must close while the allocation owner stays
live. This is still emulated-runtime plus real KMT/GPU, not ordinary D3D12.

This queue implementation requires runtime ABI **2** on both VKD3D and Mesa.
The coordinated source set is Mesa709ac5ef plus KMD00d851e5 (retained domains)
and its build/fixture repairs through89f4061c, or validated successors. Older
runtime-v1 artifacts are incompatible. The paired protocol headers have SHA256
`facd7a43c42b227b9bc9cc44d184b4801b3e5201168028fb9ae9700f8abd1999`.
See `docs/vkd3d-runtime-queue-association-20260919.md` for build evidence and
remaining target/runtime acceptance gates.

Driver-parent packaging must build from `external/vkd3d-proton`, retain the
coordinated Mesa/KMD interface set above, copy this candidate before PE
signing/catalog generation, preserve matching PDB identity, and record parent,
Mesa and vkd3d source hashes. A signed candidate still does not establish native
runtime acceptance; no active driver registration is changed by this milestone.
