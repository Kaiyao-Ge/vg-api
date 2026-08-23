# TASK-B9: Phase B Experiment Gate: Status and Gap Analysis

Status: documentation-only, no implementation this milestone.

Normative docs: `docs/vg-project/12-roadmap-and-risks.md` (Phase B section);
`docs/vg-project/09-experiment-catalog.md` (gate table, lines 236-243);
ADR-023

## Goal

Reconcile two different meanings of "Phase B complete" that this project
now has on record: TASK-B7/TASK-B8's mechanism-level completion (Tier0/
timeline/Tier0+Tier1, hardware-verified on reference+Metal, compile-review
on Vulkan) versus the roadmap's own experiment-based exit gate
(E002/E004/E007/E009/E012 producing results on Metal/Vulkan). This task
records that the latter is not met, why, and what each experiment
concretely needs -- as a punch list for whichever future milestone picks
this up, not as work performed now.

## Invariants

- This task changes no code, no build configuration, no CTest
  registration. It does not rewrite TASK-B7/TASK-B8's own status
  descriptions, which remain accurate for the milestone scope they
  described.
- No performance, feasibility, or timeline claim is made about any of the
  five gated experiments -- only what current code does and does not
  support, verified by reading it.

## Files

- `docs/reports/phase-b-gate.md` / `.json` -- gate status report, mirroring
  the `docs/reports/phase-a-gate.md`/`.json` pattern established by
  TASK-012.
- `docs/decisions/ADR-023-phase-b-experiment-gate-findings.md` --
  per-experiment gap findings and revisit triggers.

## Validation

No executable validation -- this is a documentation cross-check task.
Performed: `docs/reports/phase-b-gate.md`'s gate table checked word-for-word
against `docs/vg-project/09-experiment-catalog.md` lines 236-243 (plus
E004/E009's own definitions at lines 52-62 and 116-126); ADR-023's Decision
section checked against the same session's Explore-agent findings on
`src/core/core.h`, `src/ir/ir.cpp`, `src/backends/metal/metal_device_hal.mm`,
and `src/backends/device_hal.h`; a follow-up repo-wide `grep -rn` pass
confirmed zero matches for `AccessCertificate`/`CertifiedPinned`/
`DiscoverThenLease`/`SoftwarePaged`/`FaultManaged`/`Universe` (E004) and
`cull`/`compact` (E009) under `src/`/`include/`; cross-references between
this task, the gate report, and ADR-023 confirmed consistent (file paths
and ADR/TASK numbers match).

## Known limits

Full per-experiment gap list (mirrors ADR-023 Decision section). All five
show the same pattern: the required core capability has zero
implementation, not a partial/stub version.

- **E002 (typed pointer graph)**: needs a new IR opcode plus pointer-chasing
  codegen on both backends -- `core::Arena`/`GraphEpoch`/`PointerRef` are
  flat blob/reachability-set structures today, with no typed adjacency
  graph; the IR has only `load/store/atomic_add/publish`.
- **E004 (adapter memory policy)**: `CertifiedPinned`/`DiscoverThenLease`/
  `Universe`/`SoftwarePaged`/`FaultManaged` are all unimplemented; a
  repo-wide `grep -rn` under `src/`/`include/` returns zero matches for any
  of the five names -- none exist in any form.
- **E007 (root pointer vs. binding cost)**: needs a bindless/argument-buffer
  codegen path; only direct root-schema binding codegen exists today.
- **E009**: needs a GPU cull/compact kernel, which does not exist on either
  backend (zero `grep` matches for `cull`/`compact`); additionally blocked
  on Metal Tier1 (deferred, ADR-021) and on Vulkan hardware remaining
  unreachable on this project's development machine.
- **E012 (Effect DAG/timeline sync quality)**: needs GPU/CPU timing
  instrumentation (none exists anywhere in the codebase today) and a
  `submit()` path capable of genuine multi-encoder concurrency -- Metal's
  `submit()` is currently always exactly one encoder per one command
  buffer, fully serial via `waitUntilCompleted`, with zero fence/barrier
  usage anywhere.

This punch list is the intended starting point for the next milestone that
takes on any of these experiments -- it should not need to be re-derived by
reading the code again.
