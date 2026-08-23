# TASK-003: Phase A publication and property model

Status: complete

Normative docs: START; 02 sections 4-6 and 9; 03 sections 7-10; 05 sections 12-13;
10 sections 4-5; 12 Phase A and P0 risks; 11 sections 3-6.

## Outcome

Added the bounded internal `PublicationRing` state machine with release/acquire
publication semantics, explicit invalid-state rejection, and generation checks.
Added a deterministic randomized property test covering allocation retirement,
stale references, in-flight RepresentationEpoch guards, and publication
immutability.

## Evidence

```text
cmake --build /tmp/vg-phase-a-build.Mo6cWM --parallel 2
ctest --test-dir /tmp/vg-phase-a-build.Mo6cWM --output-on-failure
```

Result: 11/11 tests passed, including `model.phase-a`.

## Remaining limits

The test is a bounded host model, not a GPU litmus or cross-process memory test.
Device scope mapping belongs to Phase B adapters. Dynamic graph discovery,
certificate composition and full capture relocation remain open Phase A research.
