# TASK-E1 / ADR-042: phase-e runner. Included by CMakeLists.txt via
# cmake/phase-e-e*.cmake. Reconfigure after adding this file.

if(VG_ENABLE_METAL)
  add_test(NAME tooling.phase-e-runner
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_phase_e_runner.py
                   ${CMAKE_CURRENT_SOURCE_DIR}
                   ${CMAKE_CURRENT_BINARY_DIR})
endif()
