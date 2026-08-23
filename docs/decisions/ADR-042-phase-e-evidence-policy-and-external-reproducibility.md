# ADR-042: Phase E Evidence Policy and External Reproducibility

Status: Accepted

## Context

`docs/vg-project/12-roadmap-and-risks.md` §6 defines Phase E as
Research Alpha: external researchers can reproduce, evaluate, and
extend. Named products: a versioned C ABI; compiler/tool binaries;
Metal/Vulkan adapters; conformance; status for E001–E018; sample
workloads; complete run bundles; an architecture paper/report. Exit:
every P0 risk closed or given an explicit scope limit; documentation
matches the code; another Agent from a clean checkout can complete
build → conformance → one benchmark.

Phase D recorded a HostAssisted boundary list and NativeContractResearch
v1 (ADR-041). That close is research-recorded, not product-closed, and
is not an automatic entry into Phase E. Phase C remains `not-closed`
(layer1 only). Eighteen experiment definitions already exist and are
scattered across A/B/C/D gates. There is no unified E001–E018 table,
no P0 register, no external runbook, and no architecture report.

Vulkan hardware remains permanently unreachable on this host (ADR-024).
Inventing measurements is forbidden (ADR-035/041). Public ABI stays
tokens-only. `docs/vg-project/*` original sentences are not rewritten.

## Decision

**1. Research Alpha is aggregation, reproducibility, and consistency.**
Phase E does not add a new semantic object, a new public C function,
or a new lowering path. It publishes what A–D already recorded so a
second reader can rebuild and judge.

**2. Evidence shape is ADR-024 verbatim.** Real Metal + reference
results; Vulkan compile-review-only, never counted as `passed`.
`HostAssisted`, `EmulatedDevicePass`, `Unsupported`, `Deferred`, and
`unmeasured` remain legal rows. A single-PSO ICB wrapper or a host
walk relabeled `DevicePass` is still forbidden.

**3. E001–E018 rows may only cite existing evidence.** Each row is a
gate citation, `Unsupported`, `Deferred`, or `unmeasured`. E004 keeps
both files: B-era `E004-access-certificate.json` is historical;
`phase-e` loads only `E004-discovery-revisit.json`. Phase C experiments
may appear as implemented while the Phase C gate stays `not-closed`.

**4. Public C ABI remains v1.0.** `vgGetApi` / `VG_API_VERSION_1_0` is
the versioned surface. The research surface is C++ DeviceHAL plus
`vg-exp`. The ABI compatibility report states that fact; it does not
invent a larger C API.

**5. One benchmark, evidence grade P0.** `10-validation-and-benchmarks.md`
§13: P0 means a demo / single sample that shows the command runs. The
benchmark wraps an existing vertical slice, records host wall-clock
only, and does not invent `gpu_ns`, hitch, or break-even points.

**6. P0 risks are closed or scoped.** A register maps each 12 §7 P0
row to an owner, existing tests, and an explicit limit when the risk
is not retired (for example: Task publication has a host bounded
model and no GPU litmus).

**7. Roadmap text.** §6's original exit sentence is not rewritten. A
Correction points here. Research Alpha is recorded, not product-closed.
Compiler/tool binaries remain in-tree CMake targets, not a packaged
release. Sample workloads remain the existing fixtures.

**8. Parallel work after this ADR.** TASK-E1 `phase-e` runner and
18-row gate; TASK-E2 P0 register; TASK-E3 external runbook;
TASK-E4 honest Phase C status and `tooling.phase-c-runner`;
TASK-E5 architecture report skeleton; TASK-E6 P0 benchmark command;
TASK-E7 Research Alpha gate record.

## Alternatives

- Treat ADR-041 as Phase E entry: rejected — D forbade that.
- Close Phase C by relabeling layer1 as the full exit: rejected —
  ADR-030's written exit still names raster / classification /
  ConsumeInput as a whole, and peak bytes remain unmeasured.
- Expand the public C ABI to look complete: rejected — that is a
  semantic/API change, not Research Alpha aggregation.
- Invent break-even or hitch curves so the 12 §6 product list looks
  full: rejected — 01 §5.4 counts an honest negative as success.

## Consequences

Another Agent can take `docs/reports/phase-e-gate.md`, the P0 register,
the external runbook, and the architecture skeleton and say what VG
has shown on existing APIs and what remains scoped. NativeContractResearch
does not start a driver milestone from this ADR.

## Evidence

- `docs/reports/phase-e-gate.md` / `.json`
- `docs/reports/p0-risk-register.md` / `.json`
- `docs/reports/external-repro-runbook.md`
- `docs/reports/architecture-research-alpha.md`
- `tools/vg-exp/vg_exp.py` `phase-e` and `benchmark`
- TASK-E0 through TASK-E7

## Revisit trigger

Revisit if Vulkan becomes executable, if a measured break-even curve
exists, if the public C ABI grows a new version, or if a P0 scope
limit is retired by a real test (not a relabel).
