# Embedded native D3D12 DDI bridge

This target statically embeds vkd3d-proton in `viogpud3d12.dll`. It does not
import or replace the application `d3d12.dll`, export D3D12CreateDevice, register
OpenAdapter12, or claim a complete native Windows driver. The engine revision
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
range checks. Root UAV and CBV addresses resolve against the bridge's owned buffer
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
other typed formats, textures, SRVs/samplers and ranged descriptor copies
still require their native view/copy adapters.

Constant buffers support native CreateConstantBufferView and compute root
CBV callbacks, including 256-byte alignment, a 64-KiB descriptor-size bound,
buffer ranges and same-device ownership. Heap CBVs with a zero BufferLocation
create real null descriptors. Root CBVs instead require a live owned buffer
address: root descriptors have no descriptor bounds/null-read guarantee.
Unknown or zero root addresses reach the error callback before backend state
changes. See the Microsoft [root descriptor contract](https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html#root-descriptors).

Still required for a native system driver: OpenAdapter12 and version/caps
negotiation; full device/core and graphics DDIs; runtime allocation, heap,
residency and GPUVA mapping; remaining descriptor views and ranged copies;
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
round and checks zero-root rejection. Three-architecture CBV CI is pending.

`vkd3d-umd-gpu-probe --adapter LUID_LOW_HEX LUID_HIGH_HEX VENDOR_HEX DEVICE_HEX`
executes the same compute/readback workloads through the production backend.
Its build excludes the CPU test-device entrypoint. Supply the actual OS adapter
LUID and matching Vulkan IDs; device selection independently requires all of
them and Mesa Turnip's driver ID. Missing/malformed identity fails before Vulkan
loading. This test diagnoses real hardware backend integration without changing
system registration. It is not a Windows runtime DDI/Present acceptance test;
The earlier09146e1 two-workload checkpoint passed on the real ARM64 VIOGPU
in1005ms with process-local Mesa56bd30c and the exact matched OS/Vulkan identity.
Target execution of the new CBV extension remains pending.

Driver-parent packaging must build from `external/vkd3d-proton`, retain Mesa4ace
and KMD7648b72f or explicit validated successors, copy this candidate before PE
signing/catalog generation, preserve matching PDB identity, and record parent,
Mesa and vkd3d source hashes. A signed candidate still does not establish native
runtime acceptance; no active driver registration is changed by this milestone.
