# ADR-009: Phase A Certificate, Witness, Fault, and Poison Integration

## Status

Accepted for Phase A PortableCore.

## Decision

The reference executor verifies the module and validates the full inferred effect
set against the sound certificate before executing any instruction. A certificate
miss therefore cannot partially mutate an allocation. During successful execution,
the result records an `AccessWitness` containing every observed effect and source
instruction index.

Allocation lookup includes generation and representation epoch. Stale, retired,
out-of-bounds, and invalid-width accesses produce a structured `FaultRecord` while
preserving the distinction between no output and output written before the fault.
`Poisoned` means no output is trustworthy; `PartiallyProduced` means a write was
completed before the fault. Faults never roll back prior stores. `outputs_valid` is
false for every failed execution.

## Consequences

- Certificate validation is a submit-time safety gate.
- Witness remains diagnostic evidence and never weakens the certificate proof.
- E006/E015 reference fixtures are deterministic and can distinguish stale epoch,
  bounds, certificate, and atomic-width failures.
- Backend-specific fault/reset and device-lost propagation remain outside Phase A.

