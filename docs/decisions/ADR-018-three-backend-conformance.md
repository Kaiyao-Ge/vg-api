# ADR-018: Three-Backend Shared Conformance

Status: Accepted

## Context

ADR-013 established a single reference-only `conformance.device-hal`
harness and stated the intent that "future Metal/Vulkan harnesses reuse
this contract and test fixture without duplicating semantic expectations."
With B5 (ADR-016) and B6 (ADR-017) now providing real Metal and Vulkan
`compile()`/`submit()` paths against the B4 golden fixtures (ADR-015),
that intent needs to actually be implemented rather than remain aspirational.

The original single-backend harness asserted reference-specific
expectations unconditionally (e.g. `TaskPublication`, extra instrumentation
events like `canonical_ir`/`linear_access`). Reusing it verbatim against
Metal or Vulkan would misclassify a backend's honest lack of a capability
as a conformance failure -- exactly the wrong incentive, since ADR-013's
whole point is that `Unsupported`/partial-capability reporting must remain
safe to state truthfully.

## Decision

Split the old single-file harness into a shared static library,
`tests/conformance/conformance_lib.{h,cpp}`, and three thin per-backend
`main()` files (`device_hal_conformance_{reference,metal,vulkan}.cpp`),
registered as CTest `conformance.device-hal.{reference,metal,vulkan}`
(metal/vulkan gated on `VG_ENABLE_METAL`/`VG_ENABLE_VULKAN` respectively).

The shared library exposes `vg::conformance::run(device, backend_name,
ConformanceExpectation, repo_root)`. `ConformanceExpectation` is a
capability-gating struct, not a hard assertion list:
`expect_task_publication`/`expect_timeline` only assert a capability bit
*when true*; when false, the suite does not assert either way, since a
backend that has honestly not implemented a capability yet should not be
penalized for omitting it. `expect_linear_subset_only` gates whether the
suite requires the backend to additionally emit a `canonical_ir`
instrumentation event (only `ReferenceDeviceHal` does today; Metal/Vulkan
lower only the B4 linear subset and must *not* claim that extra event).
Both directions of this check are wired into `run_contract_checks()` --
this was a design gap during initial writing (the field was declared and
documented but not consulted by any assertion) that was closed before
declaring M3 complete.

Two suites run per backend:
- `run_contract_checks()`: ABI/capability-snapshot assertions gated per
  `ConformanceExpectation`, plus the two negative-path checks reused
  verbatim from the original harness (stale ABI version, non-advancing
  timeline), asserting the exact error message strings produced by the
  shared `hal::ExecutionPlan::validate()` logic in `device_hal.cpp`.
- `run_golden_fixture_invariant()`: for each of the four B4 golden
  fixtures, compiles+submits+executes on both the reference oracle and the
  backend under test, using independently-constructed `Arena`s fed the
  same fixture text (deterministic id remapping via allocation order, not
  shared state). **The one unconditional invariant across all three
  backends**: if a backend's `LoweringReport` claims `supported == true`
  for a fixture, its resulting allocation bytes must match the reference
  oracle exactly. An honest `Unsupported`/uncompiled result is logged as
  "skipped (unsupported)" and does **not** fail the suite -- only silently
  wrong bytes do.

The old `tests/conformance/device_hal_conformance.cpp` is deleted (folded
into `conformance_lib.cpp` + the reference `main()`); `CMakeLists.txt`'s
conformance section is rewritten accordingly (new `vg_conformance_lib`
STATIC target, three gated executables replacing the old single target).

## Alternatives

- Keep one hardcoded-to-reference harness and add ad hoc `#ifdef` branches
  for Metal/Vulkan inside it: rejected -- reproduces exactly the
  duplication ADR-013 already warned against.
- Assert specific `LoweringReport` event names/counts identically across
  all three backends: rejected -- reference intentionally emits additional
  instrumentation events (`canonical_ir`, `linear_access`,
  conditionally `task_publication`/`timeline`) that Metal/Vulkan have no
  reason to duplicate at this stage; the shared invariant is byte-exact
  correctness of claimed-successful lowerings, not identical
  instrumentation surface.
- Treat an backend's `Unsupported` result on a fixture as a conformance
  failure: rejected -- would directly punish honest capability reporting,
  the opposite of ADR-013's intent.

## Consequences

All three backends now share one semantic contract and one fixture set
end to end (B4 -> B5/B6 -> B1's conformance harness), closing the loop
ADR-013 opened. Reference and Metal are verified passing locally on this
machine (`conformance.device-hal.reference`, `conformance.device-hal.metal`,
alongside `vertical-slice.metal`, all pass under the `dev-metal` preset).
Vulkan's `conformance.device-hal.vulkan` is compile-review-only on this
machine, consistent with ADR-017's stated verification limits, and awaits
Linux/NVIDIA execution.

## Evidence

Verified locally: `ctest --preset dev-metal` reports 18/18 passing,
including `conformance.device-hal.reference`, `conformance.device-hal.metal`,
and `vertical-slice.metal`, with byte-exact agreement against the
reference oracle across all four golden fixtures for every backend that
reports a fixture as supported.

## Revisit trigger

Revisit once `conformance.device-hal.vulkan` is actually run on a
Linux/NVIDIA server (ADR-017's revisit trigger). Revisit
`ConformanceExpectation`'s shape if a fourth backend is added, or if B7/B8
introduce task-publication/timeline lowering on Metal or Vulkan, at which
point `expect_task_publication`/`expect_timeline` should flip to `true`
for those backends.
