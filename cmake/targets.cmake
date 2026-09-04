vg_target(vg_ir TYPE STATIC
  SOURCES src/ir/json.cpp src/ir/sha256.cpp src/ir/ir.cpp
  FEATURES_PUBLIC cxx_std_20
  INCLUDE_PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

vg_target(vg_core TYPE STATIC
  SOURCES
    src/core/arena.cpp src/core/facet.cpp src/core/representation.cpp
    src/core/pointer_graph.cpp src/core/effect_graph.cpp src/core/task_graph.cpp
    src/core/access.cpp src/core/node.cpp src/core/envelope.cpp
    src/core/execution_result.cpp src/core/execution_plan.cpp src/core/execution_schedule.cpp
    src/core/task_schema.cpp src/core/scene_root.cpp
  FEATURES_PUBLIC cxx_std_20
  INCLUDE_PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${VG_GENERATED_DIR}"
  LINK_PUBLIC vg_ir
  DEPENDS vg_schema_generate)

vg_target(vg_capture TYPE STATIC
  SOURCES src/capture/capture.cpp
  FEATURES_PUBLIC cxx_std_20
  INCLUDE_PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
  LINK_PUBLIC vg_core vg_ir)

vg_target(vg_compiler TYPE STATIC
  SOURCES
    src/compiler/compiler.cpp src/compiler/compute_task_ring.cpp
    src/compiler/compute_package.cpp src/compiler/compute_codegen.cpp
    src/compiler/shaders/task_ring.cpp src/compiler/shaders/facet.cpp
    src/compiler/shaders/raster.cpp src/compiler/shaders/cull_compact.cpp
    src/compiler/pipeline_classification.cpp
  FEATURES_PUBLIC cxx_std_20
  INCLUDE_PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${VG_GENERATED_DIR}"
  LINK_PUBLIC vg_ir
  DEPENDS vg_schema_generate)

vg_target(vg_backend_reference TYPE STATIC
  SOURCES
    src/backends/device_hal.cpp src/backends/discovery_stage.cpp
    src/backends/working_set_stage.cpp src/backends/envelope_stage.cpp
    src/backends/reference/reference_executor.cpp
    src/backends/reference/reference_device_hal.cpp
  FEATURES_PUBLIC cxx_std_20
  INCLUDE_PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
  LINK_PUBLIC vg_core vg_ir vg_compiler
  DEPENDS vg_schema_generate)

vg_target(vg_api TYPE STATIC
  SOURCES
    src/api/vg_api.cpp src/api/vg_api_device.cpp src/api/vg_api_arena.cpp
    src/api/vg_api_code.cpp src/api/vg_api_taskgraph.cpp src/api/vg_api_execution.cpp
    src/api/vg_api_facet.cpp src/backends/reference/reference_probe.cpp
  FEATURES_PRIVATE cxx_std_20
  INCLUDE_PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  INCLUDE_PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
  DEFINITIONS_PRIVATE VG_BUILDING_LIBRARY=1
  LINK_PRIVATE vg_core vg_backend_reference
  PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)

if(VG_ENABLE_METAL)
  if(NOT APPLE)
    message(FATAL_ERROR "VG_ENABLE_METAL requires macOS and the Metal framework")
  endif()
  enable_language(OBJCXX)
  target_sources(vg_api PRIVATE src/backends/metal/metal_probe.mm)
  target_link_libraries(vg_api PRIVATE "-framework Foundation" "-framework Metal")
  target_compile_definitions(vg_api PRIVATE VG_HAS_METAL=1)

  vg_target(vg_backend_metal TYPE STATIC
    SOURCES src/backends/metal/metal_device_hal.mm
    FEATURES_PUBLIC cxx_std_20
    INCLUDE_PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    INCLUDE_PRIVATE "${VG_GENERATED_DIR}")
  # vg_backend_reference is a deliberate exception to the one-directional
  # layer dependency: Metal's HostAssisted fallback reuses the reference
  # executor's byte-exact CPU implementation rather than duplicating it.
  target_link_libraries(vg_backend_metal PUBLIC vg_core vg_ir vg_compiler vg_backend_reference
                        "-framework Foundation" "-framework Metal")
  add_dependencies(vg_backend_metal vg_schema_generate)
  include(cmake/g3-metal-sources.cmake)
  target_link_libraries(vg_api PRIVATE vg_backend_metal)
endif()

if(VG_ENABLE_VULKAN)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "VG_ENABLE_VULKAN is supported only on Linux. Configure dev-reference or dev-metal on this host.")
  endif()
  find_package(Vulkan REQUIRED)
  target_sources(vg_api PRIVATE src/backends/vulkan/vulkan_probe.cpp)
  target_link_libraries(vg_api PRIVATE Vulkan::Vulkan)
  target_compile_definitions(vg_api PRIVATE VG_HAS_VULKAN=1)

  # GLSL -> SPIR-V happens at runtime (the GLSL text is generated per
  # ir::Module, not a static build asset), so glslc is located once here and
  # its path baked in as a macro rather than invoked via a CMake custom command.
  find_program(VG_GLSLC_EXECUTABLE NAMES glslc)
  if(NOT VG_GLSLC_EXECUTABLE)
    message(FATAL_ERROR "VG_ENABLE_VULKAN requires glslc (part of the Vulkan SDK/shaderc) on PATH")
  endif()

  vg_target(vg_backend_vulkan TYPE STATIC
    SOURCES src/backends/vulkan/vulkan_device_hal.cpp
    FEATURES_PUBLIC cxx_std_20
    INCLUDE_PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    DEFINITIONS_PRIVATE VG_HAS_VULKAN=1 VG_GLSLC_PATH="${VG_GLSLC_EXECUTABLE}"
    LINK_PUBLIC vg_core vg_ir vg_compiler Vulkan::Vulkan
    DEPENDS vg_schema_generate)
  include(cmake/g4-vulkan-sources.cmake)
  target_link_libraries(vg_api PRIVATE vg_backend_vulkan)
endif()

vg_target(vg-platform-probe TYPE EXECUTABLE
  SOURCES tools/vg-platform-probe.cpp
  LINK_PRIVATE vg_api
  FEATURES_PRIVATE cxx_std_20)
if(VG_ENABLE_METAL)
  target_compile_definitions(vg-platform-probe PRIVATE VG_HAS_METAL=1)
endif()

vg_target(vg-compile TYPE EXECUTABLE
  SOURCES tools/vg-compile.cpp
  LINK_PRIVATE vg_compiler vg_ir
  FEATURES_PRIVATE cxx_std_20)

vg_target(vg-reference TYPE EXECUTABLE
  SOURCES tools/vg-reference.cpp
  LINK_PRIVATE vg_backend_reference vg_capture
  FEATURES_PRIVATE cxx_std_20)

vg_target(vg-replay TYPE EXECUTABLE
  SOURCES tools/vg-replay.cpp
  LINK_PRIVATE vg_capture vg_backend_reference
  FEATURES_PRIVATE cxx_std_20)

# A standalone public-C-ABI F3--F5 legacy-raster sample. It deliberately
# includes only <vg/vg.h>; the caller supplies an explicit PPM output path so
# running it never drops a generated artifact in the repository root. F6's
# v1.7 SceneRoot path is covered by api.f6-scene-root-c instead.
vg_target(vg-offscreen-triangle-ppm TYPE EXECUTABLE
  SOURCES tools/vg-offscreen-triangle-ppm.c
  LINK_PRIVATE vg_api)

# Regenerates tests/fixtures/golden/* from tests/fixtures/ir/*.vgir.json.
# Built by default; never invoked automatically by CTest or CI.
vg_target(vg-golden-gen TYPE EXECUTABLE
  SOURCES tools/vg-golden-gen/vg_golden_gen.cpp
  LINK_PRIVATE vg_compiler vg_ir
  INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  FEATURES_PRIVATE cxx_std_20)


# Inspection CLI is a product tool, available with BUILD_TESTING=OFF.
vg_target(vg-capture-view TYPE EXECUTABLE
  SOURCES tools/vg-capture-view/vg_capture_view.cpp
  LINK_PRIVATE vg_capture vg_backend_reference
  FEATURES_PRIVATE cxx_std_20)
