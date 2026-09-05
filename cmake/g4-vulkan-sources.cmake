# G4: explicit production owners, available with or without BUILD_TESTING.
# The facade vulkan_device_hal.cpp remains registered by the root CMake file.
target_sources(vg_backend_vulkan PRIVATE
  src/backends/discovery_stage.cpp
  src/backends/working_set_stage.cpp
  src/backends/envelope_stage.cpp
  src/backends/vulkan/vulkan_resources.cpp
  src/backends/vulkan/vulkan_pipelines.cpp
  src/backends/vulkan/vulkan_lowering.cpp
  src/backends/vulkan/vulkan_commit.cpp
  src/backends/vulkan/vulkan_encoding.cpp
  src/backends/vulkan/vulkan_raster.cpp
  src/backends/vulkan/vulkan_plan_raster.cpp
  src/backends/vulkan/vulkan_user_raster.cpp
  src/backends/vulkan/vulkan_plan_indirect.cpp
  src/backends/vulkan/vulkan_tier2.cpp
  src/backends/vulkan/vulkan_diagnostics.cpp)
