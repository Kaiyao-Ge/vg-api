# TASK-D5 / E017: overflow buffer + next submit. Included from
# the root list. Production helper sources are registered unconditionally
# in the root list; this fragment only owns tests.

vg_target(vg_envelope_continuation_test TYPE EXECUTABLE
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/envelope_continuation_test.cpp"
  LINK_PRIVATE vg_backend_reference
  INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  FEATURES_PRIVATE cxx_std_20)
vg_test(core.envelope-continuation COMMAND vg_envelope_continuation_test)

if(VG_ENABLE_METAL)
  vg_target(vg_metal_envelope_continuation_test TYPE EXECUTABLE
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/vertical_slice/metal_envelope_continuation_test.cpp"
    LINK_PRIVATE vg_backend_metal vg_backend_reference vg_core
    INCLUDE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
    FEATURES_PRIVATE cxx_std_20)
  vg_test(vertical-slice.metal.envelope-continuation
           COMMAND vg_metal_envelope_continuation_test ${CMAKE_CURRENT_SOURCE_DIR})
endif()
