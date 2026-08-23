# ADR-021: Metal Task Tier0 Publication Ring

Status: Accepted

## Context

ADR-004 defined the reference Task publication protocol
(`Empty -> Writing -> Published -> Consumed -> Empty`) as a CPU memory
model, explicitly deferring the question of "how does a real GPU backend
express the release/acquire boundary" to the point where Task Tier0/1 is
actually implemented on a backend. Before this ADR, Metal's `compile()`/
`submit()` never touched `plan.task_graph` at all, and `IndirectTier1`'s
capability bit was set from `MTLFeatureSet_macOS_GPUFamily1_v1` -- a
feature-set enum that says nothing about indirect command buffer support
and was a fabricated signal left over from an earlier, less careful pass.

Per `docs/vg-project/06-backend-macos-metal.md` §8, Tier0 (GPU-side task
publication into a ring that the host can read back) is a required
capability for this backend; Tier1 (same-Node GPU-encoded indirect
dispatch/draw via ICB) is a target, not an absolute requirement. This ADR
implements Tier0 only, and separately fixes the `IndirectTier1` probe's
honesty without attempting Tier1 encoding -- see Alternatives for why
those two are deliberately not bundled.

## Decision

**Buffer layout mirrors `core::TaskRecord` field-by-field, packed as 14
little-endian `uint32` words** (`kTaskRingWordsPerRecord`, shared with
Vulkan via `compiler::kTaskRingWordsPerRecord` -- see ADR-022):
`node_index, node_generation, root_allocation_lo, root_allocation_hi,
root_generation, x, y, z, flags, contract_index, payload_size,
payload_or_offset_lo, payload_or_offset_hi` (64-bit fields split
lo/hi). Packing is explicit word-by-word (`pack_task_record`/
`unpack_task_record`), not a `memcpy` of the C++ struct -- this
sidesteps any risk of MSL/C++ struct-padding divergence between the GPU
and host compilers, which would be a silent, hard-to-detect corruption
source if relied upon instead.

**State machine**: a separate `atomic_uint` state buffer (one word per
task, `Empty=0/Writing=1/Published=2/Consumed=3` matching
`core::PublicationState`'s numeric values) is written by an MSL kernel
(`compiler::task_ring_metal_source()`, generated alongside the existing
runtime-generated MSL codegen path in `compute_package.cpp` -- this
backend has no static `.metal` file, so the Task publication kernel is
generated as a string fragment the same way the linear-subset kernel is)
via `atomic_compare_exchange` (Empty->Writing) followed by
`atomic_store_explicit` with release ordering (Writing->Published) after
the task's fields are written. Unlike Vulkan's GLSL `atomicCompSwap`
(ADR-022), Metal's MSL `atomic_compare_exchange_weak_explicit` has a
spurious-failure mode, so the kernel retries the compare-exchange in a
loop rather than treating a single failed attempt as a genuine
occupied-slot conflict.

**Each task writes only its own disjoint slot** (`gid == its own task
index in the graph's task vector`, not a separately assigned ring
position) -- the GPU's actual parallel completion order across slots has
no bearing on correctness, since slots never alias. Only the final
per-slot state and the host-chosen readback order matter.

**Consumption is synchronous, within the same `submit()` call, not a
cross-submission handoff**: `dispatch_task_publish` uses
`waitUntilCompleted`, so by the time it returns, the `MTLResourceStorageModeShared`
state/fields buffers are already host-readable with no additional
synchronization. `submit()` then walks `task_graph.deterministic_order()`,
asserts every visited slot reached `Published`, and unpacks it into
`submission->published_tasks` -- byte-comparable against
`reference::execute_task_graph()`'s oracle output (ADR-019). This is
simpler than the originally-planned "producer signals a timeline value,
consumer's next `submit()` waits on it" cross-submission design (see
Alternatives): a real implementation attempt showed Task Tier0 does not
need the timeline machinery from ADR-020 at all, since one `submit()`
call can safely dispatch, wait, and read back within itself.

**`TaskPublication` capability bit is now set unconditionally**,
alongside `EffectDag`, in `make_hal_snapshot()` -- not gated behind
`shared_events` or any other optional probe. Task ring Tier0 depends only
on atomic operations against a device buffer, which every Metal device
this backend targets supports; gating it behind an unrelated optional
feature would under-report a capability that is, in practice, always
present. (This is the fix applied in this same milestone after review
found the bit was never being set at all despite Tier0 being fully
implemented -- see Evidence.)

**`IndirectTier1` probe honesty fix, without Tier1 encoding**:
`probe_indirect_command_buffers()` now attempts a real
`newIndirectCommandBufferWithDescriptor:maxCommandCount:options:` call
(`MTLIndirectCommandTypeConcurrentDispatch`, `maxKernelBufferBindCount:1`,
private storage) and checks the result is non-nil, replacing the
`MTLFeatureSet_macOS_GPUFamily1_v1` fabrication. The bit is now honest.
No actual Tier1 ICB encoding/dispatch is implemented in this milestone --
see Alternatives for why.

**Correction (2026-08-21):** this ADR's Context/Decision/Alternatives text
above frames Metal Tier1 as requiring ICB specifically ("same-Node
GPU-encoded indirect dispatch/draw via ICB"). That framing is narrower
than `06-backend-macos-metal.md` §8's actual text: "Tier 1: same-Node
indirect dispatch/draw; 使用 indirect argument buffer/ICB, 按运行时能力选择"
(uses indirect argument buffer OR ICB, chosen by runtime capability) --
ICB is one of two valid mechanisms, not a requirement. The lighter-weight
path, `MTLComputeCommandEncoder
dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:`,
reads dispatch args straight from a buffer with no resource-inheritance
or encoder-lifetime handling at all -- structurally the same shape as
Vulkan's already-implemented Tier1 (ADR-022's zero-repack
`vkCmdCopyBuffer` + `vkCmdDispatchIndirect`), and does not carry the
GPU-hazard risk this ADR's Alternatives section attributed to "Tier1"
when it was really describing ICB specifically. Tier2 (§8), not Tier1,
is where ICB's actual distinguishing power (GPU selects among multiple
pre-encoded commands/Nodes) is required. This correction does not change
this milestone's scope decision (Tier1 remains deferred either way) --
it lowers the risk estimate a future milestone should use when picking
which mechanism to implement Tier1 with.

**`dispatch_and_wait` reads real `TaskRecord.x/y/z`** when a task graph is
present (`dispatchThreadgroups:MTLSizeMake(task.x, task.y, task.z)`),
replacing the previous hardcoded `(1,1,1)`; the hardcoded `(1,1,1)` path
is retained only for the task-graph-empty case, preserving B4/B5 vertical
slice compatibility.

## Alternatives

- Cross-submission consumption via ADR-020's timeline (producer signals,
  a later `submit()` waits before consuming): this was the original plan.
  Rejected once implementation showed it was unnecessary complexity --
  Tier0's dispatch-then-readback fits entirely inside one synchronous
  `submit()` call with `waitUntilCompleted`, and introducing a
  cross-submission handoff would only add API surface (a caller now
  needing to know to issue two `submit()` calls in a specific order) for
  no correctness benefit.
- Implement real Tier1 (ICB same-Node indirect dispatch/draw) in this same
  milestone: rejected. The spec (`06-backend-macos-metal.md` §8) states
  Tier1 is a target, not an absolute requirement -- unlike Vulkan, where
  §18 of `07-backend-linux-nvidia-vulkan.md` makes Tier1 a hard
  conformance floor (ADR-022). Bundling a non-required, GPU-hazard-prone
  feature (ICB encoding requires careful resource-inheritance and
  encoder-lifetime handling not yet exercised anywhere in this codebase)
  into the same milestone as two already-required features (Tier0,
  timeline) would risk shipping an under-verified ICB path alongside
  well-verified required ones. Fixing the `IndirectTier1` probe from a
  fabricated signal to an honest one, while deferring the encoding work
  itself, is judged more consistent with "no silent
  degradation/dishonest capability reporting" than rushing an
  under-tested Tier1 implementation to avoid an asymmetry with Vulkan.
- `memcpy` the C++ `TaskRecord` struct directly into the GPU buffer:
  rejected -- MSL and C++ struct layout/padding rules are not guaranteed
  identical, and a silent layout mismatch would corrupt task data in a
  way that might not be caught by any test that happens to use
  padding-insensitive field values.

## Consequences

Metal now has a real, hardware-verified Task Tier0 implementation whose
output is checked byte-for-byte against the reference oracle
(ADR-019). The `IndirectTier1` capability bit is now trustworthy (even
though it is not yet consumed by any code path) rather than a
feature-set-name guess. The asymmetry between Metal (Tier1 = target,
deferred) and Vulkan (Tier1 = hard requirement, implemented
review-only in ADR-022) is deliberate and mirrors the existing
HostAssisted asymmetry from ADR-016/017 -- both are documented rather
than "fixed" into false symmetry.

## Evidence

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`vertical-slice.metal.task-tier0` builds a two-task dependency graph,
publishes it through the real MSL Task ring kernel, and asserts
`submission->published_tasks` is byte-identical, in the same order, to
`reference::execute_task_graph()`'s oracle output. `ctest
--output-on-failure`: 20/20 tests passed, including
`conformance.device-hal.metal` with `expect_task_publication = true`
after this milestone's capability-bit fix (a prior state of this code
implemented Tier0 fully but never set the bit reflecting it, which is
what the fix here corrects).

## Revisit trigger

Revisit if a future milestone implements real Metal Tier1 (ICB same-Node
indirect dispatch/draw) -- at that point `IndirectTier1`'s probe (already
honest as of this ADR) starts being functionally consumed, and this
ADR's "Tier1 deferred" framing should be superseded by a new ADR
documenting the actual encoding design. Revisit the synchronous
single-`submit()` consumption model if a future milestone needs the
producer and consumer to be genuinely decoupled across separate host
call sites.
