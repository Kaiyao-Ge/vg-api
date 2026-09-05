# TASK-D4 / E010 — explicitly registered in cmake/tests.cmake.

if(BUILD_TESTING)
  # The portable Tier2 oracle is a narrow CPU test helper. The Vulkan
  # production component is owned by the Vulkan backend source fragment.
  vg_target(vg_tier2_oracle_harness TYPE STATIC
    SOURCES src/backends/reference/tier2_oracle.cpp
    LINK_PUBLIC vg_core
    FEATURES_PRIVATE cxx_std_20)

  vg_target(vg_tier2_oracle_test TYPE EXECUTABLE
    SOURCES tests/unit/tier2_oracle_test.cpp
    LINK_PRIVATE vg_tier2_oracle_harness vg_core
    FEATURES_PRIVATE cxx_std_20)
  vg_test(unit.tier2-oracle COMMAND vg_tier2_oracle_test)

  # The complete-NodeRef handoff validator remains a CPU unit. Formal sealed
  # Tier2 selection and indirect draw execution are registered in the
  # Vulkan-only G4 test fragment.
  vg_target(vg_vulkan_tier2_handoff_test TYPE EXECUTABLE
    SOURCES
      src/backends/vulkan/vulkan_tier2.cpp
      tests/vertical_slice/vulkan_tier2_submission_test.cpp
    LINK_PRIVATE vg_core
    FEATURES_PRIVATE cxx_std_20)
  vg_test(unit.vulkan-tier2-handoff COMMAND vg_vulkan_tier2_handoff_test)

  # The portable CPU/source contract pins GLSL raster compilation and MSL
  # rejection. Real user-shader import is exercised by the Vulkan-only
  # DeviceHal test in the G4 test fragment.
  vg_target(vg_vulkan_user_raster_contract_test TYPE EXECUTABLE
    SOURCES
      src/backends/vulkan/vulkan_user_raster.cpp
      tests/vertical_slice/vulkan_user_raster_contract_test.cpp
    LINK_PRIVATE vg_compiler vg_core
    INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
    FEATURES_PRIVATE cxx_std_20)
  vg_test(unit.vulkan-user-raster-contract
          COMMAND vg_vulkan_user_raster_contract_test ${CMAKE_CURRENT_SOURCE_DIR})

  # Formal Raster CPU/source contract: Reference is the oracle. It does not
  # claim Vulkan device execution; the source mode pins the real encoder path.
  vg_target(vg_vulkan_plan_raster_test TYPE EXECUTABLE
    SOURCES tests/vertical_slice/vulkan_plan_raster_test.cpp
    LINK_PRIVATE vg_backend_reference vg_core
    FEATURES_PRIVATE cxx_std_20)
  vg_test(vertical-slice.vulkan-plan-raster-cpu
          COMMAND vg_vulkan_plan_raster_test cpu-fixture ${CMAKE_CURRENT_SOURCE_DIR})
  vg_test(vertical-slice.vulkan-plan-raster-source
          COMMAND vg_vulkan_plan_raster_test source-contract ${CMAKE_CURRENT_SOURCE_DIR})

  # The portable draw-preparation unit fixes the exact indirect-command byte
  # contract and validates a pre-authorized Tier2 NodeRef handoff. Formal
  # Vulkan Raster and Tier2 device execution are registered separately.
  vg_target(vg_vulkan_draw_experiments_test TYPE EXECUTABLE
    SOURCES
      src/backends/vulkan/vulkan_tier2.cpp
      tests/support/vulkan_draw_experiments.cpp
      tests/vertical_slice/vulkan_draw_experiments_test.cpp
    LINK_PRIVATE vg_core
    INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
    FEATURES_PRIVATE cxx_std_20)
  vg_test(unit.vulkan-draw-experiments
          COMMAND vg_vulkan_draw_experiments_test cpu-oracle ${CMAKE_CURRENT_SOURCE_DIR})

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
