# ADR-023: Phase B Experiment Gate Findings (Documentation Only)

Status: Accepted

## Context

B7/B8 (ADR-019~022, TASK-B7/TASK-B8) delivered the Task Tier0/timeline/
Tier0+Tier1 mechanisms this implementation plan scoped as "Phase B" --
hardware-verified on reference+Metal, compile-review-only on Vulkan. That
completion was reported to the user as "Phase B complete."

Returning to the project's own roadmap (`docs/vg-project/12-roadmap-and-risks.md`)
and experiment catalog (`docs/vg-project/09-experiment-catalog.md`) surfaced a
different bar: the catalog's gate table (lines 236-243) defines the
`ComputeAdapter`/Phase B exit gate as **E002, E004, E007, E009, E012 producing
results on Metal/Vulkan** -- not "the mechanisms exist." None of these five
experiments exist under `experiments/` (only E001/E003/E006/E015/E018, all
Phase A/Semantic Core gate items, have definitions or folders).

Two Explore agents were used to read the actual code paths each of the
three M1-feasible experiments (E002, E007, E012) would need, to determine
whether they are benchmark-wrapper work or require new core capability.
A follow-up repo-wide grep pass then checked the remaining two
(E004, E009) against the same question. This ADR records what that
reading found, as a documentation artifact -- **it is not a decision to
build any of it now.** Per the user's explicit choice ("先只写文档，不写代码"),
this milestone is documentation-only.

**Finding that applies to all five**: every one of E002/E004/E007/E009/E012
requires a core capability that does not exist in any form today -- not a
partial implementation, not a stub, not a "wrong but present" version. The
gap is uniform across the whole Phase B gate, not specific to the three
M1-feasible experiments first suspected of it.

## Decision

Record, as verified-by-reading current-state facts, what each experiment is
missing:

**E002 (typed pointer graph)**: `core::Arena` (`src/core/core.h:27-52`) is
flat blob allocation only, with no graph/pointer structure. `GraphEpoch`/
`PointerRef` (`core.h:66-99`) is a flat reachability-set snapshot, not a
typed pointer graph -- no adjacency/traversal exists. The IR
(`src/ir/ir.cpp:32`) supports only `load, store, atomic_add, publish` --
no indirect/pointer-chasing opcode exists. E002 needs a new IR opcode plus
codegen for pointer-chasing dispatch on both backends before any benchmark
is meaningful.

**E004 (adapter memory policy)**: repo-wide search confirms zero hits for
`AccessCertificate`/`CertifiedPinned`/`DiscoverThenLease`/`SoftwarePaged`/
`FaultManaged`/`Universe` anywhere under `src/`/`include/` -- none of the
five adapter policies the roadmap names exist in any form, not even a
stub or partial variant. This is the same "zero implementation" pattern
as E002/E007/E012, not a smaller gap: E004 needs an entirely new
access-certificate concept (static/discovery/Universe/paged/fault-managed
lease semantics) built into DeviceHAL before any variant can even compile,
let alone be benchmarked.

**E007 (root pointer vs. binding cost)**: neither backend has a
bindless/argument-buffer codegen path; current codegen only produces
direct root-schema bindings. E007 needs that second codegen path built
before a binding-cost comparison is possible.

**E009 (GPU-generated same-Node work)**: blocked by three separate things,
not two. Repo-wide search confirms zero hits for any culling/compaction
kernel (`cull`/`compact`) on either backend -- the GPU cull+compact pass
E009's workload (million-instance culling, output feeding the same-Node
draw/dispatch) requires does not exist at all, independent of Tier1. On
top of that missing kernel: Metal Tier1 is itself deferred (ADR-021, a
deliberate spec-consistent choice, not an oversight), and Vulkan's Tier1
(implemented, code-review-only per ADR-022) cannot be exercised because
Vulkan hardware remains unreachable on this machine. E009 needs the
culling kernel built first, and then a real Tier1 dispatch path on at
least one backend, before the "VG Task Tier1" variant this experiment
compares against baselines can produce any result at all.

**E012 (Effect DAG/timeline sync quality)**: `metal_device_hal.mm`'s
`submit()` is structurally always exactly one `MTLComputeCommandEncoder`
per one `MTLCommandBuffer`, fully serial via `waitUntilCompleted` --
zero uses of `MTLFence`, `memoryBarrier`, or non-shared `MTLEvent` exist
anywhere in the codebase. `LoweringReport` (`device_hal.h:54-74`) has no
barrier-count or sync-structure field. Zero GPU/CPU timing instrumentation
exists anywhere (`std::chrono`, `MTLCounterSampleBuffer`, `os_signpost` --
all zero hits). `DeviceSnapshot::caps.timestamps_available` is hardcoded
`false` on both Metal and reference (Vulkan probes `timestampValidBits`
honestly but nothing consumes it). E012 needs GPU timing infrastructure
plus a `submit()` path capable of genuine multi-encoder concurrency before
sync-quality can be measured at all -- today there is nothing to measure a
difference between.

## Alternatives

- Implement one or more of E002/E007/E012 this milestone: rejected. Each
  requires a new core capability (new IR opcode, new codegen path, or new
  concurrency/timing infrastructure), not a benchmark wrapper -- the true
  scope is a multi-milestone project per experiment, discovered only after
  deep-reading the actual code paths (initial framing as "run three
  experiments" undersold this). Rushing a partial implementation to close
  the gap faster would risk exactly the kind of under-verified, silently
  degraded work this project's invariants (`docs/START.md` §4) forbid.
- Silently continue treating B7/B8's "Phase B complete" framing as
  sufficient, without reconciling it against the roadmap's own gate table:
  rejected -- this would let two different definitions of "Phase B done"
  coexist unexamined, which is the dishonest-status-reporting failure mode
  this project's documentation discipline exists to prevent.

## Consequences

This ADR changes no code and no runtime behavior. It converts a
scope-discovery finding into a reusable design input: the next milestone
that picks up any of E002/E004/E007/E009/E012 can start from this ADR's
per-experiment gap list instead of re-deriving it by reading the code
again. `docs/reports/phase-b-gate.md` and `TASK-B9` are the reporting and
task-tracking counterparts of this same finding.

## Evidence

Findings above come from two Explore-agent deep reads performed this
session, covering: `src/core/core.h` (`Arena`, `GraphEpoch`, `PointerRef`,
`TaskRecord`, `EffectGraph`/`EffectEdge`), `src/ir/ir.cpp` (opcode list),
`src/backends/metal/metal_device_hal.mm` (`submit()` structure, sync
primitive usage), and `src/backends/device_hal.h` (`LoweringReport`,
`DeviceSnapshot::caps`). A follow-up `grep -rn` pass across `src/`/`include/`
for `AccessCertificate`/`CertifiedPinned`/`DiscoverThenLease`/
`SoftwarePaged`/`FaultManaged`/`Universe` (E004) and `cull`/`compact`
(E009) returned zero matches for either set, confirming both experiments
have no partial implementation to build on. Cross-checked against
`docs/vg-project/09-experiment-catalog.md` (full 244-line read, gate table
lines 236-243, plus E004/E009's own definitions at lines 52-62 and
116-126) and `docs/vg-project/12-roadmap-and-risks.md` (Phase B section,
lines 25-34).

## Revisit trigger

Revisit -- and supersede the relevant portion of this ADR with a new,
implementation-specific ADR -- whenever a future milestone begins
implementing any of E002, E004, E007, E009, or E012. In particular: an
"E012 concurrent submit() design" ADR once Metal's submit() gains
multi-encoder/fence-based concurrency; an "E002 pointer-chasing IR" ADR
once a new opcode is added; an "E007 bindless codegen" ADR once a second
codegen path exists; an "E004 access-certificate model" ADR once any of
the five lease policies gets a real design; an "E009 GPU culling" ADR
once a cull/compact kernel exists on either backend.
