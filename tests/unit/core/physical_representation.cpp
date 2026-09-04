#include "cases.h"
#include <cassert>
#include "representation_fixture.h"

namespace vg::tests::core {

void test_physical_transform_fault(const vg::core::ConsumeProof& discharged) {
  // --- E005 catalog fault-injection, host-visible paths. These observe the
  // existing Stage 5 / consume / capture machinery rather than inventing an
  // Arena-scoped fault injector. ---
  {
    vg::core::Arena stage_arena;
    auto& probe = stage_arena.allocate(16);
    probe.bytes.assign(16, 0);
    auto& backing = stage_arena.allocate(16);
    backing.bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const auto original = backing.bytes;
    const uint32_t generation = backing.generation;
    const uint32_t epoch_before = backing.representation_epoch;
    vg::core::CanonicalView view;
    view.allocation = backing.id;
    view.allocation_generation = generation;
    view.width = 2;
    view.height = 2;
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = discharged;
    vg::core::FacetPool pool;
    const std::vector<vg::core::RepresentationRequest> requests{request};
    vg::test_support::AssembledPlanFixture fixture;
    vg::core::ExecutionPlan plan;
    vg::hal::Submission submission;
    std::string stage_error;
    assert(assemble_representation_plan(stage_arena, probe, requests, pool, &fixture, &plan,
                                        &stage_error));
    const bool physical_fault_rejected = !vg::hal::commit_representation_operations(
        plan, {{vg::hal::CompiledPlan::RepresentationOperation::CopyToPrivate, 0, "fault harness"}}, stage_arena, pool,
        [](const vg::core::RepresentationSemanticPlanItem&, const vg::hal::CompiledPlan::PhysicalRepresentationOperation&, vg::core::FacetRef, vg::hal::RepresentationTransformCost*,
           std::string* physical_error) {
          if (physical_error) *physical_error = "injected physical transform fault";
          return false;
        },
        &submission, &stage_error);
    assert(physical_fault_rejected);
    assert(stage_error.find("injected physical transform fault") != std::string::npos);
    assert(submission.consumed_allocation_count == 0);
    assert(submission.released_backing_bytes == 0);
    assert(backing.bytes == original);
    assert(backing.generation == generation);
    assert(backing.state == vg::core::ObjectState::Active);
    assert(backing.representation_epoch == epoch_before + 1);
    assert(stage_arena.lookup(vg::core::RepresentationRef{backing.id, generation, epoch_before}) == nullptr);
    assert(stage_arena.lookup(vg::core::RepresentationRef{backing.id, generation, backing.representation_epoch}) != nullptr);
  }
}

void test_physical_consume_after_retire(const vg::core::ConsumeProof& discharged) {
  {
    vg::core::Arena hold_arena;
    auto& probe = hold_arena.allocate(16);
    probe.bytes.assign(16, 0);
    auto& backing = hold_arena.allocate(16);
    backing.bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const auto original = backing.bytes;
    const uint32_t epoch_before = backing.representation_epoch;
    vg::core::CanonicalView view;
    view.allocation = backing.id;
    view.allocation_generation = backing.generation;
    view.width = 2;
    view.height = 2;
    vg::core::FacetPool pool;
    vg::core::FacetRef live{};
    std::string hold_error;
    assert(pool.acquire(hold_arena, view, vg::core::FacetKind::Sample, &live, &hold_error));
    assert(pool.references(vg::core::RepresentationRef{backing.id, backing.generation, epoch_before}));
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = discharged;
    const std::vector<vg::core::RepresentationRequest> requests{request};
    vg::hal::Submission submission;
    vg::test_support::AssembledPlanFixture blocked_fixture;
    vg::core::ExecutionPlan blocked_plan;
    const bool live_facet_rejected = !assemble_representation_plan(
        hold_arena, probe, requests, pool, &blocked_fixture, &blocked_plan, &hold_error);
    assert(live_facet_rejected);
    assert(hold_error.find("live FacetRef names its source epoch") != std::string::npos);
    assert(backing.bytes == original);
    assert(backing.representation_epoch == epoch_before);
    assert(pool.lookup(hold_arena, live) != nullptr);
    assert(pool.retire(live, &hold_error));
    assert(!pool.references(vg::core::RepresentationRef{backing.id, backing.generation, epoch_before}));
    vg::test_support::AssembledPlanFixture fixture;
    vg::core::ExecutionPlan plan;
    assert(assemble_representation_plan(hold_arena, probe, requests, pool, &fixture, &plan,
                                        &hold_error));
    const bool consume_after_retire = vg::hal::commit_representation_operations(
        plan, {{vg::hal::CompiledPlan::RepresentationOperation::CopyToPrivate, 0, "fault harness"}}, hold_arena, pool,
        [](const vg::core::RepresentationSemanticPlanItem&, const vg::hal::CompiledPlan::PhysicalRepresentationOperation&, vg::core::FacetRef, vg::hal::RepresentationTransformCost* cost,
           std::string*) {
          cost->distinct_backing = true;
          cost->new_backing_bytes = 16;
          return true;
        },
        &submission, &hold_error);
    assert(consume_after_retire);
    assert(submission.consumed_allocation_count == 1);
    assert(backing.bytes.empty());
  }
}

}  // namespace vg::tests::core
