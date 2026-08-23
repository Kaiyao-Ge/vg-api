# ADR-013: Phase B LoweringReport and Conformance Harness

## Status

Accepted for Phase B B1.

## Decision

Every compiled plan reports backend, support status, diagnostic text, and
operation events classified as `Direct`, `CachedObject`, `DevicePass`,
`HostAssisted`, `Serialized`, or `Unsupported`, including counts, bytes, and a
reason. A report containing HostAssisted events is observable as such; it cannot
be presented as a native fast path.

The B1 harness submits the same minimal canonical IR plan to the reference
DeviceHAL and checks capability/version validation, compile, submit, output
correctness, report serialization, unsupported-plan rejection, and hidden-wait
diagnostics. Future Metal/Vulkan harnesses reuse this contract and test fixture
without duplicating semantic expectations.

## Consequences

- Correctness and cost accounting are separated.
- Native adapters can report `Unsupported` or `HostAssisted` honestly.
- Performance experiments can consume a stable report schema rather than parsing
  backend debug text.

