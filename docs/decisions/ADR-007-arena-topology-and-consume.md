# ADR-007: Phase A Arena topology and ConsumeInput model

Status: Accepted

## Context

Phase A must distinguish pointer-bearing topology identity from allocation backing
and representation identity. In-flight references cannot be invalidated by retire,
transform, or destructive consume.

## Decision

- Arena increments `topology_epoch` on allocation add, retire, and consume.
- `GraphEpochBuilder` can validate references against an Arena and seals a
  deduplicated immutable reference set at the Arena topology epoch.
- Representation-aware lookup requires allocation generation and exact
  `representation_epoch`.
- `retire` rejects in-flight allocations.
- `transform` rejects in-flight or stale representation epochs and advances the
  RepresentationEpoch only after the proof succeeds.
- `consume` requires an active exact epoch and exclusive ownership, then retires
  the old generation. It does not promise rollback or in-place backend behavior.

## Alternatives

Use a single global version, mutate backing in place, or let adapters infer
exclusive ownership. Those choices allow stale pointer/facet interpretation and
make failure recovery ambiguous.

## Consequences

The reference model can express E001/E018 and the core proof required before a
future representation transform. Backend allocation/facet implementation remains
deferred to Phase B/C.

## Evidence

`core.unit` and `model.phase-a` cover topology increments, active GraphEpoch
references, stale epoch rejection, in-flight protection, retirement, and consume.

## Revisit trigger

Before dynamic pointer graph discovery, multi-version backing, or a public
allocation/representation ABI.
