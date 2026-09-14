# DroidVM VKD3D-Proton optimization scope

## Goals

Tune D3D12 workload behavior over VIOGPU without changing D3D12 semantics.

## Phase 1

- Track command list submission batching.
- Measure descriptor/resource update pressure.
- Keep diagnostic overhead disabled by default.

## Phase 2

- Optimize descriptor heap reuse.
- Reduce redundant resource transitions where legal.
- Validate shader/pipeline cache behavior.

## Validation

Performance claims require identical workload, driver and shader-cache state.
