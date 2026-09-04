# ADR-054: Mixed-domain TaskGraph Execution Schedule

Status: Accepted (design frozen; implementation delivered by backend; Linux platform gate pending)

## Context

ADR-046 deliberately made rasterization a `TaskRecord` shape instead of adding a
parallel raster-pass list or submit primitive, so compute, raster, and future
execution domains could eventually share one TaskGraph, dependency graph, resource
lifetime, and completion contract. ADR-047 and ADR-052 then imposed all-raster
scope cuts for restricted-MSL and SceneRoot submissions because the implementation
still projected one whole graph onto one `module` or `user_raster_shader`, could not
resolve every raster/root-embedded facet effect before HAL, and lacked verified
per-Node backend lowering. ADR-053 repaired device-scoped NodeRef authority and
multi-CodeObject plans but explicitly did not reopen mixed-domain execution.

The remediation program has since established a sole Stage 0--5 assembler,
per-Task effects over immutable Node snapshots, finite access/lifetime sets,
per-Node `CompiledPlan` packages, and Node-aware Reference/Metal/Vulkan compute
lowering. These repairs remove the architectural reason to treat execution domain
as a whole-plan variant. They do not by themselves make mixed-domain execution an
implemented capability: at design freeze, the runtime rejection remained required
until the schedule, backend lowering, and conformance gates in this ADR were
delivered. Current per-backend evidence and remaining restrictions are recorded
under Implementation status below; the decision itself is unchanged.

The existing `task_order` is a deterministic topological serialization used for
cross-backend comparison, capture, diagnostics, and publication. Treating it as a
mandatory serial hardware schedule would discard parallelism already proved by the
validated EffectGraph. Conversely, allowing each backend to rediscover components,
order, hazards, or failure reachability would reintroduce multiple execution facts.

The compute Task ring is an internal Tier-0 publication wire format. It is not the
TaskGraph, does not carry raster fields or a domain discriminator, and must not
become a second execution authority merely because a graph contains multiple
domains.

## Decision

### 1. One generic plan; requirements come from actual Task contents

There is no `MixedPlan`, mixed-domain handle, parallel graph type, or second submit
entry point. An `ExecutionPlan` may contain Tasks from one or more execution
domains. Stage 5 derives the set of domains and capability requirements from the
actual Tasks and their resolved Node contracts:

- `{Compute}` remains an ordinary compute plan even though the representation can
  also express raster;
- `{Raster}` requires raster lowering;
- `{Compute, Raster}` requires every Node/domain contract in that concrete plan to
  be lowerable together;
- future ray, tensor/neural, or video domains extend the same set rather than
  creating a new top-level plan variant.

A backend must not reject a compute-only instance merely because `ExecutionPlan`
can represent mixed domains. If any actual Node domain is unsupported, Stage 6
rejects the complete plan before Commit and reports the exact NodeRef/domain
requirement as `Unsupported`. Executing only supported components, dropping an
unsupported Task, or silently moving it to another backend is not allowed. Such
multi-adapter or partial-plan delegation would require a separate explicit
contract.

### 2. Stage 5 seals one `ExecutionSchedule`

The sole Core assembler derives an internal immutable execution schedule from the
validated EffectGraph after per-Task Compute/Raster effects and explicit
dependencies have been resolved. Structural edges are `Explicit` and
`InferredConflict`; submission-wide Timeline, Envelope, and publication facts
remain outside Task-component partitioning.

The schedule contains three related views produced and validated together:

1. **Independent components.** Weakly connected components of the structural
   EffectGraph form independent execution islands. Different components have no
   explicit dependency or conflicting access. They may still share read-only
   resources or non-overlapping ranges.
2. **Component-local waves.** Kahn ready frontiers form deterministic parallel
   waves inside each component. Tasks in one wave have no unsatisfied structural
   edge between them and are eligible for concurrent execution. A later wave does
   not begin until its required predecessor waves complete.
3. **Canonical `task_order`.** The existing lowest-ready-index deterministic
   topological semantics are preserved as the stable flattened observation order.
   It is not a claim that hardware executed the Tasks serially.

Backends consume this sealed schedule. They must not rebuild ordering, effects,
components, or waves from the raw TaskGraph.

### 3. Cross-domain edges become domain-neutral wave transitions

Core does not define special Compute-to-Raster, Raster-to-Compute, or future
domain-pair barrier rules. It seals a `WaveTransition` between dependent waves
that distinguishes:

- pure execution completion requirements;
- buffer/Region producer-consumer visibility derived from effects;
- facet/image representation and layout requirements;
- any already-sealed physical representation operation that must precede the
  consumer wave.

Stage 6 lowers each transition to actual backend operations. Reference uses
deterministic sequencing. Metal may use encoder boundaries, per-producer fences,
command-buffer completion, or events. Vulkan may use aggregated execution,
buffer-memory, image-memory, layout, and queue-ownership dependencies. A single
conservative wave boundary may merge multiple Task edges when it preserves every
sealed requirement, but it must not hide a global barrier, host wait, layout
transition, or serialized fallback from `LoweringReport`.

A wave abstraction removes domain-pair special cases from Core; it does not erase
the backend obligation to make writes visible or to perform image/representation
transitions.

### 4. Failure and poison follow graph reachability

A logical Task failure is not a transaction rollback:

- the failed Task's unstarted structural descendants are cancelled;
- Tasks already running may complete and may have visible stores;
- branches and independent components that do not depend on the failed Task are
  permitted to complete;
- if any output may have become visible before overall failure, the submission is
  `PartiallyProduced`; otherwise it is `Poisoned`;
- the primary `FaultRecord` is the known failed Task with the earliest canonical
  `task_order` position when multiple logical failures are observed;
- the existing aggregate `outputs_valid` cannot promise per-output validity. A
  future per-output result contract would require a separate ABI decision.

A command-buffer, queue, device-lost, or otherwise unattributable device failure
may invalidate the complete submission. Component independence is a scheduling
and logical-failure property, not a promise of hardware fault containment.

### 5. Publication is the complete canonical graph; the compute ring is narrow

`Submission::published_tasks` denotes the complete Envelope-filtered Task sequence
in canonical `task_order`, independent of execution domain. It must not mean only
the records that happened to pass through a compute-specific GPU wire format.

The existing compute-only Task ring remains an optional/narrow physical Tier-0
publication mechanism for Compute Tasks and its accepted conformance evidence is
preserved. It is not the source of TaskGraph membership, ordering, or execution.
For a mixed graph, Core's sealed schedule and Envelope remain the publication
authority; any GPU-ring use for the compute subset is reported separately and may
not cause non-compute Tasks to disappear from `published_tasks`.

No raster-only, ray-only, tensor-only, or video-only ring is introduced by this
decision. If a future workload genuinely requires the GPU to create authorized
Tasks across execution domains, it must use one versioned, discriminated internal
publication schema and a separate Tier-3 decision. That future problem is tracked
in `docs/issue/gpu-generated-cross-domain-tasks.md`.

### 6. Stage 6/7 evidence is per Node and per physical operation

Stage 6 produces one immutable package/pipeline identity per complete NodeRef and
physical transition operations for the sealed schedule. Stage 7 selects the
package for each Task, executes waves/components according to the schedule, and
records actual encoder, command-buffer, dispatch/draw, fence/barrier, transition,
cache-hit, host-wait, and fallback counts.

`LoweringReport` must match the commands actually encoded. A backend may serialize
an otherwise parallel component only through an honestly reported
`SerializedFallback` or other applicable lowering class. It may not claim Direct
parallel or mixed-domain support from capability bits when its actual submit path
uses a single program, skips a domain, or reconstructs a different order.

### 7. ABI and lifecycle remain unchanged

This decision does not modify `VgTaskRecordV2`, add a public handle, add a submit
primitive, create a parallel resource lifecycle, or authorize a backend to expose
native pipeline/resource handles. Allocation, Region, Facet, RepresentationEpoch,
NodeRef, Envelope, lifetime hold, Timeline, fault, and completion contracts remain
shared by every execution domain.

## Alternatives

- **One Task containing multiple Nodes:** rejected. That is an implicit subgraph
  with ambiguous program, effect, fault, shape, and pipeline ownership. Composition
  belongs in TaskGraph edges.
- **A top-level Compute-or-Raster plan variant:** rejected. It makes the whole
  submission choose one domain and cannot extend to future domains without
  parallel optional fields and submit paths.
- **Serialize every Task in `task_order`:** rejected as the semantic minimum.
  `task_order` remains the deterministic observation order; the sealed schedule
  preserves independent components and ready-wave parallelism.
- **Backend-local graph partition and ordering:** rejected. It would give each
  adapter a different execution fact and make Reference comparison, faults, and
  barrier counts non-portable.
- **One generic device-wide barrier after every wave:** allowed only as an honest
  reported fallback when it also satisfies resource/layout requirements. It is
  not the Core contract and must not hide avoidable global synchronization.
- **Execute supported components and skip unsupported domains:** rejected. A
  successful compile covers the complete plan; partial/multi-adapter delegation
  is a separate semantic feature.
- **Add one publication ring per execution domain:** rejected. It multiplies wire
  protocols and recreates parallel task systems.
- **Put every domain into the existing 14-word compute ring:** rejected. The wire
  has no discriminator or raster payload, and `VgTaskRecordV2` is frozen.

## Consequences

- ADR-047's and ADR-052's mixed compute+raster restrictions are reopened as an
  implementation target; each restriction remains until its validation gates
  pass. The accepted design alone is not evidence of implementation. Current
  canonical mixed support does not remove restricted-MSL or SceneRoot narrowing.
- Core gains one richer immutable schedule fact rather than a second graph or
  backend-local scheduler.
- Independent components and waves are parallelism permissions, not mandatory
  hardware concurrency or fault-containment guarantees.
- Backends that support only Compute continue accepting compute-only plans.
  Vulkan continues to reject any concrete plan containing Raster until it
  advertises and verifies Raster plus the required transition lowering.
- The compute ring remains compute-only without forcing rings for other domains.
- Future execution domains extend Node contract, Task domain/payload schema,
  capability derivation, and backend lowering while sharing this schedule and
  lifecycle.

## Evidence

At design freeze this ADR recorded a reviewed design, not implementation evidence.
The foundations at that time were:

- `ExecutionPlanAssembler` already seals per-Task effects, the validated
  EffectGraph, and deterministic `task_order` from immutable per-Node snapshots;
- Compute and Raster effects are resolved to actual allocation/facet access before
  HAL, including SceneRoot backing facts;
- Stage 6 already stores per-Node package kinds and `LoweringReport`;
- Reference executes per Task in sealed order, while Metal and Vulkan now have
  Node-aware compute package/dispatch paths;
- the compute-only ring has one generated schema and explicit Raster rejection.

Implementation is complete only when all of the following hold:

1. Core tests cover deterministic components, waves, canonical flattening,
   read/read sharing, non-overlapping ranges, inferred conflicts, explicit-only
   dependencies, fork/join, multiple independent chains, and malformed seals.
2. Reference conformance executes Compute-to-Raster and Raster-to-Compute graphs,
   independent mixed components, failure descendants, concurrent-independent
   outcomes, primary-fault selection, repeat submit, lifetime holds, and
   representation transitions.
3. Real Metal conformance demonstrates per-Task Node package selection, mixed
   compute/render encoder execution, exact wave-transition synchronization,
   independent-component eligibility, result parity where Reference is an oracle,
   and honest exceptions for restricted user shading.
4. Vulkan accepts compute-only plans based on contents and rejects concrete Raster
   Nodes with a precise `Unsupported` event until a Linux Vulkan Raster/mixed path
   is implemented and verified.
5. `published_tasks` contains the complete Envelope-filtered canonical sequence on
   every backend; optional compute-ring events and counts remain physically true.
6. Static gates confirm no backend rebuilds order/effects/components from the raw
   graph and no mixed path projects to a first/global Node package.

### Implementation status (2026-09-03)

MD-1/2 seal the schedule and shared Stage 6/7 transition contract. MD-3 supplies
the Reference mixed semantic baseline. MD-4 implements canonical Compute plus
built-in Raster on real Metal with explicit conservative serialization; its
[delivery record](../reports/md4-metal-mixed-domain-completion.md) preserves the
exact physical counts, failure evidence, and narrowing. Its R→C atomic coverage
is explicitly HostAssisted, not native render-to-compute fence evidence.

MD-5 adapts Vulkan to the sealed schedule while keeping concrete Raster plans
whole-plan Unsupported. MD-6 adds public C ABI mixed conformance with distinct
CodeObjects/NodeRefs, observable C→R/R→C, and precise rejection/no-partial-execution
checks. The [integration ledger](../reports/md5-md6-mixed-domain-integration.md)
separates implementation, local verification, platform verification, and
documentation. Linux Vulkan SDK/device validation remains pending; this is not
whole-route closure. No ABI, Task ring schema, new submit primitive, or resource
lifecycle is added. Restricted user-raster mixed and SceneRoot narrowing remain.

## Revisit trigger

Revisit when per-output validity or multiple public faults require an ABI change;
when a backend can prove profitable cross-queue scheduling beyond component/wave
lowering; when component-level multi-adapter delegation is proposed; or when GPU
generation of cross-domain Tasks reaches the trigger conditions in
`docs/issue/gpu-generated-cross-domain-tasks.md`.
