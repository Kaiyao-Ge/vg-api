vg_target(vg_abi_c_test TYPE EXECUTABLE
  SOURCES tests/abi/abi_c_test.c
  LINK_PRIVATE vg_api
  PROPERTIES LINKER_LANGUAGE CXX)
vg_test(abi.c COMMAND vg_abi_c_test)

vg_target(vg_abi_cpp_test TYPE EXECUTABLE
  SOURCES tests/abi/abi_cpp_test.cpp
  LINK_PRIVATE vg_api
  FEATURES_PRIVATE cxx_std_17)
vg_test(abi.cpp COMMAND vg_abi_cpp_test)

# Kept out of vg_abi_cpp_test so that stays a pure C++17 include test: this
# one links vg_core (C++20) to pin the public token against the core type
# it mirrors.
vg_target(vg_abi_facet_token_test TYPE EXECUTABLE
  SOURCES tests/abi/abi_facet_token_test.cpp
  LINK_PRIVATE vg_api vg_core
  FEATURES_PRIVATE cxx_std_20)
vg_test(abi.facet-token COMMAND vg_abi_facet_token_test)

# F1 (ADR-044): the actual Checkpoint-A precondition -- proves the golden
# path is reachable through <vg/vg.h> alone, with only vg_api linked, not
# vg_core/vg_backend_reference directly.
vg_target(vg_c_abi_conformance_test TYPE EXECUTABLE
  SOURCES tests/api/vg_c_abi_conformance_test.cpp
  LINK_PRIVATE vg_api
  FEATURES_PRIVATE cxx_std_17)
vg_test(api.c-abi-conformance COMMAND vg_c_abi_conformance_test)

# Device-scoped Node capability conformance: two canonical compute
# CodeObjects must execute per Task in one graph.  This stays a small,
# public-ABI-only acceptance test rather than adding more cases to the
# historical C ABI monolith.
vg_target(vg_multicode_taskgraph_conformance_test TYPE EXECUTABLE
  SOURCES tests/api/vg_multicode_taskgraph_conformance_test.cpp
  LINK_PRIVATE vg_api
  FEATURES_PRIVATE cxx_std_17)
vg_test(api.multicode-taskgraph-conformance COMMAND vg_multicode_taskgraph_conformance_test)

vg_target(vg_mixed_domain_conformance_test TYPE EXECUTABLE
  SOURCES tests/api/vg_mixed_domain_conformance_test.cpp
  LINK_PRIVATE vg_api
  FEATURES_PRIVATE cxx_std_17)
vg_test(api.mixed-domain.reference COMMAND vg_mixed_domain_conformance_test reference)
if(VG_ENABLE_METAL)
  vg_test(api.mixed-domain.metal COMMAND vg_mixed_domain_conformance_test metal)
  set_tests_properties(api.mixed-domain.metal PROPERTIES SKIP_RETURN_CODE 77)
endif()
if(VG_ENABLE_VULKAN)
  vg_test(api.mixed-domain.vulkan COMMAND vg_mixed_domain_conformance_test vulkan)
  set_tests_properties(api.mixed-domain.vulkan PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Stress the Device-owned NodeTable separately from the historical API
# conformance executable.  C++20 provides deterministic barrier scheduling
# without sleeps, and vg_core exposes the table's snapshot semantics.
vg_target(vg_node_table_concurrency_test TYPE EXECUTABLE
  SOURCES tests/unit/node_table_concurrency_test.cpp
  LINK_PRIVATE vg_core
  FEATURES_PRIVATE cxx_std_20)
vg_test(core.node-table-concurrency COMMAND vg_node_table_concurrency_test)

vg_target(vg_f7_checkpoint_a_test TYPE EXECUTABLE
  SOURCES tests/api/vg_f7_checkpoint_a.c
  LINK_PRIVATE vg_api)
vg_test(api.f7-checkpoint-a-c COMMAND vg_f7_checkpoint_a_test)
vg_target(vg_f6_scene_root_test TYPE EXECUTABLE
  SOURCES tests/api/vg_f6_scene_root.c
  LINK_PRIVATE vg_api)
vg_test(api.f6-scene-root-c COMMAND vg_f6_scene_root_test)

if(VG_ENABLE_METAL)
  # Reruns E002 (ADR-028/TASK-B15 typed pointer graph) through the public
  # C ABI alone, cross-checked against vertical-slice.metal.pointer-graph's
  # internal-machinery result. Requires real Metal hardware for the native
  # CachedObject GPU lowering; Reference consumes the same canonical
  # pointer-graph Node through its interpreter and Vulkan reports the
  # unsupported profile explicitly.
  vg_target(vg_e002_pointer_graph_abi_test TYPE EXECUTABLE
    SOURCES tests/api/vg_e002_pointer_graph_abi_test.cpp
    LINK_PRIVATE vg_api
    FEATURES_PRIVATE cxx_std_17)
  vg_test(api.e002-pointer-graph-via-abi COMMAND vg_e002_pointer_graph_abi_test)
endif()

vg_test(platform.probe COMMAND vg-platform-probe --validate)
vg_python_test(tooling.schemas tests/tools/test_schemas.py ${CMAKE_CURRENT_SOURCE_DIR})
vg_python_test(tooling.bundle tests/tools/test_bundle.py ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
vg_python_test(docs.check tools/vg-docs/vg_docs.py ${CMAKE_CURRENT_SOURCE_DIR})

vg_target(vg_core_test TYPE EXECUTABLE
  SOURCES tests/unit/core_test.cpp
  LINK_PRIVATE vg_backend_reference vg_capture
  INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  FEATURES_PRIVATE cxx_std_20)
vg_test(core.unit COMMAND vg_core_test)

vg_target(vg_execution_plan_test TYPE EXECUTABLE
  SOURCES tests/unit/execution_plan_test.cpp
  LINK_PRIVATE vg_core vg_backend_reference
  FEATURES_PRIVATE cxx_std_20)
vg_test(core.execution-plan COMMAND vg_execution_plan_test)

vg_target(vg_execution_schedule_test TYPE EXECUTABLE
  SOURCES tests/unit/execution_schedule_test.cpp
  LINK_PRIVATE vg_core
  FEATURES_PRIVATE cxx_std_20)
vg_test(core.execution-schedule COMMAND vg_execution_schedule_test)

vg_target(vg_device_hal_transition_contract_test TYPE EXECUTABLE
  SOURCES tests/unit/device_hal_transition_contract_test.cpp
  LINK_PRIVATE vg_backend_reference vg_compiler
  INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  FEATURES_PRIVATE cxx_std_20)
vg_test(core.device-hal-transition-contract
         COMMAND vg_device_hal_transition_contract_test)

# F6's generated root contract is validated in core, independently of the
# Reference/Metal physical raster paths.  This owns the malformed root and
# capture-v1 rejection matrix rather than duplicating it in an adapter test.
vg_target(vg_scene_root_contract_test TYPE EXECUTABLE
  SOURCES tests/unit/scene_root_contract_test.cpp
  LINK_PRIVATE vg_capture vg_backend_reference
  FEATURES_PRIVATE cxx_std_20)
vg_test(core.scene-root-contract COMMAND vg_scene_root_contract_test)


vg_target(vg_ir_test TYPE EXECUTABLE
  SOURCES tests/unit/ir_test.cpp
  LINK_PRIVATE vg_compiler
  FEATURES_PRIVATE cxx_std_20)
vg_test(ir.unit COMMAND vg_ir_test)

vg_target(vg_compute_package_test TYPE EXECUTABLE
  SOURCES tests/unit/compute_package_test.cpp
  LINK_PRIVATE vg_compiler
  FEATURES_PRIVATE cxx_std_20)
vg_test(compiler.compute-package COMMAND vg_compute_package_test)

vg_target(vg_compute_task_ring_test TYPE EXECUTABLE
  SOURCES tests/unit/compute_task_ring_test.cpp
  LINK_PRIVATE vg_compiler
  FEATURES_PRIVATE cxx_std_20)
vg_test(compiler.compute-task-ring COMMAND vg_compute_task_ring_test)

# pipeline_classification lives in vg_compiler, so this needs nothing else.
vg_target(vg_pipeline_classification_test TYPE EXECUTABLE
  SOURCES tests/unit/pipeline_classification_test.cpp
  LINK_PRIVATE vg_compiler
  FEATURES_PRIVATE cxx_std_20)
vg_test(compiler.pipeline-classification COMMAND vg_pipeline_classification_test)

vg_target(vg_reference_raster_test TYPE EXECUTABLE
  SOURCES tests/unit/reference_raster_test.cpp
  LINK_PRIVATE vg_backend_reference
  INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  FEATURES_PRIVATE cxx_std_20)
vg_test(reference.facet-oracles COMMAND vg_reference_raster_test)

vg_target(vg_reference_mixed_domain_conformance TYPE EXECUTABLE
  SOURCES tests/conformance/reference_mixed_domain_conformance.cpp
  LINK_PRIVATE vg_backend_reference vg_compiler
  INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  FEATURES_PRIVATE cxx_std_20)
vg_test(conformance.reference-mixed-domain COMMAND vg_reference_mixed_domain_conformance)

vg_target(vg_compute_package_golden_test TYPE EXECUTABLE
  SOURCES tests/fixtures/compute_package_golden_test.cpp
  LINK_PRIVATE vg_compiler vg_ir
  INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  FEATURES_PRIVATE cxx_std_20)
vg_test(compiler.compute-package-golden COMMAND vg_compute_package_golden_test ${CMAKE_CURRENT_SOURCE_DIR})

# Mandatory in Vulkan configurations (targets.cmake already requires glslc),
# also available on CPU-only hosts with glslc. No driver or device is loaded.
find_program(VG_GLSLC_EXECUTABLE NAMES glslc)
if(VG_GLSLC_EXECUTABLE)
  vg_python_test(compiler.compute-glsl tests/tools/check_compute_glsl.py
    --root ${CMAKE_CURRENT_SOURCE_DIR}
    --emitter $<TARGET_FILE:vg_compute_package_golden_test>
    --glslc ${VG_GLSLC_EXECUTABLE})
endif()

vg_target(vg_phase_a_model_test TYPE EXECUTABLE
  SOURCES tests/model/phase_a_model_test.cpp
  LINK_PRIVATE vg_core
  FEATURES_PRIVATE cxx_std_20)
vg_test(model.phase-a COMMAND vg_phase_a_model_test)

vg_target(vg_phase_a_witness_test TYPE EXECUTABLE
  SOURCES tests/model/phase_a_witness_test.cpp
  LINK_PRIVATE vg_core
  FEATURES_PRIVATE cxx_std_20)
vg_test(model.witness COMMAND vg_phase_a_witness_test)

vg_target(vg_phase_a_conformance TYPE EXECUTABLE
  SOURCES tests/conformance/phase_a_conformance.cpp
  LINK_PRIVATE vg_backend_reference vg_capture vg_compiler
  FEATURES_PRIVATE cxx_std_20)
vg_test(conformance.phase-a COMMAND vg_phase_a_conformance)

vg_target(vg_conformance_lib TYPE STATIC
  SOURCES tests/conformance/conformance_lib.cpp
  LINK_PUBLIC vg_backend_reference vg_compiler vg_ir
  INCLUDE_PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance" "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  FEATURES_PRIVATE cxx_std_20)

vg_target(vg_device_hal_conformance_reference TYPE EXECUTABLE
  SOURCES tests/conformance/device_hal_conformance_reference.cpp
  LINK_PRIVATE vg_conformance_lib vg_backend_reference
  FEATURES_PRIVATE cxx_std_20)
vg_test(conformance.device-hal.reference COMMAND vg_device_hal_conformance_reference ${CMAKE_CURRENT_SOURCE_DIR})

if(VG_ENABLE_METAL)
  vg_target(vg_device_hal_conformance_metal TYPE EXECUTABLE
    SOURCES tests/conformance/device_hal_conformance_metal.cpp
    LINK_PRIVATE vg_conformance_lib vg_backend_metal vg_backend_reference
    FEATURES_PRIVATE cxx_std_20)
  vg_test(conformance.device-hal.metal COMMAND vg_device_hal_conformance_metal ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(VG_ENABLE_VULKAN)
  vg_target(vg_device_hal_conformance_vulkan TYPE EXECUTABLE
    SOURCES tests/conformance/device_hal_conformance_vulkan.cpp
    LINK_PRIVATE vg_conformance_lib vg_backend_vulkan vg_backend_reference
    FEATURES_PRIVATE cxx_std_20)
  vg_test(conformance.device-hal.vulkan COMMAND vg_device_hal_conformance_vulkan ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(VG_ENABLE_METAL)
  vg_target(vg_metal_vertical_slice_test TYPE EXECUTABLE
    SOURCES tests/vertical_slice/metal_vertical_slice_test.cpp
    LINK_PRIVATE vg_backend_metal vg_backend_reference vg_ir
    INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
    FEATURES_PRIVATE cxx_std_20)
  vg_test(vertical-slice.metal COMMAND vg_metal_vertical_slice_test ${CMAKE_CURRENT_SOURCE_DIR})

  vg_target(vg_metal_identity_scene_root_cache_test TYPE EXECUTABLE
    SOURCES tests/vertical_slice/metal_identity_scene_root_cache_test.cpp
    LINK_PRIVATE vg_backend_metal vg_core
    FEATURES_PRIVATE cxx_std_20)
  vg_test(vertical-slice.metal.identity-scene-root-cache
           COMMAND vg_metal_identity_scene_root_cache_test)
  set_tests_properties(vertical-slice.metal.identity-scene-root-cache PROPERTIES SKIP_RETURN_CODE 77)

  vg_target(vg_metal_task_timeline_test TYPE EXECUTABLE
    SOURCES tests/vertical_slice/metal_task_timeline_test.cpp
    LINK_PRIVATE vg_backend_metal vg_backend_reference vg_core vg_capture
    INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
    FEATURES_PRIVATE cxx_std_20)
  vg_target(vg_metal_mixed_domain_conformance TYPE EXECUTABLE
    SOURCES tests/vertical_slice/metal_mixed_domain_conformance.cpp
    LINK_PRIVATE vg_backend_metal vg_backend_reference vg_compiler
    INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
    FEATURES_PRIVATE cxx_std_20)
  vg_test(vertical-slice.metal.mixed-domain
           COMMAND vg_metal_mixed_domain_conformance)
  set_tests_properties(vertical-slice.metal.mixed-domain PROPERTIES SKIP_RETURN_CODE 77)
  vg_test_modes(vg_metal_task_timeline_test vertical-slice.metal
    task-tier0 timeline access-certificate tier1-indirect cull-compact cull-compact-1m
    effect-dag pointer-graph indexed-binding representation-layer sample-facet
    checked-facet-generation basic-raster task-graph-raster task-graph-raster-depth
    task-graph-raster-user-shader pipeline-classification consume-input representation-churn)

  # TASK-B17: the Phase B gate runner drives ctest names registered above
  # (vertical-slice.metal.*), so it is only meaningful -- and only
  # registered -- under VG_ENABLE_METAL, matching every ctest it depends on.
  vg_python_test(tooling.phase-b-runner tests/tools/test_phase_b_runner.py ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
endif()

# No Vulkan device or SDK is needed: guards against capability advertising
# or a compile-time linear fallback that a driver-dependent vertical slice
# cannot observe on development hosts.
vg_python_test(vertical-slice.vulkan.capability-contract
  tests/vertical_slice/vulkan_capability_contract_test.py ${CMAKE_CURRENT_SOURCE_DIR})

if(VG_ENABLE_VULKAN)
  vg_target(vg_vulkan_bda_vertical_slice_test TYPE EXECUTABLE
    SOURCES tests/vertical_slice/vulkan_bda_vertical_slice_test.cpp
    LINK_PRIVATE vg_backend_vulkan vg_backend_reference vg_ir
    INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
    FEATURES_PRIVATE cxx_std_20)
  vg_test(vertical-slice.vulkan COMMAND vg_vulkan_bda_vertical_slice_test ${CMAKE_CURRENT_SOURCE_DIR})

  vg_target(vg_vulkan_task_timeline_test TYPE EXECUTABLE
    SOURCES tests/vertical_slice/vulkan_task_timeline_test.cpp
    LINK_PRIVATE vg_backend_vulkan vg_backend_reference vg_core
    INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
    FEATURES_PRIVATE cxx_std_20)
  vg_test_modes(vg_vulkan_task_timeline_test vertical-slice.vulkan
    task-tier0 timeline raster-basic raster-msl-rejected)

  # B/C parity gates require a real Vulkan device; no skip or backend fallback.
  vg_target(vg_vulkan_pointer_graph_test TYPE EXECUTABLE
    SOURCES tests/vertical_slice/vulkan_pointer_graph_test.cpp
    LINK_PRIVATE vg_backend_vulkan vg_backend_reference vg_compiler
    INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
    FEATURES_PRIVATE cxx_std_20)
  vg_test(vertical-slice.vulkan.pointer-graph COMMAND vg_vulkan_pointer_graph_test ${CMAKE_CURRENT_SOURCE_DIR})

  vg_target(vg_vulkan_pointer_graph_abi_test TYPE EXECUTABLE
    SOURCES tests/api/vg_vulkan_pointer_graph_abi_test.cpp
    LINK_PRIVATE vg_api
    FEATURES_PRIVATE cxx_std_17)
  vg_test(api.vulkan-pointer-graph COMMAND vg_vulkan_pointer_graph_abi_test)

  vg_target(vg_vulkan_discovery_working_set_test TYPE EXECUTABLE
    SOURCES tests/vertical_slice/vulkan_discovery_working_set_test.cpp
    LINK_PRIVATE vg_backend_vulkan vg_backend_reference vg_compiler
    INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
    FEATURES_PRIVATE cxx_std_20)
  vg_test(vertical-slice.vulkan.discovery COMMAND vg_vulkan_discovery_working_set_test discovery ${CMAKE_CURRENT_SOURCE_DIR})
  vg_test(vertical-slice.vulkan.working-set COMMAND vg_vulkan_discovery_working_set_test working-set ${CMAKE_CURRENT_SOURCE_DIR})
endif()

vg_python_test(schema.generate tests/tools/test_schema_generator.py ${CMAKE_CURRENT_SOURCE_DIR} ${VG_GENERATED_DIR})
vg_python_test(tooling.phase-a-runner tests/tools/test_phase_a_runner.py ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# Explicit ordered inventory; a missing fragment is a configure error.
foreach(fragment IN ITEMS
    phase-d-d2 phase-d-d3 phase-d-d4 phase-d-d5 phase-d-d6 phase-d-d7
    phase-e-e1 phase-e-e4 phase-e-e6)
  include(${CMAKE_CURRENT_LIST_DIR}/${fragment}.cmake)
endforeach()
include(cmake/g5-tests.cmake)
if(VG_ENABLE_METAL)
  include(cmake/g3-metal-tests.cmake)
endif()
if(VG_ENABLE_VULKAN)
  include(cmake/g4-vulkan-tests.cmake)
endif()

vg_python_test(tooling.phase-runner-contract tests/tools/test_phase_runner_contract.py ${CMAKE_CURRENT_SOURCE_DIR})
