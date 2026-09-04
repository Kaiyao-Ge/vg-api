#include "assembled_plan_fixture.h"
#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "compiler/compiler.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

vg::ir::Module read_module(uint64_t allocation) {
  auto compiled = vg::compiler::compile_c_like("@node @effects load(1,0,4)");
  assert(compiled.ok);
  compiled.module.instructions[0].allocation = allocation;
  compiled.module.declared_effects[0].allocation = allocation;
  compiled.module.canonical_json = vg::ir::serialize_module(compiled.module);
  return compiled.module;
}

vg::ir::Module write_module(uint64_t allocation, uint64_t value) {
  auto compiled = vg::compiler::compile_c_like("@node @effects store(1,0,4," +
                                               std::to_string(value) + ")");
  assert(compiled.ok);
  compiled.module.instructions[0].allocation = allocation;
  compiled.module.declared_effects[0].allocation = allocation;
  compiled.module.canonical_json = vg::ir::serialize_module(compiled.module);
  return compiled.module;
}

vg::core::CanonicalView rgba_view(const vg::core::Allocation &allocation) {
  vg::core::CanonicalView view;
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  view.width = 1;
  view.height = 1;
  return view;
}

void transition_identity_coverage_and_package_admission() {
  vg::core::Arena arena;
  auto &allocation = arena.allocate(64);
  vg::test_support::MultiNodePlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  assert(vg::test_support::assemble_multi_node_plan(
      arena, {write_module(allocation.id, 7), read_module(allocation.id)},
      {vg::test_support::compute_task(allocation.id),
       vg::test_support::compute_task(allocation.id)},
      {{0, 1}}, &fixture, &plan, &error));
  auto device = vg::reference::make_device_hal();
  assert(device);
  vg::hal::CompiledPlan compiled;
  assert(device->compile(plan, &compiled, &error));
  assert(compiled.transition_operations.size() == 1);
  assert(compiled.transition_operations[0].covered_region_visibility.size() ==
         1);
  assert(vg::hal::validate_stage7_compiled_plan(
      compiled, vg::hal::BackendKind::Reference, &error));

  auto missing = compiled;
  missing.transition_operations.clear();
  assert(!vg::hal::validate_stage7_compiled_plan(
      missing, vg::hal::BackendKind::Reference, &error));

  auto duplicate_coverage = compiled;
  duplicate_coverage.transition_operations[0]
      .covered_region_visibility.push_back(0);
  assert(!vg::hal::validate_stage7_compiled_plan(
      duplicate_coverage, vg::hal::BackendKind::Reference, &error));

  auto wrong_wave = compiled;
  ++wrong_wave.transition_operations[0].after_wave;
  assert(!vg::hal::validate_stage7_compiled_plan(
      wrong_wave, vg::hal::BackendKind::Reference, &error));

  auto unsupported = compiled;
  unsupported.transition_operations[0].state =
      vg::hal::CompiledPlan::TransitionLoweringState::Unsupported;
  assert(!vg::hal::validate_stage7_compiled_plan(
      unsupported, vg::hal::BackendKind::Reference, &error));

  auto unfinalized_claim = compiled;
  unfinalized_claim.transition_operations[0].state =
      vg::hal::CompiledPlan::TransitionLoweringState::BackendExecutionRequired;
  unfinalized_claim.transition_operations[0].serialized_fallback = false;
  --unfinalized_claim.report.transition_serialized_fallback_count;
  unfinalized_claim.transition_operations[0].barrier_count = 1;
  unfinalized_claim.report.transition_barrier_count = 1;
  assert(!vg::hal::validate_stage7_compiled_plan(
      unfinalized_claim, vg::hal::BackendKind::Reference, &error));

  auto lowered = compiled;
  lowered.transition_operations[0].state =
      vg::hal::CompiledPlan::TransitionLoweringState::Lowered;
  lowered.transition_operations[0].barrier_count = 1;
  lowered.report.transition_barrier_count = 1;
  assert(vg::hal::validate_stage7_compiled_plan(
      lowered, vg::hal::BackendKind::Reference, &error));
  ++lowered.report.transition_barrier_count;
  assert(!vg::hal::validate_stage7_compiled_plan(
      lowered, vg::hal::BackendKind::Reference, &error));

  auto stale_generation = compiled;
  ++stale_generation.per_node_packages[0].ref.generation;
  assert(!vg::hal::validate_stage7_compiled_plan(
      stale_generation, vg::hal::BackendKind::Reference, &error));

  auto wrong_kind = compiled;
  wrong_kind.per_node_packages[0].kind =
      vg::hal::CompiledPlan::NodePackageKind::Raster;
  wrong_kind.per_node_packages[0].package.reset();
  assert(!vg::hal::validate_stage7_compiled_plan(
      wrong_kind, vg::hal::BackendKind::Reference, &error));
}

void shared_representation_prerequisite_has_one_execution_owner() {
  vg::core::Arena arena;
  auto &allocation = arena.allocate(64);
  const auto module = read_module(allocation.id);
  const vg::core::RepresentationRequest request{
      .view = rgba_view(allocation),
      .target_kind = vg::core::FacetKind::Sample,
  };
  const std::vector<vg::core::RepresentationRequest> requests{request};
  vg::test_support::AssemblyOptions options;
  options.representation_requests = &requests;
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  auto device = vg::reference::make_device_hal();
  assert(device);
  options.facet_pool = &device->facet_pool();
  std::string error;
  assert(vg::test_support::assemble_single_node_plan(
      arena, module,
      {vg::test_support::compute_task(allocation.id),
       vg::test_support::compute_task(allocation.id)},
      &fixture, &plan, &error, options));
  assert(plan.execution_schedule.transitions.size() == 2);
  for (const auto &transition : plan.execution_schedule.transitions)
    assert(transition.representation_operations == std::vector<uint32_t>({0}));

  vg::hal::CompiledPlan compiled;
  assert(device->compile(plan, &compiled, &error));
  assert(compiled.representation_operations.size() == 1);
  assert(compiled.representation_operation_execution_order ==
         std::vector<uint32_t>({0}));
  assert(vg::hal::validate_stage7_compiled_plan(
      compiled, vg::hal::BackendKind::Reference, &error));

  auto duplicate_owner = compiled;
  duplicate_owner.representation_operation_execution_order.push_back(0);
  assert(!vg::hal::validate_stage7_compiled_plan(
      duplicate_owner, vg::hal::BackendKind::Reference, &error));

  auto missing_reference = compiled;
  missing_reference.transition_operations[0].representation_operations.clear();
  assert(!vg::hal::validate_stage7_compiled_plan(
      missing_reference, vg::hal::BackendKind::Reference, &error));

  auto out_of_range = compiled;
  out_of_range.transition_operations[0].representation_operations[0] = 1;
  assert(!vg::hal::validate_stage7_compiled_plan(
      out_of_range, vg::hal::BackendKind::Reference, &error));

  auto wrong_order = compiled;
  ++wrong_order.representation_operations[0].semantic_order;
  assert(!vg::hal::validate_stage7_compiled_plan(
      wrong_order, vg::hal::BackendKind::Reference, &error));
}

void raster_facet_coverage_is_sealed_before_stage7() {
  vg::core::Arena arena;
  auto &source = arena.allocate(4);
  auto &target = arena.allocate(4);
  auto &vertex = arena.allocate(12);
  auto device = vg::reference::make_device_hal();
  assert(device);
  vg::core::FacetRef source_ref{};
  vg::core::FacetRef target_ref{};
  vg::core::FacetRef vertex_ref{};
  std::string error;
  assert(device->facet_pool().acquire(arena, rgba_view(source),
                                      vg::core::FacetKind::Sample, &source_ref,
                                      &error));
  assert(device->facet_pool().acquire(arena, rgba_view(target),
                                      vg::core::FacetKind::Attachment,
                                      &target_ref, &error));
  assert(device->facet_pool().acquire(arena, rgba_view(vertex),
                                      vg::core::FacetKind::Address, &vertex_ref,
                                      &error));
  vg::core::TaskRecord raster;
  raster.kind = vg::core::TaskKind::Raster;
  raster.raster_facets = {source_ref, target_ref};
  raster.vertex_buffer_ref = vertex_ref;
  const vg::ir::UserRasterShaderContract shader{
      "vg.test.raster/v1", "vertex_main", "fragment_main",
      vg::ir::kRasterVertexAbiXyzuvPackedV1, "kernel source is opaque to core"};
  vg::test_support::AssemblyOptions options;
  options.facet_pool = &device->facet_pool();
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  assert(vg::test_support::assemble_single_user_raster_plan(
      arena, shader, {raster}, &fixture, &plan, &error, options));
  assert(plan.execution_schedule.transitions.size() == 1);
  assert(plan.execution_schedule.transitions[0].facet_requirements.size() == 3);
  vg::hal::CompiledPlan compiled;
  assert(device->compile(plan, &compiled, &error));
  assert(compiled.transition_operations[0].covered_facet_requirements.size() ==
         3);
  assert(vg::hal::validate_stage7_compiled_plan(
      compiled, vg::hal::BackendKind::Reference, &error));

  auto missing_facet = compiled;
  missing_facet.transition_operations[0].covered_facet_requirements.pop_back();
  assert(!vg::hal::validate_stage7_compiled_plan(
      missing_facet, vg::hal::BackendKind::Reference, &error));
}

} // namespace

int main() {
  transition_identity_coverage_and_package_admission();
  shared_representation_prerequisite_has_one_execution_owner();
  raster_facet_coverage_is_sealed_before_stage7();
  return 0;
}
