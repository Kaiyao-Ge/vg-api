# TASK-D3 / ADR-037: working-set budget (E011).

vg_target(vg_working_set_test TYPE EXECUTABLE
  SOURCES tests/unit/working_set_test.cpp
  LINK_PRIVATE vg_backend_reference vg_compiler
  INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  FEATURES_PRIVATE cxx_std_20)
vg_test(core.working-set COMMAND vg_working_set_test)

if(VG_ENABLE_METAL)
  vg_target(vg_metal_working_set_test TYPE EXECUTABLE
    SOURCES tests/vertical_slice/metal_working_set_test.cpp
    LINK_PRIVATE vg_backend_metal vg_backend_reference vg_compiler
    FEATURES_PRIVATE cxx_std_20)
  vg_test(vertical-slice.metal.working-set COMMAND vg_metal_working_set_test)
endif()
