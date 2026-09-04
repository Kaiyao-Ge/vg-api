# TASK: Linux Vulkan build and test runbook

Status: complete (architecture/command audit; remote execution pending)

## Goal

Provide an executable command sequence for the existing Linux Vulkan backend on
the user's newly available NVIDIA host. Review the actual build graph, runtime
compiler, device selection, CTest registration, and evidence limitations.

## Scope and invariants

- Read START and architecture/backend/compiler/experiment/build specifications.
- Preserve all runtime semantics, ABI, capabilities, test expectations, and gates.
- Do not confuse reference execution, internal skips, negative tests, or historical
  compile-review-only status with successful Vulkan workload execution.
- Do not disable sanitizer or Vulkan validation to manufacture success.

## Files

- `docs/reports/linux-vulkan-runbook.md`: commands and coverage audit.
- This task note.

## Verification

Cross-check commands against CMakeLists.txt, CMakePresets.json, included CMake
fragments, Vulkan DeviceHAL creation/compilation, test source, and vg_exp.py.
Check shell syntax of the runbook's executable blocks and documentation links.
Local macOS inspection cannot establish Linux compilation or NVIDIA test results;
the remote configure/build/CTest logs remain the required execution evidence.

## Findings

- Build the default ALL target: the backend library alone omits test executables
  and the generated E014 capture fixture used by capture.view.cli.
- glslc's absolute path is baked into the backend at configure time and used at
  runtime. Preserve its executable and dependent libraries.
- No-UUID Vulkan DeviceHAL creation uses the first physical device.
- Debug enables ASan/UBSan but does not request Khronos validation layers itself.
- Seven Vulkan-named tests comprise six hardware tests and one source check.
- Some inner unsupported cases return success; retain verbose test output.
- Existing public ABI acceptance tests choose reference on Linux; Vulkan
  DeviceHAL tests do not close that public-ABI coverage gap.
- Phase B-E and benchmark orchestration retain Metal/historical assumptions;
  use CTest for current Vulkan execution evidence.
