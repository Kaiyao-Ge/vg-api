# TASK-E6 / ADR-042: P0 benchmark smoke wrapping an existing Metal slice.

if(VG_ENABLE_METAL)
  add_test(NAME tooling.phase-e-benchmark
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_phase_e_benchmark.py
                   ${CMAKE_CURRENT_SOURCE_DIR}
                   ${CMAKE_CURRENT_BINARY_DIR})
endif()
