# G5: explicit source ownership only; existing CTest contracts are unchanged.
target_sources(vg_core_test PRIVATE
  tests/unit/core/access_envelope.cpp
  tests/unit/core/facet.cpp
  tests/unit/core/graph_semantics.cpp
  tests/unit/core/physical_representation.cpp
  tests/unit/core/reference_submit.cpp
  tests/unit/core/representation.cpp
  tests/unit/core/representation_fixture.cpp
)
target_sources(vg_execution_plan_test PRIVATE
  tests/unit/execution_plan/access.cpp
  tests/unit/execution_plan/fixture.cpp
  tests/unit/execution_plan/lifetime.cpp
  tests/unit/execution_plan/node_effect.cpp
  tests/unit/execution_plan/reference_submit.cpp
  tests/unit/execution_plan/representation.cpp
  tests/unit/execution_plan/validation.cpp
)
target_sources(vg_reference_raster_test PRIVATE
  tests/unit/reference/facet_oracles.cpp
  tests/unit/reference/facet_tokens.cpp
  tests/unit/reference/plan_submit.cpp
  tests/unit/reference/raster_fixture.cpp
  tests/unit/reference/raster_oracles.cpp
)
if(VG_ENABLE_METAL)
  target_sources(vg_metal_task_timeline_test PRIVATE
    tests/vertical_slice/metal/direct_compute.cpp
    tests/vertical_slice/metal/direct_facets.cpp
    tests/vertical_slice/metal/direct_raster.cpp
    tests/vertical_slice/metal/direct_representation.cpp
    tests/vertical_slice/metal/fixture.cpp
    tests/vertical_slice/metal/plan_compute.cpp
    tests/vertical_slice/metal/plan_effect.cpp
    tests/vertical_slice/metal/plan_raster.cpp
    tests/vertical_slice/metal/plan_representation.cpp
    tests/vertical_slice/metal/semantic_negative.cpp
  )
endif()
