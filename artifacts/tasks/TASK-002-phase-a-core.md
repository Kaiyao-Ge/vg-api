# TASK-002: Phase A portable semantic core vertical slice

Status: complete

Normative docs: START; 01 sections 1, 4, and 5; 02 sections 4-10; 03 sections 3,
6-10; 05 sections 4-8 and 12-16; 10 sections 3-5; 11 sections 3-6; 12 Phase A;
13 sections 2-8.

## Outcome

Added an internal C++20 reference core for allocation generation and retirement,
RepresentationEpoch guards, immutable TaskGraph seal/publication, dependency cycle
validation, monotonic Timeline, sound effect certificate coverage, CPU reference
execution, and capture v0 round-trip. Added Phase A unit and conformance tests and
restored clean CMake integration for the targets already declared by the project.

## Evidence

Clean build directory:

```text
cmake -S . -B /tmp/vg-phase-a-build.Mo6cWM -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=OFF
cmake --build /tmp/vg-phase-a-build.Mo6cWM --parallel 2
ctest --test-dir /tmp/vg-phase-a-build.Mo6cWM --output-on-failure
```

Result: 10/10 tests passed, including ABI, tooling, core unit, IR unit, Phase A
conformance, schema generation, and docs checks.

## Remaining risks

This is not the complete Phase A exit set. There is no randomized model checker,
full typed provenance IR, dynamic certificate/discovery, host/device publication
litmus, or public C ABI exposure for the new objects. Metal/Vulkan remain probes;
DeviceHAL lowering is Phase B.
