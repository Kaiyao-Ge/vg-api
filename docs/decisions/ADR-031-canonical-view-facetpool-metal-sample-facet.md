# ADR-031: CanonicalView + FacetPool, and Real Metal SampleFacet

Status: **Withdrawn / historical** (2026-08-23) — written under an over-scoped Phase C closure attempt; not authoritative for the layer-1 CanonicalView/FacetPool/facets/transform deliverable.

## Context

ADR-030 establishes Phase C's governance frame. This ADR covers its first
two milestones: TASK-C1 (`CanonicalView`/`FacetPool` core infrastructure,
no independent ADR was written for it, per the same precedent as TASK-B12
folding into ADR-026) and TASK-C2 (E008 -- the first real facet lowering).

Before this milestone, `CanonicalView`/`FacetPool`/`SampleFacet`/
`StorageFacet`/`AttachmentFacet`/texture concepts had zero implementation
anywhere in this codebase: not in `core/core.h`/`.cpp`, not in either real
backend, not in the compiler. This is a strictly larger gap than any single
Phase B experiment faced -- Phase B's five experiments all extended
existing `DeviceHal`/`ExecutionPlan`/compute-package scaffolding; E008
requires introducing an entire new resource class (textures/samplers) that
the existing buffer-only IR taxonomy and `ExecutionPlan` ABI were never
designed to express.

`02-principles-and-semantics.md` §3.3 states a Region generates
AddressFacet/SampleFacet/StorageFacet/AttachmentFacet/TransferFacet per
usage via a `CanonicalView`, and explicitly warns against two shortcuts:
folding every facet into one maximal ViewRecord, and mutating an active
reference's slot in place. `03-system-architecture.md` §10 additionally
requires GPU-visible capability references to use an index+generation
table, matching the project's existing `Arena`/`FacetPool`(new)/
`AccessCertificate` convention.

TASK-B13 (ADR-026) already established the precedent this milestone
follows for E008's device-side mechanism: `run_cull_compact` is a fully
standalone `metal::DeviceHal` method with its own dedicated pipeline slot,
its own ad hoc buffer allocation, called directly by its vertical-slice
test -- entirely bypassing `hal::ExecutionPlan`/`compile()`/`submit()`.
Mid-implementation this session, an initial design attempt for
`run_sample_facet` instead added `ExecutionPlan::requested_sample_facet` (an
`std::optional<CanonicalView>` field), following the `requested_certificate_
mode`/`request_indexed_binding` precedent used for *modal variations of the
existing linear/pointer-graph pipeline* (ADR-025, TASK-B16). Re-reading
ADR-026 clarified this was the wrong precedent to follow: `run_cull_compact`
is the established shape for *introducing an entirely new resource kind*,
not the `ExecutionPlan`-field shape used for varying behavior on an
already-existing pipeline. This ADR's design was corrected mid-session to
follow `run_cull_compact`'s shape before any test was written against the
first draft.

## Decision

**FacetPool is an independent index+generation pool, not a reuse of
`Arena`'s allocation id space.** `core::FacetSlot{generation, active, kind,
view, representation_epoch}` lives in `FacetPool`'s own `slots_`/
`free_list_`, mirroring `Arena`'s allocation table structurally but kept
fully separate -- consistent with `02-principles-and-semantics.md` §3.3's
explicit warning against folding all facets into one ViewRecord, and with
the already-established precedent of `GraphEpoch`/`PointerGraph` existing
as separate parallel epoch concepts rather than being unified.
`FacetPool::acquire(arena, view, kind, &ref)` snapshots the view's backing
allocation's current `representation_epoch` into the new slot;
`FacetPool::lookup(arena, ref)` returns `nullptr` once that allocation's
live `representation_epoch` no longer matches the snapshotted one -- the
"facet generation vs. epoch = stale token" check named in
`02-principles-and-semantics.md` §10.

**`CanonicalView`/`FacetKind`/`PixelFormat`/`ViewDimension` cover only what
E008 needs.** `FacetKind{Address, Sample, Storage, Attachment, Transfer}`
names all five facet kinds `02-principles-and-semantics.md` §3.3 lists, but
only `Sample` is exercised by any current experiment. `PixelFormat{
RGBA8Unorm, R32Float}`/`ViewDimension{Texture2D, Texture2DArray}` are
deliberately minimal -- no compressed formats, no cube maps -- because
`09-experiment-catalog.md`'s E008 workload only calls for "2D/array, common
formats, nearest/linear filtering." Extending this enum set is deferred
until a future experiment actually requires it, not designed in ahead of
need.

**`FilterMode{Nearest, Bilinear}`/`WrapMode{Clamp, Repeat}` are kept
separate from `CanonicalView`**, not embedded as view fields: a view names
a resource's shape/format contract; a sampler policy names how it is read,
and the two vary independently per dispatch (the same view can be sampled
nearest in one call and bilinear in another). Folding sampler policy into
`CanonicalView` would force a new view (and a new facet-pool acquire) for
every filter/wrap combination, which is not how any real GPU API models
this relationship.

**SampleFacet's device-side mechanism is a standalone `metal::DeviceHal::
run_sample_facet(...)` method, entirely bypassing `hal::ExecutionPlan`/
`compile()`/`submit()`** -- directly following the `run_cull_compact`
precedent (ADR-026), not the `ExecutionPlan`-field precedent. Texture/
sampler binding is a resource class the existing buffer-only IR taxonomy
was never designed to express, matching `run_cull_compact`'s own rationale
for bypassing the generic ABI (an operation orthogonal to the task-graph
publish/submit flow). `SampleFacetResult{sampled_rgba, facet_cache_hit,
descriptor_write_count}` mirrors `CullCompactResult`'s shape.

**One deliberate deviation from `run_cull_compact`'s one-shot pattern: a
persistent facet cache.** `run_cull_compact` allocates fresh buffers on
every call (it never needs to demonstrate reuse). E008's checkpoint
specifically requires demonstrating a cache hit across repeated calls
against the same `CanonicalView`+`RepresentationEpoch`, so `metal::DeviceHal
::Impl` holds a persistent `facet_map` (`std::unordered_map<uint64_t,
MetalFacetRecord>`, keyed on the backing allocation id, mirroring
`allocation_map`) and a small `sampler_cache` (keyed on `(filter, wrap)`,
of which only 4 real combinations ever occur). `ensure_texture(...)`
mirrors `ensure_buffer()`'s lazy-create/invalidate pattern, but invalidates
on **generation or representation_epoch mismatch** (not just
width/height/format mismatch) -- the same rule `core::FacetPool::lookup`
enforces. `SampleFacetResult::facet_cache_hit` reports whether an existing,
still-valid `MTLTexture` was reused with no new device-level texture object
or `replaceRegion:` byte upload -- the expensive operation the roadmap's
Phase C stop-check (`12-roadmap-and-risks.md:50`) warns about --
independent of `descriptor_write_count`, which reports the per-dispatch
`setTexture:`/`setSamplerState:` encoder binding calls that must happen on
every dispatch regardless of cache state (a fresh encoder always needs its
bindings reissued; this is not itself an expensive-recreation signal).

**Sampling is issued through the existing `MTLComputeCommandEncoder`, not a
new render pipeline.** The codebase has no render command encoder anywhere;
building one for a single sampling-readback experiment would be scope creep
into the roadmap's much larger "basic raster/software oracle" work item,
which this milestone does not claim to cover. `compiler::
sample_facet_metal_source()`/`sample_facet_vulkan_source()` are standalone
hand-written kernel strings (mirroring `cull_compact_metal_source()`'s
"independent hand-written kernel + dedicated pipeline" precedent, not a new
generalized IR opcode with a single consumer): one thread per uv
coordinate, sampling `texture2d<float, access::sample>` through a bound
`sampler` and writing a `float4` to an output buffer. The GLSL analogue
deliberately binds `sampler2D` through a classic descriptor set rather than
buffer_reference/push-constants (unlike this project's other
`*_vulkan_source()` functions) -- combined image samplers are always
descriptor-set bound in Vulkan regardless of BDA use elsewhere, so a
push-constant-only scheme would misrepresent the kernel's real binding
model. It is compile-review-only (ADR-024/030); not wired into
`vulkan_device_hal.cpp`.

**Reference oracle: a pure CPU `reference::sample_facet(...)` function**,
reading the backing allocation's bytes directly out of `core::Arena` (no
facet/GPU resource involved), using the standard hardware texture-sampling
convention shared by Metal/D3D/Vulkan: Nearest = `floor(uv * size)` with no
half-texel offset; Bilinear = `uv * size - 0.5`, floor+frac, bilinear-blend
across the four neighbor texels. `RGBA8Unorm` channels are returned as
float in `[0,1]`; `R32Float`'s single channel is returned as `{value, 0, 0,
1}`. Both filter modes' wrap-mode texel-index handling (`wrap_index`) is
shared between Clamp (edge-clamp) and Repeat (modulo, made non-negative).

## Alternatives

- Route SampleFacet through `ExecutionPlan::requested_sample_facet` (the
  initial in-session design): rejected once ADR-026's precedent was
  re-read carefully -- that field-based shape is for modal variation of an
  *existing* pipeline (ADR-025's `requested_certificate_mode`), not for
  introducing an entirely new resource kind, which is exactly what
  `run_cull_compact`'s standalone-method shape already exists to handle.
- Fold `FilterMode`/`WrapMode` into `CanonicalView` as fields: rejected --
  sampler policy and view identity vary independently per dispatch; folding
  them together would force a new facet-pool acquire per filter/wrap
  combination for no benefit.
- Reuse `Arena`'s allocation id space for facet slots instead of a separate
  `FacetPool`: rejected -- explicitly contradicted by
  `02-principles-and-semantics.md` §3.3's "cannot fold every facet into one
  maximal ViewRecord" constraint, and inconsistent with the established
  `GraphEpoch`/`PointerGraph` parallel-epoch precedent.
- Build a render pipeline / `AttachmentFacet` to support sampling: rejected
  as scope creep -- E008 does not require attachments or rasterization, and
  the roadmap's "basic raster/software oracle" work item is intentionally
  larger and separate.
- Introduce a general-purpose IR `sample` opcode consumed by a generic
  compute package instead of a hand-written kernel: rejected -- texture/
  sampler binding is a different resource class than the buffer-only IR
  taxonomy every existing `ComputePackage` compiles, and a generalized
  opcode with exactly one consumer is premature generalization, the same
  judgment already made for `cull_compact_metal_source()`.

## Consequences

Phase C now has its foundational facet data structure (pure `core`, no
backend coupling) and its first real device-backed facet: a real
`MTLTexture`/`MTLSamplerState` pair, sampled through the existing compute
encoder, matching a CPU oracle within float tolerance, with a persistent
per-allocation cache whose hit/miss state is directly observable via
`SampleFacetResult::facet_cache_hit`. TASK-C4 (E005) can now build its
linear-to-sample-optimal transform on top of this real facet
infrastructure rather than a stub. The `run_cull_compact`-style
standalone-method precedent is now confirmed (by a second independent use)
as this project's convention for introducing a wholly new resource kind
without touching the fixed `compile()`/`submit()` ABI.

## Evidence

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`vertical-slice.metal.sample-facet` builds a 2x2 RGBA8Unorm allocation with
four distinct known texel colors, samples all four texel centers with
Nearest filtering (exact match against `reference::sample_facet` within
1/255 + 1e-4 tolerance) via a real `MTLTexture`/`MTLSamplerState`/compute
dispatch, then samples the shared corner `(0.5, 0.5)` with Bilinear
filtering (matching the CPU oracle's four-way blend within 1/255 + 1e-3
tolerance). The Bilinear call reuses the same `CanonicalView` (same
allocation id/generation, unchanged `representation_epoch`) as the first
call and asserts `SampleFacetResult::facet_cache_hit == true`, proving
`ensure_texture()` reused the cached `MTLTexture` rather than recreating
and re-uploading it. `ctest --output-on-failure` under `dev-metal`: 28/28
tests passed, including all 24 pre-existing tests unchanged.

## Revisit trigger

Revisit if a future milestone needs `AttachmentFacet`/`StorageFacet` or
array/mip textures for real (not just as unused `CanonicalView` fields) --
at that point a render pipeline and/or a broader `PixelFormat` set would
need to be designed for real, rather than deferred. Revisit if E008-style
facet caching is found to force an expensive object/descriptor update on
every use in a later, larger workload -- per the Phase C stop-check
(`12-roadmap-and-risks.md:50`), that would require re-evaluating the facet
ABI/cache design rather than exposing the texture object back through the
public API.
