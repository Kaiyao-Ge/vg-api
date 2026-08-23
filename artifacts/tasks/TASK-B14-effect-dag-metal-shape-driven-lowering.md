# TASK-B14: E012 -- General Effect DAG + Metal Shape-Driven Encoder/Fence Lowering

Status: complete.

Normative docs: ADR-027 (general Effect DAG + Metal shape-driven
encoder/fence lowering, finalizing ADR-008's deferred "backend-specific
barrier merging and queue/encoder lowering" for 3 of 4 shapes); ADR-024
(Phase B closure criterion); ADR-008 (EffectGraph/happens-before design
this milestone builds on); TASK-B12 (shared observability foundation this
milestone's report fields reuse). This is the fifth milestone (TASK-B14)
of the eight-milestone plan approved this session.

## Goal

Give E012 ("Effect DAG sync quality") a real Metal + reference result:
build an arbitrary-shaped effect DAG across independently-compiled passes,
classify its structural shape (`core::classify_effect_graph_shape`), and
lower exactly 3 recognized shapes (`LinearChain`, `IndependentBranches`,
`ForkJoin`) to real, hardware-verified `MTLComputeCommandEncoder`/
`MTLFence` strategies -- honestly reporting everything else `Unsupported`
rather than guessing a synchronization strategy. Vulkan stays
compile-review-only per ADR-024 (documentation-only shape mapping, zero
functional wiring).

## Files

- `src/core/core.h` / `src/core/core.cpp` -- new sibling
  `core::EffectGraphBuilder` (not a generalization of
  `TaskGraphBuilder::seal` -- see ADR-027 Alternatives), producing an
  `EffectGraph` from generic `ir::Effect` lists via the same
  `conflicts()`/`validate_happens_before()` algorithm; new
  `classify_effect_graph_shape(graph, node_count) -> EffectGraphShape`
  structural classifier (`LinearChain`/`IndependentBranches`/`ForkJoin`/
  `Unsupported`); new `effect_graph_deterministic_order(...)` Kahn's-
  algorithm topological sort with lowest-ready-index-first tie-break.
- `src/backends/device_hal.h` -- `ExecutionPlan` gains
  `effect_dag_passes` (`std::vector<ir::Module>`) and
  `effect_dag_dependencies` (`std::vector<std::pair<uint32_t, uint32_t>>`);
  `CompiledPlan` gains `effect_dag_shape`. No new virtual method --
  `hal::DeviceHal` stays fixed at `compile()`/`submit()`, matching the
  `requested_certificate_mode`/`request_tier1_indirect` precedent.
- `src/backends/metal/metal_device_hal.mm` -- `compile()`'s new
  effect-dag branch: compiles every pass independently via
  `build_linear_compute_package`, builds and seals an `EffectGraphBuilder`
  from each pass's `declared_effects` plus `plan.effect_dag_dependencies`,
  classifies the shape; `Unsupported` sets
  `compiled->report.supported=false` and adds an `effect_dag_lowering`
  report event classified `LoweringClass::Unsupported`, returning `false`
  rather than guessing. `Impl::dispatch_effect_dag(...)` lowers the 3
  in-scope shapes: `LinearChain` (single encoder, topo-ordered dispatch,
  no fences); `IndependentBranches` (one encoder per pass, no fences,
  relying on the shape's own zero-conflict precondition); `ForkJoin` (one
  encoder per pass, a fully general per-node `MTLFence` scheme -- each
  node waits on the fence of every direct predecessor from
  `graph.edges()`'s actual `Explicit`+`InferredConflict` edges, then
  creates and signals its own new fence -- corrected mid-milestone from an
  initial independent-middles assumption once empirical construction
  disproved it; see ADR-027 Decision). `submit()`'s effect-dag branch
  bypasses the single-module `dispatch_and_wait` path entirely when
  `plan.effect_dag_passes` is non-empty, memcpys per-pass GPU buffer
  contents back into `core::Arena`, and fills
  `submission->report.encoder_count/barrier_count/command_buffer_count/queue_wait_count`
  from `DispatchStats` (TASK-B12 fields).
- `src/backends/vulkan/vulkan_device_hal.cpp` -- no functional changes;
  a documentation-only comment block inserted immediately before
  `DeviceHal::compile(...)` maps the same 3 in-scope shapes onto
  `vkCmdPipelineBarrier2`/multiple `vkCmdDispatch` calls, explicitly
  compile-review-only (ADR-024/027). `compile()`/`submit()` still fall
  through to ordinary single-module compilation when
  `effect_dag_passes` is set -- the rest is silently unused, unchanged
  from pre-B14 behavior.
- `tests/vertical_slice/metal_task_timeline_test.cpp` -- new
  `run_effect_dag` mode, following the existing `run_task_tier0`/
  `run_timeline`/`run_access_certificate`/`run_tier1_indirect`/
  `run_cull_compact` structural pattern and `main()`'s mode-dispatch.
  Covers all 3 in-scope shapes plus one honestly-`Unsupported`
  construction (a diamond with both explicit dependencies and
  overlapping conflicting effects between adjacent nodes, which produces
  duplicate Explicit+InferredConflict edges past what the classifier
  recognizes).
- `CMakeLists.txt` -- one new `add_test` entry,
  `vertical-slice.metal.effect-dag`, on the existing
  `vg_metal_task_timeline_test` executable.
- `experiments/definitions/E012-effect-dag-sync-quality.json` -- new
  experiment definition (schema `vg.experiment/v1`).
- `docs/decisions/ADR-027-effect-dag-metal-shape-driven-encoder-fence-lowering.md` --
  new ADR.

## Validation

`cmake --build build/dev-metal` rebuilds clean (no new warnings beyond the
pre-existing benign duplicate-library linker warning). Standalone run of
`vg_metal_task_timeline_test effect-dag <repo_root>` passes all 4
sub-cases (`independent-branches`, `linear-chain`, `fork-join`,
`unsupported-shape`). `ctest --output-on-failure` under the `dev-metal`
preset: 24/24 tests passed -- the 23 pre-existing tests unchanged, plus
`vertical-slice.metal.effect-dag`, which asserts: `independent-branches`
(3 passes, disjoint allocations, 0 edges, `encoder_count==3`,
`barrier_count==0`); `linear-chain` (3 passes, disjoint allocations, 2
explicit dependency edges, `encoder_count==1`); `fork-join` (4 passes, one
shared allocation, 6 auto-inferred conflict edges, `encoder_count==4`,
`barrier_count==3`, final buffer bytes matching the deterministic
topological order's last writer); and `unsupported-shape` (a
diamond construction that fails `compile()`, with `report.supported==false`
and an `effect_dag_lowering` event classified `LoweringClass::Unsupported`).
`tooling.schemas` and `docs.check` pass with the new ADR and experiment
JSON in place.

## Known limits

- **`ForkJoin`'s edge-count invariant forces at least one direct
  middle-to-middle edge once `node_count>3`** -- this is a documented
  property of `classify_effect_graph_shape`, not a bug: a textbook diamond
  with mutually-independent middles never actually classifies `ForkJoin`
  at `node_count>3` (it falls short of the required
  `2*(node_count-1)` structural edges). The Metal fence scheme is
  general (predecessor-driven from `graph.edges()`, not role-driven), so
  it stays correct regardless; see ADR-027 for the empirical derivation.
- **Only 3 of 4 `EffectGraphShape` values are lowered** -- `Unsupported`
  covers everything else, including cross-queue (would need a second
  `MTLCommandQueue`), representation-transition, and external-present
  shaped graphs. This is an explicit, documented scope boundary
  (ADR-027 Revisit trigger), not an oversight: those constructions are
  never attempted, only ever honestly reported `Unsupported`.
- **Combining `effect_dag_passes` with `task_graph` in the same
  submission is out of scope** -- mirrors ADR-026's identical
  Tier1/`atomic_add` scope boundary; both are deliberate, documented
  boundaries rather than gaps.
- **Vulkan wiring is documentation-only, zero functional change** --
  `vulkan_device_hal.cpp` still silently ignores `effect_dag_passes` and
  falls through to single-module compilation; this milestone does not add
  a second, independently-runnable Vulkan multi-pass dispatch path, since
  E012 only requires Vulkan evidence to be compile-review-only (ADR-024).
- **`MTLFence`'s `waitForFence:`/`updateFence:` ordering requires one
  fence per node, not one shared fence per branch** -- documented on the
  `ForkJoin` dispatch branch and in ADR-027, since a shared fence would
  only unambiguously order against the most recent `updateFence:` call in
  encode order, not the specific predecessor a given node actually depends
  on.
