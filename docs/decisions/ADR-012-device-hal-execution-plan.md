# ADR-012: Phase B DeviceHAL and ExecutionPlan Contract

## Status

Accepted for Phase B B0.

## Decision

DeviceHAL receives an immutable `ExecutionPlan` after PortableCore validation.
The plan carries a versioned capability snapshot, canonical IR, certificate,
sealed TaskGraph, graph/timeline points, and publication state. It contains no
unvalidated user pointers or backend object handles.

DeviceHAL exposes capability inspection, compile/lowering, and submit through an
internal C++ interface. Compiled plans retain the input plan and a
`LoweringReport`; submissions retain the execution result, completion timeline,
and report. Capability absence is a validation failure or explicit unsupported
result, never a silent fallback.

The initial ABI is internal and versioned independently from the public C ABI.
Metal/Vulkan adapters must implement the same contract but may use different
backend resources and lowering classes.

## Consequences

- Core semantics remain backend-independent.
- Adapter hidden costs have a structured home before native lowering exists.
- Reference HAL provides a deterministic oracle and conformance target.
- Adding allocation/facet/timeline plugin calls requires a follow-up ABI decision;
  this slice deliberately keeps the contract minimal.

