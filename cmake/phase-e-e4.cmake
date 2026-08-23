# TASK-E4 / ADR-042: register the existing Phase C runner. The script
# already lived under tests/tools/; it was not an add_test.

if(VG_ENABLE_METAL)
  add_test(NAME tooling.phase-c-runner
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_phase_c_runner.py
                   ${CMAKE_CURRENT_SOURCE_DIR}
                   ${CMAKE_CURRENT_BINARY_DIR})
endif()
