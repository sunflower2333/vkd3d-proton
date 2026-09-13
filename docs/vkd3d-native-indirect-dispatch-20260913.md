# Native indirect compute dispatch

The R0 device table now provides command-signature size/create/destroy and the
0003 command-list table provides ExecuteIndirect. Supported signatures contain
exactly one DISPATCH record. ByteStride is at least12 and DWORD-aligned; padding
between records is preserved. A dispatch-only signature has no root signature
association. Draw, graphics/root-update and mixed signatures are rejected.

R0 buffer placements contain UMD resource handles and offsets. Both required
argument and optional count buffers resolve in the same Context's live registry;
an unknown nonnull count handle cannot silently become an absent count buffer.
The backend validates recording state, command-list type, device ownership,
DWORD alignment and overflow-safe argument/count extents before recording. A
zero maximum records no GPU operation. A nonzero maximum uses the embedded
ID3D12GraphicsCommandList ExecuteIndirect, including GPU count clamping.
Neither buffer's contents are mapped or interpreted by the CPU.

Signature construction copies input fields before entering the backend and
does not publish to runtime-private memory if the device retires during that
call. Execution holds separate references to command, signature and both
buffers, releases callback/registry locks across backend work, and reports
failure using copied command-list ownership. Runtime private storage may be
destroyed and poisoned reentrantly without invalidating the retained peers.

## Validation

The Linux backend test executes the production vkd3d engine on CPU Vulkan. Five
independently initialized1024-word rounds use padded records at byte64 and GPU
count at byte20. Argument/count data reaches DEFAULT memory through GPU copies.
Counts0/1/2, count9 clamped to maximum2, and no count with maximum3 produce
distinct0/17/41/41/73-word prefixes; every remaining sentinel is checked. Invalid
foreign/wrong-type/closed/copy-list/misaligned/overflow inputs precede the valid
command so unintended partial recording is observable.

The Windows fixture invokes the actual WDK callbacks with controlled backend
peers. It checks unsupported signatures, copied descriptors, untouched failed
construction, arbitrary/foreign-shaped/stale handles, exact GPU count and buffer
offset forwarding, backend errors, callback-lock release, and retirement with
poisoned private memory. Its deliberate dropped-count negative control must fail
for the exact semantic reason. Existing UAV/queue/completion tests remain enabled.
Windows builds check x86 stdcall and actual ARM64/x64/x86 layouts; the ARM64 CI
runner executes the same fixture natively.

These are source/backend/WDK fixture tests. Real Turnip indirect dispatch and the
Microsoft system D3D12 runtime require separate target verification. Runtime
GetSupportedVersions remains0; monitored-fence backing, WDDM2 GPUVA/residency,
graphics/Present and complete feature-level contracts remain admission gates.

## Microsoft contracts

- [ExecuteIndirect](https://learn.microsoft.com/windows/win32/direct3d12/indirect-drawing)
- [Command signature DDI](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/ns-d3d12umddi-d3d12ddiarg_create_command_signature_0001)
- [ExecuteIndirect DDI](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_execute_indirect)

The local Windows Kit26100 d3d12umddi.h is authoritative for the compiled ABI.
