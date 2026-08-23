# ADR-030: Phase C Evidence Policy and E013 Scope

Status: **Superseded in part (2026-08-23)** — premature Phase C closure
claims withdrawn. Retained only as historical evidence-policy notes; do
not treat this ADR as authorizing `gate-closed` while layer-1
(CanonicalView / FacetPool / sample·storage·attachment / representation
transform) is the active deliverable scope.

## Context

`docs/vg-project/12-roadmap-and-risks.md` §4 (lines 42-50) defines Phase C
("Representation and Raster") as: goal, validate "unified semantics +
dedicated facet" feasibility for texture/raster; work items CanonicalView,
facet pool/generation, sample/storage/attachment, representation transform,
RepresentationEpoch, basic raster/software oracle, pipeline classification,
ConsumeInput; exit E005/E008/E013/E016, image correctness, facet stale
tests, peak memory report; stop-check: if every facet use forces an
expensive object/descriptor update, re-evaluate the facet ABI/cache rather
than exposing the texture object back through the public API.

Unlike Phase B (ADR-024), this roadmap text does not itself write "results
on both backends" as a hard requirement, so no deviation ADR is needed here
the way ADR-024 was needed for Phase B's Vulkan-unreachable constraint.
Vulkan hardware remains permanently unreachable on this host (the same
constraint ADR-024 already established); Phase C reuses that established
evidence shape without re-litigating it.

Two research passes this session (Explore agents) established the starting
state precisely: `CanonicalView`/`FacetPool`/`SampleFacet`/`StorageFacet`/
`AttachmentFacet`/texture concepts had zero prior implementation anywhere in
the codebase (core, both real backends, compiler) -- more thorough greenfield
territory than Phase B's experiments, which at least had `DeviceHal`/
`ExecutionPlan`/compute-package scaffolding to extend. By contrast,
`RepresentationEpoch` and ConsumeInput's core semantics already existed and
worked, in `core::Allocation::representation_epoch`, `Arena::transform(...)`
(`in_flight==0` gates epoch advancement), `Arena::consume(...)`, and the
epoch-aware `Arena::lookup(...)` overload -- so E005/E016 are about wiring
already-correct core semantics to real Metal resources and measurement, not
inventing new core semantics.

A second, separate finding requires an explicit governance call rather than
a silent resolution: `docs/vg-project/09-experiment-catalog.md`'s gate table
(around line 242, the "Representation" row) lists only **E005, E008, E016**
as the Phase C gate row. E013 (pipeline classification) is explicitly named
as a Phase C work item in the roadmap text (line 46), but does not appear as
its own row in the gate table -- it is only implicitly covered by Phase E's
catch-all closing statement ("E001-E018 all have results or explicit
Unsupported/Deferred"). This is a real ambiguity between two governing
documents (roadmap work-item list vs. experiment-catalog gate table) and
this ADR resolves it explicitly rather than letting either document's
implicit reading win by default.

## Decision

**1. Evidence shape**: Phase C reuses ADR-024's established evidence
policy verbatim -- real Metal + reference (CPU) hardware results, plus
compile-review-only Vulkan evidence (GLSL analogues that compile and are
reviewed for design symmetry, explicitly labeled `compile-review-only`,
never claimed as executed). No new deviation ADR is required because the
roadmap text itself does not mandate dual-backend execution for Phase C.

**2. E013 scope**: E013 (pipeline/StateBlock classification) is implemented
as a roadmap work item, but is **not** one of the three experiments whose
results gate Phase C's closure. Only E005, E008, and E016 -- the three rows
the experiment-catalog gate table actually lists for "Representation" --
determine whether Phase C is closed. E013's result is still produced,
recorded, and referenced (its own ADR, its own experiment definition, its
own ctest), but a Phase C closure report never blocks on it. This is a
recorded scope judgment, not a silent skip and not a silent promotion to
equal-gate status.

## Alternatives

- Promote E013 to an equal hard gate alongside E005/E008/E016: rejected --
  this would bind Phase C's scope/timeline to an experiment the gate table
  itself does not list, effectively amending `09-experiment-catalog.md` by
  fiat from inside an implementation ADR rather than through an explicit,
  reviewable catalog change.
- Skip E013 entirely since it isn't in the gate table: rejected -- the
  roadmap work-item list names it explicitly (line 46); dropping it because
  a different document's gate table omits it would silently discard a named
  deliverable rather than making a recorded scope call.

## Consequences

Phase C's closure report (`docs/reports/phase-c-gate.md`/`.json`, TASK-C7)
determines closed/open status from E005, E008, and E016 results only. E013's
result is documented in its own ADR (ADR-034) and referenced from the gate
report as a completed-but-non-gating roadmap work item. Future readers of
the gate report see this scope judgment explicitly rather than having to
infer it from the absence of an E013 row in the older catalog table.

## Evidence

This is a governance ADR; its own "evidence" is the two Explore-agent
research passes' findings (zero prior facet/texture implementation;
already-correct `RepresentationEpoch`/ConsumeInput core semantics; the
E013 gate-table omission) that motivated this milestone ordering and scope
decision, plus the plan file (`compressed-bouncing-meerkat.md`) built from
them. TASK-C1 through TASK-C7's own ADRs (ADR-031 through ADR-034) carry
the actual implementation evidence for each experiment.

## Revisit trigger

Revisit if `09-experiment-catalog.md`'s gate table is ever revised to list
E013 explicitly under a gate row, or if Vulkan hardware becomes reachable
on any host used for this project's evidence -- either would change the
inputs this decision was based on.
