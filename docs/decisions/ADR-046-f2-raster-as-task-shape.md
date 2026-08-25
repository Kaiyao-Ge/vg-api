# ADR-046: F2 — Rasterization as a Task/ExecutionPlan Shape

Status: Accepted

## Context

ADR-043 opened Phase F ("Raster SDK"). Its Decision #3 states rasterization
must be "added as an optional shape of Task/ExecutionPlan, not a parallel
API": `run_raster_triangles`'s already-verified Metal hardware path is to be
"moved -- not rewritten -- so that `compile()`/`submit()` recognize a raster
pass and build a real `MTLRenderPipelineState` through it," with a
raster-shaped task record gaining `index_count`/`topology`/
`vertex_buffer_ref`/`index_buffer_ref` fields, and the existing
`Capability::Raster` bit "finally read by a real task type instead of
sitting unused." F1 (ADR-045) reached `core::ExecutionResult` through the
public C ABI (`getSubmissionExecutionResult`, `VG_API_VERSION_1_2`), closing
the v1.1 execution-outcome gap but leaving rasterization exactly where
ADR-043 found it.

Before F2, `run_raster_triangles` (Metal, `src/backends/metal/
metal_device_hal.h/.mm`) and `raster_triangles` (CPU reference oracle,
`src/backends/reference/reference_executor.h/.cpp`) were reachable only by
direct method call -- `tests/vertical_slice/metal_task_timeline_test.cpp`'s
`basic-raster` mode calls `DeviceHal::run_raster_triangles` directly, and the
reference oracle is called the same way from `tests/unit/
reference_raster_test.cpp`. Neither path went through `TaskGraph`/
`ExecutionPlan`/`compile()`/`submit()`. `core::TaskRecord`
(`src/core/core.h`) had only an x/y/z compute-dispatch shape. `Capability::
Raster` was already unconditionally advertised on both backends'
`capabilities()` snapshots (`src/backends/metal/metal_device_hal.mm:156`,
`src/backends/reference/reference_device_hal.cpp:31`) but never read by any
real task type -- the bit existed ahead of the feature it names.

## Decision

Three sub-decisions, settled during Plan Mode and implemented as described
below.

**1. Extend `core::TaskRecord` directly; do not add a parallel
`ExecutionPlan.raster_passes` list.** `TaskRecord` (`src/core/core.h:411`)
gains: `TaskKind kind` (`enum class TaskKind : uint32_t { Compute, Raster
};`, `core.h:250`, default `Compute` so every pre-F2 caller is unaffected),
`Topology topology` (`enum class Topology : uint32_t { TriangleList };`,
`core.h:255` -- the only value F2 supports), `RasterFacetPair raster_facets`,
`FacetRef vertex_buffer_ref`, `FacetRef index_buffer_ref`, `uint32_t
index_count` (>0 signals an indexed draw was requested), `FilterMode
raster_filter`, `WrapMode raster_wrap`, and `std::array<float, 4>
raster_tint`. Rationale: `TaskGraphBuilder::append` already takes a whole
`TaskRecord` by value, so no signature changes are needed anywhere in the
publication path, and a raster task automatically gets `add_dependency`/
`add_effect`/`set_quota`/`append_published`/`seal()` for free. A separate
`raster_passes` list would permanently exclude raster tasks from the
dependency graph, closing off future milestones (F6 per-frame SceneRoot, F9
frames-in-flight) that will need compute<->raster cross-dependencies inside
one graph.

**2. Build the Metal render pipeline lazily at `submit()` time, reusing the
existing `ensure_raster_pipeline` cache unchanged.** `compile()` has no
`Arena` parameter, and the new raster fields are `FacetRef`s (not inlined
`CanonicalView`s the way `RepresentationRequest::view` is), so a pixel
format cannot be resolved at compile time without `Arena`/`FacetPool`. The
render pipeline is therefore still built where `ensure_raster_pipeline`
(`metal_device_hal.mm:1536`) already builds it -- inside the submit-time
raster path -- rather than moved earlier.

**3. Expose only a "neutral subset" of attachment configuration on
`TaskRecord`.** `FilterMode`/`WrapMode`/tint go public (`raster_filter`,
`raster_wrap`, `raster_tint`) -- these are already core-level types
(`core::FilterMode`, `core::WrapMode`), and tint is plain shader-read data.
`AttachmentLoadAction`/`AttachmentStoreAction`/`clear_rgba`/`sample_count`/
`AttachmentSubresource` (`AttachmentFacetDesc`, `metal_device_hal.h:134`,
mirrored at `reference_executor.h:181`) stay backend-private with fixed F2
defaults (Clear / Store / `{0,0,0,1}` / 1 / `{0,0}`). Rationale: promoting
the full attachment config to `TaskRecord` would be exactly the "adapter
feature upgraded to core minimum capability" anti-pattern `docs/START.md`
§5 rules out ("不把 adapter 特性升级成核心最低能力").

## Implementation

`src/backends/device_hal.h` gains a backend-neutral `hal::RasterTaskResult`
struct (`task_index`, `resolved_rgba`, `width`, `height`, `stored`,
`contents_defined`, `device_hal.h:270`) and `Submission::raster_results`
(`device_hal.h:360`) -- one entry per Raster-kind task that actually ran, in
`task_graph.tasks()` order, empty when the plan carried no raster task
(same convention `published_tasks` already uses for a plan with no
task_graph).

**Metal** (`src/backends/metal/metal_device_hal.mm`): `run_raster_triangles`'s
non-buffer-build logic (facet resolution/validation, `ensure_raster_pipeline`,
render-pass/encode/draw/readback) was extracted verbatim into a new private
`Impl::run_raster_pass` helper (`metal_device_hal.mm:1714`), parameterized on
already-built `id<MTLBuffer>` vertex/tint buffers instead of a host vertex
vector. `run_raster_triangles`'s public signature is unchanged
(`metal_device_hal.mm:3584`); it now just builds the two buffers and
delegates to `run_raster_pass`. `CompileOps::reject_unsupported`
(`metal_device_hal.mm:2148`) gained an `index_count > 0` rejection
(`metal_device_hal.mm:2167`) that fails with an `Unsupported` LoweringEvent,
`"raster_task"`, `"indexed raster draws deferred to F5"`
(`metal_device_hal.mm:2169`). A new `SubmitOps::raster`
(`metal_device_hal.mm:2464`, called from `submit()` at
`metal_device_hal.mm:2877`) runs after `stage5` -- a plain `bool`, not the
`Flow` short-circuit convention used later in the submit chain, since a
raster task must not prevent the rest of a mixed compute+raster graph from
submitting. It resolves `vertex_buffer_ref` via the same `ensure_facet_buffer`
helper `run_address_facet` (`metal_device_hal.mm:2985`) already uses
(GPU-resident, no host round-trip), then calls `Impl::run_raster_pass`.

**Reference** (`src/backends/reference/reference_device_hal.cpp`):
`raster_triangles` (both overloads, in `reference_executor.h/.cpp`) is left
completely unchanged. `compile()` gained the same `index_count > 0`
rejection (`reference_device_hal.cpp:110-113`), mirroring the existing
`consume_input` rejection's exact style (`reference_device_hal.cpp:92-98`).
`submit()` gained raster-task execution inserted after `execute()` (so any
IR-written vertex bytes are already in the arena), inside the existing
task-graph-non-empty block: it resolves `vertex_buffer_ref`'s backing
`Allocation::bytes` (`reference_device_hal.cpp:217-241`, host bytes,
reinterpreted as a `RasterVertex` array; count =
`bytes.size()/sizeof(RasterVertex)`) and calls the unchanged facet-pool
`raster_triangles` overload (`reference_device_hal.cpp:252`).

Both backends already unconditionally advertised `Capability::Raster` before
F2 (`metal_device_hal.mm:156`, `reference_device_hal.cpp:31`), so no new
capability-gating logic was needed.

## Alternatives

- **`ExecutionPlan.raster_passes` parallel list**: rejected -- see Decision
  #1's rationale. Would permanently exclude raster tasks from
  `TaskGraph`'s dependency machinery, closing off F6/F9's cross-dependency
  needs.
- **Eager pipeline build at `compile()` time**: rejected -- see Decision
  #2's rationale. Blocked by `compile()`'s missing `Arena` parameter and by
  the raster fields being `FacetRef`s rather than inlined views.
- **Exposing the full attachment config (load/store/clear/sample-count/
  subresource) on `TaskRecord`**: rejected -- see Decision #3's rationale.
  Would promote an adapter-local feature to core minimum capability, the
  anti-pattern `docs/START.md` §5 names explicitly.

## Consequences

- `same_task()` (`tests/vertical_slice/metal_task_timeline_test.cpp:45`),
  the `TaskRecord` field-by-field comparator used to cross-validate
  `submission.published_tasks` against the reference oracle, needed
  updating to cover the new fields.
- New tests: `vertical-slice.metal.task-graph-raster` (Metal, full
  `TaskGraph` -> `compile()` -> `submit()` path against the reference
  oracle), a reference-backend equivalent in `tests/unit/
  reference_raster_test.cpp`, and `TaskRecord` default-value assertions in
  `tests/unit/core_test.cpp`.
- No public C-ABI (`include/vg/vg.h`'s `VgTaskRecord`) changes -- F2 is
  fully internal to C++; a public raster ABI entry point is deferred to a
  later F milestone.
- No real indexed draws before F5: `index_count > 0` is rejected at
  `compile()` time, not silently downgraded, per `docs/START.md` §4
  invariant 10 ("任何无法在当前硬件表达的语义必须返回 `Unsupported`、明确
  降级或进入 reference backend；不允许静默伪装").
- No depth (deferred to F4, ADR-043 Decision #5), no user-authored shaders
  (F3, ADR-043 Decision #4), no Vulkan implementation (ADR-043 §7: Vulkan
  stays permanently compile-review-only).
- `Topology` stays single-valued (`TriangleList`) in F2.

## Evidence

- `build/dev-metal`: full build clean; **55/55 ctest passed**, including
  the new `vertical-slice.metal.task-graph-raster` (plan-driven, full
  `TaskGraph` -> `compile()` -> `submit()` path, pixel-compared against the
  reference oracle) and the unchanged `vertical-slice.metal.basic-raster`
  (still calls `run_raster_triangles` directly, confirming the extraction
  in `Impl::run_raster_pass` did not change its observable behavior).
- `build/dev-reference`: full build clean; **26/26 ctest passed**,
  including `reference.facet-oracles` (13, now covering the new
  plan-driven raster test case that asserts `submission.raster_results`
  matches a direct `raster_triangles` call exactly) and `core.unit` (9,
  now covering the new `TaskRecord` raster-field default-value
  assertions).
- `same_task()` (`tests/vertical_slice/metal_task_timeline_test.cpp:45`)
  updated to compare the new fields; no regression in any pre-existing
  task-graph test that depends on it.

### Post-implementation review fixes

An independent code review against this ADR and TASK-F2 (post-"Done", pre-
commit) surfaced two correctness gaps, both fixed and re-verified:

- **Major-1 (Metal only)**: `SubmitOps::raster` (`metal_device_hal.mm`)
  originally `return false;`d on any per-task raster failure, and
  `DeviceHal::submit()` treated that as a hard `Flow`-style abort --
  contradicting this ADR's own Implementation section ("a raster task must
  not prevent the rest of a mixed compute+raster graph from submitting").
  Fixed by making `SubmitOps::raster` a pure soft-fail function (never
  touches `submission->result`, only returns `bool` + `*out_message`) and
  restructuring `DeviceHal::submit()` to fold the raster outcome into
  `submission->result` through a `finish()` lambda applied at every return
  point, *after* the remaining stages (`host_assisted`/`certificate`/
  `effect_dag`/`indexed`/`linear`) have run -- and only if `result.ok` is
  still `true` at that point. The deferral matters because those later
  stages each unconditionally set `result.ok = true` on their own success;
  folding the raster failure in eagerly (mirroring the reference backend's
  idiom verbatim, where the raster block runs textually after `execute()`)
  would have let a later stage's success silently overwrite an earlier
  raster failure in Metal's staged chain.
- **Major-2 (both backends)**: vertex count was derived as
  `bytes.size() / sizeof(RasterVertex)` via plain integer division, with no
  remainder check -- a malformed vertex buffer was silently truncated
  rather than rejected (not a memory-safety issue: all downstream reads
  stay bounded by the truncated count, but a `docs/START.md` §4 invariant
  10 "no silent degradation" gap). Fixed identically in both backends by
  rejecting (soft-fail) when `bytes.size() % sizeof(RasterVertex) != 0`.

New regression test (Metal): a third sub-case in
`run_task_graph_raster` (`metal_task_timeline_test.cpp`) builds a graph
mixing one normal `Compute` task with one `Raster` task whose
`vertex_buffer_ref` was never acquired, so `SubmitOps::raster` fails it
deterministically. Asserts `submit()` still returns `true`,
`submission.result.ok == false` with a non-empty message, and --
critically -- that `submission.result.trace`/`timeline_value`/
`published_tasks` (all only written by a *completed* `SubmitOps::linear()`
dispatch) show the compute half ran to completion despite the raster
failure. Re-verified after the fixes and this test: `build/dev-metal`
**55/55 ctest passed** (including this new sub-case), `build/dev-reference`
**26/26 ctest passed** (unaffected -- reference's raster block already ran
after `execute()`, so it had no Major-1-equivalent bug).

## Revisit trigger

Revisit when F5 implements real indexed draws (the `index_count > 0`
rejection is lifted), when a public C-ABI raster entry point is added
(`VgTaskRecord` growth, following ADR-044/ADR-045's append-only
version-negotiation contract), or when F6/F9 actually need compute<->raster
cross-dependencies in one graph (proving out Decision #1's rationale).
