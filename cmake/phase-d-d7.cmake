# TASK-D7 / ADR-041: phase-d runner mapping. Included by CMakeLists.txt via
# cmake/phase-d-d*.cmake. Reconfigure after adding this file.

add_test(NAME tooling.phase-d-runner
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_phase_d_runner.py
                 ${CMAKE_CURRENT_SOURCE_DIR}
                 ${CMAKE_CURRENT_BINARY_DIR})
