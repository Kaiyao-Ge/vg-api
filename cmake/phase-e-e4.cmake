# TASK-E4 / ADR-042: register the existing Phase C runner. The script
# already lived under tests/tools/; it was not an add_test.

if(VG_ENABLE_METAL)
  vg_python_test(tooling.phase-c-runner tests/tools/test_phase_c_runner.py
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
endif()
