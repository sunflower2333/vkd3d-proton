# Native query execution

Implementation `11ddefdcdc2c0bcf174715ae44d315dacca8fbe8` fills the
CORE_0003 BeginQuery, EndQuery and ResolveQueryData slots. It records genuine
embedded Vulkan-backed queries and GPU result copies. Ordinary native D3D12
admission remains zero; this does not establish full D3D12 or Present support.

The actual WDK CORE_0003 Begin/End callback places query type before index.
The older unsuffixed callback reverses those fields. The Windows fixture calls
the actual CORE_0003 function table with distinct values and requires a mutation
that swaps them to fail.

Native entrypoints resolve query and destination handles in the owning device's
registry before dereferencing them, retain backend owners around unlocked
Vulkan calls, and preserve command-list error identity if a callback destroys
and frees runtime storage. They recheck the reset generation after the call;
device retirement suppresses further callbacks into detached runtime state.

The backend rejects wrong device/type/index/range, timestamp Begin, unmatched
or duplicate scopes, resolving an active range, and closing an active scope.
Supported heap types are occlusion, timestamp, pipeline statistics and stream
output statistics; copy-queue timestamp heaps remain unsupported. Results use
the matching 8-byte, pipeline-statistics or SO-statistics layout, with aligned
offset and overflow-safe destination bounds.

Recording retains the actual query heap and result-buffer COM owners on the
embedded command allocator. Queue submissions already retain this allocator
until their real timeline completion callback. Legal allocator reset or final
allocator destruction releases the retained owners exactly once. Software
enqueue completion alone does not release them. The existing early Reset path
while allocator submissions remain pending preserves its retained resources.
No Mesa runtime-v2, optional pageable-provider or KMD wire ABI changes are made.

Local validation passes:

- Five actual CPU Vulkan graphics rounds check exact occlusion sample counts,
  binary0/1 results, IA vertices/primitives, PS/CS statistics, ordered timestamps
  and every untouched result-buffer sentinel byte.
- Actual destruction notifications prove retained query/result ownership for
  cancelled recording, legal reset and a GPU submission blocked by a fence.
- Separately compiled omitted-resolve and dropped-owner controls fail on the
  expected result/lifetime invariants.
- Existing compute, descriptor, texture, shader and five-round pixel tests pass.

Windows multiarch [CI35460336098](https://github.com/sunflower2333/vkd3d-proton/actions/runs/35460336098)
is terminalSUCCESS, all5jobs. ARM64/x64/x86 compiled and the actual-WDK
fixtures plus11semantic negatives ran successfully on all three architectures.
The native ARM64 runtime job is105943859712. The new actual-WDK fixture checks
Begin/End/Resolve argument order, rejected/expired handles, backend error
propagation, VirtualFree of query caller storage, recursive command/resource
destruction, generation change and device retirement. No device test has been
performed by this worker. Root owns target integration, signing and testing.

Artifact receipts:

| Artifact | ID | Archive bytes |
| --- | --- | --- |
| vkd3d-native-ddi-arm64 | 10589423936 | 13232967 |
| vkd3d-native-ddi-x64 | 10589154579 | 13614194 |
| vkd3d-native-ddi-x86 | 10589122977 | 13502323 |
| vkd3d-runtime-arm64-validation | 10589508780 | 12117 |

ARM64 archiveSHA256:
`f7c4460533d435a57c1de10e4a6fd1264e7ee07d059d12ffe26f31b346e62fda`.
Implementation is based on the validated pageable consumer479a3f9 and its
documentation checkpoint18173b4. It is independent of the separately validated
Mesa alignment fixes93a48425/cd9bfb8e and optional Mesa pageable providerecfb2b2e.
Runtime-v2 headerSHA256 remains
`facd7a43c42b227b9bc9cc44d184b4801b3e5201168028fb9ae9700f8abd1999`;
optional pageable protocol1 remains
`82ae2fa0f8a57e76541b7f1f7b1f52f3d49de386b2c860cb0bef427451584000`.
