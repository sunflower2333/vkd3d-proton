# Native runtime-v2 integration receipt

This is a build/test handoff, not target acceptance. Ordinary native D3D12 DDI
admission remains zero. The parent owns the performance trunk, joint signed
package and all target operations.

## Exact sources

| Component | Tested source | Build evidence |
| --- | --- | --- |
| VKD3D bridge and graphics probes | `a823c15140f5e71580b0f47f76086da97118080f` | Run `35453533088`, all five jobs passed |
| Mesa runtime provider | `709ac5ef875a50c1793d5741eec5025b43c88d2c` | Run `35450196385`, regression, ARM64 transport and complete Turnip link passed |
| Isolated KMD domain implementation | `89f4061cee819b1a54dd1413b06d741ee3b21c83` | Run `35450586472`, default/Advanced Color/opt-in builds and export/INF/PE gates passed |

From installed 58556 KMD `4463fba5`, only these KMD commits need importing:

- `00d851e56bdee54dd6cd65729d5548fefefe2a7f`: retained shared allocation domains,
  independent scheduler contexts and production domain tests.
- `8b3d09b4e08cee37a1f310bf7e35b19ec017e155`: bounded Windows fixture executable
  cleanup retry.

The parent already imported them as `57cdfeab` and `10316c1c` in
`work/runtime-v2-integration-20260920`, on 58556 plus destroy-preflight repair
`0b8c297f`, and reserved 58557 for the coordinated package. Existing Advanced
Color fixture/build repairs are already present in that baseline; do not
reapply the whole isolated KMD branch.

Pin Mesa `709ac5ef` including all seven commits after `a304f0ff`:
`97c2f0e9`, `16b9e809`, `ccaf9ddb`, `69cdca52`, `cf73653c`, `1405c576`,
`709ac5ef`. The fixture repairs preserve the prior residency/status tests.

## Installed provider identity

`libs/vkd3d-umd/runtime.inc` loads System32 `vulkan-1.dll` and obtains
`vkGetInstanceProcAddr`. `libs/vkd3d/device.c` queries `MWD_STYPE_SUPPORT` through
`vkGetPhysicalDeviceProperties2`; it requires exact magic, version, callback
structure size and capability flags before passing runtime callbacks to
`vkCreateDevice`.

Mesa `src/freedreno/vulkan/tu_device.cc` supplies that reply and accepts the
runtime callback owner. `tu_knl_wddm.cc` implements shared allocation imports
and per-submit retained queue routes. These compile into the registered Turnip
ICD, normally `vulkan_freedreno.dll`, rather than the D3D11 frontend
`viogpud3d.dll`. Consequently, changing only the D3D `external/mesa` source pin
while reusing a frozen GL/Vulkan artifact is insufficient.

The prior frozen GL/Vulkan source `c84e3d16` is an ancestor of `709ac5ef`; all
its source changes are retained. The parent dispatched the GL/Vulkan workflow
at `709ac5ef` to produce matching architecture-specific installed ICDs. This
receipt does not claim that separate packaging workflow or target install has
already passed. Installed 58556, without these KMD and ICD additions, is not
runtime-v2-ready.

## ABI checks

In-process runtime ABI version is 2. The following headers are byte-identical:

- Mesa `src/freedreno/vulkan/mesa_wddm_runtime.h`.
- VKD3D `include/private/mesa_wddm_runtime.h`.

SHA256: `facd7a43c42b227b9bc9cc44d184b4801b3e5201168028fb9ae9700f8abd1999`.

The KMD wire ABI stays version 0. The original context request stays 32 bytes;
the separate shared-context request is 40 bytes, appending
`AllocationContextId` and `Reserved`. The updated shared header SHA256 is
`78995b4923702492f64eda7d9412e98459b26ad87afd61d17fca81557c7a0a38`.
The `expected-pre-v1.txt` manifest SHA256 is
`399353fd93d73f5af6588a9123c9b910dcc2a096be406c40f309becebacf2070`.

## Ready probes

Run `35453533088` artifact `vkd3d-native-ddi-arm64`, ID `10586819734`, contains:

```
vkd3d-umd-shared-gpu-probe.exe --luid-low HEX --luid-high HEX --run-runtime-queues
vkd3d-umd-shared-gpu-probe.exe --luid-low HEX --luid-high HEX --run-shared-graphics
```

These have an internal 90-second watchdog. They use controlled emulated runtime
callbacks with real KMT/GPU execution, exact allocation backing, runtime queue
association, software-enqueue-before-Execute-return ordering and ordered OS
completion. The graphics case additionally checks four rounds of 16,384
pixel/sentinel words and final resource/heap/alias allocation retirement.
They do not establish ordinary Microsoft `D3D12CreateDevice` admission.

The backend-only probe can run before runtime-v2 integration:

```
vkd3d-umd-gpu-probe.exe --adapter LUID_LOW_HEX LUID_HIGH_HEX VENDOR_HEX DEVICE_HEX
```

It uses the exact selected Vulkan adapter and tests the embedded backend,
including five graphics pixel rounds. It does not establish native runtime
allocation imports or queue association. The parent supplies the process
deadline and owns all target execution. No probe in this receipt has been
executed on the target by this worker.
