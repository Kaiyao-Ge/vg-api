# ADR-005: GraphEpoch, AccessWitness, and reference poison diagnostics

Status: Accepted

## Context

Phase A needs a portable model for immutable pointer-bearing topology and for
diagnosing the difference between a declared certificate and actual accesses.
Faults are not transactional rollback; stores before a fault may already be
visible and must be classified accordingly.

## Decision

- `GraphEpochBuilder` deduplicates typed allocation/generation references and
  produces an immutable `GraphEpoch` with a monotonically assigned epoch value.
- `AccessWitness` records actual effect plus instruction index and computes both
  certificate misses and certificate ranges not observed in the run.
- The reference executor returns `FaultRecord`, `missing_effects`, and poison
  state. Invalid IR/certificate failures are `Poisoned`; a later stale, bounds,
  or malformed operation after an earlier trace is `PartiallyProduced`.

These are internal reference semantics. A witness is diagnostic evidence and
never replaces a sound certificate. Backend validation may add source/Node IDs,
but cannot weaken the classification.

## Alternatives

Return only a string error, treat all failures as rollback, or infer topology
identity from raw addresses. Those choices lose the distinction required for
replay, fault analysis, and stale-reference detection.

## Consequences

E006/E015/E018 fixtures can be run deterministically on CPU. Capture v0 can later
serialize epoch and witness records without changing the public ABI. Full graph
mutation, discovery certificates, and cross-process relocation remain future
work.

## Evidence

`model.witness` and `conformance.phase-a` cover GraphEpoch freeze/deduplication,
certificate missing/unused ranges, stale-reference fault records, and poison
classification.

## Revisit trigger

Before dynamic pointer graph discovery, GPU-generated topology publication, or
capture replay across a different address domain.
