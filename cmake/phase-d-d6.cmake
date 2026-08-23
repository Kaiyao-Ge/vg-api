# TASK-D6 / E014: capture view report and same-environment replay.
# Included from CMakeLists.txt only when BUILD_TESTING is on.

add_executable(vg-capture-view tools/vg-capture-view/vg_capture_view.cpp)
target_link_libraries(vg-capture-view PRIVATE vg_capture vg_backend_reference)
target_compile_features(vg-capture-view PRIVATE cxx_std_20)

add_executable(vg_capture_view_test tests/unit/capture_view_test.cpp)
target_link_libraries(vg_capture_view_test PRIVATE vg_capture vg_compiler vg_backend_reference)
target_compile_features(vg_capture_view_test PRIVATE cxx_std_20)
target_compile_definitions(vg_capture_view_test PRIVATE VG_CAPTURE_VIEW="$<TARGET_FILE:vg-capture-view>")
add_dependencies(vg_capture_view_test vg-capture-view)

set(VG_E014_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/e014-capture-view-fixture.json")
add_custom_command(
  OUTPUT "${VG_E014_FIXTURE}"
  COMMAND vg_capture_view_test --write-fixture "${VG_E014_FIXTURE}"
  DEPENDS vg_capture_view_test
  VERBATIM
)
add_custom_target(vg_e014_capture_fixture ALL DEPENDS "${VG_E014_FIXTURE}")

add_test(NAME capture.view COMMAND vg_capture_view_test)
add_test(NAME capture.view.cli COMMAND vg-capture-view --format markdown "${VG_E014_FIXTURE}")
