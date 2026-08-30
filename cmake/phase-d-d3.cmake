# TASK-D3 / ADR-037: working-set budget (E011).
# Included by CMakeLists.txt's cmake/phase-d-d*.cmake glob (BUILD_TESTING).
# Reconfigure after adding this file -- file(GLOB) is configure-time.

target_sources(vg_backend_reference PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/backends/working_set_stage.cpp")

add_executable(vg_working_set_test tests/unit/working_set_test.cpp)
target_link_libraries(vg_working_set_test PRIVATE vg_backend_reference vg_compiler)
target_include_directories(vg_working_set_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
target_compile_features(vg_working_set_test PRIVATE cxx_std_20)
add_test(NAME core.working-set COMMAND vg_working_set_test)

if(VG_ENABLE_METAL)
  add_executable(vg_metal_working_set_test tests/vertical_slice/metal_working_set_test.cpp)
  target_link_libraries(vg_metal_working_set_test PRIVATE vg_backend_metal vg_backend_reference
                        vg_compiler)
  target_compile_features(vg_metal_working_set_test PRIVATE cxx_std_20)
  add_test(NAME vertical-slice.metal.working-set COMMAND vg_metal_working_set_test)
endif()
