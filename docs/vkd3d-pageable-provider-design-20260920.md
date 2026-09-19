# Optional pageable backing provider

Root authorized full GPU-backed descriptor/query residency development after
the bounded native-heap checkpoint40f7da3. Work is isolated from58557 and the
runtime-v2 provider709ac5ef. Mesa branch work/d3d12-pageable-provider-20260920
is based on709ac5ef in a separate workspace worktree. Source implementation is
complete; consumer Windows validation is pending. No device or ordinary native
D3D12 admission result is claimed.

The existing runtime ABI2 and KMD wire ABI0 remain unchanged. A separate
version1 optional private physical-device properties query advertises a
pageable-backing acquire function. Old providers return no function and GPU
backing enumeration rejects explicitly. The function receives the actual Vulkan
device, expected runtime owner/reset generation, object type and opaque object
handle; outputs contain either one retained native allocation or an explicit
zero-backing result.

Each VkDeviceMemory, VkDescriptorPool and VkQueryPool registers its exact BO in
the owning Turnip device. Lookup compares the opaque type/handle in that device
registry before dereferencing. Foreign/stale objects reject. Under the provider
lock, acquire retains the BO then drops the lock before runtime callbacks.
It retains the native allocation token, checks owner/generation and rechecks
device status before publishing. Errors unwind all acquired references. Object
destruction unregisters before releasing BO ownership; a callback may retire the
original object without invalidating the retained backing.

Embedded VKD3D enumerates the actual backing objects: descriptor-buffer memory,
auxiliary memory and descriptor pool; query result memory and query pool. The
wrapper retains backend owners while extracting, deduplicates allocation tokens
and adopts them into the native residency batch. Runtime handles never cross
the Vulkan boundary. Descriptor CPU visibility alone is not a zero-backing
proof. Lookup/callback ownership, stale handles, reset during acquire, duplicate
backing and rollback all require production-backed tests and Windows WDK CI.

Implementation limits: at most64 pageable objects and192 distinct allocations
per native request, at most three backing allocations per embedded object. The
whole request array is copied before any provider callback. Backend objects
remain retained through paging callbacks. Cleanup drops residency exclusions
before backend destructors while independent native token references still pin
metadata; direct native MapHeap owners keep their busy eviction guard. ICD
persistent mappings are not silently removed; runtime eviction failures are
returned unchanged. Query heap creation supports occlusion, timestamp, pipeline
statistics and stream-output statistics, with generation checks before and
after backend construction. BeginQuery/EndQuery/ResolveQuery remain absent.

Validation so far:
- Mesa `ecfb2b2e66edfd2577ad2de672d18f2aca89fda7`, CI35457230230:
  all three jobs pass, including real ARM64 Turnip SDK/WDK compile and DLL link,
  production provider sanitizer tests and three semantic negative controls.
  ICD bundle artifact10589020320,3214351 bytes, archive SHA256
  `72d54f121808ced1140226ef705003b3b715d1aa0a3d775963ed482ed7401bbb`.
- Both optional headers match SHA256
  `82ae2fa0f8a57e76541b7f1f7b1f52f3d49de386b2c860cb0bef427451584000`.
- Embedded collector ASan/UBSan passes actual production enumeration/dedup,
  missing provider, zero backing and failure rollback. Deliberate descriptor
  pool omission, query pool omission and missing dedup all fail semantically.
- Native WDK fixtures cover CPU-visible descriptors with GPU backing, multiple
  tokens, shared backing, E_PENDING, real Evict dispatch/rejection, stale and
  foreign ownership, poisoned caller arrays, unmapped request storage during
  device retirement, and query constructor reset/cancellation. Their Windows
  ARM64/x64/x86 execution remains pending until the paired consumer CI passes.

These two source revisions are isolated follow-ons, not substitutes for the
root's target-tested graphicsa823c15 or58557 deployment candidate. Runtime-v2
and KMD wire ABI0 have not changed. Ordinary public native admission stays0.
