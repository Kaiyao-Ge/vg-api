#include "fixture.h"

namespace vg::tests::execution_plan {

[[noreturn]] void check_failed(const char* expression, const char* file, int line) {
  std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
  std::exit(EXIT_FAILURE);
}

vg::core::TaskRecord task(uint32_t node, uint64_t root) {
  vg::core::TaskRecord value;
  value.node_index = node;
  value.node_generation = 1;
  value.root_allocation = root;
  value.root_generation = 1;
  return value;
}

vg::ir::Module canonical_module(uint64_t allocation) {
  vg::ir::Module module;
  module.root_schema = "vg.root/v1";
  module.instructions = {{"store", allocation, 0, 4, 7, 1, 0, 0, ""}};
  module.declared_effects = {{allocation, 0, 4, vg::ir::Access::Write, 0}};
  module.canonical_json = vg::ir::serialize_module(module);
  module.hash = vg::ir::sha256_hex(module.canonical_json);
  return module;
}

std::shared_ptr<const vg::core::CodeObject> canonical_code_object(uint64_t allocation) {
  auto object = std::make_shared<vg::core::CodeObject>();
  object->module = canonical_module(allocation);
  return object;
}

bool assemble_representation_case(
    vg::core::Arena& arena, uint64_t probe_allocation, uint32_t probe_generation,
    const std::vector<vg::core::RepresentationRequest>& requests,
    const vg::core::FacetPool& pool, vg::core::ExecutionPlan* plan, std::string* error) {
  vg::test_support::AssembledPlanFixture fixture;
  vg::test_support::AssemblyOptions options;
  options.representation_requests = &requests;
  options.facet_pool = &pool;
  return vg::test_support::assemble_single_node_plan(
      arena, canonical_module(probe_allocation),
      {vg::test_support::compute_task(probe_allocation, probe_generation)}, &fixture, plan,
      error, options);
}

vg::core::TaskGraph published_graph(std::initializer_list<vg::core::TaskRecord> tasks) {
  vg::core::TaskGraphBuilder builder;
  for (const auto& value : tasks) CHECK(builder.append(value));
  vg::core::TaskGraph graph;
  CHECK(builder.seal(&graph));
  CHECK(graph.publish());
  return graph;
}

vg::core::CanonicalView rgba_view(const vg::core::Allocation& allocation,
                                  uint32_t width, uint32_t height) {
  vg::core::CanonicalView view;
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  view.width = width;
  view.height = height;
  return view;
}

}  // namespace vg::tests::execution_plan
