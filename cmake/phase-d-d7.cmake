# TASK-D7 / ADR-041: phase-d runner mapping, registered in cmake/tests.cmake.
#
# Gated on VG_ENABLE_METAL like every other Phase C/D/E runner (phase-e-e4,
# phase-e-e1, phase-e-e6): Phase D's gate experiments depend on Metal-only
# ctests (vertical-slice.metal.discovery/tier2-nodes/working-set/
# envelope-continuation), so this runner cannot pass on a non-Metal build.
if(VG_ENABLE_METAL)
  vg_python_test(tooling.phase-d-runner tests/tools/test_phase_d_runner.py
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
endif()
