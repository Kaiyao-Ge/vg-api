# ADR-001: Phase 0 public ABI boundary

Status: Accepted

## Context

Phase 0 needs a stable loader and observable adapter enumeration without claiming
that Task, allocation, or submission semantics are implemented.

## Decision

`VgApi` version 1.0 exposes `createRuntime`, `destroyRuntime`, and
`enumerateAdapters` only. All public structures start with `VgStructHeader`; runtime
allocator callbacks are an all-or-nothing pair. Host handles remain opaque and are
validated through the runtime registry before use.

## Alternatives

Declare the full future API with unsupported stubs, or expose only runtime creation.

## Consequences

Future Phase A/B functions require an ABI revision decision and layout golden update.
The Phase 0 table is small enough to smoke test from C11 and C++17.

## Evidence

`tests/abi` covers loader sizing, callback pairing, output preservation, lifecycle,
and adapter count semantics.

## Revisit trigger

Before adding public allocation, Task, Timeline, or submission calls.
