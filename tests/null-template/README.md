# Null descriptor template checks

These checks use the production private headers and function bodies. The contract
fixture intercepts Vulkan descriptor encoding/update calls, supplies distinct
nonzero payloads, and checks active types, companion sets, byte guards, metadata,
copies, and initialization. It does not claim GPU execution for descriptor-buffer
or embedded layouts.

With a native Meson build containing `vkd3d-umd-backend-test`, run:

```sh
python3 tests/null-template/run.py --build /absolute/build --output /new/contract-output
python3 tests/null-template/run-entry.py --build /absolute/build --output /new/entry-output --mode split --scope entries
python3 tests/null-template/run-entry.py --build /absolute/build --output /new/readback-output --mode split --scope readbacks
```

All output directories must be new. `run.py --mutation NAME` modifies an extracted
production body only and requires exit 1 plus the exact intended diagnostic. The
nine names are `ignore-list`, `always-initial`, `buffer-global`, `write-global`,
`initial-image`, `skip-companion`, `keep-range`, `keep-raw-va`, and `drop-copy-mask`.
`--baseline` checks the original source's exact failures; the immutable evidence
contains that source and the corresponding original fixture and runner.

The entry fixture invokes real D3D12 null CBV/SRV/UAV and descriptor-copy entry
points. Its readbacks reuse the unchanged five constant-buffer and eight
shader-resource data oracles (1024 values each). Modes are `split`, `merged`, and
`nonmutable`; `--baseline-resource /original/resource.c` links the original
resource implementation for a counterfactual. The runner uses the workspace's
pinned CPU Vulkan and validation-layer settings and checks the selected layout,
two active validation probes, disabled shader cache, data result, and exact VUIDs.

The frozen run has 15 contract cases and nine diagnosed mutations passing. Real
split and merged entry/readback runs have no validation errors. Nonmutable entry
writes pass, while its readback run remains a strict failure: 180 descriptor-limit
errors also occur with the original resource implementation. The full backend
data oracle passes, but its strict validation run retains 47 inherited errors.
These failures are preserved, not accepted as clean results. The CPU ICD does not
select descriptor-buffer or embedded layouts; those modes have contract evidence
only. Existing validation-layer extension warnings also remain visible.
