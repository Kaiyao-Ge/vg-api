# TASK-B11: E004 AccessCertificate (CertifiedPinned/Universe/DiscoverThenLease Real, SoftwarePaged/FaultManaged Honest Unsupported)

Status: complete.

Normative docs: `docs/vg-project/09-experiment-catalog.md` (E004 definition);
`docs/START.md` §4 (anti-dishonest-degradation invariants); ADR-024
(Phase B closure criterion); ADR-025 (this milestone's decision record).

## Goal

Implement real, hardware-verified results for 3 of E004's 5 adapter
memory-visibility policies (`CertifiedPinned`, `Universe`,
`DiscoverThenLease`) on both the reference and Metal backends, and honest
`LoweringClass::Unsupported` reporting for the remaining 2
(`SoftwarePaged`, `FaultManaged`) rather than approximating or fabricating
results for policies neither backend actually implements. This is the
second milestone (TASK-B11) of the eight-milestone plan implementing
E002/E004/E007/E009/E012 per the plan approved this session, chosen to go
first among the five experiments because it required zero new IR opcodes
and zero new GPU kernels.

## Files

- `src/core/core.h`/`.cpp` — new `core::AccessCertificateMode` enum and
  `core::AccessCertificate` struct; new `core::build_access_certificate()`
  free function, reusing the existing `GraphEpoch`/`GraphEpochBuilder` as
  the certified/discovered-set representation.
- `src/backends/device_hal.h` — `ExecutionPlan::requested_certificate_mode`
  (new `std::optional<core::AccessCertificateMode>` field) and
  `Submission::access_certificate` (new
  `std::optional<core::AccessCertificate>` field). Both are `std::optional`
  and unset by default, so every pre-existing caller's behavior is
  unchanged.
- `src/backends/reference/reference_device_hal.cpp` — `compile()` rejects
  `SoftwarePaged`/`FaultManaged` honestly (`report.supported = false`, an
  `"access_certificate"` event classified `Unsupported`); `submit()` builds
  the touched-allocation set from `module.instructions` and calls
  `core::build_access_certificate` for the 3 real modes, attaching the
  result to `submission->access_certificate`.
- `src/backends/metal/metal_device_hal.mm` — identical compile()-time
  rejection block; a new `attach_access_certificate()` helper (anonymous
  namespace, alongside the existing `has_host_assisted_pipeline`) called
  from both `submit()` success paths (the host-assisted-pipeline branch and
  the main GPU-dispatch branch).
- `tests/vertical_slice/metal_task_timeline_test.cpp` — new
  `run_access_certificate()` exercising all 5 modes against a live Metal
  device; new `"access-certificate"` CLI mode in `main()`.
- `tests/unit/core_test.cpp` — new block exercising
  `core::build_access_certificate` directly (all 5 modes, plus a
  bogus-touched-reference rejection case) and the reference backend's
  `compile()`/`submit()` handling of `requested_certificate_mode`,
  independent of the Metal-only vertical-slice test.
- `CMakeLists.txt` — new `add_test(NAME vertical-slice.metal.access-certificate ...)`.
- `docs/decisions/ADR-025-access-certificate-modes.md` — decision record,
  including the resolved ABI fork (new `ExecutionPlan` field, not a new
  `DeviceHal` virtual method).
- `experiments/definitions/E004-access-certificate.json` — experiment
  definition (`vg.experiment/v1` schema).
- `tests/tools/test_schemas.py` — fixed a latent assumption (`E*.json` glob
  in `experiments/definitions` == the exact Phase A id set) that broke the
  moment a Phase B experiment file existed alongside Phase A's; now asserts
  Phase A's ids are a subset of what's present and validates every `E*.json`
  file found, rather than asserting an exact-match set.
- `tools/vg-exp/vg_exp.py` — same latent assumption in `PHASE_A_DEFINITIONS`
  (an unfiltered glob of `E*.json`); now filtered to only the five ids in
  `PHASE_A_TESTS`, so `vg_exp.py phase-a` ignores Phase B definition files
  instead of rejecting them as "unsupported Phase A experiment id".

## Validation

`ctest --output-on-failure` under the `dev-metal` preset: 21/21 tests
passed (20 pre-existing + the new `vertical-slice.metal.access-certificate`,
0.35s). `core.unit` (`vg_core_test`) passed with the new
`build_access_certificate`/reference-backend assertions included. Running
`vg_metal_task_timeline_test access-certificate <repo_root>` directly shows
all 5 sub-checks passing:

```
access-certificate: certified-pinned ok
access-certificate: universe ok
access-certificate: discover-then-lease ok
access-certificate: software-paged honestly unsupported
access-certificate: fault-managed honestly unsupported
access-certificate: ok
```

`CertifiedPinned` certifies exactly the 1 allocation the probe module
touches; `Universe`/`DiscoverThenLease` both certify the full 2-allocation
arena (the arena is constructed with one untouched allocation specifically
to make this distinction assertable). `DiscoverThenLease` is classified
`HostAssisted` (not `Direct`) in the report, reflecting its real
`std::chrono`-measured host rescan cost.

## Known limits

- **`SoftwarePaged`/`FaultManaged` are not implemented anywhere** — neither
  backend has a real paging or fault-handling mechanism, so both modes
  report `Unsupported` at `compile()` time on both reference and Metal.
  This is the honest result the backend design docs (`06` §12, `07` §10)
  pre-authorize, not a gap to close later without a real underlying
  mechanism.
- **`DiscoverThenLease` degenerates to `Universe`'s full-arena set** — under
  this project's unified-memory model (both reference and Metal on Apple
  Silicon), there is no GPU-resident subset distinct from the arena itself
  for a host rescan to discover a smaller leased set from. This is an
  accurate adapter result (see ADR-025 Alternatives), not an
  under-implemented "discovery" step.
- **Vulkan is untouched** — `vulkan_device_hal.cpp` does not read
  `requested_certificate_mode` at all in this milestone, per ADR-024's
  compile-review-only closure criterion for Vulkan. It is not registered as
  a backend in `E004-access-certificate.json`.
- **The shared 3-backend conformance harness (`tests/conformance/conformance_lib.cpp`)
  was deliberately not extended** with AccessCertificate checks in this
  milestone — doing so would either require implementing
  `requested_certificate_mode` in the untouched Vulkan backend or an
  explicit per-backend opt-out in the shared harness, both out of this
  milestone's approved scope (see ADR-025 Revisit trigger).
