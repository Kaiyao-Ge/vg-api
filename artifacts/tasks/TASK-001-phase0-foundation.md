# TASK-001: Phase 0 repository and evidence foundation

Status: in-progress

Normative docs: START; 01 sections 1 and 4; 03 sections 1, 3, and 5; 04 sections 1-7 and 16; 06 sections 1-3; 08 sections 3-6 and 16; 10 sections 1-4; 12 Phase 0; 13 sections 1, 4, 8, 10-13.

Invariants: no implicit adapter selection; no backend handles in public ABI; M1 results
are `MetalAdapter` or `SemanticReference`; unsupported capabilities are omitted or
reported, never fabricated.

Files: CMake presets, public header, runtime/probe backends, tests, schemas, runners,
ADRs, README.

Tests: ABI C/C++, reference and Metal CTest, runner bundle, docs checker.

Risks: Linux Vulkan code is conditionally compiled and cannot be validated on this M1.
Decision needed: none.
