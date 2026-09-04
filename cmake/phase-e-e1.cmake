# TASK-E1 / ADR-042: phase-e runner, registered in cmake/tests.cmake.

if(VG_ENABLE_METAL)
  vg_python_test(tooling.phase-e-runner tests/tools/test_phase_e_runner.py
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
endif()
