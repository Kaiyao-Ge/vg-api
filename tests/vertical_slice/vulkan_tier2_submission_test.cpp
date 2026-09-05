// CPU validation of the production-facing physical handoff. The registered
// Vulkan plan-tier2 test separately covers formal Stage 6/7 device execution.
#include "backends/vulkan/vulkan_tier2.h"
#include "../support/assembled_plan_fixture.h"

#include <algorithm>
#include <iostream>

namespace {
vg::ir::Module module_for(const vg::core::Allocation &allocation) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/tier2-sealed";
  module.instructions.push_back({"load", allocation.id, 0, 4, 0,
                                 allocation.generation,
                                 allocation.representation_epoch, 0, "load"});
  module.declared_effects.push_back({allocation.id, 0, 4, vg::ir::Access::Read,
                                     allocation.representation_epoch});
  return module;
}

bool sealed_request_cases() {
  vg::core::Arena arena;
  auto &a = arena.allocate(4);
  auto &b = arena.allocate(4);
  std::vector<vg::core::TaskRecord> tasks(2);
  tasks[0].root_allocation = a.id;
  tasks[0].root_generation = a.generation;
  tasks[1].root_allocation = b.id;
  tasks[1].root_generation = b.generation;
  vg::test_support::MultiNodePlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  // The fixture needs node refs before options can reference them, so first
  // assemble normally, then use those live refs in a second fixture.
  if (!vg::test_support::assemble_multi_node_plan(
          arena, {module_for(a), module_for(b)}, tasks, {}, &fixture, &plan,
          &error))
    return false;
  const auto requested = fixture.node_refs;
  vg::test_support::MultiNodePlanFixture requested_fixture;
  vg::core::ExecutionPlan requested_plan;
  vg::test_support::AssemblyOptions options;
  // Recreate the fixture cannot reuse node refs; this successful check is
  // performed by constructing inputs directly below.
  vg::core::ExecutionPlanAssemblerInputs inputs{
      &fixture.graph, &fixture.nodes, &fixture.envelope, &arena,
      nullptr,        nullptr,        nullptr,           0};
  inputs.tier2_selection_nodes = &requested;
  if (!vg::core::ExecutionPlanAssembler::assemble(inputs, &requested_plan,
                                                  &error))
    return false;
  const bool same_request =
      requested_plan.tier2_selection_nodes.size() == requested.size() &&
      std::ranges::equal(requested_plan.tier2_selection_nodes, requested,
                         [](const auto &left, const auto &right) {
                           return left.index == right.index &&
                                  left.generation == right.generation;
                         });
  if (!requested_plan.tier2_selection_requested || !same_request ||
      !std::ranges::any_of(
          requested_plan.required_capabilities, [](auto value) {
            return value ==
                   vg::core::CapabilityRequirement::IndirectTier2Select;
          }))
    return false;
  auto unauthorized = requested;
  unauthorized[1].generation++;
  inputs.tier2_selection_nodes = &unauthorized;
  vg::core::ExecutionPlan rejected;
  if (vg::core::ExecutionPlanAssembler::assemble(inputs, &rejected, &error))
    return false;
  requested_plan.tier2_selection_nodes[0].generation++;
  if (requested_plan.validate(&error))
    return false;
  requested_plan.tier2_selection_nodes = requested;
  requested_plan.tier2_selection_requested = false;
  if (requested_plan.validate(&error))
    return false;
  return true;
}
} // namespace

int main() {
  if (!sealed_request_cases()) {
    std::cerr << "sealed Tier2 request cases failed\n";
    return 1;
  }
  using vg::core::NodeTable;
  using vg::vulkan::tier2::AuthorizedBucket;
  using vg::vulkan::tier2::SelectionRecord;
  std::vector<AuthorizedBucket> authorized{{NodeTable::Ref{4, 7}, 0, 0},
                                           {NodeTable::Ref{9, 2}, 1, 1}};
  std::vector<SelectionRecord> tail{
      {NodeTable::Ref{4, 7}}, {NodeTable::Ref{9, 2}}, {NodeTable::Ref{4, 7}}};
  vg::vulkan::tier2::ValidatedSelection result;
  std::string error;
  if (!vg::vulkan::tier2::validate_pre_authorized_selection(tail, authorized,
                                                            &result, &error) ||
      result.selected_buckets != std::vector<uint32_t>({0, 1, 0})) {
    std::cerr << "tier2 physical handoff failed: " << error << "\n";
    return 1;
  }
  if (vg::vulkan::tier2::validate_pre_authorized_selection(
          {{NodeTable::Ref{4, 0}}}, authorized, &result, &error) ||
      // Same index with a different non-zero generation must not match.
      vg::vulkan::tier2::validate_pre_authorized_selection(
          {{NodeTable::Ref{4, 8}}}, authorized, &result, &error) ||
      vg::vulkan::tier2::validate_pre_authorized_selection(
          {{NodeTable::Ref{2, 1}}}, authorized, &result, &error) ||
      vg::vulkan::tier2::validate_pre_authorized_selection({}, authorized,
                                                           &result, &error) ||
      vg::vulkan::tier2::validate_pre_authorized_selection(
          tail, {{NodeTable::Ref{4, 7}, 0, 0}}, &result, &error) ||
      vg::vulkan::tier2::validate_pre_authorized_selection(
          tail, {{NodeTable::Ref{4, 7}, 0, 0}, {NodeTable::Ref{4, 7}, 1, 1}},
          &result, &error) ||
      vg::vulkan::tier2::validate_pre_authorized_selection(
          tail, {{NodeTable::Ref{4, 7}, 0, 0}, {NodeTable::Ref{9, 2}, 0, 1}},
          &result, &error) ||
      vg::vulkan::tier2::validate_pre_authorized_selection(
          tail, {{NodeTable::Ref{4, 7}, 0, 0}, {NodeTable::Ref{9, 2}, 1, 0}},
          &result, &error) ||
      vg::vulkan::tier2::validate_pre_authorized_selection(
          tail, {{NodeTable::Ref{4, 7}, 2, 0}, {NodeTable::Ref{9, 2}, 1, 1}},
          &result, &error) ||
      vg::vulkan::tier2::validate_pre_authorized_selection(
          tail, {{NodeTable::Ref{4, 7}, 0, 2}, {NodeTable::Ref{9, 2}, 1, 1}},
          &result, &error) ||
      vg::vulkan::tier2::validate_pre_authorized_selection(tail, authorized,
                                                           nullptr, &error)) {
    std::cerr << "tier2 physical handoff accepted invalid authority\n";
    return 1;
  }
  std::cout << "tier2-physical-cpu: ok\n";
  return 0;
}
