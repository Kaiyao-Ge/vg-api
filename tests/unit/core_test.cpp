#include "core/cases.h"

using namespace vg::tests::core;

int main() {
  vg::core::Arena arena;
  const vg::core::ConsumeProof discharged{true, true, true, true};
  test_arena_task_graph(arena, discharged);
  test_reference_task_timeline();
  test_reference_access_certificate();
  test_facet_pool();
  test_view_and_representation_epoch();
  test_consume_proof(discharged);
  test_physical_transform_fault(discharged);
  test_capture_consumed_representation(discharged);
  test_physical_consume_after_retire(discharged);
  test_representation_backpressure();
  test_facet_generation_table();
  test_reference_representation_submit(discharged);
  test_lease_budget_overflow();
  test_certificate_composition();
  return 0;
}
