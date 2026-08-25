cmake_minimum_required(VERSION 3.25)
project(vg VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(VG_ENABLE_METAL "Build the macOS Metal capability probe" OFF)
option(VG_ENABLE_VULKAN "Build the Linux Vulkan capability probe" OFF)
option(VG_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

if(VG_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(CTest)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

set(VG_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(VG_SCHEMA_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/schemas/ir/task-root.vg.json")
set(VG_SCHEMA_HEADER "${VG_GENERATED_DIR}/vg_task_root.h")
set(VG_SCHEMA_REFLECTION "${VG_GENERATED_DIR}/vg_task_root.reflection.json")
set(VG_SCHEMA_GOLDEN "${VG_GENERATED_DIR}/vg_task_root.layout.json")
add_custom_command(
  OUTPUT "${VG_SCHEMA_HEADER}" "${VG_SCHEMA_REFLECTION}" "${VG_SCHEMA_GOLDEN}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${VG_GENERATED_DIR}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/vg-schema/vg_schema.py"
          "${VG_SCHEMA_INPUT}" --out-dir "${VG_GENERATED_DIR}"
  DEPENDS "${VG_SCHEMA_INPUT}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/vg-schema/vg_schema.py"
  VERBATIM
)
add_custom_target(vg_schema_generate DEPENDS "${VG_SCHEMA_HEADER}" "${VG_SCHEMA_REFLECTION}" "${VG_SCHEMA_GOLDEN}")

add_library(vg_ir STATIC
    src/ir/json.cpp
    src/ir/sha256.cpp
    src/ir/ir.cpp
)
target_compile_features(vg_ir PUBLIC cxx_std_20)
target_include_directories(vg_ir PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(vg_core STATIC src/core/core.cpp src/core/task_schema.cpp)
target_compile_features(vg_core PUBLIC cxx_std_20)
target_include_directories(vg_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(vg_core PUBLIC "${VG_GENERATED_DIR}")
target_link_libraries(vg_core PUBLIC vg_ir)
add_dependencies(vg_core vg_schema_generate)

add_library(vg_capture STATIC src/capture/capture.cpp)
target_compile_features(vg_capture PUBLIC cxx_std_20)
target_include_directories(vg_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vg_capture PUBLIC vg_core vg_ir)

add_library(vg_compiler STATIC
    src/compiler/compiler.cpp
    src/compiler/compute_package.cpp
    src/compiler/pipeline_classification.cpp
)
target_compile_features(vg_compiler PUBLIC cxx_std_20)
target_include_directories(vg_compiler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vg_compiler PUBLIC vg_ir)

add_library(vg_backend_reference STATIC
    src/backends/device_hal.cpp
    src/backends/reference/reference_executor.cpp
    src/backends/reference/reference_device_hal.cpp
)
target_compile_features(vg_backend_reference PUBLIC cxx_std_20)
target_include_directories(vg_backend_reference PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vg_backend_reference PUBLIC vg_core vg_ir vg_compiler)
add_dependencies(vg_backend_reference vg_schema_generate)

add_library(vg_api STATIC
    src/api/vg_api.cpp
    src/api/vg_api_device.cpp
    src/api/vg_api_arena.cpp
    src/api/vg_api_code.cpp
    src/api/vg_api_taskgraph.cpp
    src/api/vg_api_execution.cpp
    src/backends/reference/reference_probe.cpp
)
target_compile_features(vg_api PRIVATE cxx_std_20)
target_include_directories(vg_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(vg_api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(vg_api PRIVATE VG_BUILDING_LIBRARY=1)
target_link_libraries(vg_api PRIVATE vg_core vg_backend_reference)
set_target_properties(vg_api PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)

if(VG_ENABLE_METAL)
  if(NOT APPLE)
    message(FATAL_ERROR "VG_ENABLE_METAL requires macOS and the Metal framework")
  endif()
  enable_language(OBJCXX)
  target_sources(vg_api PRIVATE src/backends/metal/metal_probe.mm)
  target_link_libraries(vg_api PRIVATE "-framework Foundation" "-framework Metal")
  target_compile_definitions(vg_api PRIVATE VG_HAS_METAL=1)

  add_library(vg_backend_metal STATIC
      src/backends/metal/metal_device_hal.mm
  )
  target_compile_features(vg_backend_metal PUBLIC cxx_std_20)
  target_include_directories(vg_backend_metal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  # vg_backend_reference is a deliberate exception to the one-directional
  # layer dependency: Metal's HostAssisted fallback reuses the reference
  # executor's byte-exact CPU implementation rather than duplicating it.
  target_link_libraries(vg_backend_metal PUBLIC vg_core vg_ir vg_compiler vg_backend_reference
                        "-framework Foundation" "-framework Metal")
  add_dependencies(vg_backend_metal vg_schema_generate)
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

  add_library(vg_backend_vulkan STATIC src/backends/vulkan/vulkan_device_hal.cpp)
  target_compile_features(vg_backend_vulkan PUBLIC cxx_std_20)
  target_include_directories(vg_backend_vulkan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(vg_backend_vulkan PRIVATE VG_HAS_VULKAN=1 VG_GLSLC_PATH="${VG_GLSLC_EXECUTABLE}")
  target_link_libraries(vg_backend_vulkan PUBLIC vg_core vg_ir vg_compiler Vulkan::Vulkan)
  add_dependencies(vg_backend_vulkan vg_schema_generate)
  target_link_libraries(vg_api PRIVATE vg_backend_vulkan)
endif()

add_executable(vg-platform-probe tools/vg-platform-probe.cpp)
target_link_libraries(vg-platform-probe PRIVATE vg_api)
target_compile_features(vg-platform-probe PRIVATE cxx_std_20)
if(VG_ENABLE_METAL)
  target_compile_definitions(vg-platform-probe PRIVATE VG_HAS_METAL=1)
endif()

add_executable(vg-compile tools/vg-compile.cpp)
target_link_libraries(vg-compile PRIVATE vg_compiler vg_ir)
target_compile_features(vg-compile PRIVATE cxx_std_20)

add_executable(vg-reference tools/vg-reference.cpp)
target_link_libraries(vg-reference PRIVATE vg_backend_reference vg_capture)
target_compile_features(vg-reference PRIVATE cxx_std_20)

add_executable(vg-replay tools/vg-replay.cpp)
target_link_libraries(vg-replay PRIVATE vg_capture vg_backend_reference)
target_compile_features(vg-replay PRIVATE cxx_std_20)

# Regenerates tests/fixtures/golden/* from tests/fixtures/ir/*.vgir.json.
# Built by default; never invoked automatically by CTest or CI.
add_executable(vg-golden-gen tools/vg-golden-gen/vg_golden_gen.cpp)
target_link_libraries(vg-golden-gen PRIVATE vg_compiler vg_ir)
target_include_directories(vg-golden-gen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
target_compile_features(vg-golden-gen PRIVATE cxx_std_20)

if(BUILD_TESTING)
  add_executable(vg_abi_c_test tests/abi/abi_c_test.c)
  target_link_libraries(vg_abi_c_test PRIVATE vg_api)
  set_target_properties(vg_abi_c_test PROPERTIES LINKER_LANGUAGE CXX)
  add_test(NAME abi.c COMMAND vg_abi_c_test)

  add_executable(vg_abi_cpp_test tests/abi/abi_cpp_test.cpp)
  target_link_libraries(vg_abi_cpp_test PRIVATE vg_api)
  target_compile_features(vg_abi_cpp_test PRIVATE cxx_std_17)
  add_test(NAME abi.cpp COMMAND vg_abi_cpp_test)

  # Kept out of vg_abi_cpp_test so that stays a pure C++17 include test: this
  # one links vg_core (C++20) to pin the public token against the core type
  # it mirrors.
  add_executable(vg_abi_facet_token_test tests/abi/abi_facet_token_test.cpp)
  target_link_libraries(vg_abi_facet_token_test PRIVATE vg_api vg_core)
  target_compile_features(vg_abi_facet_token_test PRIVATE cxx_std_20)
  add_test(NAME abi.facet-token COMMAND vg_abi_facet_token_test)

  # F1 (ADR-044): the actual Checkpoint-A precondition -- proves the golden
  # path is reachable through <vg/vg.h> alone, with only vg_api linked, not
  # vg_core/vg_backend_reference directly.
  add_executable(vg_c_abi_conformance_test tests/api/vg_c_abi_conformance_test.cpp)
  target_link_libraries(vg_c_abi_conformance_test PRIVATE vg_api)
  target_compile_features(vg_c_abi_conformance_test PRIVATE cxx_std_17)
  add_test(NAME api.c-abi-conformance COMMAND vg_c_abi_conformance_test)

  if(VG_ENABLE_METAL)
    # Reruns E002 (ADR-028/TASK-B15 typed pointer graph) through the public
    # C ABI alone, cross-checked against vertical-slice.metal.pointer-graph's
    # internal-machinery result. Requires real Metal hardware because only
    # metal_device_hal.mm branches on pointer_graph (CachedObject lowering);
    # reference/Vulkan always take build_linear_compute_package.
    add_executable(vg_e002_pointer_graph_abi_test tests/api/vg_e002_pointer_graph_abi_test.cpp)
    target_link_libraries(vg_e002_pointer_graph_abi_test PRIVATE vg_api)
    target_compile_features(vg_e002_pointer_graph_abi_test PRIVATE cxx_std_17)
    add_test(NAME api.e002-pointer-graph-via-abi COMMAND vg_e002_pointer_graph_abi_test)
  endif()

  add_test(NAME platform.probe COMMAND vg-platform-probe --validate)
  add_test(NAME tooling.schemas COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_schemas.py ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME tooling.bundle COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_bundle.py ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
  add_test(NAME docs.check COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/vg-docs/vg_docs.py ${CMAKE_CURRENT_SOURCE_DIR})

  add_executable(vg_core_test tests/unit/core_test.cpp)
  target_link_libraries(vg_core_test PRIVATE vg_backend_reference vg_capture)
  target_compile_features(vg_core_test PRIVATE cxx_std_20)
  add_test(NAME core.unit COMMAND vg_core_test)

  add_executable(vg_ir_test tests/unit/ir_test.cpp)
  target_link_libraries(vg_ir_test PRIVATE vg_compiler)
  target_compile_features(vg_ir_test PRIVATE cxx_std_20)
  add_test(NAME ir.unit COMMAND vg_ir_test)

  add_executable(vg_compute_package_test tests/unit/compute_package_test.cpp)
  target_link_libraries(vg_compute_package_test PRIVATE vg_compiler)
  target_compile_features(vg_compute_package_test PRIVATE cxx_std_20)
  add_test(NAME compiler.compute-package COMMAND vg_compute_package_test)

  # pipeline_classification lives in vg_compiler, so this needs nothing else.
  add_executable(vg_pipeline_classification_test tests/unit/pipeline_classification_test.cpp)
  target_link_libraries(vg_pipeline_classification_test PRIVATE vg_compiler)
  target_compile_features(vg_pipeline_classification_test PRIVATE cxx_std_20)
  add_test(NAME compiler.pipeline-classification COMMAND vg_pipeline_classification_test)

  add_executable(vg_reference_raster_test tests/unit/reference_raster_test.cpp)
  target_link_libraries(vg_reference_raster_test PRIVATE vg_backend_reference)
  target_compile_features(vg_reference_raster_test PRIVATE cxx_std_20)
  add_test(NAME reference.facet-oracles COMMAND vg_reference_raster_test)

  add_executable(vg_compute_package_golden_test tests/fixtures/compute_package_golden_test.cpp)
  target_link_libraries(vg_compute_package_golden_test PRIVATE vg_compiler vg_ir)
  target_include_directories(vg_compute_package_golden_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
  target_compile_features(vg_compute_package_golden_test PRIVATE cxx_std_20)
  add_test(NAME compiler.compute-package-golden COMMAND vg_compute_package_golden_test ${CMAKE_CURRENT_SOURCE_DIR})

  add_executable(vg_phase_a_model_test tests/model/phase_a_model_test.cpp)
  target_link_libraries(vg_phase_a_model_test PRIVATE vg_core)
  target_compile_features(vg_phase_a_model_test PRIVATE cxx_std_20)
  add_test(NAME model.phase-a COMMAND vg_phase_a_model_test)

  add_executable(vg_phase_a_witness_test tests/model/phase_a_witness_test.cpp)
  target_link_libraries(vg_phase_a_witness_test PRIVATE vg_core)
  target_compile_features(vg_phase_a_witness_test PRIVATE cxx_std_20)
  add_test(NAME model.witness COMMAND vg_phase_a_witness_test)

  add_executable(vg_phase_a_conformance tests/conformance/phase_a_conformance.cpp)
  target_link_libraries(vg_phase_a_conformance PRIVATE vg_backend_reference vg_capture vg_compiler)
  target_compile_features(vg_phase_a_conformance PRIVATE cxx_std_20)
  add_test(NAME conformance.phase-a COMMAND vg_phase_a_conformance)

  add_library(vg_conformance_lib STATIC tests/conformance/conformance_lib.cpp)
  target_link_libraries(vg_conformance_lib PUBLIC vg_backend_reference vg_compiler vg_ir)
  target_include_directories(vg_conformance_lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
  target_compile_features(vg_conformance_lib PRIVATE cxx_std_20)

  add_executable(vg_device_hal_conformance_reference tests/conformance/device_hal_conformance_reference.cpp)
  target_link_libraries(vg_device_hal_conformance_reference PRIVATE vg_conformance_lib vg_backend_reference)
  target_compile_features(vg_device_hal_conformance_reference PRIVATE cxx_std_20)
  add_test(NAME conformance.device-hal.reference COMMAND vg_device_hal_conformance_reference ${CMAKE_CURRENT_SOURCE_DIR})

  if(VG_ENABLE_METAL)
    add_executable(vg_device_hal_conformance_metal tests/conformance/device_hal_conformance_metal.cpp)
    target_link_libraries(vg_device_hal_conformance_metal PRIVATE vg_conformance_lib vg_backend_metal vg_backend_reference)
    target_compile_features(vg_device_hal_conformance_metal PRIVATE cxx_std_20)
    add_test(NAME conformance.device-hal.metal COMMAND vg_device_hal_conformance_metal ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  if(VG_ENABLE_VULKAN)
    add_executable(vg_device_hal_conformance_vulkan tests/conformance/device_hal_conformance_vulkan.cpp)
    target_link_libraries(vg_device_hal_conformance_vulkan PRIVATE vg_conformance_lib vg_backend_vulkan vg_backend_reference)
    target_compile_features(vg_device_hal_conformance_vulkan PRIVATE cxx_std_20)
    add_test(NAME conformance.device-hal.vulkan COMMAND vg_device_hal_conformance_vulkan ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  if(VG_ENABLE_METAL)
    add_executable(vg_metal_vertical_slice_test tests/vertical_slice/metal_vertical_slice_test.cpp)
    target_link_libraries(vg_metal_vertical_slice_test PRIVATE vg_backend_metal vg_backend_reference vg_ir)
    target_include_directories(vg_metal_vertical_slice_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
    target_compile_features(vg_metal_vertical_slice_test PRIVATE cxx_std_20)
    add_test(NAME vertical-slice.metal COMMAND vg_metal_vertical_slice_test ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(vg_metal_task_timeline_test tests/vertical_slice/metal_task_timeline_test.cpp)
    target_link_libraries(vg_metal_task_timeline_test PRIVATE vg_backend_metal vg_backend_reference vg_core
                          vg_capture)
    target_include_directories(vg_metal_task_timeline_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
    target_compile_features(vg_metal_task_timeline_test PRIVATE cxx_std_20)
    add_test(NAME vertical-slice.metal.task-tier0 COMMAND vg_metal_task_timeline_test task-tier0 ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.timeline COMMAND vg_metal_task_timeline_test timeline ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.access-certificate COMMAND vg_metal_task_timeline_test access-certificate ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.tier1-indirect COMMAND vg_metal_task_timeline_test tier1-indirect ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.cull-compact COMMAND vg_metal_task_timeline_test cull-compact ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.cull-compact-1m COMMAND vg_metal_task_timeline_test cull-compact-1m ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.effect-dag COMMAND vg_metal_task_timeline_test effect-dag ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.pointer-graph COMMAND vg_metal_task_timeline_test pointer-graph ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.indexed-binding COMMAND vg_metal_task_timeline_test indexed-binding ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.representation-layer COMMAND vg_metal_task_timeline_test representation-layer ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.sample-facet COMMAND vg_metal_task_timeline_test sample-facet ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.checked-facet-generation COMMAND vg_metal_task_timeline_test checked-facet-generation ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.basic-raster COMMAND vg_metal_task_timeline_test basic-raster ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.pipeline-classification COMMAND vg_metal_task_timeline_test pipeline-classification ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.consume-input COMMAND vg_metal_task_timeline_test consume-input ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.metal.representation-churn COMMAND vg_metal_task_timeline_test representation-churn ${CMAKE_CURRENT_SOURCE_DIR})

    # TASK-B17: the Phase B gate runner drives ctest names registered above
    # (vertical-slice.metal.*), so it is only meaningful -- and only
    # registered -- under VG_ENABLE_METAL, matching every ctest it depends on.
    add_test(NAME tooling.phase-b-runner COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_phase_b_runner.py ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
  endif()

  if(VG_ENABLE_VULKAN)
    add_executable(vg_vulkan_bda_vertical_slice_test tests/vertical_slice/vulkan_bda_vertical_slice_test.cpp)
    target_link_libraries(vg_vulkan_bda_vertical_slice_test PRIVATE vg_backend_vulkan vg_backend_reference vg_ir)
    target_include_directories(vg_vulkan_bda_vertical_slice_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
    target_compile_features(vg_vulkan_bda_vertical_slice_test PRIVATE cxx_std_20)
    add_test(NAME vertical-slice.vulkan COMMAND vg_vulkan_bda_vertical_slice_test ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(vg_vulkan_task_timeline_test tests/vertical_slice/vulkan_task_timeline_test.cpp)
    target_link_libraries(vg_vulkan_task_timeline_test PRIVATE vg_backend_vulkan vg_backend_reference vg_core)
    target_include_directories(vg_vulkan_task_timeline_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
    target_compile_features(vg_vulkan_task_timeline_test PRIVATE cxx_std_20)
    add_test(NAME vertical-slice.vulkan.task-tier0 COMMAND vg_vulkan_task_timeline_test task-tier0 ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME vertical-slice.vulkan.timeline COMMAND vg_vulkan_task_timeline_test timeline ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  add_test(NAME schema.generate COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_schema_generator.py ${CMAKE_CURRENT_SOURCE_DIR} ${VG_GENERATED_DIR})
  add_test(NAME tooling.phase-a-runner COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_phase_a_runner.py ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

  # Phase D slices land in cmake/phase-d-d*.cmake so D2–D6 can land in
  # parallel without editing this file. Missing fragments are skipped.
  file(GLOB VG_PHASE_D_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/phase-d-d*.cmake")
  foreach(VG_PHASE_D_FRAGMENT IN LISTS VG_PHASE_D_CMAKE)
    include(${VG_PHASE_D_FRAGMENT})
  endforeach()

  # Phase E slices land in cmake/phase-e-e*.cmake (TASK-E1 / E4 / E6).
  file(GLOB VG_PHASE_E_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/phase-e-e*.cmake")
  foreach(VG_PHASE_E_FRAGMENT IN LISTS VG_PHASE_E_CMAKE)
    include(${VG_PHASE_E_FRAGMENT})
  endforeach()
endif()
