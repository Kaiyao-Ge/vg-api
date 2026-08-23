# TASK-D4 / E010 — included by CMakeLists.txt via
#   file(GLOB cmake/phase-d-d*.cmake)
# Do not edit CMakeLists.txt to consume this fragment.

if(TARGET vg_backend_reference)
  target_sources(vg_backend_reference PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends/reference/tier2_oracle.cpp")
endif()

if(TARGET vg_backend_metal)
  target_sources(vg_backend_metal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends/metal/metal_tier2.mm")
endif()

if(BUILD_TESTING)
  add_executable(vg_tier2_oracle_test tests/unit/tier2_oracle_test.cpp)
  target_link_libraries(vg_tier2_oracle_test PRIVATE vg_backend_reference vg_core)
  target_compile_features(vg_tier2_oracle_test PRIVATE cxx_std_20)
  add_test(NAME unit.tier2-oracle COMMAND vg_tier2_oracle_test)

  if(VG_ENABLE_METAL AND TARGET vg_backend_metal)
    add_executable(vg_metal_tier2_test tests/vertical_slice/metal_tier2_test.cpp)
    target_link_libraries(vg_metal_tier2_test PRIVATE vg_backend_metal vg_backend_reference vg_core
                          vg_ir)
    target_include_directories(vg_metal_tier2_test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
    target_compile_features(vg_metal_tier2_test PRIVATE cxx_std_20)
    add_test(NAME vertical-slice.metal.tier2-nodes COMMAND vg_metal_tier2_test
             ${CMAKE_CURRENT_SOURCE_DIR})
  endif()
endif()
