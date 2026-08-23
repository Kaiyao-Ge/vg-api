# ADR-004: Phase A task publication protocol

Status: Accepted

## Context

VG Task publication is a data-plane operation with a control-plane boundary. A
consumer must never observe a partially written Task, and a published slot must
not become writable again until it has been consumed and returned to the ring.

## Decision

The Phase A reference model uses a bounded `PublicationRing` with these states:

```text
Empty -> Writing -> Published -> Consumed -> Empty
```

`reserve` changes `Empty` to `Writing`; the producer writes the complete Task;
`publish` performs a release transition to `Published`; `acquire` observes the
slot with acquire ordering; `consume` transitions it to `Consumed`. A slot cannot
be written, acquired, or consumed in an invalid state. Task generation fields are
validated before publication.

This is a reference memory-model simulation, not a claim about a particular
Metal/Vulkan command primitive. Backend adapters must later map the release/acquire
contract to an expressible device/queue/system scope or return `Unsupported`.

## Alternatives

Use a mutex-only queue, expose a mutable Task object, or let the consumer infer
publication from a non-atomic count. Those alternatives do not model the required
release/acquire boundary or torn-record failure.

## Consequences

The portable core has a deterministic litmus target and can run property tests
without a GPU. Ring capacity and overflow are explicit. Multi-producer fairness,
GPU cache scopes, and cross-process publication remain backend/capture work.

## Evidence

`tests/model/phase_a_model_test.cpp` checks invalid state transitions, publication
visibility, consumed-slot immutability, deterministic generation retirement, and
1,000 randomized allocation/epoch operations.

## Revisit trigger

Before implementing GPU-generated Task Tier 0/1 or exposing publication through the
public C ABI.

**Triggered:** GPU-generated Task Tier0 (Metal, hardware-verified) and Tier0+Tier1
(Vulkan, code-review-only) were implemented in B7/B8 -- see ADR-019 (reference
oracle), ADR-020 (Metal timeline), ADR-021 (Metal Task Tier0), ADR-022 (Vulkan
Task Tier0/Tier1 + timeline). The release/acquire boundary this ADR specified
maps to `atomic_compare_exchange`+`atomic_store_explicit` (Metal MSL) and
`atomicCompSwap`+`atomicExchange` (Vulkan GLSL) against a GPU buffer, exactly
as the "backend adapters must later map the release/acquire contract to an
expressible device/queue/system scope" clause above anticipated. Public C ABI
exposure of publication remains untriggered.
