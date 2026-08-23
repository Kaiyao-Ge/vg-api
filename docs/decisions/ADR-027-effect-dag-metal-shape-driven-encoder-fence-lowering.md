# ADR-027: General Effect DAG + Metal Shape-Driven Encoder/Fence Lowering

Status: Accepted

## Context

ADR-008 (Phase A) built `EffectGraph`'s reason-bearing edges (`Explicit`,
`InferredConflict`, `Timeline`, `Publication`) and `TaskGraphBuilder::seal`'s
conflict-inference algorithm, but explicitly deferred "backend-specific
barrier merging and queue/encoder lowering" to Phase B -- no backend ever
turned an `EffectGraph`'s shape into an actual multi-encoder/fence dispatch
strategy. This is that milestone (TASK-B14), driven by E012 ("Effect DAG
sync quality"), one of the five Phase B gate experiments ADR-024 committed
to real Metal/reference implementations for.

Unlike Task Tier0/Tier1 (ADR-021/026), which lower a fixed `TaskRecord`
publication/dispatch shape, E012 needs a *general* mechanism: an arbitrary
number of independently-compiled passes with an arbitrary dependency/
conflict structure between them, where the lowering strategy (single
encoder vs. multiple independent encoders vs. multiple fenced encoders)
depends on the graph's actual shape. Recognizing every possible DAG shape
and inventing a correct lowering for each is unbounded scope; this
milestone recognizes exactly 3 shapes (`LinearChain`, `IndependentBranches`,
`ForkJoin`) and honestly reports everything else `Unsupported` rather than
guessing a synchronization strategy for a shape it wasn't designed against
-- consistent with `docs/START.md` §4's anti-dishonest-degradation
invariant.

## Decision

**A new sibling builder, `core::EffectGraphBuilder`, not a generalization of
`TaskGraphBuilder::seal`.** `TaskGraphBuilder` bakes in Task-specific
invariants (quota, `PublicationRing` integration) that a general-purpose
Effect DAG builder must not inherit; `EffectGraph` itself carries no
Task-specific fields, so a second independent producer over generic
`ir::Effect` lists, using the exact same `conflicts()`/
`validate_happens_before()` algorithm `TaskGraphBuilder::seal` already
uses, is the lowest-risk option. `add_node(effects)` registers a node's
declared effects; `add_dependency(before, after)` registers an explicit
ordering edge; `seal(...)` adds one `Explicit` edge per registered
dependency, then separately adds an `InferredConflict` edge for every
`(before, after)` pair (`before < after`) whose effects conflict via
`EffectGraph::conflicts()` -- **regardless of whether an explicit edge
already connects that same pair.** This mirrors `TaskGraphBuilder::seal`'s
existing behavior exactly (no new de-duplication logic was introduced) and
is the reason a single node pair can carry two structural edges at once
(see the classifier and Unsupported-construction notes below).

**`classify_effect_graph_shape` is pure structural analysis over in/out-
degree, counting only `Explicit`+`InferredConflict` edges** (`Timeline`/
`Publication` are cross-cutting metadata, not shape-relevant structure).
`node_count<=1` is trivially `LinearChain`. Zero structural edges is
`IndependentBranches`. Exactly `node_count-1` edges, every node at in/out-
degree <=1, forming one path from the sole in-degree-0 node, is
`LinearChain`. Otherwise: a node with `out_degree==node_count-1 &&
in_degree==0` is a fan-out source, a node with `in_degree==node_count-1 &&
out_degree==0` is a fan-in join; if both exist, are distinct, and
`structural_edges==2*(node_count-1)` exactly, the shape is `ForkJoin`.
Everything else is `Unsupported`.

**`ForkJoin`'s "middle" nodes are not, in general, mutually independent --
this was verified empirically, not just by inspection, and the Metal fence
scheme was written (and, mid-milestone, corrected) to match reality rather
than the more intuitive assumption.** A textbook diamond (one source, one
join, N-2 mutually-independent middles, source->each middle->join, zero
middle-to-middle edges) produces only `2*(node_count-2)` structural edges
for `node_count>3` -- short of the `2*(node_count-1)` the classifier
requires. Direct construction (`core::EffectGraphBuilder` +
`classify_effect_graph_shape`, probed standalone against the project's
built `libvg_core.a`/`libvg_ir.a` before writing any product code) confirmed
this: a 4-node diamond with independent middles classifies `Unsupported`,
and the only 4-node construction that actually classifies `ForkJoin` is one
where every node pair conflicts (a complete C(4,2)=6-edge transitive
closure -- e.g. 4 passes all writing the same allocation with overlapping,
mutually-conflicting effects and zero explicit dependencies, letting
`seal()`'s automatic conflict detection produce the full closure). That
construction *always* has at least one direct edge between two non-source/
non-join nodes once `node_count>3`. The Metal `ForkJoin` dispatch branch
(`Impl::dispatch_effect_dag` in `metal_device_hal.mm`) was initially written
against the *assumed* independent-middles model (one fence per branch, join
waits on all middle fences, middles wait on nothing) before this was
checked; that scheme would have silently dropped the real ordering
requirement between two middles in the only graphs this classifier ever
actually recognizes as `ForkJoin`, racing on real Metal hardware. It was
corrected, in the same milestone before any test exercised it, to a fully
general per-node-fence scheme: compute the deterministic topological order
(`effect_graph_deterministic_order`), build a `predecessors` adjacency list
from `graph.edges()`'s actual `Explicit`+`InferredConflict` edges, and for
every node in that order, wait on the fence of every direct predecessor
(if created), dispatch, create and signal a *new* fence for that node, end
encoding. This is correct for both the (never-actually-reachable-at-
`node_count>3`) textbook diamond and the classifier's real one-extra-edge
case alike, with no special-casing of source/join roles beyond "no
predecessors to wait on" / "nothing waits on it further." `MTLFence`'s
`waitForFence:` only unambiguously orders against the *most recent*
`updateFence:` call on that fence object in encode order, which is exactly
why one fence per node (not one shared fence per branch) is required here.

**Metal lowering, one `MTLCommandBuffer` per `submit()` call, branching on
shape:**
- `LinearChain`: a single `MTLComputeCommandEncoder`, every pass dispatched
  in topological order within it -- no fence needed, in-encoder ordering is
  already sequential.
- `IndependentBranches`: one `MTLComputeCommandEncoder` per pass, no fences
  between them at all, relying on Metal's default implicit hazard tracking
  across encoders within a single command buffer (the same rationale
  ADR-021/026 already used for Tier0/Tier1 ordering) -- and, more
  fundamentally, justified structurally: zero declared conflicts/
  dependencies is this shape's own precondition, so there is nothing to
  order in the first place.
- `ForkJoin`: one `MTLComputeCommandEncoder` per pass, general per-node
  fencing as described above.

**Wired into `submit()` via `ExecutionPlan::effect_dag_passes` (a
`std::vector<ir::Module>`) plus `effect_dag_dependencies` (a
`std::vector<std::pair<uint32_t, uint32_t>>`), not a new virtual method** --
consistent with the project's fixed `compile()`+`submit()` `DeviceHal`
interface (ADR-012, reaffirmed by ADR-025/026 for
`requested_certificate_mode`/`request_tier1_indirect`). `plan.module` is
still required to equal `effect_dag_passes[0]` by convention purely so
`ExecutionPlan::validate()`'s existing `ir::verify(module)` call keeps
working unmodified; when `effect_dag_passes` is non-empty, `submit()`
branches entirely away from the single-module `dispatch_and_wait` path
before it is ever reached, so `plan.module`/pass 0 is never separately
(re-)dispatched -- avoiding a double-dispatch of pass 0's work. Each pass is
compiled independently via its own `build_linear_compute_package` call (a
fresh `ComputePackage`/pipeline per pass, cached by IR hash in a new
`effect_dag_pipelines` map alongside the existing single-module pipeline
cache) rather than trusting `plan.module`/the top-level `compute_package` to
represent the whole DAG.

**Combining `effect_dag_passes` with `task_graph` in the same submission is
out of scope for this milestone**, mirroring ADR-026's identical "combining
Tier1 with atomic_add is out of scope" precedent -- both are deliberate,
documented scope boundaries rather than oversights.

**Vulkan stays compile-review-only (ADR-024).** `vulkan_device_hal.cpp`
does not act on `effect_dag_passes` at all -- a Vulkan plan that sets it
simply gets ordinary single-module compilation, with the rest silently
unused. A documentation-only comment block placed above `DeviceHal::compile`
maps all 3 in-scope shapes onto this backend's already-implemented
primitives (`vkCmdPipelineBarrier2` for ordering, multiple `vkCmdDispatch`
calls per pass), including the same general "barrier against every direct
predecessor, not an assumed source/join role split" requirement for
`ForkJoin`. No Vulkan hardware is reachable from this machine (permanent
constraint, reconfirmed by ADR-024); this milestone does not add a second,
independently-runnable Vulkan multi-pass dispatch path.

## Alternatives

- Generalize `TaskGraphBuilder::seal` to accept non-Task nodes instead of a
  new sibling `EffectGraphBuilder`: rejected in the original 8-milestone
  plan (see that plan's "已解决的设计分叉" section) -- `TaskGraphBuilder`'s
  Task-specific invariants (quota, `PublicationRing`) would leak into a
  supposedly-general path; a second independent producer is lower risk.
- Assume `ForkJoin`'s middle nodes are mutually independent (the initial,
  pre-verification implementation): rejected once empirical construction
  showed this is never actually true for any graph the classifier
  recognizes as `ForkJoin` at `node_count>3` -- the assumption would have
  shipped a real GPU race, not merely an inelegant simplification.
- Recognize additional shapes beyond `LinearChain`/`IndependentBranches`/
  `ForkJoin` (e.g. general DAGs, diamonds-of-diamonds): rejected for this
  milestone -- unbounded scope, and the 3 recognized shapes already cover
  the sync-quality question E012 asks (serial, parallel, fan-in/fan-out);
  everything else is honestly `Unsupported` per `docs/START.md` §4.
- Add a new virtual method to `hal::DeviceHal` for multi-pass submission
  (e.g. `submit_effect_dag(...)`): rejected -- breaks the project's fixed
  `compile()`+`submit()` convention (ADR-012/025/026); `ExecutionPlan`
  fields already extend cleanly for this.
- Implement a real second Vulkan multi-pass dispatch path for E012: rejected
  -- Vulkan evidence only needs to be compile-review-only (ADR-024), and
  this backend has no existing independently-runnable multi-pass dispatch
  loop to extend by analogy the way ADR-026 extended Metal's Tier0
  infrastructure for Tier1.

## Consequences

`EffectGraph`'s shape can now drive a real, hardware-verified Metal
encoder/fence lowering strategy for 3 shapes, closing the gap ADR-008
explicitly deferred to Phase B. The `ForkJoin` fence scheme is general
(predecessor-driven, not role-driven), so it remains correct even though
the classifier's own edge-count invariant guarantees non-independent
middles at `node_count>3` -- a property future maintainers might otherwise
assume away again without the empirical record documented here. A 4th
bucket (`Unsupported`) is a first-class, honestly-reported outcome, not a
silent gap: `cross queue`, representation-transition, and external-present
shaped graphs are explicitly out of scope and reported as such, never
lowered with a guessed synchronization strategy.

## Evidence

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`vertical-slice.metal.effect-dag` builds all 3 in-scope shapes through a
real `compile()`+`submit()` round trip -- `IndependentBranches` (3 passes,
disjoint allocations, zero edges, 3 encoders, 0 barriers), `LinearChain` (3
passes, disjoint allocations, 2 explicit dependency edges, 1 encoder), and
`ForkJoin` (4 passes, one shared allocation, 6 auto-inferred conflict
edges, 4 encoders, 3 barriers) -- and asserts each pass's written bytes
match the expected final value, including `ForkJoin`'s: with 4 encoders all
writing the same allocation and a deterministic topological order, the
buffer must hold exactly the last writer's value, which only holds if the
per-node fence scheme actually serializes the writes rather than racing.
A 4th construction (an explicit-dependency diamond with also-conflicting
adjacent effects, replicating the duplicate-edge situation documented
above) is asserted to compile() as `Unsupported` with an honest report
event. `cmake --build build/dev-metal`: clean, no new warnings beyond the
pre-existing benign duplicate-library linker warning. `ctest
--output-on-failure` under `dev-metal`: 24/24 tests passed -- all 23
pre-existing tests unchanged, plus the new test.

## Revisit trigger

Revisit if a future milestone needs `EffectGraph`/`EffectGraphShape` to
recognize additional shapes (general DAGs beyond the 3 covered here), or
needs cross-queue, representation-transition, or external-present
synchronization -- all three are explicitly `Unsupported` today and would
need new classifier buckets plus new lowering strategies, not an extension
of the existing 3 branches. Revisit if Vulkan hardware ever becomes
reachable from this environment (permanent constraint today, per ADR-024)
-- at that point the documentation-only mapping above would need a real,
executable, hardware-verified implementation.
