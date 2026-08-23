# ADR-024: Phase B Gate Closure — Metal+Reference Hardware Results, Vulkan Compile-Review (Permanent Host Constraint)

Status: Accepted

## Context

`docs/vg-project/12-roadmap-and-risks.md:31` defines the Phase B exit gate
as: "E002/E003/E007/E009/E012 两后端结果；无隐藏 host wait；capability
rejection 完整" — E002/E004/E007/E009/E012 (per the authoritative gate table
in `09-experiment-catalog.md:236-243`; the roadmap's own text says E003
where the catalog says E004, an established discrepancy, E004 is correct)
producing results on **两后端** (both backends) — Metal and Vulkan.

ADR-023 and TASK-B9 recorded that none of these five experiments have any
implementation today. `docs/reports/phase-b-gate.md`/`.json` currently carry
status `mechanism-complete-experiments-not-started`.

Vulkan hardware is permanently unreachable on this project's development
machine — not a scheduling gap, a standing constraint reconfirmed multiple
times this project (including a failed SSH attempt to a remote host that the
user explicitly chose not to continue pursuing). This is not new: ADR-021/022
already established that Vulkan Task Tier0/Tier1/timeline work proceeds
compile-review-only — implemented, reviewed, honestly labeled
`compile-review-only`, never presented as execution evidence — while Metal
and reference carry the real hardware-verification burden. Phase B's gate
experiments face the identical constraint at a larger scale.

The user has explicitly chosen, when presented with this tension, to build
real Metal+reference implementations and results for all five gate
experiments, with Vulkan staying compile-review-only per the ADR-021/022
precedent, rather than waiting for Vulkan hardware or leaving the gate
permanently unclosed.

## Decision

1. **Phase B's practical exit gate is satisfied by**: (a) real,
   hardware-verified Metal+reference results for E002/E004/E007/E009/E012,
   and (b) Vulkan-side compile-review-only evidence for the same five
   experiments — code that compiles, has been read and reasoned about, and
   is explicitly labeled `compile-review-only` in every report and result
   artifact. Vulkan compile-review evidence is never treated as, or reported
   alongside, execution evidence without that label.

2. **This is a documented deviation from the roadmap's literal "两后端结果"
   text, not a silent reinterpretation.** `12-roadmap-and-risks.md:31` is
   annotated with an ADR-021-style "Correction" note pointing to this ADR —
   the original text is not rewritten or deleted, matching the precedent
   ADR-021 itself set when correcting its own prior Tier1/ICB framing.

3. Each of the five experiments gets its own implementation-specific ADR
   (ADR-025 through ADR-029, one per experiment/milestone) as the actual
   engineering lands — this ADR is the governance decision that licenses
   that work and sets its evidentiary bar; it does not itself describe any
   experiment's implementation.

4. `docs/reports/phase-b-gate.md`/`.json` are updated to a new status string
   once the milestones implementing E002/E004/E007/E009/E012 land (tracked
   as TASK-B17), replacing `mechanism-complete-experiments-not-started` with
   a status reflecting real per-experiment results plus explicit Vulkan
   compile-review labeling.

## Alternatives

- **Wait for Vulkan hardware to become reachable before closing Phase B**:
  rejected. This is a permanent constraint on this development machine, not
  a temporary scheduling gap — waiting has no defined end condition and
  would leave Phase B's gate unclosable indefinitely.
- **Software-emulate Vulkan execution (e.g. lavapipe, SwiftShader) to
  produce "results"**: rejected. A software Vulkan implementation does not
  represent real GPU hardware behavior for the timeline/Tier0/Tier1/
  capability-rejection questions these experiments are designed to answer;
  presenting emulated output as hardware evidence would violate this
  project's anti-dishonest-degradation invariants (`docs/START.md` §4) more
  directly than being explicit about the gap ever would.
- **Redefine Phase B as Metal-only, dropping Vulkan entirely from the exit
  gate**: rejected. This would discard the real design-review value Vulkan's
  compile-review-only work has already produced (ADR-017, ADR-022) and lose
  the cross-backend comparison the experiment catalog is designed around,
  for no benefit over labeling the Vulkan side honestly instead.
- **Silently treat B7/B8's mechanism-level completion as sufficient without
  reconciling it against the roadmap's own experiment gate**: rejected —
  this is the exact failure mode ADR-023 was written to prevent; it would
  let two incompatible definitions of "Phase B done" coexist unexamined.

## Consequences

Phase B's closure criterion is now precisely defined and asymmetric by
design: Metal and reference carry the hardware-verification burden, Vulkan
carries a compile-review burden. This mirrors the asymmetry ADR-016/017
already established for the vertical-slice work and ADR-021/022 established
for Task Tier0/Tier1 — it is a consistent project-wide pattern, not a
one-off exception invented for this milestone. Every future report,
experiment result, and ADR referencing these five experiments' Vulkan side
must carry the `compile-review-only` label; omitting it would misrepresent
the evidence.

## Evidence

This ADR changes no code. It is a governance decision made after the user,
presented with the tension between ADR-023's findings and the roadmap's
literal both-backend gate text, explicitly selected "M1/Metal 结果 + Vulkan
代码审查级" as this project's practical Phase B closure path over the
alternatives above.

## Revisit trigger

Revisit if Vulkan hardware becomes reachable on this project's development
machine (or an equivalent real-hardware access path opens) — at that point,
re-evaluate whether the five gate experiments' Vulkan side should be
upgraded from compile-review-only to real hardware verification, and whether
`docs/reports/phase-b-gate.md`/`.json`'s status should change accordingly.
