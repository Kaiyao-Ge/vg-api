# TASK-D5 / E017: overflow buffer + next submit. Included from
# CMakeLists.txt via cmake/phase-d-d*.cmake so this slice does not edit
# the root list. Attach the helper to vg_backend_reference even though
# this fragment lives inside BUILD_TESTING -- submit() needs the symbol.

target_sources(vg_backend_reference PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/backends/envelope_stage.cpp")

add_executable(vg_envelope_continuation_test
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/envelope_continuation_test.cpp")
target_link_libraries(vg_envelope_continuation_test PRIVATE vg_backend_reference)
target_compile_features(vg_envelope_continuation_test PRIVATE cxx_std_20)
add_test(NAME core.envelope-continuation COMMAND vg_envelope_continuation_test)

if(VG_ENABLE_METAL)
  add_executable(vg_metal_envelope_continuation_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/vertical_slice/metal_envelope_continuation_test.cpp")
  target_link_libraries(vg_metal_envelope_continuation_test PRIVATE
    vg_backend_metal vg_backend_reference vg_core)
  target_include_directories(vg_metal_envelope_continuation_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
  target_compile_features(vg_metal_envelope_continuation_test PRIVATE cxx_std_20)
  add_test(NAME vertical-slice.metal.envelope-continuation
           COMMAND vg_metal_envelope_continuation_test ${CMAKE_CURRENT_SOURCE_DIR})
endif()
