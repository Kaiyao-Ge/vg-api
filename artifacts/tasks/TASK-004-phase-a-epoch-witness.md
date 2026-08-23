# TASK-004: Phase A graph epoch and witness diagnostics

Status: complete

Normative docs: START; 02 sections 4, 6, 8-10; 03 sections 7-10; 05 sections 6-8
and 14; 09 E006/E015/E018; 10 sections 3-5; 12 Phase A; 11 sections 3-6.

## Outcome

Added immutable GraphEpoch construction for pointer-bearing allocation/generation
references, AccessWitness collection and certificate diffing, and structured
FaultRecord/missing-effect diagnostics in the CPU reference executor. Added
negative and model tests for stale references, certificate misses, unused ranges,
epoch freezing, and poison classification.

## Evidence

```text
cmake --build /tmp/vg-phase-a-build.Mo6cWM --parallel 2
ctest --test-dir /tmp/vg-phase-a-build.Mo6cWM --output-on-failure
```

Result: 12/12 tests passed.

## Remaining limits

Capture v0 does not yet serialize witness/fault records; dynamic discovery and
cross-process address relocation are intentionally deferred. This remains
PortableCore evidence and does not imply Metal/Vulkan support.
