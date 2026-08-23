# ADR-019: ExecutionPlan Cross-Validation, graph_epoch Activation, and Reference Task/Timeline Execution

Status: Accepted

## Context

ADR-012 defined `ExecutionPlan` as an immutable, pre-validated carrier for a
sealed `TaskGraph` and timeline wait/signal points, but `validate()` never
actually called `TaskGraph::validate_execution()`, and `graph_epoch` was a
dead field -- present in the struct, never compared against anything. A
plan could claim `published = true` over a `TaskGraph` that was never
sealed, or reference a task graph built against an `Arena` topology that
had since mutated, and no layer would catch it before a backend touched
GPU state.

Separately, `vg::reference::execute()` never received `task_graph`,
`timeline_wait`, or `timeline_signal` at all -- it only interpreted
`module.instructions` sequentially. `submission->timeline_value =
plan.timeline_signal` was pure pass-through with no wait-satisfaction
check. This meant the CPU reference backend could not act as a byte-exact
oracle for what B7/B8 ask Metal/Vulkan to do: there was nothing to compare
against.

## Decision

**`ExecutionPlan::validate()`** gains two checks at the end, in addition to
its existing ABI/module/capability checks: `timeline_signal != 0 &&
timeline_signal <= timeline_wait` is rejected (a plan may not claim to
signal a value that doesn't advance past its own wait point); and when
`published && !task_graph.tasks().empty()`, `task_graph.validate_execution()`
must succeed. `validate()`'s signature is unchanged (`const`, no arena
argument) -- it only checks internal plan consistency.

**`graph_epoch` activation is a separate method, not folded into
`validate()`**: `ExecutionPlan::graph_epoch_matches(const core::Arena&
arena, std::string* error) const`. It needs the live arena, which
`validate()` does not have and should not need (validate() runs at plan
construction time, potentially before an arena reference is even
available to the caller). A plan with an empty task graph is exempt
(`graph_epoch` is meaningless when there are no tasks); otherwise
`graph_epoch != arena.topology_epoch()` is a hard rejection. All three
backends (reference, Metal, Vulkan) call this at the top of `submit()`,
before touching any GPU/device state -- a stale-topology submission is
now caught at the single common entry point rather than left to
whatever downstream code happens to notice.

**`vg::reference::execute()`** gains four new parameters: `const
core::Certificate* certificate`, `core::Timeline* timeline`, `uint64_t
timeline_wait = 0`, `uint64_t timeline_signal = 0`. When `timeline !=
nullptr && timeline_wait != 0`, execution is refused unless
`timeline->validate_wait(timeline_wait, &wait_error)` succeeds -- a real
check, not a transcription. On success, `timeline != nullptr &&
timeline_signal != 0` calls `timeline->signal(timeline_signal,
&signal_error)`, which enforces the Timeline's own monotonic-advance
invariant rather than trusting the caller.

**`vg::reference::execute_task_graph(const core::TaskGraph& task_graph)`**
is new: it calls `task_graph.validate_execution()`, computes
`task_graph.deterministic_order()`, and publishes every task through a
real `core::PublicationRing` in that order, returning
`TaskGraphExecutionResult{ok, message, published_tasks}`. This is the
byte-exact oracle both `metal_task_timeline_test.cpp` and (code-review-only)
`vulkan_task_timeline_test.cpp` compare their GPU-published task records
against -- it did not exist before this ADR, so there was previously no
oracle for GPU task-publication correctness to be checked against at all.

`reference_device_hal.cpp`'s `compile()`/`submit()` are adapted to pass
`task_graph`/`timeline_wait`/`timeline_signal`/a `core::Timeline*` through
to the new `execute()` signature, and to call `execute_task_graph()` when
`plan.task_graph.tasks()` is non-empty, mirroring the "task ring" pattern
Metal/Vulkan than build their GPU implementations against.

## Alternatives

- Fold `graph_epoch_matches` into `validate()` by adding an optional
  `const core::Arena*` parameter: rejected -- it would make `validate()`'s
  signature backend-shaped (arena-dependent) when its purpose up to now
  has been backend-independent plan-shape validation; keeping it a
  separate explicitly-named method also makes the "must be called at
  `submit()` time, not `compile()` time" requirement visible at call
  sites instead of buried in an optional-argument default.
- Have Metal/Vulkan silently ignore a `graph_epoch` mismatch (proceed
  anyway): rejected outright by `docs/START.md` §4's "no silent
  degradation" invariant -- a stale topology reference is exactly the
  kind of use-after-invalidate bug the epoch field exists to catch.
- Keep `reference::execute()`'s pass-through `timeline_value` semantics
  and only fix Metal/Vulkan: rejected -- without a reference oracle that
  actually enforces wait/signal semantics, there is nothing for
  Metal/Vulkan's timeline behavior to be checked against, and any bug in
  either backend's `MTLSharedEvent`/`VkSemaphore` wiring would be
  invisible until real hardware exercised it (which, for Vulkan, never
  happens on this machine).

## Consequences

`ExecutionPlan` now has a real internal-consistency floor, and
`graph_epoch` is load-bearing rather than decorative. The reference
backend is now the byte-exact oracle for GPU task-publication ordering
and timeline wait/signal correctness that M8/M9's tests depend on -- this
was the necessary precondition for those milestones, not an optional
nicety. `reference::execute()`'s signature grew by four parameters; all
existing call sites (including `tools/vg-reference.cpp`,
`tests/unit/core_test.cpp`) needed updating to pass the new
defaulted-to-zero/nullptr arguments, which is why this had to land before
M8/M9 rather than alongside them.

## Evidence

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`core.unit`, `conformance.device-hal.reference`, and all Metal-side
vertical-slice/conformance tests pass against the new `validate()`/
`graph_epoch_matches()` checks and the new `execute()`/
`execute_task_graph()` behavior. `ctest --output-on-failure`: 20/20 tests
passed.

## Revisit trigger

Revisit if a future milestone needs `validate()` to see live arena state
(e.g. a check that genuinely cannot wait until `submit()`) -- at that
point, reconsider whether `graph_epoch_matches` should be folded back in
rather than kept as a second method. Revisit `execute_task_graph()`'s
"publish every task in deterministic order, no partial-graph execution"
scope if a future milestone needs partial/incremental task-graph
execution.
