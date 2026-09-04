# TASK-D4 / E010 — explicitly registered in cmake/tests.cmake.

if(BUILD_TESTING)
  # Tier2 is a narrow physical experiment, not the plan-driven backend path.
  # Keep its implementation out of production backend archives in both modes.
  vg_target(vg_tier2_oracle_harness TYPE STATIC
    SOURCES src/backends/reference/tier2_oracle.cpp
    LINK_PUBLIC vg_core
    FEATURES_PRIVATE cxx_std_20)

  vg_target(vg_tier2_oracle_test TYPE EXECUTABLE
    SOURCES tests/unit/tier2_oracle_test.cpp
    LINK_PRIVATE vg_tier2_oracle_harness vg_core
    FEATURES_PRIVATE cxx_std_20)
  vg_test(unit.tier2-oracle COMMAND vg_tier2_oracle_test)

  if(VG_ENABLE_METAL AND TARGET vg_backend_metal)
    vg_target(vg_metal_tier2_harness TYPE STATIC
      SOURCES src/backends/metal/metal_tier2.mm
      LINK_PUBLIC vg_backend_reference "-framework Foundation" "-framework Metal"
      FEATURES_PRIVATE cxx_std_20)

    vg_target(vg_metal_tier2_test TYPE EXECUTABLE
      SOURCES tests/vertical_slice/metal_tier2_test.cpp
      LINK_PRIVATE vg_metal_tier2_harness vg_tier2_oracle_harness vg_core vg_ir
      INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
      FEATURES_PRIVATE cxx_std_20)
    vg_test(vertical-slice.metal.tier2-nodes COMMAND vg_metal_tier2_test
             ${CMAKE_CURRENT_SOURCE_DIR})
  endif()
endif()
