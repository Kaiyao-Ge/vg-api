# ADR-003: Phase A reference-core vertical slice

Status: Accepted

## Context

Phase A needs an executable semantic baseline before backend lowering. The existing
Phase 0 probe and ABI do not provide allocation lifetime, task publication, effect
validation, or deterministic reference execution.

## Decision

Implement these semantics first as internal C++20 `vg_core` contracts:

- `Arena` owns allocations and validates active generation references;
- retirement increments generation and rejects stale references;
- representation transforms advance an allocation's `RepresentationEpoch` only
  when no in-flight reference exists;
- `TaskGraphBuilder` is mutable until `seal`, then `TaskGraph::publish` makes the
  submission-visible graph immutable;
- dependency edges are checked for cycles;
- `Timeline` accepts strictly increasing signal values;
- `Certificate` uses the existing IR effect coverage relation;
- the CPU reference executor reports `Valid`, `PartiallyProduced`, or `Poisoned`;
- capture v0 stores canonical IR, IR hash, allocations, and task observations.

This is an internal Phase A slice. It does not expand the public C ABI or claim
Metal/Vulkan execution support.

## Alternatives

Expose the full future C ABI immediately, or implement a backend before the
portable semantics exist. Both would make invalid or unsupported behavior hard to
distinguish from successful lowering.

## Consequences

The core can be tested deterministically on every host and can serve as the
reference oracle for later adapters. Allocation identity and publication remain
deliberately simple until the address/capture and Task publication ADRs are
revisited with E003/E018 evidence.

## Evidence

`core.unit`, `ir.unit`, and `conformance.phase-a` cover generation retirement,
representation epoch guards, publication, dependency cycles, timeline monotonicity,
certificate rejection, poison, reference execution, and capture round-trip.

## Revisit trigger

Before adding public allocation/Task/Timeline calls, dynamic graph discovery,
cross-process capture relocation, or a real DeviceHAL submit path.
