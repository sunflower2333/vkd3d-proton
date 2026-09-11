# Embedded native D3D12 DDI bridge

This target statically embeds vkd3d-proton in `viogpud3d12.dll`. It does not
import or replace the application `d3d12.dll` or export D3D12CreateDevice.
It exports the genuine WDK OpenAdapter12 entrypoint, but advertises no complete
DDI version or feature level yet and is not registered as the active D3D12 UMD.
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
L1, textures, primary/coherent-systemwide heaps and all resource creation remain
unsupported. Runtime-owned heap storage need not be pre-zeroed. Failed cleanup
keeps the allocation and VA reservation until device teardown; no AssumeNotInUse
flag or synthetic KMT handle is used. Nested CPU mappings share one kernel lock,
including ownership of handles renamed by LockCb. Synchronous device destruction
detaches callbacks safely and delegates residual kernel handles to runtime device
teardown, without stale callbacks or touching expired private slots.

This is a KMD heap owner, not a Vulkan resource-import or WDDM2 GPUVA mapping
implementation. Turnip currently owns a separate KMT device/context and disables
external-memory import on WDDM. A resource cannot use these heaps until that
ownership bridge is implemented. Therefore the R0 resource argument is rejected,
all native DDI versions stay unadvertised, and no system D3D12 acceptance is
claimed. The WDK fixture checks heap address alignment/exhaustion, runtime handle
identity, foreign-device rejection, nested/renamed/null-pointer maps, failed
context and allocation cleanup, mid-allocation reset and callback reentrancy.

Still required for a native system driver: complete negotiated feature levels;
full device/core and graphics DDIs; resource/heap import and placement,
residency and WDDM2 GPUVA mapping; remaining descriptor views;
monitored fences referring to the runtime's actual GPU backing; shared surfaces,
presentation, device-removal/TDR recovery and WDDM KMD integration. The backend
fences used by the test are not the runtime's monitored-fence contract.
Caller must keep resources, shaders and command allocators alive until submitted
work has retired and reset allocators only after completion, as required by D3D12.

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

Driver-parent packaging must build from `external/vkd3d-proton`, retain Mesa4ace
and KMD7648b72f or explicit validated successors, copy this candidate before PE
signing/catalog generation, preserve matching PDB identity, and record parent,
Mesa and vkd3d source hashes. A signed candidate still does not establish native
runtime acceptance; no active driver registration is changed by this milestone.
