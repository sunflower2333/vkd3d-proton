# Bounded native texture import

This continuation of `3be413c` adds native runtime-owned 2D textures. The native
entry still advertises **zero DDI versions**. This is not ordinary Microsoft
D3D12CreateDevice, graphics or Present acceptance.

## Implementation

- One mip, layer and sample; R32_FLOAT or R8G8B8A8_UNORM; default non-RT/DS heap.
- Real GetResourceAllocationInfo validates 64-KiB placement and required bytes
  before AllocateCb. The runtime owns one allocation, IOVA and import token.
- The embedded heap imports that exact token. Placed images bind its VkMemory;
  imported heaps reject absent memory, incompatible memory type and separate
  linear staging storage. They never fall back to a committed allocation.
- Native texture SRV accepts exact format and default mapping. Handles resolve
  through the device-owned live registry. Textures expose no fabricated GPUVA.
- Native CopyTextureRegion accepts subresource zero or physical-pitched buffer
  footprints. Offset, pitch, extents, source box, destination position, format
  and ownership validate before recording. Virtual-pitched and same-resource
  copies remain unsupported.
- Caller descriptions are copied; native owners remain retained while backend
  calls run outside registry and callback locks, including reentrant retirement.
- No shared runtime ABI, KMD or Mesa change is introduced.

## Validation

Local Linux CPU Vulkan backend tests pass upload/image/image/readback copies
for both formats, nonzero 512-byte offset, 256-byte row pitch, a boxed copy,
and rejection that preserves the valid destination. Every one of 512 words
per format is checked, including untouched sentinel regions. Existing buffer,
descriptor, indirect and sampler checks also pass.

The independently built CPU-only negative executable deliberately drops the
source footprint offset. The actual Vulkan readback rejects the corrupted
result at `texture copy format41 word128`; production contains no negative
control branch. This verifies the test can detect a lost offset.

Windows WDK fixtures add heap-import cleanup/alias lifetime/request poisoning,
texture SRV/copy translation, negative signed coordinates and source/command-slot
poisoning. Candidate `9da2e23e62aeb3e6bc39372e4d0c5d3b1b7cfb46` passed all five
jobs in [CI35437025132](https://github.com/sunflower2333/vkd3d-proton/actions/runs/35437025132):
Linux backend, x86/x64/ARM64 build and separate ARM64 runtime. Logs confirm the
new native heap/import and SRV/copy tests ran successfully on all three Windows
architectures. All six prior runtime semantic negative controls also pass.
These controlled-peer fixtures do not establish VIOGPU hardware acceptance.

## Root-owned target probe

After root confirms the matching shared-runtime-v1 Mesa/KMD environment:

```text
vkd3d-umd-shared-gpu-probe.exe --luid-low HEX --luid-high HEX --run-shared-textures
```

The probe uses emulated runtime callbacks forwarding to real KMT. It calls
production native paired heap/resource, SRV, sampler/root and CopyTextureRegion
DDIs. Each format runs four changed-input rounds with GPU SampleLevel, native
texture-to-readback copy and all 16,384 mapped words checked. RenderCb must
reference the exact native texture allocation; importing a second alias must
create no new allocation. The final alias must retain/release backing after
heap/resource slots expire. Cleanup must leave zero native allocations.

The existing `--run-shared-backing` buffer probe remains available unchanged.
No target execution is claimed here. KMD WDDM2 allocation/paging failures are
not repaired or hidden by this engine change.

## References and deferred work

[CopyTextureRegion](https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion),
[placed resources](https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-createplacedresource),
[texture-copy locations](https://learn.microsoft.com/windows/win32/api/d3d12/ns-d3d12-d3d12_texture_copy_location).

Native GetResourceAllocationInfo/DDI capability reporting, multiple subresources,
render-target/depth views, graphics pipeline, public monitored fences, residency,
GPUVA and Present must still be implemented before widening native admission.
