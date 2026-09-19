# Bounded native D3D12 graphics

The native bridge can now translate VS/PS token streams and draw triangles into
one RGBA8 render target. Native render targets allocate once through the runtime
KMT callback, import that exact backing and create a placed image. The existing
WDDM import guards reject committed-resource and linear-staging fallbacks.
This slice preserves zero public DDI versions/caps/admission.

## Contracts and implementation

The actual WDK26100 R0 tables supply CreateVertexShader/CreatePixelShader with
SM5 token streams and register signatures, not application DXBC containers.
The signatures may contain unused registers. A bounded token-declaration scan
filters that superset, validates register masks and builtin identities, then
constructs checksummed ISGN/OSGN/SHEX chunks. This avoids falsely demanding
vertex attributes for unused runtime input entries. Unsupported stages,
declaration encodings, builtins and precision modes return an error.

Graphics PSO creation resolves live same-device VS, PS, blend, raster and depth
state handles, snapshots their bytes and retains the backend root before
releasing the callback lock. Backend calls can recursively destroy original
slots without invalidating those snapshots. Command callbacks similarly retain
backend owners and copy the runtime error target before entering Vulkan.

RTV handles resolve against live CPU-visible RTV heaps; resource handles must
resolve to live owned RGBA8 textures. Draw state covers triangle topology,
viewport, scissor, one RT, graphics root/PSO and optional16/32-bit index buffers.
Rejected replacements preserve existing backend state. Reset clears draw state.

References: local Microsoft `d3d12umddi.h`,
`PFND3D12DDI_CREATE_SHADER_0003`, `D3D12DDIARG_STAGE_IO_SIGNATURES`,
`PFND3D12DDI_CREATE_PIPELINE_STATE_0001`, `PFND3D12DDI_DRAW_INSTANCED`,
`PFND3D12DDI_DRAW_INDEXED_INSTANCED`, and the matching Microsoft DDI docs.

## Validation and boundaries

Local CPU Vulkan execution passes five rounds x1024 checked words: direct draw,
indexed draw with nonzero first index/base, zero instances, zero indices and a
VS-to-PS interpolated RGBA gradient. The gradient checks all four components
with at most one UNORM quantization step of tolerance.
Each verifies changing scissor pixels over a red clear plus readback padding and
sentinels. The shaders are reconstructed through the production helper, including
unused runtime signature entries. A separately compiled scissor-drop mutation
must fail at round0 word128 (white instead of red). The ID-generated vertex
shader does not test base-vertex attribute fetch because it has no attributes.

Actual-WDK fixtures cover wrong/stale/foreign shader/state/RTV handles, copied
shader data, unsupported PSO state, exact draw/index arguments, request poisoning,
recursive slot destruction, preserved runtime command-error target, and exact
native RT allocation/import/placement and alias ownership.

Tested implementation: `a823c15140f5e71580b0f47f76086da97118080f`.
GitHub Actions run `35453533088` passed all five jobs: Linux production backend
and semantic controls, ARM64/x64/x86 Windows builds, and actual Windows ARM64
runtime fixture execution. The ARM64 execution includes the native shared RT
allocation/import/placement and retained alias checks, native graphics callback
ownership/argument checks, and all six lifecycle semantic negative controls.
The five pixel rounds run through CPU Vulkan on Linux; Windows CI does not
execute the real-GPU target probe.

The ready ARM64 artifact is `vkd3d-native-ddi-arm64`, artifact ID `10586819734`
from that run. Its ZIP is 13,189,557 bytes. The runtime validation artifact is
`vkd3d-runtime-arm64-validation`, ID `10587541866`.

The target command is:

```
vkd3d-umd-shared-gpu-probe.exe --luid-low HEX --luid-high HEX --run-shared-graphics
```

It requires the shared-domain KMD and matching Mesa in-process runtime ABI-v2.
The provider is the registered Turnip Vulkan ICD, normally
`vulkan_freedreno.dll`, reached through System32 `vulkan-1.dll`; replacing only
`viogpud3d.dll` does not install it. See the exact source and ABI pairing in
[the integration receipt](vkd3d-runtime-v2-integration-20260920.md).
It creates native VS/PS/PSO/RTV,
draws into imported RT backing, requires target Render before Execute returns
and ordered OS completion on the associated queue context, then checks four
rounds x16384 readback words and last-import KMT allocation teardown. This
controlled emulated-runtime/real-GPU probe is not yet target-executed here.

Still missing: graphics descriptors/constants/root bindings, vertex input
layout, additional builtins, shader stages/formats, depth/stencil, blending,
multisampling, MRT, complete residency/fence/Present contracts, ordinary runtime
scheduling/teardown/TDR and feature-level acceptance. No system D3D12, DWM,
benchmark, display or target performance pass follows from this work.
