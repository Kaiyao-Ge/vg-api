# G4 owns its narrow test harness target and existing test linkage here.
# Included after test registration, only for BUILD_TESTING + VG_ENABLE_VULKAN.
add_library(vg_vulkan_adapter_harness STATIC
  tests/support/vulkan_adapter_harness.cpp
  tests/support/vulkan_tier2_harness.cpp)
target_compile_features(vg_vulkan_adapter_harness PRIVATE cxx_std_20)
target_compile_definitions(vg_vulkan_adapter_harness PRIVATE VG_HAS_VULKAN=1)
target_link_libraries(vg_vulkan_adapter_harness PUBLIC vg_backend_vulkan)
target_include_directories(vg_vulkan_adapter_harness PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
# Narrow mechanisms are test-only; D/F below invoke them explicitly.
# Their registration does not advertise a production DeviceHAL capability.
target_link_libraries(vg_vulkan_task_timeline_test PRIVATE vg_vulkan_adapter_harness)

# D: physical facet probes plus Core-assembled representation/consume paths.
vg_target(vg_vulkan_facet_representation_test TYPE EXECUTABLE
  SOURCES tests/vertical_slice/vulkan_facet_representation_test.cpp
  LINK_PRIVATE vg_vulkan_adapter_harness vg_backend_reference vg_compiler
  FEATURES_PRIVATE cxx_std_20)
foreach(mode IN ITEMS facets representation consume-input facet-raster pipeline-classification)
  vg_test(vertical-slice.vulkan.${mode} COMMAND vg_vulkan_facet_representation_test ${mode} ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# F: independent compute-only physical experiments, never production TaskGraph lowering.
vg_target(vg_vulkan_gpu_experiments_test TYPE EXECUTABLE
  SOURCES tests/vertical_slice/vulkan_gpu_experiments_test.cpp
  LINK_PRIVATE vg_vulkan_adapter_harness vg_tier2_oracle_harness vg_backend_reference vg_compiler
  FEATURES_PRIVATE cxx_std_20)
foreach(mode IN ITEMS indirect cull-compact indexed-address tier2)
  vg_test(vertical-slice.vulkan.${mode} COMMAND vg_vulkan_gpu_experiments_test ${mode} ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# Formal sealed Tier2 selection with repeated/skewed Raster NodeRefs.
vg_target(vg_vulkan_plan_tier2_test TYPE EXECUTABLE
  SOURCES tests/vertical_slice/vulkan_plan_tier2_test.cpp
  LINK_PRIVATE vg_backend_vulkan vg_backend_reference vg_compiler
  FEATURES_PRIVATE cxx_std_20)
vg_test(vertical-slice.vulkan.plan-tier2 COMMAND vg_vulkan_plan_tier2_test)

# Real DeviceHal import and execution of the Vulkan Stage7 GLSL raster ABI.
vg_target(vg_vulkan_user_glsl_device_test TYPE EXECUTABLE
  SOURCES tests/vertical_slice/vulkan_user_glsl_device_test.cpp
  LINK_PRIVATE vg_backend_vulkan vg_backend_reference vg_compiler
  FEATURES_PRIVATE cxx_std_20)
vg_test(vertical-slice.vulkan.user-glsl-device COMMAND vg_vulkan_user_glsl_device_test)

# Plan-driven indexed Raster with authoritative SceneRoot and D32 depth.
vg_target(vg_vulkan_plan_depth_scene_test TYPE EXECUTABLE
  SOURCES tests/vertical_slice/vulkan_plan_depth_scene_test.cpp
  LINK_PRIVATE vg_backend_vulkan vg_backend_reference vg_compiler
  FEATURES_PRIVATE cxx_std_20)
vg_test(vertical-slice.vulkan.plan-depth-scene COMMAND vg_vulkan_plan_depth_scene_test)
