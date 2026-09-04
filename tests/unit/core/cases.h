#pragma once
#include "core/core.h"

namespace vg::tests::core {
void test_arena_task_graph(vg::core::Arena& arena, const vg::core::ConsumeProof& discharged);
void test_reference_task_timeline();
void test_reference_access_certificate();
void test_facet_pool();
void test_view_and_representation_epoch();
void test_consume_proof(const vg::core::ConsumeProof& discharged);
void test_physical_transform_fault(const vg::core::ConsumeProof& discharged);
void test_capture_consumed_representation(const vg::core::ConsumeProof& discharged);
void test_physical_consume_after_retire(const vg::core::ConsumeProof& discharged);
void test_representation_backpressure();
void test_facet_generation_table();
void test_reference_representation_submit(const vg::core::ConsumeProof& discharged);
void test_lease_budget_overflow();
void test_certificate_composition();
}
