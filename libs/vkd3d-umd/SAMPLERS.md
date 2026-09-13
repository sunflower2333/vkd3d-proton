# Native sampler checkpoint

The R0 bridge now implements `pfnCreateSampler` and static samplers in root
signatures. Dynamic sampler fields and static border enums are copied explicitly
from the real WDK layouts. Sampler descriptor handles must resolve to a live,
owned, CPU-visible sampler heap. The backend reference survives a reentrant
heap destruction; callback and resource-registry locks are released before
entering Vulkan. Root creation copies all source descriptions and suppresses
publication if the context retires during the backend call.

Backend validation covers standard/comparison point, linear and anisotropic
filters, address modes, bias/LOD and static binding conflicts. Unsupported
minimum/maximum reduction modes fail explicitly. The bridge retains zero native
advertised versions; this is not ordinary Microsoft `D3D12CreateDevice` admission.

The existing `vkd3d-umd-gpu-probe.exe --adapter LOW HIGH VENDOR DEVICE` gains nine
independent sampled-output rounds, with every 1024-word readback checked against
its fresh expected output and untouched sentinels. It uses the normal Vulkan
loader and exact selected Turnip identity. Dynamic point/linear, wrap/mirror/
clamp/custom-border/mirror-once, copied descriptors and static border/linear
samplers are exercised. Invalid replacement descriptions target the descriptor
subsequently consumed by the GPU, proving they preserve valid existing state.

The bounded single-mip texture upload/SRV helpers use real committed embedded
backend resources. They do not broaden native CreateHeapAndResource texture
admission, which remains buffer-only, and they do not prove native shared
surfaces, Present or system-runtime graphics. Runtime-v1 and Mesa sources are
unchanged. Root owns the forward integration onto current Mesa and target tests.

The controlled Windows DDI fixture uses actual production callbacks with fake
backend peers. It verifies field conversion, invalid handle rejection,
reference balance, failure propagation and source/heap poisoning during unlocked
backend callbacks. Its retirement test marks the actual context retired, and
does not claim a real Microsoft teardown guarantee. All three process ABIs run
the descriptor fixture in CI. The Linux CPU Vulkan test runs real sampled
work; a separate CPU-only binary deliberately drops AddressU translation and
must fail the wrap round's exact output oracle. Neither is target GPU proof.

Official contracts:
- https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_create_sampler
- https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/ns-d3d12umddi-d3d12ddi_static_sampler
- https://learn.microsoft.com/windows/win32/api/d3d12/ns-d3d12-d3d12_sampler_desc
