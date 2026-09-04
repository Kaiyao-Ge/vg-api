#include "fixture.h"

namespace vg::tests::execution_plan {

void test_effect_conflicts_are_deterministic_or_reject_reverse_cycle() {
  // Stage 3: RAW and WAW aliases without explicit HB get one deterministic
  // inferred edge. A reverse explicit edge forms a cycle and is rejected.
  for (const auto second_access : {vg::ir::Access::Read, vg::ir::Access::Write}) {
    vg::core::TaskGraphBuilder builder;
    CHECK(builder.append(task(10, 91)));
    CHECK(builder.append(task(11, 91)));
    CHECK(builder.add_effect(0, {91, 0, 16, vg::ir::Access::Write, 0}));
    CHECK(builder.add_effect(1, {91, 0, 16, second_access, 0}));
    vg::core::TaskGraph graph;
    CHECK(builder.seal(&graph));
    CHECK(graph.dependencies().size() == 1);
    CHECK((graph.dependencies()[0] == std::pair<uint32_t, uint32_t>{0, 1}));
    std::vector<uint32_t> order;
    CHECK(graph.deterministic_order(&order));
    CHECK((order == std::vector<uint32_t>{0, 1}));
  }

  vg::core::TaskGraphBuilder cyclic;
  CHECK(cyclic.append(task(10, 91)));
  CHECK(cyclic.append(task(11, 91)));
  CHECK(cyclic.add_dependency(1, 0));
  CHECK(cyclic.add_effect(0, {91, 0, 16, vg::ir::Access::Write, 0}));
  CHECK(cyclic.add_effect(1, {91, 0, 16, vg::ir::Access::Write, 0}));
  vg::core::TaskGraph ignored;
  std::string error;
  CHECK(!cyclic.seal(&ignored, &error));
  CHECK(error == "task graph dependency cycle");
}

void test_execution_plan_assembler_sound_counterexamples() {
  vg::core::Arena arena;
  const auto& first = arena.allocate(32);
  const auto& second = arena.allocate(32);
  const vg::core::PointerRef first_ref{first.id, first.generation};
  const vg::core::PointerRef second_ref{second.id, second.generation};
  vg::core::NodeTable nodes;
  const auto first_node = nodes.create(canonical_code_object(first.id), "first");
  const auto second_node = nodes.create(canonical_code_object(second.id), "second");
  auto first_task = task(first_node.index, first.id);
  first_task.node_generation = first_node.generation;
  auto second_task = task(second_node.index, second.id);
  second_task.node_generation = second_node.generation;
  const auto graph = published_graph({first_task, second_task});
  vg::core::Certificate certificate{{{first.id, 0, 4, vg::ir::Access::Write, 0},
                                     {second.id, 0, 4, vg::ir::Access::Write, 0}}};
  vg::core::ExecutionEnvelope envelope;
  envelope.allowed_nodes = {first_node};  // Task 2 is deliberately unauthorized.
  vg::core::ExecutionPlanAssemblerInputs inputs{&graph, &nodes, &envelope, &arena, &certificate};
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(error.find("authorize") != std::string::npos);

  // Restoring complete NodeRef authority but providing a lease that omits
  // task 2's root must still refuse; certificate cannot certify itself.
  envelope.allowed_nodes = {first_node, second_node};
  envelope.has_certificate_mode = true;
  envelope.certificate_mode = vg::core::AccessCertificateMode::CertifiedPinned;
  envelope.certificate_touched = {first_ref};
  vg::core::AccessCertificate incomplete_lease;
  CHECK(vg::core::build_access_certificate(arena, vg::core::AccessCertificateMode::CertifiedPinned,
                                           {first_ref}, &incomplete_lease, &error));
  inputs.access_certificate = &incomplete_lease;
  CHECK(!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(error == "execution envelope certificate_touched does not authorize every task access");

  // There is no caller-provided resolved-node list; the assembler creates one
  // complete snapshot and validation rejects any later missing/duplicate edit.
  const auto duplicate_graph = published_graph({first_task});
  inputs.task_graph = &duplicate_graph;
  envelope.has_certificate_mode = false;
  envelope.certificate_touched.clear();
  inputs.access_certificate = nullptr;
  CHECK(vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(plan.resolved_nodes.size() == 1);
  CHECK(plan.resolved_nodes[0].ref.index == first_node.index);
  CHECK(plan.task_order == std::vector<uint32_t>({0}));
  CHECK(plan.touched_allocations.size() == 1);
  CHECK((plan.required_capabilities == std::vector<vg::core::CapabilityRequirement>{
      vg::core::CapabilityRequirement::LinearAddress,
      vg::core::CapabilityRequirement::TaskPublication,
      vg::core::CapabilityRequirement::CheckedFacetGeneration}));
  auto missing_snapshot = plan;
  missing_snapshot.resolved_nodes.clear();
  CHECK(!missing_snapshot.validate(&error));
  CHECK(error == "assembled execution plan has a duplicate or missing resolved task node");
  auto duplicate_snapshot = plan;
  duplicate_snapshot.resolved_nodes.push_back(plan.resolved_nodes[0]);
  CHECK(!duplicate_snapshot.validate(&error));
  CHECK(error == "assembled execution plan has a duplicate or missing resolved task node");

  // A stale canonical hash is rejected before it can enter the single plan.
  auto tampered = std::make_shared<vg::core::CodeObject>();
  tampered->module = canonical_module(first.id);
  tampered->module->hash = "tampered";
  const auto tampered_node = nodes.create(tampered, "tampered");
  auto tampered_task = task(tampered_node.index, first.id);
  tampered_task.node_generation = tampered_node.generation;
  const auto tampered_graph = published_graph({tampered_task});
  inputs.task_graph = &tampered_graph;
  envelope.allowed_nodes = {tampered_node};
  CHECK(!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(error == "materialized module canonical hash does not match its serialized module");

  // The sealed graph and per-Task effects are now the only executable
  // dependency facts Stage 6 sees.
  inputs.task_graph = &duplicate_graph;
  inputs.nodes = &nodes;
  envelope.allowed_nodes = {first_node, second_node};
  CHECK(vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(plan.task_effects.size() == 1);
  CHECK(plan.validated_effect_graph_shape == vg::core::EffectGraphShape::LinearChain);
  CHECK((plan.required_capabilities == std::vector<vg::core::CapabilityRequirement>{
      vg::core::CapabilityRequirement::LinearAddress,
      vg::core::CapabilityRequirement::TaskPublication,
      vg::core::CapabilityRequirement::CheckedFacetGeneration}));
}

void test_validated_effect_graph_and_full_noderef_packages_are_sealed() {
  vg::core::Arena arena;
  const auto& first = arena.allocate(16);
  const auto& second = arena.allocate(16);
  std::vector<vg::ir::Module> modules{canonical_module(first.id), canonical_module(second.id)};
  std::vector<vg::core::TaskRecord> tasks{
      vg::test_support::compute_task(first.id, first.generation),
      vg::test_support::compute_task(second.id, second.generation)};
  vg::test_support::MultiNodePlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(vg::test_support::assemble_multi_node_plan(arena, modules, tasks, {}, &fixture, &plan, &error));
  CHECK(plan.validated_effect_graph_shape == vg::core::EffectGraphShape::IndependentBranches);
  CHECK(plan.resolved_nodes.size() == 2);
  CHECK(plan.task_effects.size() == 2);

  auto effect_tampered = plan;
  ++effect_tampered.task_effects[0][0].offset;
  CHECK(!effect_tampered.validate(&error));
  CHECK(error == "sealed per-Task effects disagree with the immutable Node program");

  auto shape_tampered = plan;
  shape_tampered.validated_effect_graph_shape = vg::core::EffectGraphShape::LinearChain;
  CHECK(!shape_tampered.validate(&error));
  CHECK(error == "validated EffectGraph shape was tampered after semantic assembly");

  auto cycle_tampered = plan;
  CHECK(cycle_tampered.validated_effect_graph.add_edge(0, 1));
  CHECK(cycle_tampered.validated_effect_graph.add_edge(1, 0));
  CHECK(!cycle_tampered.validate(&error));

  auto extra_edge_tampered = plan;
  CHECK(extra_edge_tampered.validated_effect_graph.add_edge(0, 1));
  extra_edge_tampered.validated_effect_graph_shape =
      vg::core::EffectGraphShape::LinearChain;
  CHECK(!extra_edge_tampered.validate(&error));
  CHECK(error == "validated EffectGraph disagrees with canonical TaskGraph/effect derivation");

  auto out_of_range_order = plan;
  out_of_range_order.task_order = {0, 2};
  CHECK(!out_of_range_order.validate(&error));
  CHECK(error == "assembled execution plan task order is out of range or contains a duplicate");
  auto duplicate_order = plan;
  duplicate_order.task_order = {0, 0};
  CHECK(!duplicate_order.validate(&error));
  CHECK(error == "assembled execution plan task order is out of range or contains a duplicate");

  auto module_drift = plan;
  module_drift.resolved_nodes[0].module->root_schema = "vg.test.drift/v1";
  module_drift.resolved_nodes[0].module->canonical_json =
      vg::ir::serialize_module(*module_drift.resolved_nodes[0].module);
  module_drift.resolved_nodes[0].module->hash =
      vg::ir::sha256_hex(module_drift.resolved_nodes[0].module->canonical_json);
  CHECK(!module_drift.validate(&error));
  CHECK(error == "resolved Node module drifted from its immutable CodeObject snapshot");

  auto extra_resolved_node = plan;
  auto unattached = plan.resolved_nodes[0];
  unattached.ref = {999, 1};
  extra_resolved_node.resolved_nodes.push_back(std::move(unattached));
  CHECK(!extra_resolved_node.validate(&error));
  CHECK(error == "assembled execution plan contains a resolved Node with no Task");

  auto missing_effect_dag_requirement = plan;
  const auto effect_dag = std::ranges::find(
      missing_effect_dag_requirement.required_capabilities,
      vg::core::CapabilityRequirement::EffectDag);
  CHECK(effect_dag != missing_effect_dag_requirement.required_capabilities.end());
  missing_effect_dag_requirement.required_capabilities.erase(effect_dag);
  CHECK(!missing_effect_dag_requirement.validate(&error));
  CHECK(error == "sealed capability requirements disagree with the execution semantics");

  // With no caller dependency, actual WAW effects still create the one
  // deterministic semantic ordering edge in the assembler-sealed graph.
  vg::test_support::MultiNodePlanFixture hazard_fixture;
  vg::core::ExecutionPlan hazard_plan;
  std::vector<vg::ir::Module> hazards{canonical_module(first.id), canonical_module(first.id)};
  CHECK(vg::test_support::assemble_multi_node_plan(arena, hazards,
      {vg::test_support::compute_task(first.id, first.generation),
       vg::test_support::compute_task(first.id, first.generation)},
      {}, &hazard_fixture, &hazard_plan, &error));
  CHECK(hazard_plan.validated_effect_graph_shape == vg::core::EffectGraphShape::LinearChain);
  CHECK(hazard_plan.validated_effect_graph.edges().size() == 1);
  CHECK(hazard_plan.task_order == std::vector<uint32_t>({0, 1}));

  auto device = vg::reference::make_device_hal();
  vg::hal::CompiledPlan compiled;
  CHECK(device->compile(plan, &compiled, &error));
  CHECK(compiled.per_node_packages.size() == plan.resolved_nodes.size());
  for (const auto& node : plan.resolved_nodes) {
    CHECK(std::count_if(compiled.per_node_packages.begin(), compiled.per_node_packages.end(),
                        [&](const auto& package) {
      return package.ref.index == node.ref.index && package.ref.generation == node.ref.generation;
    }) == 1);
  }
  CHECK(std::count_if(compiled.report.events.begin(), compiled.report.events.end(),
                      [](const auto& event) {
    return event.operation == "node_compute_package" &&
           event.classification == vg::hal::LoweringClass::Direct;
  }) == static_cast<std::ptrdiff_t>(plan.resolved_nodes.size()));

  // Package identity is admitted before the execution loop.  Tampering Task
  // 1's package must therefore leave Task 0's destination untouched.
  auto hash_tampered_package = compiled;
  hash_tampered_package.per_node_packages[1].package->canonical_ir_hash = "tampered";
  vg::hal::Submission rejected_submission;
  CHECK(!device->submit(hash_tampered_package, arena, &rejected_submission, &error));
  CHECK(error == "compiled per-Node package disagrees with the resolved immutable module");
  CHECK(std::all_of(first.bytes.begin(), first.bytes.end(), [](uint8_t byte) { return byte == 0; }));
  CHECK(std::all_of(second.bytes.begin(), second.bytes.end(), [](uint8_t byte) { return byte == 0; }));
  auto schema_tampered_package = compiled;
  schema_tampered_package.per_node_packages[1].package->root_schema = "vg.tampered/v1";
  CHECK(!device->submit(schema_tampered_package, arena, &rejected_submission, &error));
  CHECK(error == "compiled per-Node package disagrees with the resolved immutable module");
  CHECK(std::all_of(first.bytes.begin(), first.bytes.end(), [](uint8_t byte) { return byte == 0; }));
  CHECK(std::all_of(second.bytes.begin(), second.bytes.end(), [](uint8_t byte) { return byte == 0; }));
  vg::hal::Submission submission;
  CHECK(device->submit(compiled, arena, &submission, &error));
  CHECK(submission.result.ok);
  CHECK(submission.result.trace.size() == 2);
  CHECK(submission.result.witness.entries().size() == 2);

  vg::hal::CompiledPlan repeated_compiled;
  CHECK(device->compile(hazard_plan, &repeated_compiled, &error));
  vg::hal::Submission repeated_submission;
  CHECK(device->submit(repeated_compiled, arena, &repeated_submission, &error));
  CHECK(repeated_submission.result.ok);
  CHECK(repeated_submission.result.trace.size() == 2);
  CHECK(repeated_submission.result.witness.entries().size() == 2);

  vg::hal::CompiledPlan reused_compiled;
  CHECK(device->compile(plan, &reused_compiled, &error));
  CHECK(reused_compiled.per_node_packages.size() == 2);
  CHECK(device->compile(hazard_plan, &reused_compiled, &error));
  CHECK(reused_compiled.per_node_packages.size() == 2);
  auto validation_failure = hazard_plan;
  validation_failure.task_order = {0, 9};
  CHECK(!device->compile(validation_failure, &reused_compiled, &error));
  CHECK(reused_compiled.per_node_packages.empty());
  CHECK(reused_compiled.representation_operations.empty());
  CHECK(!reused_compiled.report.supported);

  auto wrong_generation = compiled;
  ++wrong_generation.per_node_packages[0].ref.generation;
  CHECK(!device->submit(wrong_generation, arena, &submission, &error));
  CHECK(error == "compiled plan contains a duplicate or missing NodeRef package");
  auto duplicate_package = compiled;
  duplicate_package.per_node_packages.push_back(compiled.per_node_packages[0]);
  CHECK(!device->submit(duplicate_package, arena, &submission, &error));
  CHECK(error == "compiled plan does not contain exactly one package per resolved NodeRef");
}

}  // namespace vg::tests::execution_plan
