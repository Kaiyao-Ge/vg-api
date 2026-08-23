# ADR-033: RepresentationEpoch Backpressure -- Bounded In-Flight Budget, Explicit Reject

Status: **Withdrawn / historical** (2026-08-23) — written under an over-scoped Phase C closure attempt; not authoritative for the layer-1 CanonicalView/FacetPool/facets/transform deliverable.

## Context

E016 asks whether multi-version representation streaming under
frames-in-flight 1–8 produces an unacceptable watermark, and requires a
backpressure policy that forbids unbounded version creation
(`09-experiment-catalog.md` E016). TASK-C4/ADR-032 already wired a single
transform+ConsumeInput measurement; E016 is that path's high-frequency
churn counterpart with an explicit budget.

Before this milestone, `Arena::transform` always bumped
`representation_epoch` whenever `in_flight==0`. There was no bound on how
many concurrent representation versions an allocation could accumulate,
and Metal's `ensure_texture` erased the previous epoch's MTLTexture on
rebuild -- which would have hidden the multi-version peak cost E016
exists to measure.

## Decision

**Arena-level budget**: `Arena::set_max_in_flight_representations(N)`.
`N == 0` means unbounded (legacy default). When `N > 0`, `transform()`
rejects with `"in-flight representation budget exceeded"` once
`Allocation::live_representations >= N`, before bumping the epoch.

**`Allocation::live_representations`**: starts at 1 (the initial representation).
Each successful `transform()` increments it; new
`Arena::release_representation()` decrements it (not below 1). Distinct
from `in_flight` (acquire/release reader leases). `consume()` still
retires the whole allocation and zeroes `live_representations`.

**Standalone `run_representation_churn`** with three `ChurnPolicy` values:
- `MultiVersionUnbounded`: budget 0; retain every new MTLTexture in a
  host-side version ring (does **not** use `ensure_texture`, which would
  erase prior epochs).
- `Backpressure`: budget = `frames_in_flight`; on reject, release oldest
  ring entry (producer stall) and retry -- except `frames_in_flight==1`,
  where the sole initial representation already saturates the budget and
  every attempt is a pure reject.
- `ConsumeInput`: after each accept, release older representations so the
  ring stays near the transform window (`live_representations <= 2`) without
  retiring the allocation via `consume()` (which would prevent further
  transforms on the same id/generation).

**drop/quality policy** (catalog variant) is explicitly out of scope: it
is an application choice after backpressure has refused a transform, not
a core/Arena mechanism.

## Alternatives

- Soft-stall inside `transform()` until a version is released: rejected --
  would hide the reject from callers and encourage implicit blocking;
  catalog wants predictable failure.
- Key multi-version peak only via accounting without retaining MTLTextures:
  rejected -- would not exercise real Metal object residency.
- Use `Arena::consume` every frame for the ConsumeInput variant: rejected
  for churn -- consume retires the allocation, forcing re-allocate each
  iteration and conflating allocation churn with representation churn;
  `release_representation` keeps the allocation Active.

## Consequences

- Unbounded multi-version growth is opt-in (`max == 0`) and measurable;
  bounded mode fails closed with an explicit error string.
- `vertical-slice.metal.representation-churn` sweeps fif 1–8 and asserts
  MultiVersionUnbounded peak exceeds ConsumeInput peak, and Backpressure
  actually reports rejects.
- Vulkan remains compile-review-only (ADR-030).

## Evidence

- `experiments/definitions/E016-representation-epoch-churn.json`
- `src/core/core.{h,cpp}` (`live_representations`, `set_max_in_flight_representations`,
  `release_representation`)
- `src/backends/metal/metal_device_hal.{h,mm}` (`run_representation_churn`)
- `tests/unit/core_test.cpp` (budget unit tests)
- ctest `vertical-slice.metal.representation-churn`

## Revisit trigger

Revisit if representation versions become first-class Arena objects (rather
than a counter + backend ring), or if an application-level drop/quality
policy needs a shared VG hook beyond the explicit reject.
