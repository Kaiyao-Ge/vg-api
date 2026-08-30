#ifndef VG_TESTS_SUPPORT_ASSEMBLED_PLAN_FIXTURE_H_
#define VG_TESTS_SUPPORT_ASSEMBLED_PLAN_FIXTURE_H_

// Test-only construction aid for plan-driven adapter tests.  It deliberately
// exercises the same CodeObject -> device NodeTable -> TaskGraph -> Envelope
// -> ExecutionPlanAssembler ownership path as the C API; it is not a second
// plan type and never stamps sealed fields by hand.

#include "core/execution_plan.h"
#include "ir/sha256.h"

#include <memory>

namespace vg::test_support {

struct AssembledPlanFixture {
  core::NodeTable nodes;
  core::ExecutionEnvelope envelope;
  core::TaskGraph graph;
  std::shared_ptr<const core::CodeObject> code_object;
  core::NodeTable::Ref node;
};

// Keep the test vocabulary intentionally typed.  This is a compact mirror of
// the existing semantic assembly inputs, rather than an untyped options bag
// which could accidentally become a second plan-construction API.
struct AssemblyOptions {
  const core::Certificate* certificate{};
  const core::AccessCertificate* access_certificate{};
  const std::vector<core::PointerRef>* discovery_witness{};
  const std::vector<core::PointerRef>* discovery_seeds{};
  const core::WorkingSetBudget* working_set_budget{};
  const core::WorkingSetLease* working_set_lease{};
  const core::EnvelopeOverflow* pending_overflow{};
  const std::vector<std::pair<uint32_t, uint32_t>>* dependencies{};
  uint64_t graph_epoch{};
  bool request_tier1_indirect{};
  bool request_indexed_binding{};
  const std::vector<ir::Module>* effect_dag_passes{};
  const std::vector<std::pair<uint32_t, uint32_t>>* effect_dag_dependencies{};
  const std::vector<core::RepresentationRequest>* representation_requests{};
  const core::FacetPool* facet_pool{};
  std::optional<core::AccessCertificateMode> certificate_mode;
  std::vector<core::PointerRef> certificate_touched;
  uint64_t timeline_wait{};
  uint64_t timeline_signal{};
  std::optional<uint32_t> task_quota;
};

inline bool assemble_single_node_plan(core::Arena& arena, ir::Module module,
                                      const std::vector<core::TaskRecord>& task_templates,
                                      AssembledPlanFixture* fixture,
                                      core::ExecutionPlan* out,
                                      std::string* error = nullptr, const AssemblyOptions& options = {}) {
  if (fixture == nullptr || out == nullptr || task_templates.empty()) {
    if (error) *error = "assembled-plan fixture requires tasks, fixture, and output";
    return false;
  }
  const std::string canonical = ir::serialize_module(module);
  module.canonical_json = canonical;
  module.hash = ir::sha256_hex(canonical);
  auto object = std::make_shared<core::CodeObject>();
  object->module = std::move(module);
  fixture->code_object = object;
  fixture->node = fixture->nodes.create(object, "test-entry");
  core::TaskGraphBuilder builder;
  for (auto task : task_templates) {
    task.node_index = fixture->node.index;
    task.node_generation = fixture->node.generation;
    if (!builder.append(task)) {
      if (error) *error = "test fixture could not append task";
      return false;
    }
  }
  if (options.dependencies != nullptr) {
    for (const auto& [before, after] : *options.dependencies)
      if (!builder.add_dependency(before, after)) {
        if (error) *error = "test fixture could not add explicit task dependency";
        return false;
      }
  }
  if (!builder.seal(&fixture->graph, error) || !fixture->graph.publish()) return false;
  fixture->envelope = {};
  fixture->envelope.allowed_nodes = {fixture->node};
  fixture->envelope.timeline_wait = options.timeline_wait;
  fixture->envelope.timeline_signal = options.timeline_signal;
  fixture->envelope.certificate_touched = options.certificate_touched;
  if (options.certificate_mode.has_value()) {
    fixture->envelope.has_certificate_mode = true;
    fixture->envelope.certificate_mode = *options.certificate_mode;
  }
  if (options.task_quota.has_value()) {
    fixture->envelope.has_task_quota = true;
    fixture->envelope.task_quota = *options.task_quota;
  }
  core::ExecutionPlanAssemblerInputs inputs{&fixture->graph, &fixture->nodes, &fixture->envelope,
                                              &arena, options.certificate, options.access_certificate,
                                              options.discovery_witness, options.graph_epoch};
  inputs.request_tier1_indirect = options.request_tier1_indirect;
  inputs.request_indexed_binding = options.request_indexed_binding;
  inputs.effect_dag_passes = options.effect_dag_passes;
  inputs.effect_dag_dependencies = options.effect_dag_dependencies;
  inputs.representation_requests = options.representation_requests;
  if (options.representation_requests != nullptr && options.facet_pool == nullptr) {
    if (error) *error = "representation fixture assembly requires the submitting DeviceHAL FacetPool";
    return false;
  }
  inputs.facet_pool = options.facet_pool;
  inputs.discovery_seeds = options.discovery_seeds;
  inputs.working_set_budget = options.working_set_budget;
  inputs.working_set_lease = options.working_set_lease;
  inputs.pending_overflow = options.pending_overflow;
  return core::ExecutionPlanAssembler::assemble(inputs, out, error);
}

inline core::TaskRecord compute_task(uint64_t root, uint32_t generation = 1) {
  core::TaskRecord task;
  task.kind = core::TaskKind::Compute;
  task.root_allocation = root;
  task.root_generation = generation;
  return task;
}

inline bool assemble_single_user_raster_plan(
    core::Arena& arena, const ir::UserRasterShaderContract& shader,
    const std::vector<core::TaskRecord>& task_templates, AssembledPlanFixture* fixture,
    core::ExecutionPlan* out, std::string* error = nullptr, const AssemblyOptions& options = {}) {
  if (fixture == nullptr || out == nullptr || task_templates.empty()) {
    if (error) *error = "assembled raster-plan fixture requires tasks, fixture, and output";
    return false;
  }
  auto object = std::make_shared<core::CodeObject>();
  object->user_raster_shader = shader;
  fixture->code_object = object;
  fixture->node = fixture->nodes.create(object, "test-raster-entry");
  core::TaskGraphBuilder builder;
  for (auto task : task_templates) {
    task.node_index = fixture->node.index;
    task.node_generation = fixture->node.generation;
    if (!builder.append(task)) {
      if (error) *error = "test fixture could not append raster task";
      return false;
    }
  }
  if (options.dependencies != nullptr)
    for (const auto& [before, after] : *options.dependencies)
      if (!builder.add_dependency(before, after)) {
        if (error) *error = "test fixture could not add explicit raster task dependency";
        return false;
      }
  if (!builder.seal(&fixture->graph, error) || !fixture->graph.publish()) return false;
  fixture->envelope = {};
  fixture->envelope.allowed_nodes = {fixture->node};
  fixture->envelope.timeline_wait = options.timeline_wait;
  fixture->envelope.timeline_signal = options.timeline_signal;
  fixture->envelope.certificate_touched = options.certificate_touched;
  if (options.certificate_mode.has_value()) {
    fixture->envelope.has_certificate_mode = true;
    fixture->envelope.certificate_mode = *options.certificate_mode;
  }
  if (options.task_quota.has_value()) {
    fixture->envelope.has_task_quota = true;
    fixture->envelope.task_quota = *options.task_quota;
  }
  core::ExecutionPlanAssemblerInputs inputs{&fixture->graph, &fixture->nodes, &fixture->envelope,
                                              &arena, options.certificate, options.access_certificate,
                                              options.discovery_witness, options.graph_epoch};
  inputs.request_tier1_indirect = options.request_tier1_indirect;
  inputs.request_indexed_binding = options.request_indexed_binding;
  inputs.effect_dag_passes = options.effect_dag_passes;
  inputs.effect_dag_dependencies = options.effect_dag_dependencies;
  inputs.representation_requests = options.representation_requests;
  if (options.representation_requests != nullptr && options.facet_pool == nullptr) {
    if (error) *error = "representation fixture assembly requires the submitting DeviceHAL FacetPool";
    return false;
  }
  inputs.facet_pool = options.facet_pool;
  inputs.discovery_seeds = options.discovery_seeds;
  inputs.working_set_budget = options.working_set_budget;
  inputs.working_set_lease = options.working_set_lease;
  inputs.pending_overflow = options.pending_overflow;
  return core::ExecutionPlanAssembler::assemble(inputs, out, error);
}

}  // namespace vg::test_support

#endif
