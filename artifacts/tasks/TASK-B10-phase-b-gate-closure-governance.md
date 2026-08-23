# TASK-B10: Phase B Gate Closure Governance (ADR-024, Documentation Only)

Status: complete (documentation-only milestone).

Normative docs: `docs/vg-project/12-roadmap-and-risks.md` (Phase B section,
line 31); ADR-023; ADR-024.

## Goal

Turn the user's explicit choice ("M1/Metal 结果 + Vulkan 代码审查级") into a
recorded governance decision that formally adjusts Phase B's practical exit
criterion, without silently rewriting or reinterpreting the roadmap's
original "两后端结果" text. This is the first of eight milestones
(TASK-B10 through TASK-B17) implementing real operational foundations for
E002/E004/E007/E009/E012, per the plan approved in this session.

## Invariants

- This task changes no code, no build configuration, no CTest
  registration — it is governance/documentation only, matching TASK-B9's
  own scope discipline.
- The roadmap's original exit-gate text at `12-roadmap-and-risks.md:31` is
  not deleted or rewritten — an ADR-021-style "Correction" annotation is
  added alongside it, pointing to ADR-024, following the precedent ADR-021
  itself established when correcting its own prior framing.

## Files

- `docs/decisions/ADR-024-phase-b-gate-closure-metal-reference-vulkan-compile-review.md` —
  the governance decision itself (six-section ADR).
- `docs/vg-project/12-roadmap-and-risks.md` — Correction annotation added
  after the Phase B exit-gate line.
- `docs/reports/phase-b-gate.md`/`.json` — new "关闭标准（ADR-024）" section /
  `gate_closure_criterion` object added, pointing at ADR-024; existing
  `not_started` experiment statuses and `mechanism-complete-experiments-not-started`
  top-level status are left unchanged, to be updated by TASK-B17 once the
  five experiment milestones actually land.

## Validation

No executable validation for the ADR/roadmap-annotation edits (documentation
change). `docs/reports/phase-b-gate.json` checked for valid JSON syntax
(`python3 -c "import json; json.load(...)"` — passed). No source, build, or
CTest configuration was touched, so the existing ctest suite is unaffected
by this task.

## Known limits

This task does not implement any of E002/E004/E007/E009/E012 — it only
establishes the closure criterion those five experiments' implementation
milestones (TASK-B11 through TASK-B17) will be measured against. The
`phase-b-gate.md`/`.json` per-experiment statuses remain `not_started` until
their respective milestones complete.
