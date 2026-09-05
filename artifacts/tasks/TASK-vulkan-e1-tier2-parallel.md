# Vulkan E1 / F Tier2 parallel execution freeze

> Execution result (2026-09-04): this freeze document records the original assignment. The previously pending paths are now integrated: formal built-in Raster, `vg.glsl.raster/v1`, direct/indexed Tier2, SceneRoot, and D32 depth all pass real llvmpipe execution under Khronos validation with zero VUID. See the corresponding `docs/reports/vulkan-*` completion reports.


Date: 2026-09-04. Work only in `/Users/gokyrie/projects/vg-api`.
Baseline HEAD `fe2b97ba0dce710f1b96f32a8ab0ab162adeb341` plus preserved uncommitted B/C/D/F-compute integration. A is user accepted. D/F-compute local gates are complete; Linux SDK/device gates remain pending. Two fresh GPT-5.6 Terra / medium subagents implement E1 and F Tier2; coordinator owns shared integration.

## Dependency and scope

E1 implements the formal built-in Vulkan Raster route through Core assembled `ExecutionPlan -> DeviceHAL::compile -> DeviceHAL::submit`, consuming the existing sealed per-Node Raster package and `ExecutionSchedule`. It covers the current Reference/Metal built-in contract: non-indexed and indexed triangle-list draws, optional Depth32Float attachment/state, SceneRootRaster root/albedo, facet lifetime, wave transitions and complete canonical publication. It must not use the D legacy direct raster helper as evidence for formal submission. Restricted user raster shader import remains E2 and must stay explicitly Unsupported. No presentation/windowing, clipping/perspective, blend, new topology, new ABI or mixed-domain policy invention.

F Tier2 productionizes the already-built compute-only Vulkan bucket/fill/per-authorized-node indirect experiment. It must first identify a Core-sealed semantic fact that unambiguously requests GPU selection among preauthorized Nodes. If no such fact exists in the current assembler, it may extract a reusable backend physical component and tests, but it must not wire it into ordinary multi-Node execution, add a caller-controlled `request_*` flag, advertise `IndirectTier2Select`, or claim formal completion. Any semantic-entry gap is a coordinator decision/blocker. Tier3, DGC assumptions, raster/draw Tier2, arbitrary pipeline creation and host readback/re-encode masquerading as GPU-driven are excluded.

E1 and F Tier2 can develop independently because E1 owns Raster packages/commands and F owns compute selection. Their only intersection is coordinator-owned DeviceState declarations, lowering/commit orchestration and CMake; agents deliver those proposed changes as unapplied patches/notes.

## Strict direct-edit inventory

E1 agent only:
- `src/backends/vulkan/vulkan_plan_raster.h` (new)
- `src/backends/vulkan/vulkan_plan_raster.cpp` (new)
- `src/compiler/shaders/raster.cpp` (only built-in Vulkan Raster shader parity proven necessary)
- `tests/vertical_slice/vulkan_plan_raster_test.cpp` (new)
- `docs/reports/vulkan-e1-raster.md` (new)
- `/tmp/vg-vulkan-e1-*` and `build/vulkan-e1-reference`

F Tier2 agent only:
- `src/backends/vulkan/vulkan_tier2.h` (new)
- `src/backends/vulkan/vulkan_tier2.cpp` (new)
- `tests/vertical_slice/vulkan_tier2_submission_test.cpp` (new)
- `tests/support/vulkan_tier2_harness.cpp` and `tests/support/vulkan_gpu_experiments.h` only when extracting the existing prototype without weakening its frozen tests
- `docs/reports/vulkan-f-tier2-production.md` (new)
- `/tmp/vg-vulkan-f-tier2-*` and `build/vulkan-f-tier2-reference`

Coordinator only:
- `src/backends/vulkan/vulkan_device_internal.h`
- `src/backends/vulkan/vulkan_device_hal.cpp`
- `src/backends/vulkan/vulkan_lowering.cpp`
- `src/backends/vulkan/vulkan_commit.cpp`
- `src/backends/vulkan/vulkan_resources.cpp`, `vulkan_pipelines.cpp`, `vulkan_encoding.cpp`, `vulkan_raster.cpp`
- all Core/HAL/public headers and sources
- `cmake/tests.cmake`, `cmake/g4-vulkan-tests.cmake`, runner/catalog and contract tests
- this freeze and final integration report
- `/tmp/vg-vulkan-e1-tier2-*` and `build/vulkan-e1-tier2-reference`

Agents do not edit coordinator-owned files. They put exact proposed shared-file edits and insertion points in `/tmp/vg-vulkan-{e1,f-tier2}-integration.patch` or their report. No agent edits another owner's build or files. Scope additions require coordinator review before mutation.

## E1 acceptance

- Formal assembled plan; no synthetic seal and no direct-only helper accepted as proof.
- Compile creates one Raster package for each complete NodeRef and rejects mismatched/compute package projection.
- Submit follows the sealed schedule, executes every Raster Task exactly once, publishes the complete canonical graph and records actual draw/barrier/command-buffer/encoder/wait counts.
- Built-in GLSL matches current xyzuv vertex ABI, binding slots, SceneRoot matrix/material authority, indexed/non-indexed draws, color/depth formats and depth compare/write state.
- Pixel/depth results compare with Reference on nontrivial fixtures; repeated submit observes SceneRoot changes and stable cache identity.
- Stale/missing/wrong-kind facets, bad vertex/index sizes/ranges, unsupported format/sample/topology, invalid SceneRoot and restricted import fail before partial effects where the shared contract requires it.
- No Raster capability advertisement until every advertised built-in obligation is wired. Real Vulkan tests must fail, not skip or fall back, when no device exists.

## F Tier2 acceptance

- Selection is among at least two complete, envelope-authorized NodeRefs/classes and matches the Reference oracle for uniform, skewed, empty-bucket and tail cases.
- GPU writes bucket counts and tightly packed indirect commands; consumers run without host count readback/re-encode. Producer-to-consumer and consumer-to-host synchronization is explicit.
- Unauthorized/stale/duplicate authorization, excessive class/task count and wrong package class reject deterministically; all transient Vulkan resources clean up on every failure.
- LoweringReport names bucket/fill/indirect passes, temporary requested bytes, pipeline switches, command counts and actual classification. Bucket fallback is `EmulatedDevicePass`, never `DevicePass`.
- `Capability::IndirectTier2Select` stays clear unless a formal Core-sealed selection fact reaches Stage 6 and Stage 7 consumes it. Missing semantic input is a documented blocker, not an inferred request from an ordinary multi-Node graph.
- Real Vulkan tests fail, not skip or fall back, without a device. CPU oracle is separately named and unregistered as a GPU pass.

## Gates and constraints

Agents may run source-format, syntax and meaningful CPU/reference fixtures in their exclusive outputs. Coordinator reviews all diffs, applies shared integration sequentially, builds a fresh sanitizer Reference tree, runs the unchanged Reference baseline plus contracts and CPU fixtures, and verifies no-device failure. Linux Vulkan SDK, glslc, validation layer and real-device execution remain a separate platform gate; arithmetic test counts are never platform evidence. No remote, install, commit, push, branch/worktree change, public ABI growth, new execution domain, new Task ring discriminator, Tier3 or fallback to another backend.

## Ledger

| Package | Implementation | Local | Platform | Docs |
|---|---|---|---|---|
| E1 built-in formal Raster | active; cache/shader/validation prepared, command encoding not accepted | CPU oracle and 40-test baseline passed | pending Linux SDK/GPU | draft complete |
| E2 restricted Vulkan raster import | deferred | not run | pending E1 | scope recorded |
| F Tier2 physical handoff | complete for current semantic boundary | CPU unit passed | pending Linux SDK/GPU harness | complete |
| F Tier2 formal plan integration | blocked: no Core-sealed selection fact | not applicable | pending semantic ADR | complete audit |
