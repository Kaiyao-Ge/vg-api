# ADR-025: AccessCertificate Modes -- 3 Implementations, 2 Honest Deferrals, and the ExecutionPlan Field ABI Decision

Status: Accepted

## Context

`docs/vg-project/09-experiment-catalog.md` defines E004 (AccessCertificate)
as one of Phase B's exit-gate experiments: it exercises the adapter
memory-visibility policy space `CertifiedPinned`/`Universe`/
`DiscoverThenLease`/`SoftwarePaged`/`FaultManaged`. Before this ADR, none
of the five modes existed anywhere in the codebase -- not as a type, not
as a `DeviceHal` code path, not as a report event. ADR-024 established
that Phase B's exit gate is satisfied by real reference+Metal results
plus Vulkan compile-review-only evidence; this ADR is the first
milestone (TASK-B11) executing that plan, chosen to go first because it
required zero new IR opcodes, zero new GPU kernels, and largely
formalizes bookkeeping the reference and Metal backends already perform
internally (a per-instruction `allocation`/`generation` walk, and a full
`arena.allocations()` scan) rather than computing.

`core::GraphEpoch`/`GraphEpochBuilder` (`core.h`) already provide an
immutable, deduplicated, sealed flat reachability-set snapshot
(`PointerRef{allocation, generation}` list) built against a specific
`Arena::topology_epoch()`. This is structurally identical to what an
AccessCertificate needs to represent (a certified/discovered set of
allocations, stamped to a topology version) -- reinventing a parallel
set type here would duplicate `GraphEpochBuilder`'s dedup/seal logic for
no benefit.

## Decision

**New `core::AccessCertificateMode` enum** (`CertifiedPinned`, `Universe`,
`DiscoverThenLease`, `SoftwarePaged`, `FaultManaged`) and **`core::AccessCertificate`**
struct (`mode`, `epoch: GraphEpoch`, `discovery_host_ns`,
`discovery_gpu_ns`, `scanned_bytes`, `result_bytes`, `working_set_bytes`),
placed immediately after `AccessWitness` in `core.h` -- adjacent to, but
distinct from, the pre-existing `Certificate`/`AccessWitness` pair (those
validate a static declared-range certificate against a runtime witness;
`AccessCertificate` instead classifies *how* a backend can even discover
its accessible set).

**`core::build_access_certificate(arena, mode, touched, out, error)`** is
the single mode-dispatching entry point, shared by reference and Metal
(no per-backend duplication of the certificate-building logic itself):

- **`CertifiedPinned`**: certifies exactly `touched` (the allocations the
  compiled module statically references, derived from
  `module.instructions[i].allocation/.generation`) -- each is validated
  live via `arena.lookup()` before being added to the `GraphEpochBuilder`,
  so a stale or bogus touched-reference is rejected rather than silently
  certified.
- **`Universe`**: certifies every `ObjectState::Active` allocation in
  `arena.allocations()`, ignoring `touched` entirely.
- **`DiscoverThenLease`**: performs the identical full-arena scan as
  `Universe`, but wraps it in `std::chrono::steady_clock` timing reported
  as `discovery_host_ns`, and is classified `HostAssisted` (not `Direct`)
  by callers -- see Alternatives for why this, not a smaller certified
  set, is the honest result.
- **`SoftwarePaged`/`FaultManaged`**: `build_access_certificate` returns
  `false` unconditionally for these two modes. It does not attempt any
  approximation -- callers (reference's and Metal's `compile()`) must
  classify the request `LoweringClass::Unsupported` themselves; this
  mirrors `docs/START.md` §4's anti-dishonest-degradation invariant and
  is exactly what both backend documents (`06-backend-macos-metal.md`
  §12, `07-backend-linux-nvidia-vulkan.md` §10) pre-authorize: "Unsupported
  unless explicitly provided."

**ABI decision (the one open fork from the approved plan): a new
`ExecutionPlan` field, not a new virtual method.** `hal::DeviceHal` is
deliberately minimal -- exactly `capabilities()`, `compile()`, `submit()`.
Every existing per-submission variability (`graph_epoch`, `timeline_wait`/
`timeline_signal`, `published`) is expressed as an `ExecutionPlan` field,
never a new virtual method. E004 adds
`std::optional<core::AccessCertificateMode> requested_certificate_mode`
to `ExecutionPlan` (unset by default, preserving every pre-E004 caller's
behavior exactly -- no certificate is built, no report event added) and
`std::optional<core::AccessCertificate> access_certificate` to
`Submission`, following that same convention.

**compile()-time rejection, submit()-time attachment**: both reference
and Metal reject `SoftwarePaged`/`FaultManaged` at `compile()` --
`compiled->report.supported = false`, an `"access_certificate"` event
classified `Unsupported`, and `compile()` returns `false` -- rather than
accepting the plan and failing later at `submit()`. For the three real
modes, `submit()` builds the `touched` set from
`compiled.plan.module.instructions`, calls
`core::build_access_certificate`, and on success populates
`submission->access_certificate` plus an `"access_certificate"` report
event classified `Direct` (`CertifiedPinned`/`Universe`) or
`HostAssisted` (`DiscoverThenLease`).

## Alternatives

- **New `DeviceHal::discover_access(...)` virtual method**: rejected.
  Every backend (including Vulkan, permanently compile-review-only per
  ADR-024) would be forced to implement a method it may never
  meaningfully call, and it breaks the established convention that
  per-submission variability lives in `ExecutionPlan` fields. A new
  virtual method also can't be added optionally the way an
  `std::optional` field can -- every existing `DeviceHal` subclass would
  need a new override the day this ships, whereas the field approach
  requires zero changes to any code path that doesn't set it.
- **Backend-private helper invoked ad hoc from each backend's own
  `submit()`, with no shared `core::build_access_certificate`**:
  rejected. Reference and Metal need identical CertifiedPinned/Universe/
  DiscoverThenLease semantics (both are unified-memory-model backends
  with no real GPU-resident-subset distinction) -- duplicating the same
  logic in two `.cpp` files invites drift, and the shared entry point
  costs nothing since both backends already have `core::Arena` in scope
  at `submit()` time.
- **Approximate `SoftwarePaged`/`FaultManaged` with a smaller heuristic
  certified set** (e.g., reuse `CertifiedPinned`'s touched-set as a stand-in
  "paged" result): rejected outright. Neither backend has any actual
  paging or fault-handling mechanism; fabricating a certificate for a
  policy that isn't implemented would misrepresent the adapter's real
  memory-visibility behavior to any experiment consuming these results,
  directly violating `docs/START.md` §4.
- **Certify a smaller "recently touched" set for `DiscoverThenLease`
  instead of the full arena**: rejected. Under this project's unified
  memory model (both reference and Metal on Apple Silicon), there is no
  GPU-resident subset distinct from the arena itself for a host rescan to
  discover -- a full-arena result is the accurate answer, not an
  under-implemented one. Classifying it `HostAssisted` (for the real,
  measured rescan cost) rather than `Direct` is what keeps this honest
  rather than indistinguishable from `Universe`.

## Consequences

E004 now has real, hardware-verified results for 3 of its 5 defined
modes on both reference and Metal, and honest `Unsupported` results for
the remaining 2 -- matching ADR-024's "real results + honestly reported
gaps, never fabricated" closure standard. `ExecutionPlan`/`Submission`
gain two `std::optional` fields with zero impact on any pre-existing
caller. The shared `tests/conformance/conformance_lib.cpp` 3-backend
harness was deliberately left untouched in this milestone (see Revisit
trigger) since Vulkan's `vulkan_device_hal.cpp` does not read
`requested_certificate_mode` at all; adding shared assertions that
expect a populated `access_certificate` would silently fail there.

## Evidence

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`vertical-slice.metal.access-certificate` exercises all 5 modes against
a live Metal device -- CertifiedPinned/Universe/DiscoverThenLease each
produce a real `AccessCertificate` with the expected reference-set size
(1 for CertifiedPinned's touched-only set vs. 2 for Universe/
DiscoverThenLease's full-arena set, using an arena with one allocation
the probe module touches and one it does not), and SoftwarePaged/
FaultManaged both fail `compile()` with an honest `Unsupported` report
event. `core.unit` independently exercises
`core::build_access_certificate` directly (bypassing any `DeviceHal`) for
all 5 modes plus a bogus-touched-reference rejection case, and exercises
the reference backend's `compile()`/`submit()` handling of
`requested_certificate_mode` separately from the Metal-only
vertical-slice test. `ctest --output-on-failure`: 21/21 tests passed.

## Revisit trigger

Revisit if a future milestone adds AccessCertificate coverage to the
shared `tests/conformance/conformance_lib.cpp` 3-backend harness --
that requires either implementing `requested_certificate_mode` handling
in `vulkan_device_hal.cpp` (compile-review-only, matching ADR-024) or an
explicit per-backend opt-out mechanism in the shared harness, neither of
which was in this milestone's scope. Revisit `DiscoverThenLease`'s
full-arena-scan semantics if a future backend (or a future Metal
discrete-GPU target) introduces a real GPU-resident/host-resident
memory split, at which point "discover" should certify an actual
discovered subset rather than degenerate to `Universe`'s full set.
