# ADR-026: Metal Task Tier1 Real Indirect Dispatch + GPU Cull/Compact Kernel

Status: Accepted

## Context

ADR-021 implemented Metal Task Tier0 (GPU-side publication into a ring the
host reads back) and deliberately deferred Tier1 (same-Node GPU-authored
indirect dispatch), since `06-backend-macos-metal.md` §8 states Tier1 is a
target, not an absolute requirement, and bundling it with two
already-required features risked shipping an under-verified path. ADR-021's
own Revisit trigger named the condition for superseding its "Tier1
deferred" framing: a future milestone that actually implements the
encoding. This is that milestone, driven by TASK-B13 (E009 -- "GPU-generated
same-Node work"), one of the five Phase B gate experiments ADR-024
committed to real Metal/reference implementations for.

This milestone also folds in TASK-B12's shared-observability foundation
(no independent ADR was written for it, per the original plan): before this
work, `LoweringReport`/`Submission` carried no real timing or dispatch-shape
counters at all. `LoweringReport` gained `barrier_count`, `encoder_count`,
`command_buffer_count`, `queue_wait_count`; `Submission` gained
`cpu_encode_ns`, `cpu_submit_ns`, and `std::optional<uint64_t> gpu_ns` (left
`std::nullopt` since no backend in this project sets
`capabilities().timestamps_available`, per `docs/START.md` §4's
anti-dishonest-degradation invariant -- these fields are never fabricated
from CPU timing). Metal's Tier0 dispatch path was instrumented with these
counters first; this ADR's Tier1/cull-compact additions extend the same
`DispatchStats` accumulator rather than inventing a second scheme.

E009 also requires a GPU-authored stream-compaction kernel (cull/compact:
each GPU thread evaluates one instance's visibility and, if visible,
atomically claims a slot in a compacted output array) as a second,
independent vertical slice alongside Tier1 -- both are "the GPU decides how
much same-Node work to do without a host round trip," just expressed two
different ways (dispatch sizing vs. output-size stream compaction).

## Decision

**Tier1 mechanism: `dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:`,
not ICB.** Per ADR-021's own 2026-08-21 Correction paragraph,
`06-backend-macos-metal.md` §8 permits either an indirect argument buffer
or ICB for Tier1 ("使用 indirect argument buffer/ICB, 按运行时能力选择") --
ICB is one option, not a requirement, and Tier2 (not Tier1) is where ICB's
actual distinguishing power (GPU selects among multiple pre-encoded
Nodes) is required. The lighter-weight indirect-buffer mechanism carries
none of ICB's resource-inheritance/encoder-lifetime risk and mirrors
Vulkan's already-implemented Tier1 shape (ADR-022's zero-repack
`vkCmdCopyBuffer` + `vkCmdDispatchIndirect`).

**Dispatch dims are GPU-authored, never host-read before dispatching.**
After Tier0 publication, `fields_buffer` already holds every task's real
`x/y/z` words (GPU-resident, per ADR-021's packed layout, words 5/6/7 of
each task's 14-word record). `dispatch_task_tier1_indirect` opens a single
`MTLCommandBuffer` containing, in order: (1) an `MTLBlitCommandEncoder`
that copies each task's 3 dispatch-dim words from `fields_buffer` directly
into a dedicated `MTLBuffer` sized and laid out as
`MTLDispatchThreadgroupsIndirectArguments` (one slot per task, in
`deterministic_order()`), then (2) an `MTLComputeCommandEncoder` that
issues one `dispatchThreadgroupsWithIndirectBuffer:` call per task against
that same buffer. `MTLResourceStorageModeShared` plus Metal's default
automatic hazard tracking across encoders within one command buffer make
the blit-then-dispatch ordering safe with no explicit `MTLFence` --
consistent with ADR-021's Tier0 rationale for not needing cross-encoder
fences here either. The host knows the task *count* (the task graph is a
static, host-authored structure) but the dispatch *dims themselves* are
never read back to the host before the indirect dispatch runs.

**Tier1 is wired into the existing generic `submit()` flow via a new
opt-in `ExecutionPlan::request_tier1_indirect` bool**, not a new virtual
method -- consistent with the project's established rule that
`hal::DeviceHal` stays fixed at exactly `compile()`/`submit()`, with all
per-submission variability expressed as `ExecutionPlan` fields (the same
shape as `requested_certificate_mode` in ADR-025). Default `false`
preserves every pre-B13 caller's behavior exactly (Tier0-only). Backends
with no real GPU dispatch concept (reference) simply ignore it. Metal only
acts on it when `capabilities().supports(Capability::IndirectTier1)` is
also true, so a caller can never force Tier1 dispatch against a Metal
device where the underlying probe reports it unavailable.

**Tier1 verification is a debug/test-only introspection accessor,
`DeviceHal::last_tier1_indirect_dims()`, not a new counting kernel.** After
the indirect dispatch completes, the backend reads back the indirect-args
buffer's bytes (a legitimate out-of-band host read after GPU work is done
-- the same pattern already used to turn `fields_buffer` into
`submission->published_tasks`) purely so a test can assert the blit copied
the right GPU-resident bytes. The dispatch itself never depends on this
readback; it exists only for verification.

**Correctness scope limit: Tier1 re-dispatch is safe only for idempotent
instruction sets, and this milestone keeps it that way deliberately.**
Tier1 re-dispatches the *same* already-compiled module pipeline and the
*same* buffers used by the initial Tier0-adjacent `dispatch_and_wait` call,
once more per task. For a load/store-only module this is idempotent
(repeated identical stores produce the same result). Combining Tier1 with
`atomic_add` would double- or multiply-accumulate and corrupt results
against the reference oracle. This milestone's probe module (and its
ctest) is deliberately load-only; the arena readback in `submit()` that
populates `core::Arena` byte contents happens once, immediately after the
initial `dispatch_and_wait`, *before* the Tier1 block runs -- Tier1's
effect on those same buffer bytes is therefore never read back into the
arena in this implementation. This is an explicit, honest scope boundary,
not an oversight: `vertical-slice.metal.tier1-indirect` validates Tier1
solely via `last_tier1_indirect_dims()` (that the right dims were
GPU-authored and dispatched), not via arena byte contents. Combining Tier1
with non-idempotent instructions, and/or adding a second arena readback
pass after Tier1 to validate its effect on stored bytes, is out of scope
for this milestone.

**Cull/compact is a standalone Metal-only method
(`DeviceHal::run_cull_compact`), bypassing the generic ABI entirely** --
unlike Tier1, cull/compact is orthogonal to the task-graph publish/submit
flow (it operates on instance-visibility arrays, not `TaskRecord`s), so it
follows the `probe_buffer()`/`snapshot()` precedent of a concrete-class
public method beyond the `hal::DeviceHal` interface rather than forcing an
unrelated concept through `ExecutionPlan`. The kernel
(`compiler::cull_compact_metal_source()`, mirroring
`task_ring_metal_source()`'s "independent hand-written kernel + dedicated
pipeline" precedent) assigns one GPU thread per instance: if
`instance_visible[gid]` is nonzero, the thread claims an output slot via
`atomic_fetch_add_explicit` on a shared counter and writes its
`instance_ids[gid]` into that slot.

**Cull/compact correctness property: compare as a set, not by position.**
GPU thread execution order is non-deterministic, so `atomic_fetch_add`
slot assignment does not preserve original instance-index order. This is a
correct, expected property of GPU-side stream compaction, not a bug. The
reference oracle (`reference::cull_compact`) walks instances in ascending
index order and is therefore only comparable against the GPU output as a
sorted multiset/set of ids, never element-by-element by position -- this
caveat is documented on both the Metal-side (`CullCompactResult`) and
reference-side (`CullCompactResult`/`cull_compact`) doc comments, and
`vertical-slice.metal.cull-compact` asserts equality only after sorting
both sides.

`cull_compact_vulkan_source()` exists in `compute_package.cpp` as the GLSL
analogue (per ADR-024's compile-review-only precedent for Vulkan) but is
deliberately left unwired into `vulkan_device_hal.cpp` in this milestone --
Vulkan's Tier1 mechanism is already implemented and reviewed
(`dispatch_task_ring_and_tier1`, ADR-022) and is reused as-is; wiring the
cull/compact kernel into the Vulkan backend is not required for E009's
compile-review-only Vulkan evidence and is left for a future milestone if
ever needed.

## Alternatives

- Implement Tier1 via ICB, matching ADR-021's original (superseded)
  framing: rejected for the same reasons ADR-021's Correction already
  gives -- ICB is not required for Tier1, carries resource-inheritance/
  encoder-lifetime risk not yet exercised anywhere in this codebase, and
  the indirect-buffer mechanism already matches the Vulkan Tier1 shape.
- Add a new virtual method to `hal::DeviceHal` for Tier1 (e.g.
  `dispatch_tier1(...)`): rejected -- breaks the project's established
  "exactly `compile()`+`submit()`, all variability via `ExecutionPlan`
  fields" convention (see ADR-012, reaffirmed in ADR-025 for
  `requested_certificate_mode`), and would force Vulkan/reference to
  implement a method they either don't need or can't meaningfully support.
- Add a second arena readback after the Tier1 dispatch, so Tier1's effect
  on buffer bytes is reflected into `core::Arena`: rejected for this
  milestone. It would only be meaningful in combination with allowing
  non-idempotent instructions (atomic_add) through Tier1, which this ADR
  separately scopes out; adding the readback without that would exercise a
  code path with no distinguishable test outcome from the existing
  load/store-only, dims-only verification.
- Invent a new GPU counting/verification kernel to validate Tier1 dispatch
  dims, instead of reading back the indirect-args buffer directly:
  rejected -- the indirect-args buffer already holds exactly the bytes
  that need verifying, and reading it back host-side after the dispatch
  completes is a strictly simpler, already-precedented pattern (mirrors
  the `fields_buffer` -> `published_tasks` readback from ADR-021).
- Wire `cull_compact_vulkan_source()` into `vulkan_device_hal.cpp` in this
  same milestone: rejected -- E009's Vulkan evidence only needs to be
  compile-review-only (ADR-024), and Vulkan's Tier1 story is already
  covered by ADR-022's existing implementation; adding an unused wiring
  path would be scope creep with no experiment-catalog requirement behind
  it.

## Consequences

Metal now has a real, hardware-verified Tier1 implementation, closing the
gap ADR-021 explicitly left open and superseding its "Tier1 deferred"
framing. `IndirectTier1`'s capability bit (made honest but unconsumed by
ADR-021) is now functionally exercised. `LoweringReport`/`Submission` carry
real CPU timing and dispatch-shape counters project-wide (TASK-B12),
available to every future milestone that wants to report them rather than
inventing per-milestone ad hoc metrics. The Tier1/idempotent-only and
arena-readback scope boundaries are explicit rather than silently absent,
consistent with `docs/START.md` §4. The cull/compact vertical slice adds a
second, independent demonstration of GPU-authored same-Node work sizing,
using atomic-append stream compaction whose non-deterministic ordering is
documented as a correctness property rather than treated as a flaw to
paper over.

## Evidence

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`vertical-slice.metal.tier1-indirect` builds a two-task dependency graph,
requests Tier1 via `ExecutionPlan::request_tier1_indirect`, and asserts
`last_tier1_indirect_dims()` matches each published task's real `x/y/z` in
dispatch order, plus a `tier1_indirect_dispatch` report event classified
`LoweringClass::Direct` with the correct task count.
`vertical-slice.metal.cull-compact` runs a 9-instance visibility array
through `run_cull_compact`, and asserts the GPU-compacted id set equals
`reference::cull_compact`'s oracle output after sorting both sides. `ctest
--output-on-failure` under `dev-metal`: 23/23 tests passed, including all
21 pre-existing tests unchanged (TASK-B12's additive counter fields did
not break any existing assertion).

## Revisit trigger

Revisit if a future milestone needs Tier1 to support non-idempotent
instruction sets (e.g. `atomic_add`) -- at that point the "re-dispatch the
same pipeline once per task" mechanism needs either a guard rejecting
non-idempotent modules or a redesign (e.g. a dedicated Tier1-only kernel
variant), and the arena-readback-after-Tier1 question deferred here would
need to be resolved for real. Revisit if E012 (TASK-B14) or any later
milestone needs true cross-queue or cross-command-buffer Tier1
synchronization -- the current single-command-buffer implicit hazard
tracking would not cover that case and would need explicit `MTLFence`/
`MTLEvent` handling.
