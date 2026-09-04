#include "execution_plan/cases.h"

using namespace vg::tests::execution_plan;

int main() {
  test_basic_execution_plan_validation();
  test_consume_input_proof_rejections();
  test_stage6_capability_preflight_rejects_without_weakening();
  test_validation_profile_matrix_and_reset();
  test_effect_conflicts_are_deterministic_or_reject_reverse_cycle();
  test_certificate_and_access_witness_reject_partial_coverage();
  test_bounded_pointer_graph_canonical_identity();
  test_reference_multi_node_runtime_pointer_fault_preserves_prefix();
  test_execution_plan_assembler_sound_counterexamples();
  test_validated_effect_graph_and_full_noderef_packages_are_sealed();
  test_execution_plan_assembler_bounded_pointer_graph_access();
  test_execution_plan_assembler_seals_access_planning();
  test_representation_stage5_assembler_boundaries();
  test_representation_semantic_plan_is_sealed();
  test_submission_lifetime_hold_is_transactional_and_repeatable();
  test_submission_lifetime_hold_deduplicates_facets_and_backing();
  test_representation_outputs_join_lifetime_after_physical_stage();
  test_reference_submit_releases_holds_on_success_and_repeat();
  return 0;
}
