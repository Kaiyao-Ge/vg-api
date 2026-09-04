# G4 owns its narrow test harness target and existing test linkage here.
# Included after test registration, only for BUILD_TESTING + VG_ENABLE_VULKAN.
add_library(vg_vulkan_adapter_harness STATIC tests/support/vulkan_adapter_harness.cpp)
target_compile_features(vg_vulkan_adapter_harness PRIVATE cxx_std_20)
target_compile_definitions(vg_vulkan_adapter_harness PRIVATE VG_HAS_VULKAN=1)
target_link_libraries(vg_vulkan_adapter_harness PUBLIC vg_backend_vulkan)
target_include_directories(vg_vulkan_adapter_harness PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
# This compiles the existing physical mechanisms without pretending the
# assembler-driven test calls them. No new CTest or capability is introduced.
target_link_libraries(vg_vulkan_task_timeline_test PRIVATE vg_vulkan_adapter_harness)
