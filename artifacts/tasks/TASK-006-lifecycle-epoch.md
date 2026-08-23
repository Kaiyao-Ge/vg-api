# TASK-006: Arena topology and representation lifecycle

Status: complete

Normative docs: START; 02 sections 4, 8-10; 03 sections 7-10; 05 sections 6-8;
09 E001/E005/E018; 10 sections 3-5; 11 sections 3-6; 12 Phase A.

## Outcome

Added Arena topology epochs, Arena-bound GraphEpoch validation, exact
RepresentationEpoch lookup, in-flight retire protection, stale-epoch transform
rejection, and exclusive ConsumeInput retirement. Added unit/model coverage for
topology publication, stale references, in-flight protection, and generation
invalidation.

## Evidence

The clean Phase A build reports 12/12 tests passed. Sanitizer validation is run
with `VG_ENABLE_SANITIZERS=ON` after TASK-006 integration.

## Remaining limits

This is a deterministic CPU lifecycle model. Multi-version backing, capture
relocation, dynamic discovery, and backend representation transforms remain later
phase work.
