# TASK-D6 / E014: capture view report and same-environment replay.
# Included from CMakeLists.txt only when BUILD_TESTING is on.

vg_target(vg_capture_view_test TYPE EXECUTABLE
  SOURCES tests/unit/capture_view_test.cpp
  LINK_PRIVATE vg_capture vg_compiler vg_backend_reference
  FEATURES_PRIVATE cxx_std_20
  DEFINITIONS_PRIVATE VG_CAPTURE_VIEW="$<TARGET_FILE:vg-capture-view>"
  DEPENDS vg-capture-view)

set(VG_E014_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/e014-capture-view-fixture.json")
add_custom_command(
  OUTPUT "${VG_E014_FIXTURE}"
  COMMAND vg_capture_view_test --write-fixture "${VG_E014_FIXTURE}"
  DEPENDS vg_capture_view_test
  VERBATIM
)
add_custom_target(vg_e014_capture_fixture ALL DEPENDS "${VG_E014_FIXTURE}")

vg_test(capture.view COMMAND vg_capture_view_test)
vg_test(capture.view.cli COMMAND vg-capture-view --format markdown "${VG_E014_FIXTURE}")
