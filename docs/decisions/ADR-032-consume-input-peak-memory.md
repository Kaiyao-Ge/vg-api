# ADR-032: E005 ConsumeInput Peak Memory -- Reuse Arena::transform/consume, Real Metal Measurement

Status: **Withdrawn / historical** (2026-08-23) — written under an over-scoped Phase C closure attempt; not authoritative for the layer-1 CanonicalView/FacetPool/facets/transform deliverable.

## Context

Phase C's Representation gate (`09-experiment-catalog.md` gate table) requires
E005 ("ConsumeInput peak memory") alongside E008 and E016. ADR-030 set the
evidence policy (Metal+reference hardware + Vulkan compile-review-only);
ADR-031 delivered CanonicalView/FacetPool and the first real SampleFacet
lowering that E005's "linear → sample optimal" transform target depends on.

`Arena::transform` / `Arena::consume` already implemented ConsumeInput's
core contract before Phase C (`in_flight==0` required; consume retires the
allocation and bumps generation). E005 is therefore a wiring-and-measurement
milestone, not a new core-semantics invention: connect those Arena calls to
a real Metal texture transform (TASK-C2's `ensure_texture`) and report
peak/steady bytes honestly.

## Decision

**Standalone `metal::DeviceHal::run_consume_input(...)`**, following the
`run_cull_compact` / `run_sample_facet` shape -- not an `ExecutionPlan`
field. ConsumeInput is an Arena+resource operation outside the
IR/`compile()`/`submit()` flow; inventing a modal flag on the existing
linear compute pipeline would misrepresent the mechanism.

**Two variants, identical transform**:
1. `multi_version_baseline=true`: `ensure_texture` + `Arena::transform`;
   never calls `Arena::consume`. Old linear host bytes and the new
   MTLTexture both stay resident (`steady_bytes == peak_bytes`).
2. `multi_version_baseline=false` (ConsumeInput): same transform, then
   `Arena::consume` and actual `bytes.clear()`/`shrink_to_fit()` of the
   retired linear backing (`steady_bytes == new_backing_bytes`).

**Fault-injection Deferred**: catalog names fault-before/during/after
transform variants. Existing poison/fault machinery is scoped to IR
instruction execution inside `compile()`/`submit()`. `run_consume_input`
executes no IR module. Rather than fabricate an unrelated Arena-level
fault injector, the experiment definition records `fault_injection.status
= Deferred` with an explicit reason.

**Shared accounting fields** (TASK-C3) on `LoweringReport`/`Submission`
(`old_backing_bytes`/`new_backing_bytes`/`temporary_bytes`/
`heap_fragmentation_bytes`/`completion_delay_ns`) exist for future
`compile()`/`submit()` paths; E005's vertical slice reports the same
quantities via `ConsumeInputResult` because it bypasses that path.

## Alternatives

- Drive ConsumeInput through `ExecutionPlan::request_consume_input` +
  `compile()`/`submit()`: rejected -- no IR module participates; would
  force a fake module solely to carry a flag.
- Approximate peak memory without creating a real MTLTexture: rejected --
  E005's workload is specifically linear→sample-optimal, which requires
  TASK-C2's SampleFacet path.
- Implement catalog fault-injection by reusing IR poison: rejected --
  wrong layer; would produce misleading "fault" results.

## Consequences

- `vertical-slice.metal.consume-input` demonstrates a real steady-state
  memory reduction (fixture: peak 32B, baseline steady 32B, ConsumeInput
  steady 16B for 2x2 RGBA8Unorm).
- Lost capability is explicit: after ConsumeInput, the old linear
  representation is unrecoverable (no replay/rollback to the
  pre-transform epoch).
- Vulkan remains compile-review-only (ADR-030).

## Evidence

- `experiments/definitions/E005-consume-input-peak-memory.json`
- `src/backends/metal/metal_device_hal.{h,mm}` (`run_consume_input`,
  `ConsumeInputResult`)
- `tests/vertical_slice/metal_task_timeline_test.cpp` (`consume-input`)
- ctest `vertical-slice.metal.consume-input`

## Revisit trigger

Revisit if an Arena-scoped fault-injection mechanism is designed for real,
or if Vulkan hardware becomes reachable and a Vulkan SampleFacet+ConsumeInput
path is implemented.
