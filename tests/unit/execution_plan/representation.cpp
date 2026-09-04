#include "fixture.h"

namespace vg::tests::execution_plan {

void test_representation_stage5_assembler_boundaries() {
  // These cases deliberately instantiate no DeviceHal.  Every refusal must
  // therefore come from the canonical Stage-5 assembler before Stage 6 can
  // lower or compile a physical operation.
  vg::core::Arena arena;
  const auto& probe = arena.allocate(32);
  const uint64_t probe_id = probe.id;
  const uint32_t probe_generation = probe.generation;
  const auto& image = arena.allocate(64);
  const uint64_t image_id = image.id;
  const uint32_t image_generation = image.generation;
  vg::core::FacetPool pool;

  vg::core::RepresentationRequest request;
  request.view.allocation = image_id;
  request.view.allocation_generation = image_generation;
  request.view.width = 2;
  request.view.height = 2;
  request.target_kind = vg::core::FacetKind::Sample;

  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(assemble_representation_case(arena, probe_id, probe_generation, {request}, pool,
                                     &plan, &error));
  CHECK(plan.validate(&error));

  auto oversized = request;
  oversized.view.width = 8;
  oversized.view.height = 8;
  CHECK(!assemble_representation_case(arena, probe_id, probe_generation, {oversized}, pool,
                                      &plan, &error));
  CHECK(error == "canonical view describes more texels than its allocation backs");

  auto depth_sample = request;
  depth_sample.view.format = vg::core::PixelFormat::Depth32Float;
  CHECK(!assemble_representation_case(arena, probe_id, probe_generation, {depth_sample}, pool,
                                      &plan, &error));
  CHECK(error == "Depth32Float canonical views may only acquire Attachment facets");

  auto stale_generation = request;
  ++stale_generation.view.allocation_generation;
  CHECK(!assemble_representation_case(arena, probe_id, probe_generation, {stale_generation},
                                      pool, &plan, &error));
  CHECK(error == "canonical view allocation is not active in arena");

  vg::core::FacetRef live{};
  CHECK(pool.acquire(arena, request.view, request.target_kind, &live, &error));
  auto consume = request;
  consume.consume_input = true;
  consume.consume_proof = {true, true, true, true};
  CHECK(!assemble_representation_case(arena, probe_id, probe_generation, {consume}, pool,
                                      &plan, &error));
  CHECK(error ==
        "representation request asks for ConsumeInput while a live FacetRef names its source epoch");
  CHECK(pool.retire(live, &error));
  CHECK(assemble_representation_case(arena, probe_id, probe_generation, {consume}, pool,
                                     &plan, &error));
  CHECK(plan.validate(&error));
}

void test_representation_semantic_plan_is_sealed() {
  vg::core::Arena arena;
  const auto& probe = arena.allocate(32);
  const uint64_t probe_id = probe.id;
  const uint32_t probe_generation = probe.generation;
  const auto& image = arena.allocate(64);
  vg::core::FacetPool pool;
  vg::core::RepresentationRequest request;
  request.view.allocation = image.id;
  request.view.allocation_generation = image.generation;
  request.view.width = 2;
  request.view.height = 2;
  request.target_kind = vg::core::FacetKind::Sample;
  request.consume_input = true;
  request.consume_proof = {true, true, true, true};

  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(assemble_representation_case(arena, probe_id, probe_generation, {request}, pool,
                                     &plan, &error));
  CHECK(plan.validate(&error));

  auto swizzle_tampered = plan;
  swizzle_tampered.representation_requests[0].view.swizzle.red = vg::core::Swizzle::One;
  CHECK(!swizzle_tampered.validate(&error));
  CHECK(error == "representation request and frozen semantic plan item disagree");

  auto proof_tampered = plan;
  proof_tampered.representation_requests[0].consume_proof.no_external_references = false;
  CHECK(!proof_tampered.validate(&error));
  CHECK(error == "representation request and frozen semantic plan item disagree");

  auto order_tampered = plan;
  order_tampered.representation_plan[0].transform_order = 1;
  CHECK(!order_tampered.validate(&error));
  CHECK(error == "representation request and frozen semantic plan item disagree");

  auto epoch_tampered = plan;
  ++epoch_tampered.representation_plan[0].target_representation_epoch;
  CHECK(!epoch_tampered.validate(&error));
  CHECK(error == "representation request and frozen semantic plan item disagree");
}

void test_consume_input_proof_rejections() {
  // These are semantic-boundary negatives: no assembled facts are forged and
  // no adapter compile path is involved.  The request itself must carry a
  // complete destructive-transform proof before any lowering is considered.
  vg::core::Arena arena;
  const auto& root = arena.allocate(64);
  const auto& image = arena.allocate(64);
  vg::core::RepresentationRequest request;
  request.view.allocation = image.id;
  request.view.allocation_generation = 1;
  request.view.width = 4;
  request.view.height = 4;
  request.target_kind = vg::core::FacetKind::Sample;
  request.consume_input = true;
  vg::test_support::AssemblyOptions options;
  options.representation_requests = nullptr;
  auto device = vg::reference::make_device_hal();
  options.facet_pool = &device->facet_pool();
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  const std::vector<vg::core::RepresentationRequest> incomplete_proof{request};
  options.representation_requests = &incomplete_proof;
  CHECK(!vg::test_support::assemble_single_node_plan(
      arena, canonical_module(root.id), {vg::test_support::compute_task(root.id, root.generation)},
      &fixture, &plan, &error, options));
  CHECK(error.find("ConsumeInput but its proof is incomplete") != std::string::npos);

  request.consume_proof = {true, false, true, true};
  const std::vector<vg::core::RepresentationRequest> incomplete_external{request};
  options.representation_requests = &incomplete_external;
  CHECK(!vg::test_support::assemble_single_node_plan(
      arena, canonical_module(root.id), {vg::test_support::compute_task(root.id, root.generation)},
      &fixture, &plan, &error, options));
  CHECK(error.find("an external reference to the old representation still exists") != std::string::npos);
}

}  // namespace vg::tests::execution_plan
