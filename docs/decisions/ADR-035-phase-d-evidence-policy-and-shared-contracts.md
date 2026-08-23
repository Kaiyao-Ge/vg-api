# ADR-035: Phase D Evidence Policy and Shared Lease / Budget / Overflow Contracts

Status: Accepted

## Context

`docs/vg-project/12-roadmap-and-risks.md` §5 defines Phase D ("Dynamic Graph
and Residency Research") as locating the real boundary between existing
API adapters and a native VG contract. Named work: discovery pass, witness,
Tier2 lowering, quota continuation, working set pressure, optional sparse,
capture visualization. Exit: E004, E010, E011, E014, E017; break-even
curves; a HostAssisted boundary list; NativeContractResearch v1.

Phase B already closed a first E004 row (ADR-025): `CertifiedPinned` /
`Universe` / `DiscoverThenLease` are real on Metal+reference, but
`DiscoverThenLease` degenerates to a host full-arena scan — the same set
as `Universe`, plus measured rescan cost. ADR-025's Revisit trigger says
that result is accurate for unified memory, not a gap to paper over, and
that a later milestone should certify an actual discovered subset when
one exists. Phase D is that revisit. Overwriting
`experiments/definitions/E004-access-certificate.json` as if B never ran
would erase an honest HostAssisted conclusion.

E010 / E011 / E014 / E017 have no Phase D implementation yet. Capture v1
(ADR-011) and ConsumeInput's "cannot restore a consumed representation"
path exist; they are not E014 closure. Task-graph `set_quota` (ADR-010)
refuses at build time; it is not envelope continuation.

Vulkan hardware remains permanently unreachable on this host (ADR-024).
Phase C reused that evidence shape (ADR-030). Phase D does the same.

Four implementation streams (discovery, working-set, Tier2, continuation
+ capture) will otherwise invent three copies of "lease / budget /
overflow". Those nouns must be one core contract, independent of
`AccessCertificate` (sound over-approximation) and of `Allocation`
eviction policy.

## Decision

**1. Evidence shape.** Phase D reuses ADR-024 verbatim: real Metal +
reference results, plus Vulkan compile-review-only evidence, labeled as
such and never reported as execution. `HostAssisted` and `Unsupported`
are legal Phase D conclusions, not gate failures. Unified memory is not
infinite memory. Vulkan sparse is not automatic fault recovery. Public
ABI stays tokens-only.

**2. B's E004 is not D's E004.** D2 revisits discovery so a reachable set
can be smaller than Universe. The Phase B definition and judgement stay
as historical evidence. D adds a revisit definition or an additive
judgement; it does not rewrite B's row into a success it did not earn.

**3. Shared contracts are independent types, not certificate fields.**

- `WorkingSetBudget` — optional limit. Unset (`has_limit == false`) is
  distinct from a set limit of 0. A set limit that the requested bytes
  exceed is a predictable refusal, not a silent clamp.
- `WorkingSetLease` — this submission's residency hold: allocation
  id+generation, a byte ceiling, and a `complete` flag. A lease cannot
  name an allocation absent from a caller-supplied proven set
  (certificate or discovery witness). It is not an `AccessCertificate`
  and is not stored on `Allocation`.
- `EnvelopeOverflow` — work this submit could not fit. `Rejected` cannot
  be reported as continued. `Deferred` is leftover for the next submit
  and requires a non-zero continuation token. This is not
  `TaskGraphBuilder::set_quota` and not a silent quota increase.

`ExecutionPlan` / `Submission` carry these as unset-by-default optionals.
`submit()` does not consume them in this milestone.

**4. Parallel work after this ADR.** TASK-D2 discovery (E004 revisit),
TASK-D3 working set (E011), TASK-D4 Tier2 (E010), TASK-D5 continuation
(E017), TASK-D6 capture view (E014). TASK-D7 writes the HostAssisted
boundary list and NativeContractResearch v1 only after those rows have
honest classifications. NativeContractResearch v1 is a research note,
not a KMD start order. Phase E does not start from this ADR.

**5. Roadmap text.** `12-roadmap-and-risks.md` §5 is annotated with a
Correction pointing here. The original exit sentence is not rewritten.

## Alternatives

- Fold the lease into `AccessCertificate`: rejected — 02 §10 keeps
  certificate (proof) and witness (observation) distinct; residency for
  *this* submit is a third thing.
- Treat budget 0 as "unset": rejected — D1's observable is that the two
  are distinguishable; Universe with `universe_budget == 0` must be able
  to refuse.
- Mark overflow-rejected as continued so a later submit can "retry":
  rejected — that is a silent quota enlarge.
- Wait for Vulkan hardware before Phase D: rejected — same standing
  constraint as ADR-024.

## Consequences

D2–D6 share these types and this evidence rule. Experiment runners that
gain a `phase-d` mapping (TASK-D7) report Vulkan as compile-review-only.
A HostAssisted discovery or bucketed Tier2 path is recordable evidence.

## Evidence

- `src/core/core.h` / `.cpp` — `WorkingSetBudget`, `WorkingSetLease`,
  `EnvelopeOverflow`
- `src/backends/device_hal.h` — optional plan/submission fields
- `tests/unit/core_test.cpp` — construction and illegal combinations
- TASK-D0, TASK-D1

## Revisit trigger

Revisit if Vulkan hardware becomes reachable (execution evidence for D),
or if a discrete-GPU Metal target makes a GPU-resident subset observable
as something other than a software reachability set (ADR-025 Revisit).
