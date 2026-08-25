# TASK-D7 / ADR-041: phase-d runner mapping. Included by CMakeLists.txt via
# cmake/phase-d-d*.cmake. Reconfigure after adding this file.
#
# Gated on VG_ENABLE_METAL like every other Phase C/D/E runner (phase-c-e4,
# phase-e-e1, phase-e-e6): Phase D's gate experiments depend on Metal-only
# ctests (vertical-slice.metal.discovery/tier2-nodes/working-set/
# envelope-continuation), so this runner cannot pass on a non-Metal build.
if(VG_ENABLE_METAL)
  add_test(NAME tooling.phase-d-runner
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_phase_d_runner.py
                   ${CMAKE_CURRENT_SOURCE_DIR}
                   ${CMAKE_CURRENT_BINARY_DIR})
endif()
