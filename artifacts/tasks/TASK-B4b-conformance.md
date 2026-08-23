# TASK-B4b: Three-Backend Shared Conformance

Status: complete (reference + Metal verified locally; Vulkan compile-review-only)

Normative docs: `docs/START.md`; `docs/vg-project/13-repository-layout.md`

## Goal

Extend the B1 conformance harness (ADR-013), previously hardcoded to the
reference backend only, into a shared suite reused by all three backends
without duplicating semantic expectations, per ADR-013's original stated
intent.

## Invariants

- Capability-gated assertions, not hard pass/fail lists:
  `ConformanceExpectation.expect_task_publication`/`expect_timeline` only
  assert a bit when `true`; a backend that has honestly not implemented a
  capability is not penalized for omitting it.
- `expect_linear_subset_only` gates whether the backend is required (or
  forbidden) to emit the reference-only `canonical_ir` instrumentation
  event, in addition to the universal `compute_package` event every
  backend must emit.
- The one unconditional cross-backend invariant: if a backend's
  `LoweringReport` claims `supported == true` for a golden fixture, its
  output bytes must match the reference oracle exactly for that fixture.
  An honest `Unsupported` is logged and skipped, never treated as a
  failure.
- All three suites consume the same B4 golden fixtures (`tests/fixtures/ir/*.vgir.json`); no backend redefines expected semantics locally.

## Files

- `tests/conformance/conformance_lib.h`, `.cpp`
- `tests/conformance/device_hal_conformance_{reference,metal,vulkan}.cpp`
- `CMakeLists.txt` (`vg_conformance_lib` target; `conformance.device-hal.{reference,metal,vulkan}` CTest entries)
- `docs/decisions/ADR-018-three-backend-conformance.md`

Superseded: `tests/conformance/device_hal_conformance.cpp` (deleted; folded
into `conformance_lib.cpp` + `device_hal_conformance_reference.cpp`).

## Validation

Verified locally under the `dev-metal` preset: `ctest` reports 18/18
passing, including `conformance.device-hal.reference` and
`conformance.device-hal.metal`, both with byte-exact agreement against the
reference oracle across all four golden fixtures.
`conformance.device-hal.vulkan` is compile-review-only on this machine
(cannot configure `VG_ENABLE_VULKAN` on macOS); awaits Linux/NVIDIA
verification per TASK-B6.

## Known limits

Only the linear subset is exercised; task-publication/timeline lowering on
Metal/Vulkan remains out of scope until B7/B8, at which point
`ConformanceExpectation` for those backends should be revisited.
