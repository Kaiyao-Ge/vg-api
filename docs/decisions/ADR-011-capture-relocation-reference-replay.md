# ADR-011: Phase A Capture Relocation and Reference Replay

## Status

Accepted for Phase A PortableCore.

## Decision

Capture v1 stores canonical IR and its hash together with allocation snapshots,
initial bytes, generation, representation epoch, state, stable graph references,
Timeline value, certificate, witness, execution fault/poison metadata, and source,
compiler, and schema hashes. Runtime addresses are never serialized; references use
allocation stable IDs plus offsets.

Replay imports snapshots into a fresh Arena and builds an explicit stable-ID
relocation map before invoking the CPU reference executor. Replay therefore checks
the actual result and poison/fault behavior instead of only comparing an IR hash.
Capture content is canonicalized and protected by a content hash. Unsupported
schema/version, IR hash mismatch, malformed bytes, duplicate allocation, missing
relocation, and unknown required fields are rejected without repair. Legacy v1
allocation snapshots without byte payloads remain readable as zero-initialized
metadata-only captures.

## Consequences

- Same-environment normal and fault captures can be re-executed deterministically.
- Fault captures preserve partial output bytes and diagnostic poison state.
- Capture remains backend-neutral and does not leak Metal/Vulkan addresses.
- Cross-device/backend migration policy and external resource import remain later
  phase work.

