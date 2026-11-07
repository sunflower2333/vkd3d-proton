# Original AIO64 DX12 depth initialization

The unchanged desktop `AIO-Graphics-Test-64bit.exe` (SHA256 `a6b100a9732dc3ac5e10be337e69b38ca6520815b62951cb59c3839e12085246`) now initializes and renders its embedded Direct3D 12 page through the current driver. The depth-resource flag correction was built, installed into the active cached x64 UMD path, and exercised by the original interactive application. The earlier D32 failure and all failed receipts are retained below.

## Actual failure

The original application trace creates the device, DIRECT queue, RTV and DSV heaps, and the RGBA8 render target. It checks D32 format support and then cleans up, before any depth allocation reaches the backend.

A diagnostic extraction using the same 844 by 573 dimensions and original descriptors records `CreateCommittedResource(D32_FLOAT, DEPTH_WRITE, clear depth 1)` returning `0x887A0005` with a null resource. All preceding creation calls succeed. The helper submits no rendering commands, signals no queue, waits on no GPU fence, and returns from cleanup.

CDB captures the actual UMD callback parameters:

| Resource | DDI flags | Allocation size | Alignment |
| --- | --- | --- | --- |
| RGBA8 render target | `0x11` | 2097152 | 65536 |
| D32 depth resource | `0x12` | 0 | 0 |

`D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE` is `0x10`. The production `native_heap_desc` accepts that bit for color resources but recognizes depth only when the complete flags value equals `DEPTH_STENCIL` (`0x02`). A valid `DEPTH_STENCIL | SHADER_RESOURCE` request therefore takes the color-validation branch and fails. Windows then receives zero allocation metadata and the API device enters an error state.

The KMD reset/request counters, loaded module, disk image, boot, and DWM/Explorer lifetimes remain unchanged. The complete Health55 check after the diagnostic passes all thirteen current identity observations, ten samples, and preservation of the original 64 files (63 original paths plus the separately retained original UMD).

## Actual correction and validation

The local review `.planning/continuation56-aio-runtime-20261107-01/root-d32-first-failure01.json` binds the original application trace, helper build, first HRESULT, actual callback trace, passive observations and complete post-health receipt. Raw device images and logs remain on the device; the local receipts record remote shell observations.

The correction normalizes `SHADER_RESOURCE` before classifying and validating color or depth resources. Unsupported flag combinations and resource shapes still fail. The production file `vkd3d-umd-port/libs/vkd3d-umd/native_heap.inc` is byte-identical to the actually compiled candidate source (`a7f04ca5...`). Existing unrelated worktree changes are preserved.

Candidate `e545993fec03077a145562c07848ed4d4a106bb73aadc0ea07b59e5fa6c56ba0` (6,369,280 bytes) was applied exactly once to the existing DriverStore `x64/viogpud3d12.dll`. The original `e6f0518a...` core is separately backed up. The active ARM64X frontend, native ARM64 core, and both Vulkan ICDs retain their previous bytes. The operation preserved the target file identity, registry state, all four other active files, and all 64 original retained contents.

After forcing 165 Hz, the unchanged x64 initialization diagnostic completed all 26 stages: 24 successful HRESULT results and two void calls. The former D32 creation now returns `S_OK` and a resource; shader compilation, pipeline creation, buffers, command list and fence also succeed. Its process exited normally without submitting commands, signaling or waiting on the GPU.

The canonical application was then opened from its original desktop path in the existing interactive session. A recorded click selected the Direct3D 12 page. Its process loaded the current cached x64 UMD and paired x64 ICD. Two device-local captures identify Direct3D 12 / VKD3D / Turnip and contain no unavailable message. A fixed central scene region changed at 24,827 of 50,000 sampled pixels between the captures, with a changing saturated object. Images remain on the device; no visual review or complete pixel-shape oracle is claimed. Twenty-two process observations contain both x64 modules; the last follows the DX12 selection by 112.71452 seconds. The original process exited with code 0 after an owned close request, without forced termination or a deadline, and both owned tasks were removed.

Full Health56 checks before initialization, after initialization, and after the canonical AIO run each pass 13 current identity queries, ten samples, and 70 retained/current file locations. KMD 58652, LUID `00000000:00007137`, generation 2, module/boot/desktop lifetimes and original file contents remain stable; reset and reset-request counters remain zero. Final metadata rehashes the original desktop executable and installed candidate. Refresh was forced to 165 Hz before both tests and confirmed again afterward.

The root receipt `.planning/continuation56-aio-runtime-20261107-01/root-aio56-candidate-runtime-review01.json` (`38e0c1ae...`) binds the actual build/deployment, initialization, interactive AIO, scene observations, cleanup and health evidence. Independent final review `.planning/continuation56-aio-health-20261107-01/bounded-aio56-final-peer-review01.json` (`10a3ef9e...`) passes without blocking findings; the root separately rehashed and joined both in `root-and-peer-final-closure01.json`. The parsed pretty deployment copy is preserved separately; the actual compact target receipt is `deployment56-actual-bytes02.json` (`d12bedd9...`, 113,999 bytes), independently verified against the remote file. The first Health56 attempt rejected the pretty-copy byte pin before health sampling; no deployment was replayed.

This closes the reported original AIO64 embedded-DX12 initialization failure and demonstrates a bounded animated scene on the current driver. It does not establish full DX12 conformance, long stability, native ARM64 depth-fix deployment, MSAA coverage, continuous 165 Hz cadence, or zero-copy acceptance. The log still contains format `0x59` multisample-query fixme messages. Further work should target those remaining gaps rather than repeat this unchanged AIO case.
