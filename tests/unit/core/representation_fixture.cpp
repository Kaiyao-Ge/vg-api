#include "representation_fixture.h"

namespace vg::tests::core {

vg::ir::Module make_representation_probe_module(const vg::core::Allocation& allocation) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/v1";
  vg::ir::Instruction load;
  load.op = "load";
  load.allocation = allocation.id;
  load.generation = allocation.generation;
  load.representation_epoch = allocation.representation_epoch;
  load.offset = 0;
  load.size = 4;
  module.instructions.push_back(load);
  module.declared_effects.push_back(
      {allocation.id, 0, 4, vg::ir::Access::Read, allocation.representation_epoch});
  return module;
}

bool assemble_representation_plan(
    vg::core::Arena& arena, const vg::core::Allocation& probe,
    const std::vector<vg::core::RepresentationRequest>& requests,
    const vg::core::FacetPool& pool, vg::test_support::AssembledPlanFixture* fixture,
    vg::core::ExecutionPlan* plan, std::string* error) {
  vg::test_support::AssemblyOptions options;
  options.representation_requests = &requests;
  options.facet_pool = &pool;
  return vg::test_support::assemble_single_node_plan(
      arena, make_representation_probe_module(probe),
      {vg::test_support::compute_task(probe.id, probe.generation)}, fixture, plan, error,
      options);
}

}  // namespace vg::tests::core
