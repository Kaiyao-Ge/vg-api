# ADR-006: Canonical IR v1 validation boundary

Status: Accepted

## Context

The first Phase A IR implementation could round-trip instructions but accepted
too many malformed modules and conflated compiler-produced declarations with the
runtime's certificate proof. Schema v1 must be stable enough for reference
execution and capture without freezing a full source language.

## Decision

Canonical `vg.ir/v1` modules require `schema`, `version`, `root_schema`, a
non-empty `instructions` array, and an `effects` array. Every instruction carries
non-zero allocation identity and generation, non-zero size, checked range
arithmetic, operation kind, and representation epoch. Effects use the same
allocation/range/access/epoch relation as certificate coverage. Serialization is
canonical before hashing.

The Phase A C-like frontend remains a fixture frontend. Its inferred effects are
verified by the IR verifier; it is not a complete language implementation and
does not define the public ABI.

## Alternatives

Accept arbitrary JSON and defer validation to the backend, or make the frontend's
declared effects authoritative. Both would allow malformed/stale access contracts
to reach execution.

## Consequences

Malformed modules fail deterministically before execution. The schema can evolve
through versioned modules and capture migration rather than undocumented fields.
Typed SSA, provenance, dynamic graph effects, and source-language syntax remain
later work.

## Evidence

`ir.unit`, `conformance.phase-a`, schema generation, and docs checks cover canonical
round-trip, hash stability, invalid generation, range overflow, missing effects,
and layout compatibility.

## Revisit trigger

Before adding typed SSA, imported SPIR-V, dynamic pointer effects, or public
CodeObject loading.
