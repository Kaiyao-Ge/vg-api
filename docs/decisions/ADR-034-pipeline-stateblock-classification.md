# ADR-034: Compute Pipeline StateBlock Classification (No Render PSO Yet)

Status: **Withdrawn / historical** (2026-08-23) — written under an over-scoped Phase C closure attempt; not authoritative for the layer-1 CanonicalView/FacetPool/facets/transform deliverable.

## Context

E013 asks whether Node/StateBlock classification reduces meaningless
pipeline permutations (`09-experiment-catalog.md` E013). ADR-030 already
recorded that E013 is a roadmap work item but **not** a hard Phase C gate
row (the catalog Representation gate lists only E005/E008/E016).

This codebase has compute command encoders and several ad hoc
`MTLComputePipelineState` slots (`ensure_pipeline`,
`ensure_effect_dag_pipeline`, cull/compact, sample-facet) but **no render
command encoder and no raster PSO**. A full material/format/sample-count
raster specialization matrix would invent infrastructure the product does
not have yet.

## Decision

**Scope: compute only.** Introduce `StateBlockKind` /
`StateBlock` / `PipelineClassificationResult` on the Metal vertical-slice
surface and a standalone `run_pipeline_classification` that compiles a
tiny function-constant-specialized compute kernel across a parameterized
matrix.

**Classification rules** (mirroring the catalog judgement):
- `PipelineKeyState` → enters the PSO cache key (realized as a Metal
  function constant).
- `MetalDynamicState` / `ShaderVisibleData` → encode-time bindings; must
  **not** enter the cache key under classification.
- `UnsupportedNeedsConversion` → rejected with an error, never silently
  folded into a key.

**Naive vs classified**: naive full permutation compiles one PSO per
`(key, dynamic, shader_data)` triple; classified caches only on `key`.
The vertical-slice matrix is 2×2×2 → naive 8 / classified 2 / hits 6 /
misses 2.

**E013 does not gate Phase C closure** (ADR-030). Results are still
produced and recorded in `phase-c-gate.md` as a non-blocking row.

## Alternatives

- Invent a render PSO path solely to satisfy E013's catalog wording about
  materials/sample counts: rejected -- out of proportion; no AttachmentFacet
  / render encoder exists (ADR-031 explicitly deferred that).
- Skip E013 entirely because it is not in the gate table: rejected --
  roadmap Phase C work items name it; ADR-030 chose "implement but
  non-blocking".
- Promote E013 to an equal hard gate: rejected -- ADR-030.

## Consequences

- Demonstrates that classification reduces compute PSO count on a real
  Metal device without claiming raster specialization coverage.
- Future raster/AttachmentFacet work can reuse the same StateBlock kind
  taxonomy once a render encoder exists.
- Vulkan remains compile-review-only (ADR-030).

## Evidence

- `experiments/definitions/E013-pipeline-classification.json`
- `src/backends/metal/metal_device_hal.{h,mm}` (`run_pipeline_classification`)
- ctest `vertical-slice.metal.pipeline-classification`
- ADR-030 (E013 scope judgement)

## Revisit trigger

Revisit when a render command encoder / AttachmentFacet path exists and
a real material×format×sample-count matrix can be measured, or if the
catalog gate table is revised to make E013 a hard Representation gate.
