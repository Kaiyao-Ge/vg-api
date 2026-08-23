# TASK-B13: E009 -- Metal Task Tier1 Real Indirect Dispatch + GPU Cull/Compact Kernel

Status: complete.

Normative docs: ADR-026 (Metal Task Tier1 indirect dispatch + cull/compact,
supersedes ADR-021's "Tier1 deferred" framing); ADR-024 (Phase B closure
criterion); TASK-B12 (shared observability foundation this milestone
builds on). This is the fourth milestone (TASK-B13) of the eight-milestone
plan approved this session.

## Goal

Give E009 ("GPU-generated same-Node work") real Metal + reference results:
(1) a real Metal Task Tier1 implementation -- GPU-authored indirect
dispatch driven by each published task's own `x/y/z` dims, never read back
to the host before dispatching -- and (2) a real GPU cull/compact
stream-compaction kernel, both hardware-verified on Apple Silicon. Vulkan
stays compile-review-only per ADR-024 (reuses its already-implemented
Tier1, ADR-022; its cull/compact GLSL analogue exists but is deliberately
not wired in).

## Files

- `src/compiler/compiler.h` / `src/compiler/compute_package.cpp` -- new
  `cull_compact_metal_source()` (MSL kernel: one thread per instance,
  `atomic_fetch_add_explicit` claims an output slot when
  `instance_visible[gid]` is nonzero) and `cull_compact_vulkan_source()`
  (GLSL analogue, unwired), mirroring `task_ring_metal_source()`'s
  "independent hand-written kernel + dedicated pipeline" precedent.
- `src/backends/device_hal.h` -- `ExecutionPlan` gains
  `bool request_tier1_indirect{}` (default `false`, preserving every
  pre-B13 caller's behavior exactly). Matches the existing
  `requested_certificate_mode` precedent: no new virtual method, since
  `hal::DeviceHal` stays fixed at `compile()`/`submit()`.
- `src/backends/metal/metal_device_hal.h` -- new `CullCompactResult`
  struct (`visible_count`, `compact_ids`); new public methods
  `run_cull_compact(...)` (standalone, Metal-only, bypasses the generic
  ABI -- mirrors the `probe_buffer()`/`snapshot()` precedent) and
  `last_tier1_indirect_dims()` (debug/test-only introspection accessor).
- `src/backends/metal/metal_device_hal.mm` -- `ensure_cull_compact_pipeline()`
  (mirrors `ensure_task_ring_pipeline`); `dispatch_task_tier1_indirect(...)`
  (one command buffer: an `MTLBlitCommandEncoder` copies each task's 3
  dispatch-dim words from `fields_buffer` into a dedicated indirect-args
  `MTLBuffer` laid out as `MTLDispatchThreadgroupsIndirectArguments`, then
  an `MTLComputeCommandEncoder` issues one
  `dispatchThreadgroupsWithIndirectBuffer:` per task against the same
  already-compiled module pipeline/buffers used by the initial
  `dispatch_and_wait`; Shared storage + Metal's implicit cross-encoder
  hazard tracking need no explicit `MTLFence`); wired into `submit()`'s
  task-graph block immediately after the Tier0 readback loop, gated on
  `compiled.plan.request_tier1_indirect && capabilities().supports(Capability::IndirectTier1)`;
  `run_cull_compact()` (4 Shared buffers, one dispatch, synchronous
  readback of `visible_count` and `compact_ids[0..visible_count)`);
  `last_tier1_indirect_dims()` accessor.
- `src/backends/reference/reference_executor.h` / `.cpp` -- reference
  oracle `CullCompactResult`/`cull_compact(instance_visible, instance_ids)`:
  walks instances in ascending index order, returns every id whose visible
  flag is nonzero. Doc comments on both the Metal and reference
  `CullCompactResult` types document the set-vs-order comparison
  requirement (see Known limits).
- `src/backends/vulkan/vulkan_device_hal.cpp` -- no changes (compile-
  review-only; reuses `dispatch_task_ring_and_tier1` from ADR-022 as-is).
- `tests/vertical_slice/metal_task_timeline_test.cpp` -- two new modes,
  `run_tier1_indirect`/`run_cull_compact`, following the existing
  `run_task_tier0`/`run_timeline`/`run_access_certificate` structural
  pattern and `main()`'s mode-dispatch.
- `CMakeLists.txt` -- two new `add_test` entries,
  `vertical-slice.metal.tier1-indirect` and
  `vertical-slice.metal.cull-compact`, on the existing
  `vg_metal_task_timeline_test` executable.
- `experiments/definitions/E009-gpu-generated-same-node-work.json` -- new
  experiment definition (schema `vg.experiment/v1`).
- `docs/decisions/ADR-026-metal-task-tier1-indirect-dispatch-cull-compact.md` --
  new ADR.

## Validation

`cmake --build build/dev-metal` rebuilds clean (no new warnings beyond the
pre-existing benign duplicate-library linker warning). `ctest
--output-on-failure` under the `dev-metal` preset: 23/23 tests passed --
the 21 pre-existing tests unchanged, plus both new tests:
`vertical-slice.metal.tier1-indirect` (asserts `last_tier1_indirect_dims()`
matches each published task's real `x/y/z` in dispatch order, plus a
`tier1_indirect_dispatch` report event classified `LoweringClass::Direct`
with the correct task count) and `vertical-slice.metal.cull-compact`
(asserts the GPU-compacted id set from a 9-instance visibility array
equals `reference::cull_compact`'s oracle output once both sides are
sorted). `tooling.schemas` and `docs.check` pass with the new ADR and
experiment JSON in place.

## Known limits

- **Tier1 re-dispatch is idempotent-instruction-only** -- it re-dispatches
  the same already-compiled module pipeline/buffers once more per task on
  top of the initial `dispatch_and_wait`, which is only correctness-safe
  for load/store (idempotent); combining it with `atomic_add` would
  double/multiply-accumulate. This milestone's probe module and ctest are
  deliberately load-only. See ADR-026 Revisit trigger.
- **Arena readback does not reflect Tier1's effect on buffer bytes** -- the
  arena readback memcpy in `submit()` runs once, immediately after the
  initial `dispatch_and_wait`, before the Tier1 block executes. Tier1's
  effect on those same buffers is never copied back into `core::Arena` in
  this implementation. This is an explicit, documented scope boundary
  (ADR-026), not an oversight: `vertical-slice.metal.tier1-indirect`
  validates Tier1 solely via `last_tier1_indirect_dims()`, not arena byte
  contents.
- **Cull/compact output order is non-deterministic by design** -- GPU
  thread arrival order determines atomic-append slot assignment, not
  instance index order. Comparison against the reference oracle must
  always be as a sorted multiset/set, never by position; this is a correct
  property of GPU stream compaction, documented on both sides'
  `CullCompactResult`.
- **No comparative CPU-frame-time benchmark across the catalog's full four
  variants** (CPU cull+commands; GPU cull+readback+CPU; GPU cull+indirect;
  VG Task Tier1) at million-instance scale -- this milestone is a
  correctness vertical slice for the Tier1 and cull/compact mechanisms
  individually, not a perf-harness comparison; see the E009 experiment
  JSON's `judgement` field for the explicit scope statement.
- **`cull_compact_vulkan_source()` exists but is unwired** -- GLSL analogue
  written for review parity, deliberately not integrated into
  `vulkan_device_hal.cpp` since it is not required for E009's
  compile-review-only Vulkan evidence.
- **GPU-side hardware timing remains out of scope** -- results are CPU
  wall-clock metrics (TASK-B12's `cpu_encode_ns`/`cpu_submit_ns`) plus
  `has_hidden_host_wait()` partial-but-real evidence, never claimed as
  complete GPU-side timing (`gpu_ns` stays `std::nullopt`).
