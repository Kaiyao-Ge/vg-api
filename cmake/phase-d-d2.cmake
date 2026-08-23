# TASK-D2 / ADR-036: discovery pass + E004 revisit.
# Included by CMakeLists.txt's cmake/phase-d-d*.cmake glob (BUILD_TESTING).
# Reconfigure after adding this file -- file(GLOB) is configure-time.

target_sources(vg_backend_reference PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/backends/discovery_stage.cpp")

add_executable(vg_discovery_test tests/unit/discovery_test.cpp)
target_link_libraries(vg_discovery_test PRIVATE vg_core)
target_compile_features(vg_discovery_test PRIVATE cxx_std_20)
add_test(NAME core.discovery COMMAND vg_discovery_test)

if(VG_ENABLE_METAL)
  add_executable(vg_metal_discovery_test tests/vertical_slice/metal_discovery_test.cpp)
  target_link_libraries(vg_metal_discovery_test PRIVATE vg_backend_metal vg_backend_reference
                        vg_compiler)
  target_compile_features(vg_metal_discovery_test PRIVATE cxx_std_20)
  add_test(NAME vertical-slice.metal.discovery COMMAND vg_metal_discovery_test)
endif()
