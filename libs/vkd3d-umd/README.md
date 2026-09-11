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
range checks. Unsupported table fields remain null; these partial tables must
not yet be advertised to the Windows runtime.

Still required for a native system driver: OpenAdapter12 and version/caps
negotiation; full device/core and graphics DDIs; runtime allocation, heap,
residency and GPUVA mapping; descriptor tables and root GPUVA resolution;
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

Driver-parent packaging must build from `external/vkd3d-proton`, retain Mesa4ace
and KMD7648b72f or explicit validated successors, copy this candidate before PE
signing/catalog generation, preserve matching PDB identity, and record parent,
Mesa and vkd3d source hashes. A signed candidate still does not establish native
runtime acceptance; no active driver registration is changed by this milestone.
