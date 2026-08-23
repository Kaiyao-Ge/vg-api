# TASK-005: Canonical IR and schema validation

Status: complete

Normative docs: START; 03 sections 3 and 7; 05 sections 4-8, 14-16; 10 sections
3-5; 11 sections 3-6; 12 Phase A.

## Outcome

Aligned `schemas/ir/v1.json` with the actual canonical module format, hardened IR
parsing/verifying for non-zero identity/generation, range overflow, valid
instruction kinds, root schema, and effect coverage, and extended negative IR
unit tests. No public ABI changed.

## Evidence

The clean Phase A build reports 12/12 tests passed, including `ir.unit` and
`schema.generate`.

## Remaining limits

The frontend is still a fixture C-like parser. Typed SSA, provenance, dynamic
effect expressions, and imported shader IR remain outside TASK-005.
