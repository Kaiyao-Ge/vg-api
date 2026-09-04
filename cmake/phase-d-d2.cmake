# TASK-D2 / ADR-036: discovery pass + E004 revisit.

vg_target(vg_discovery_test TYPE EXECUTABLE
  SOURCES tests/unit/discovery_test.cpp
  LINK_PRIVATE vg_core
  FEATURES_PRIVATE cxx_std_20)
vg_test(core.discovery COMMAND vg_discovery_test)

if(VG_ENABLE_METAL)
  vg_target(vg_metal_discovery_test TYPE EXECUTABLE
    SOURCES tests/vertical_slice/metal_discovery_test.cpp
    LINK_PRIVATE vg_backend_metal vg_backend_reference vg_compiler
    FEATURES_PRIVATE cxx_std_20)
  vg_test(vertical-slice.metal.discovery COMMAND vg_metal_discovery_test)
endif()
