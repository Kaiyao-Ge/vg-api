#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"

#include "ir/sha256.h"
#include "vg_scene_root_layout.h"
#include "../support/assembled_plan_fixture.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

[[noreturn]] void check_failed(const char* expression, const char* file, int line) {
  std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
  std::exit(EXIT_FAILURE);
}

#define CHECK(condition) \
  do { \
    if (!(condition)) check_failed(#condition, __FILE__, __LINE__); \
  } while (false)

using vg::hal::Capability;

vg::hal::CapabilitySnapshot fully_capable_reference_snapshot() {
  vg::hal::CapabilitySnapshot capabilities;
  capabilities.backend = vg::hal::BackendKind::Reference;
  capabilities.capability_bits = static_cast<uint64_t>(Capability::LinearAddress) |
                                  static_cast<uint64_t>(Capability::TaskPublication) |
                                  static_cast<uint64_t>(Capability::Timeline) |
                                  static_cast<uint64_t>(Capability::EffectDag) |
                                  static_cast<uint64_t>(Capability::CaptureReplay) |
                                  static_cast<uint64_t>(Capability::IndirectTier1) |
                                  static_cast<uint64_t>(Capability::Raster) |
                                  static_cast<uint64_t>(Capability::RepresentationTransform) |
                                  static_cast<uint64_t>(Capability::CheckedFacetGeneration) |
                                  static_cast<uint64_t>(Capability::UserShaderImport) |
                                  static_cast<uint64_t>(Capability::IndirectTier2Select) |
                                  static_cast<uint64_t>(Capability::IndexedBinding);
  capabilities.validation_available = true;
  return capabilities;
}

void expect_missing_capability(vg::core::ExecutionPlan plan, Capability missing,
                               const char* capability_name) {
  auto capabilities = fully_capable_reference_snapshot();
  capabilities.capability_bits &= ~static_cast<uint64_t>(missing);
  vg::hal::CompiledPlan compiled;
  std::string error;
  CHECK(!vg::hal::preflight_stage6(plan, capabilities, vg::hal::BackendKind::Reference,
                                   &compiled, &error));
  CHECK(error.find(capability_name) != std::string::npos);
  CHECK(!compiled.report.supported);
  CHECK(compiled.report.count(vg::hal::LoweringClass::Unsupported) >= 1);
}

vg::core::ExecutionPlan sealed_requirement_plan(std::initializer_list<vg::core::CapabilityRequirement> requirements) {
  vg::core::ExecutionPlan plan;
  plan.assembled = true;
  plan.capability_requirements_derived = true;
  plan.required_capabilities.assign(requirements);
  return plan;
}

void test_stage6_capability_preflight_rejects_without_weakening() {
  const auto basic = sealed_requirement_plan({vg::core::CapabilityRequirement::LinearAddress});
  expect_missing_capability(basic, Capability::LinearAddress, "LinearAddress");

  const auto indexed = sealed_requirement_plan({vg::core::CapabilityRequirement::IndexedBinding});
  expect_missing_capability(indexed, Capability::IndexedBinding, "IndexedBinding");

  const auto timeline = sealed_requirement_plan({vg::core::CapabilityRequirement::Timeline});
  expect_missing_capability(timeline, Capability::Timeline, "Timeline");

  const auto publication = sealed_requirement_plan({vg::core::CapabilityRequirement::TaskPublication});
  expect_missing_capability(publication, Capability::TaskPublication, "TaskPublication");

  const auto effect_dag = sealed_requirement_plan({vg::core::CapabilityRequirement::EffectDag});
  expect_missing_capability(effect_dag, Capability::EffectDag, "EffectDag");

  const auto representation = sealed_requirement_plan({vg::core::CapabilityRequirement::RepresentationTransform});
  expect_missing_capability(representation, Capability::RepresentationTransform,
                            "RepresentationTransform");

  const auto checked = sealed_requirement_plan({vg::core::CapabilityRequirement::CheckedFacetGeneration});
  expect_missing_capability(checked, Capability::CheckedFacetGeneration,
                            "CheckedFacetGeneration");

  const auto raster = sealed_requirement_plan({vg::core::CapabilityRequirement::Raster});
  expect_missing_capability(raster, Capability::Raster, "Raster");

  const auto user_shader = sealed_requirement_plan({vg::core::CapabilityRequirement::UserShaderImport});
  expect_missing_capability(user_shader, Capability::UserShaderImport, "UserShaderImport");

  const auto tier1 = sealed_requirement_plan({vg::core::CapabilityRequirement::IndirectTier1});
  expect_missing_capability(tier1, Capability::IndirectTier1, "IndirectTier1");

  const auto tier2 = sealed_requirement_plan({vg::core::CapabilityRequirement::IndirectTier2Select});
  expect_missing_capability(tier2, Capability::IndirectTier2Select, "IndirectTier2Select");

}

void test_validation_profile_matrix_and_reset() {
  auto plan = sealed_requirement_plan({});
  auto capabilities = fully_capable_reference_snapshot();
  vg::hal::CompiledPlan compiled;
  std::string error;

  for (const auto profile : {vg::core::ValidationProfile::FastNative,
                             vg::core::ValidationProfile::CheckedNative,
                             vg::core::ValidationProfile::ReferenceStrict,
                             vg::core::ValidationProfile::Capture}) {
    plan.required_capabilities.clear();
    if (profile == vg::core::ValidationProfile::CheckedNative)
      plan.required_capabilities = {vg::core::CapabilityRequirement::CheckedFacetGeneration};
    if (profile == vg::core::ValidationProfile::ReferenceStrict)
      plan.required_capabilities = {vg::core::CapabilityRequirement::ReferenceStrict};
    if (profile == vg::core::ValidationProfile::Capture)
      plan.required_capabilities = {vg::core::CapabilityRequirement::CaptureReplay};
    compiled.per_node_packages.emplace_back();
    compiled.representation_operations.push_back({});
    compiled.transition_operations.emplace_back();
    compiled.representation_operation_execution_order.push_back(0);
    CHECK(!vg::hal::preflight_stage6(plan, capabilities,
                                     vg::hal::BackendKind::Reference,
                                     &compiled, &error));
    CHECK(error.find("Stage6 requires a valid immutable core plan") !=
          std::string::npos);
    CHECK(compiled.per_node_packages.empty());
    CHECK(compiled.representation_operations.empty());
    CHECK(compiled.transition_operations.empty());
    CHECK(compiled.representation_operation_execution_order.empty());
  }

  plan.required_capabilities = {vg::core::CapabilityRequirement::ReferenceStrict};
  auto metal = capabilities;
  metal.backend = vg::hal::BackendKind::Metal;
  CHECK(!vg::hal::preflight_stage6(plan, metal, vg::hal::BackendKind::Metal, &compiled, &error));
  CHECK(error.find("ReferenceStrict") != std::string::npos);

  plan.required_capabilities = {vg::core::CapabilityRequirement::CheckedFacetGeneration};
  capabilities.validation_available = false;
  CHECK(!vg::hal::preflight_stage6(plan, capabilities, vg::hal::BackendKind::Reference,
                                   &compiled, &error));
  CHECK(error.find("CheckedNative") != std::string::npos);

  capabilities = fully_capable_reference_snapshot();
  plan.required_capabilities = {vg::core::CapabilityRequirement::CaptureReplay};
  capabilities.capability_bits &= ~static_cast<uint64_t>(Capability::CaptureReplay);
  compiled.per_node_packages.emplace_back();
  compiled.representation_operations.push_back({});
  compiled.transition_operations.emplace_back();
  compiled.representation_operation_execution_order.push_back(0);
  CHECK(!vg::hal::preflight_stage6(plan, capabilities, vg::hal::BackendKind::Reference,
                                   &compiled, &error));
  CHECK(compiled.per_node_packages.empty());
  CHECK(compiled.representation_operations.empty());
  CHECK(compiled.transition_operations.empty());
  CHECK(compiled.representation_operation_execution_order.empty());
}

vg::core::TaskRecord task(uint32_t node, uint64_t root) {
  vg::core::TaskRecord value;
  value.node_index = node;
  value.node_generation = 1;
  value.root_allocation = root;
  value.root_generation = 1;
  return value;
}

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

void test_certificate_and_access_witness_reject_partial_coverage() {
  // Stage 4: same Node, two Tasks, two distinct roots. Covering task 0's
  // root must not silently authorize task 1's root.
  const vg::ir::Effect first{101, 0, 16, vg::ir::Access::Write, 0};
  const vg::ir::Effect second{202, 0, 16, vg::ir::Access::Read, 0};
  const vg::core::Certificate certificate{{first}};
  std::string error;
  CHECK(!vg::core::validate_certificate(certificate, {first, second}, &error));
  CHECK(error == "certificate does not cover inferred effect");

  vg::core::Arena arena;
  const auto& first_allocation = arena.allocate(32);
  const auto& second_allocation = arena.allocate(32);
  const vg::core::PointerRef first_ref{first_allocation.id, first_allocation.generation};
  const vg::core::PointerRef second_ref{second_allocation.id, second_allocation.generation};

  vg::core::AccessCertificate pinned;
  CHECK(vg::core::build_access_certificate(arena, vg::core::AccessCertificateMode::CertifiedPinned,
                                           {first_ref}, &pinned, &error));
  CHECK(!vg::core::certificate_covers_discovery_witness(pinned, {first_ref, second_ref}, &error));
  CHECK(error == "discovery witness is not covered by the certificate");

  // Current DiscoverThenLease is an arena scan. Its resulting lease must
  // nevertheless cover the entire witness before it can be accepted.
  vg::core::AccessCertificate discovered;
  CHECK(vg::core::build_access_certificate(arena, vg::core::AccessCertificateMode::DiscoverThenLease,
                                           {first_ref}, &discovered, &error));
  CHECK(vg::core::certificate_covers_discovery_witness(discovered, {first_ref, second_ref}, &error));

  for (const auto mode : {vg::core::AccessCertificateMode::SoftwarePaged,
                          vg::core::AccessCertificateMode::FaultManaged}) {
    vg::core::AccessCertificate unsupported;
    CHECK(!vg::core::build_access_certificate(arena, mode, {first_ref}, &unsupported, &error));
    CHECK(error.find("no implementation") != std::string::npos);
  }
}

void test_bounded_pointer_graph_canonical_identity() {
  // ADR-028's load_ref/store_via is a bounded graph: PointerEdge resolves
  // the dereference target statically.  This fixture only protects the
  // module's canonical serialization/hash identity before assembly.
  vg::ir::Module module;
  module.root_schema = "vg.root/v1";
  module.instructions = {{"load_ref", 301, 0, 12, 0, 1, 0, 0, ""},
                         {"store_via", 302, 0, 4, 7, 1, 0, 1, ""}};
  module.declared_effects = {{301, 0, 12, vg::ir::Access::Read, 0}};
  module.declared_pointer_edges = {{301, 0, 302}};
  module.canonical_json = vg::ir::serialize_module(module);
  module.hash = vg::ir::sha256_hex(module.canonical_json);
  CHECK(vg::ir::verify(module).ok);

  module.hash = "tampered";
  CHECK(vg::ir::sha256_hex(vg::ir::serialize_module(module)) != module.hash);
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

void test_reference_multi_node_runtime_pointer_fault_preserves_prefix() {
  vg::core::Arena arena;
  auto& first_output = arena.allocate(16);
  auto& pointer_root = arena.allocate(16);
  auto& pointer_target = arena.allocate(16);
  const auto target_ref = vg::core::PointerRef{pointer_target.id, pointer_target.generation};
  std::memcpy(pointer_root.bytes.data(), &target_ref.allocation, sizeof(target_ref.allocation));
  std::memcpy(pointer_root.bytes.data() + sizeof(target_ref.allocation), &target_ref.generation,
              sizeof(target_ref.generation));

  auto pointer_module = canonical_module(pointer_root.id);
  pointer_module.instructions = {{"load_ref", pointer_root.id, 0, 12, 0, pointer_root.generation, 0, 0, ""},
                                 {"store_via", pointer_target.id, 0, 4, 9, pointer_target.generation, 0, 1, ""}};
  pointer_module.declared_effects = {{pointer_root.id, 0, 12, vg::ir::Access::Read, 0}};
  pointer_module.declared_pointer_edges = {{pointer_root.id, 0, pointer_target.id}};
  pointer_module.canonical_json = vg::ir::serialize_module(pointer_module);
  pointer_module.hash = vg::ir::sha256_hex(pointer_module.canonical_json);

  vg::test_support::MultiNodePlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(vg::test_support::assemble_multi_node_plan(
      arena, {canonical_module(first_output.id), pointer_module},
      {vg::test_support::compute_task(first_output.id, first_output.generation),
       vg::test_support::compute_task(pointer_root.id, pointer_root.generation)},
      {{0, 1}}, &fixture, &plan, &error));
  auto device = vg::reference::make_device_hal();
  vg::hal::CompiledPlan compiled;
  CHECK(device->compile(plan, &compiled, &error));

  const uint32_t stale_generation = target_ref.generation + 1;
  std::memcpy(pointer_root.bytes.data() + sizeof(target_ref.allocation), &stale_generation,
              sizeof(stale_generation));
  vg::hal::Submission submission;
  CHECK(device->submit(compiled, arena, &submission, &error));
  CHECK(!submission.result.ok);
  CHECK(!submission.result.outputs_valid);
  CHECK(submission.result.poison == vg::core::PoisonState::PartiallyProduced);
  CHECK(submission.result.fault.task_index == 1);
  CHECK(!submission.result.fault.code.empty());
  CHECK(!submission.result.fault.message.empty());
  // The merged prefix retains Task 0's successful store as well as both
  // accesses observed by Task 1 before store_via rejects the stale ref.
  const auto matches_effect = [](const vg::ir::Effect& effect, uint64_t allocation,
                                 uint64_t offset, uint64_t size, vg::ir::Access access) {
    return effect.allocation == allocation && effect.offset == offset && effect.size == size &&
           effect.access == access && effect.representation_epoch == 0;
  };
  CHECK(submission.result.trace.size() == 3);
  CHECK(matches_effect(submission.result.trace[0], first_output.id, 0, 4, vg::ir::Access::Write));
  CHECK(matches_effect(submission.result.trace[1], pointer_root.id, 0, 12, vg::ir::Access::Read));
  CHECK(matches_effect(submission.result.trace[2], pointer_target.id, 0, 4, vg::ir::Access::Write));
  const auto& witness = submission.result.witness.entries();
  CHECK(witness.size() == 3);
  CHECK(witness[0].instruction_index == 0 &&
        matches_effect(witness[0].effect, first_output.id, 0, 4, vg::ir::Access::Write));
  CHECK(witness[1].instruction_index == 0 &&
        matches_effect(witness[1].effect, pointer_root.id, 0, 12, vg::ir::Access::Read));
  CHECK(witness[2].instruction_index == 1 &&
        matches_effect(witness[2].effect, pointer_target.id, 0, 4, vg::ir::Access::Write));
  CHECK(first_output.bytes[0] == 7);
  CHECK(std::all_of(pointer_target.bytes.begin(), pointer_target.bytes.end(),
                    [](uint8_t byte) { return byte == 0; }));
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

void test_execution_plan_assembler_bounded_pointer_graph_access() {
  vg::core::Arena arena;
  const auto& root = arena.allocate(32);
  const auto& target = arena.allocate(32);
  const vg::core::PointerRef root_ref{root.id, root.generation};
  const vg::core::PointerRef target_ref{target.id, target.generation};
  auto object = std::make_shared<vg::core::CodeObject>();
  object->module = canonical_module(root.id);
  object->module->instructions = {{"load_ref", root.id, 0, 12, 0, root.generation, 0, 0, ""},
                                  {"store_via", target.id, 0, 4, 7, target.generation, 0, 1, ""}};
  object->module->declared_effects = {{root.id, 0, 12, vg::ir::Access::Read, 0}};
  object->module->declared_pointer_edges = {{root.id, 0, target.id}};
  object->module->canonical_json = vg::ir::serialize_module(*object->module);
  object->module->hash = vg::ir::sha256_hex(object->module->canonical_json);
  vg::core::NodeTable nodes;
  const auto node = nodes.create(object, "bounded-pointer-graph");
  auto bounded_task = task(node.index, root.id);
  bounded_task.node_generation = node.generation;
  const auto graph = published_graph({bounded_task});
  vg::core::Certificate certificate{{{root.id, 0, 12, vg::ir::Access::Read, 0},
                                     {target.id, 0, 4, vg::ir::Access::Write, 0}}};
  vg::core::ExecutionEnvelope envelope;
  envelope.allowed_nodes = {node};
  envelope.has_certificate_mode = true;
  envelope.certificate_mode = vg::core::AccessCertificateMode::CertifiedPinned;
  envelope.certificate_touched = {root_ref};
  vg::core::AccessCertificate pinned;
  std::string error;
  CHECK(vg::core::build_access_certificate(arena, envelope.certificate_mode, {root_ref}, &pinned, &error));
  vg::core::ExecutionPlanAssemblerInputs inputs{&graph, &nodes, &envelope, &arena, &certificate, &pinned};
  vg::core::ExecutionPlan plan;
  CHECK(!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(error == "execution envelope certificate_touched does not authorize every task access");

  // The target is a finite, verifier-proven PointerEdge target.  It needs
  // CertifiedPinned authority but not a discovery witness.
  envelope.certificate_touched = {root_ref, target_ref};
  CHECK(vg::core::build_access_certificate(arena, envelope.certificate_mode,
                                           envelope.certificate_touched, &pinned, &error));
  CHECK(vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(plan.assembled);
  CHECK(plan.access_certificate.has_value());
  CHECK(plan.touched_allocations.size() == 2);

  // The public E002 path is certificate-mode-free: the same bounded graph
  // remains finite and assembles without inventing a Universe fallback.
  envelope.has_certificate_mode = false;
  envelope.certificate_touched.clear();
  inputs.access_certificate = nullptr;
  CHECK(vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(plan.touched_allocations.size() == 2);

  // Reference owns the same NodeRef-keyed pointer-graph package shape as
  // Metal's canonical path; it must not fail through the linear builder or
  // construct a second certificate-effect fact at submit time.
  auto* root_allocation = arena.lookup(root_ref);
  std::memcpy(root_allocation->bytes.data(), &target_ref.allocation,
              sizeof(target_ref.allocation));
  std::memcpy(root_allocation->bytes.data() + sizeof(target_ref.allocation),
              &target_ref.generation, sizeof(target_ref.generation));
  auto reference = vg::reference::make_device_hal();
  vg::hal::CompiledPlan compiled;
  CHECK(reference->compile(plan, &compiled, &error));
  CHECK(compiled.per_node_packages.size() == 1);
  CHECK(compiled.per_node_packages[0].package.has_value());
  vg::hal::Submission submission;
  CHECK(reference->submit(compiled, arena, &submission, &error));
  CHECK(submission.result.ok);
  CHECK(arena.lookup(target_ref)->bytes[0] == 7);
  CHECK(submission.result.trace.size() == plan.task_effects[0].size());
  CHECK(std::equal(submission.result.trace.begin(), submission.result.trace.end(),
                   plan.task_effects[0].begin(), [](const auto& left, const auto& right) {
    return left.allocation == right.allocation && left.offset == right.offset &&
           left.size == right.size && left.access == right.access &&
           left.representation_epoch == right.representation_epoch;
  }));

  // An unproved dereference is not downgraded to a dynamic graph.  It is
  // rejected by canonical IR verification before assembly can name a target.
  object->module->declared_pointer_edges.clear();
  object->module->canonical_json = vg::ir::serialize_module(*object->module);
  object->module->hash = vg::ir::sha256_hex(object->module->canonical_json);
  CHECK(!vg::ir::verify(*object->module).ok);
  CHECK(!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(error == "materialized module failed canonical IR verification: declared pointer edges do not cover this dereference");
}

void test_execution_plan_assembler_seals_access_planning() {
  vg::core::Arena arena;
  const auto& root = arena.allocate(32);
  const auto& extra = arena.allocate(32);
  const vg::core::PointerRef root_ref{root.id, root.generation};
  const vg::core::PointerRef extra_ref{extra.id, extra.generation};
  vg::core::NodeTable nodes;
  const auto node = nodes.create(canonical_code_object(root.id), "access-plan");
  auto work = task(node.index, root.id);
  work.node_generation = node.generation;
  const auto graph = published_graph({work});
  vg::core::ExecutionEnvelope envelope;
  envelope.allowed_nodes = {node};
  envelope.has_certificate_mode = true;
  envelope.certificate_mode = vg::core::AccessCertificateMode::CertifiedPinned;
  envelope.certificate_touched = {root_ref};
  vg::core::ExecutionPlanAssemblerInputs inputs{&graph, &nodes, &envelope, &arena};
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(plan.access_plan_derived);
  CHECK(plan.access_certificate.has_value());
  CHECK(plan.access_certificate->epoch.references().size() == 1);

  // A supplied CertifiedPinned certificate may be narrower than Envelope
  // authority, but it cannot smuggle another live allocation into it.
  vg::core::AccessCertificate expanded;
  CHECK(vg::core::build_access_certificate(arena, vg::core::AccessCertificateMode::CertifiedPinned,
                                           {root_ref, extra_ref}, &expanded, &error));
  inputs.access_certificate = &expanded;
  CHECK(!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(error == "CertifiedPinned access certificate exceeds execution envelope authority");
  inputs.access_certificate = nullptr;
  CHECK(vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));

  // Stage 7 receives the sealed certificate and must not recreate it from a
  // backend module projection.
  vg::hal::Submission submission;
  CHECK(vg::hal::run_discovery_stage(plan, arena, &submission, &error));
  CHECK(submission.access_certificate.has_value());
  CHECK(submission.access_certificate->epoch.references().size() ==
        plan.access_certificate->epoch.references().size());
  CHECK(submission.access_certificate->epoch.references().front().allocation ==
        plan.access_certificate->epoch.references().front().allocation);
  CHECK(!vg::hal::run_discovery_stage(plan, arena, nullptr, &error));
  CHECK(error == "submission output is required");

  // A stale assembly epoch fails before discovery / lowering can start.
  inputs.graph_epoch = arena.topology_epoch() - 1;
  CHECK(!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(error.find("graph epoch") != std::string::npos);
  inputs.graph_epoch = arena.topology_epoch();

  // Universe pays and checks its full-Arena working-set request in core.
  envelope.certificate_mode = vg::core::AccessCertificateMode::Universe;
  envelope.certificate_touched.clear();
  const auto zero_budget = vg::core::WorkingSetBudget::limited(0);
  inputs.working_set_budget = &zero_budget;
  CHECK(!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(error == "working-set budget exceeded");
  inputs.working_set_budget = nullptr;

  // DiscoverThenLease accepts only the core's frozen seed walk.  A supplied
  // witness that adds an Active allocation is not an authority expansion.
  envelope.certificate_mode = vg::core::AccessCertificateMode::DiscoverThenLease;
  const std::vector<vg::core::PointerRef> seeds{root_ref};
  const std::vector<vg::core::PointerRef> forged_witness{root_ref, extra_ref};
  inputs.discovery_seeds = &seeds;
  inputs.discovery_witness = &forged_witness;
  CHECK(!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(error.find("disagrees") != std::string::npos);
  inputs.discovery_witness = nullptr;
  inputs.working_set_budget = &zero_budget;
  CHECK(!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(error == "working-set budget exceeded");
  inputs.working_set_budget = nullptr;
  CHECK(vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK(plan.discovery_result.has_value());
  CHECK(plan.working_set_lease.has_value() && plan.working_set_lease->complete);

  for (const auto unsupported : {vg::core::AccessCertificateMode::SoftwarePaged,
                                 vg::core::AccessCertificateMode::FaultManaged}) {
    envelope.certificate_mode = unsupported;
    CHECK(!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
    CHECK(error.find("Unsupported") != std::string::npos);
  }
}

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

vg::core::CanonicalView rgba_view(const vg::core::Allocation& allocation,
                                  uint32_t width = 1, uint32_t height = 1) {
  vg::core::CanonicalView view;
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  view.width = width;
  view.height = height;
  return view;
}

void test_submission_lifetime_hold_is_transactional_and_repeatable() {
  vg::core::Arena arena;
  const auto& root = arena.allocate(32);
  const uint64_t root_id = root.id;
  const uint32_t root_generation = root.generation;
  vg::core::FacetPool pool;
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(vg::test_support::assemble_single_node_plan(
      arena, canonical_module(root_id),
      {vg::test_support::compute_task(root_id, root_generation)}, &fixture, &plan, &error));
  CHECK(plan.lifetime_plan_derived);
  CHECK(plan.lifetime_facets.empty());
  CHECK(plan.touched_allocations.size() == 1);  // root/effect dedup

  for (int submit_index = 0; submit_index < 2; ++submit_index) {
    vg::hal::SubmissionLifetimeHold hold;
    CHECK(hold.prepare(plan, arena, pool, &error));
    CHECK(hold.acquire({}, &error));
    CHECK(hold.held());
    CHECK(hold.allocation_count() == 1);
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 1);
    uint32_t ignored_epoch = 0;
    CHECK(!arena.transform(root_id, root_generation, &ignored_epoch, &error));
    CHECK(error == "representation epoch is referenced in flight");
    CHECK(!arena.retire({root_id, root_generation}));
    CHECK(!arena.consume(root_id, root_generation, 0, {true, true, true, true}, &error));
    CHECK(error == "consume requires exclusive ownership");
    hold.release();
    CHECK(!hold.held());
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);
  }

  // A future asynchronous backend may move the owner into a completion
  // object. Moving over an already-held destination releases that
  // destination first, then transfers the source without duplicating either
  // count; explicit release plus both destructors remains exactly-once.
  {
    vg::hal::SubmissionLifetimeHold source_hold;
    vg::hal::SubmissionLifetimeHold destination_hold;
    CHECK(source_hold.prepare(plan, arena, pool, &error));
    CHECK(source_hold.acquire({}, &error));
    CHECK(destination_hold.prepare(plan, arena, pool, &error));
    CHECK(destination_hold.acquire({}, &error));
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 2);
    destination_hold = std::move(source_hold);
    CHECK(!source_hold.held());
    CHECK(destination_hold.held());
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 1);
    vg::hal::SubmissionLifetimeHold completion_hold(std::move(destination_hold));
    CHECK(!destination_hold.held());
    CHECK(completion_hold.held());
    completion_hold.release();
    completion_hold.release();
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);
  }
  CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);

  // prepare() is read-only. If a later identity goes stale, acquire() may
  // already have retained earlier sorted entries; it must roll them all back.
  const auto& second = arena.allocate(32);
  const uint64_t second_id = second.id;
  const uint32_t second_generation = second.generation;
  vg::test_support::AssembledPlanFixture partial_fixture;
  vg::core::ExecutionPlan partial_plan;
  CHECK(vg::test_support::assemble_single_node_plan(
      arena, canonical_module(second_id),
      {vg::test_support::compute_task(root_id, root_generation)},
      &partial_fixture, &partial_plan, &error));
  CHECK(partial_plan.touched_allocations.size() == 2);
  vg::hal::SubmissionLifetimeHold partial;
  CHECK(partial.prepare(partial_plan, arena, pool, &error));
  CHECK(arena.retire({second_id, second_generation}));
  CHECK(!partial.acquire({}, &error));
  CHECK(error.find("stale or retired") != std::string::npos);
  CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);
  CHECK(partial.allocation_count() == 0);
  CHECK(partial.facet_count() == 0);
}

void test_submission_lifetime_hold_deduplicates_facets_and_backing() {
  vg::core::Arena arena;
  const auto& source = arena.allocate(4);
  const auto& target = arena.allocate(4);
  const auto& vertex = arena.allocate(12);  // Address view is a 4-byte prefix.
  vg::core::FacetPool pool;
  vg::core::FacetRef source_ref{};
  vg::core::FacetRef target_ref{};
  vg::core::FacetRef vertex_ref{};
  std::string error;
  CHECK(pool.acquire(arena, rgba_view(source), vg::core::FacetKind::Sample,
                     &source_ref, &error));
  CHECK(pool.acquire(arena, rgba_view(target), vg::core::FacetKind::Attachment,
                     &target_ref, &error));
  CHECK(pool.acquire(arena, rgba_view(vertex), vg::core::FacetKind::Address,
                     &vertex_ref, &error));

  vg::core::TaskRecord raster;
  raster.kind = vg::core::TaskKind::Raster;
  raster.raster_facets = {source_ref, target_ref};
  raster.vertex_buffer_ref = vertex_ref;
  const vg::ir::UserRasterShaderContract shader{
      "vg.test.raster/v1", "vertex_main", "fragment_main",
      vg::ir::kRasterVertexAbiXyzuvPackedV1, "kernel source is opaque to core"};
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions raster_options;
  raster_options.facet_pool = &pool;
  CHECK(vg::test_support::assemble_single_user_raster_plan(
      arena, shader, {raster, raster}, &fixture, &plan, &error, raster_options));
  CHECK(plan.lifetime_plan_derived);
  CHECK(plan.lifetime_facets.size() == 3);  // two Tasks, one capability set
  CHECK(plan.task_effects.size() == 2);
  CHECK(plan.task_facet_uses.size() == 2);
  CHECK(plan.task_facet_uses[0].size() == 3);
  CHECK(plan.task_facet_uses[1].size() == 3);
  CHECK(plan.task_facet_uses[0][0].ref.index == source_ref.index);
  CHECK(plan.task_facet_uses[0][0].kind == vg::core::FacetKind::Sample);
  CHECK(plan.task_facet_uses[0][0].view.allocation == source.id);
  CHECK(plan.task_facet_uses[0][1].kind == vg::core::FacetKind::Attachment);
  CHECK(plan.task_facet_uses[0][2].kind == vg::core::FacetKind::Address);
  CHECK(plan.task_effects[0].size() == 3);  // source read, target write, vertex read
  CHECK(plan.task_effects[1].size() == 3);
  CHECK(plan.task_effects[0][2].size == vertex.size);
  CHECK(plan.validated_effect_graph.edges().size() == 1);
  CHECK(plan.validated_effect_graph.edges()[0].kind ==
        vg::core::EffectEdgeKind::InferredConflict);
  CHECK(plan.task_order == std::vector<uint32_t>({0, 1}));
  CHECK(plan.execution_schedule_derived);
  CHECK(plan.execution_schedule.components.size() == 1);
  CHECK(plan.execution_schedule.components[0].waves.size() == 2);
  CHECK(plan.execution_schedule.components[0].waves[0].tasks ==
        std::vector<uint32_t>({0}));
  CHECK(plan.execution_schedule.components[0].waves[1].tasks ==
        std::vector<uint32_t>({1}));
  CHECK(plan.execution_schedule.structural_successors[0] ==
        std::vector<uint32_t>({1}));
  CHECK(plan.resolved_nodes.size() == 1);
  CHECK(plan.resolved_nodes[0].execution_domain == vg::core::TaskKind::Raster);

  auto schedule_tampered = plan;
  schedule_tampered.execution_schedule.components[0].waves[0].tasks[0] = 1;
  CHECK(!schedule_tampered.validate(&error));
  CHECK(error == "assembled execution plan semantic facts disagree with the immutable assembler seal");

  auto transition_tampered = plan;
  transition_tampered.execution_schedule.transitions.back().requires_execution_completion = false;
  CHECK(!transition_tampered.validate(&error));
  CHECK(error == "assembled execution plan semantic facts disagree with the immutable assembler seal");

  auto successor_tampered = plan;
  successor_tampered.execution_schedule.structural_successors[0].clear();
  CHECK(!successor_tampered.validate(&error));
  CHECK(error == "assembled execution plan semantic facts disagree with the immutable assembler seal");

  auto domain_tampered = plan;
  domain_tampered.resolved_nodes[0].execution_domain = vg::core::TaskKind::Compute;
  CHECK(!domain_tampered.validate(&error));
  CHECK(error == "resolved Node execution domain disagrees with its Tasks or contract");

  auto facet_tampered = plan;
  ++facet_tampered.task_facet_uses[0][0].view.width;
  CHECK(!facet_tampered.validate(&error));
  CHECK(error == "assembled execution plan semantic facts disagree with the immutable assembler seal");
  auto raster_tampered = plan;
  --raster_tampered.task_effects[0][1].size;
  --raster_tampered.task_effects[1][1].size;
  raster_tampered.instantiated_effects.clear();
  for (uint32_t task_index : raster_tampered.task_order)
    raster_tampered.instantiated_effects.insert(
        raster_tampered.instantiated_effects.end(),
        raster_tampered.task_effects[task_index].begin(),
        raster_tampered.task_effects[task_index].end());
  raster_tampered.certificate.ranges = raster_tampered.instantiated_effects;
  CHECK(!raster_tampered.validate(&error));
  CHECK(error == "assembled execution plan semantic facts disagree with the immutable assembler seal");

  // A NodeRef owns exactly one execution domain even though a complete plan
  // may now contain both domains through distinct NodeRefs.
  vg::test_support::AssembledPlanFixture mixed_fixture;
  vg::core::ExecutionPlan mixed_plan;
  auto compute = vg::test_support::compute_task(source.id, source.generation);
  vg::test_support::AssemblyOptions mixed_options;
  mixed_options.facet_pool = &pool;
  error.clear();
  CHECK(!vg::test_support::assemble_single_node_plan(
      arena, canonical_module(source.id), {compute, raster}, &mixed_fixture,
      &mixed_plan, &error, mixed_options));
  CHECK(error == "one resolved NodeRef is used by multiple execution domains");

  const auto expect_raster_assembly_rejection = [&](vg::core::TaskRecord invalid,
                                                     const char* expected) {
    vg::test_support::AssembledPlanFixture invalid_fixture;
    vg::core::ExecutionPlan invalid_plan;
    error.clear();
    CHECK(!vg::test_support::assemble_single_user_raster_plan(
        arena, shader, {invalid}, &invalid_fixture, &invalid_plan, &error,
        raster_options));
    CHECK(error == expected);
  };
  auto missing_source = raster;
  missing_source.raster_facets.source = {};
  expect_raster_assembly_rejection(missing_source, "raster source facet is required");
  auto missing_target = raster;
  missing_target.raster_facets.target = {};
  expect_raster_assembly_rejection(missing_target, "raster target facet is required");
  auto missing_vertex = raster;
  missing_vertex.vertex_buffer_ref = {};
  expect_raster_assembly_rejection(missing_vertex, "raster vertex facet is required");
  auto missing_index = raster;
  missing_index.index_count = 3;
  expect_raster_assembly_rejection(
      missing_index, "raster index facet is required when index_count is non-zero");
  auto missing_depth = raster;
  missing_depth.depth_test_enable = true;
  expect_raster_assembly_rejection(
      missing_depth, "raster depth state requires a depth attachment facet");
  vg::test_support::AssembledPlanFixture no_pool_fixture;
  vg::core::ExecutionPlan no_pool_plan;
  error.clear();
  CHECK(!vg::test_support::assemble_single_user_raster_plan(
      arena, shader, {raster}, &no_pool_fixture, &no_pool_plan, &error));
  CHECK(error ==
        "raster semantic assembly requires the submitting FacetPool to resolve raster source facet");

  // An unrelated caller may already retain the facet. The plan adds exactly
  // one independent use and returns the external count unchanged.
  CHECK(pool.begin_gpu_use(arena, source_ref, &error));
  vg::hal::SubmissionLifetimeHold hold;
  CHECK(hold.prepare(plan, arena, pool, &error));
  CHECK(hold.acquire({}, &error));
  CHECK(hold.facet_count() == 3);
  CHECK(hold.allocation_count() == 3);
  CHECK(pool.in_flight(source_ref) == 2);
  CHECK(pool.in_flight(target_ref) == 1);
  CHECK(pool.in_flight(vertex_ref) == 1);

  // Retiring a token invalidates it immediately, but its slot must not be
  // reused while this submission still owns a GPU-use count.
  CHECK(pool.retire(target_ref, &error));
  vg::core::FacetRef while_held{};
  CHECK(pool.acquire(arena, rgba_view(target), vg::core::FacetKind::Attachment,
                     &while_held, &error));
  CHECK(while_held.index != target_ref.index);
  hold.release();
  CHECK(pool.in_flight(source_ref) == 1);
  CHECK(pool.in_flight(vertex_ref) == 0);
  CHECK(pool.end_gpu_use(source_ref, &error));
  CHECK(pool.in_flight(source_ref) == 0);
  vg::hal::SubmissionLifetimeHold stale_facet_hold;
  CHECK(!stale_facet_hold.prepare(plan, arena, pool, &error));
  CHECK(error.find("retired") != std::string::npos);

  // The task's source token names the pre-transform epoch. Since a concrete
  // target token is not available until the physical operation publishes it,
  // the same submit cannot silently transform that backing and then execute
  // through the now-stale TaskRecord token.
  vg::core::RepresentationRequest conflicting_transform;
  conflicting_transform.view = rgba_view(source);
  conflicting_transform.target_kind = vg::core::FacetKind::Storage;
  const std::vector<vg::core::RepresentationRequest> transforms{conflicting_transform};
  vg::test_support::AssemblyOptions options;
  options.representation_requests = &transforms;
  options.facet_pool = &pool;
  vg::test_support::AssembledPlanFixture conflicting_fixture;
  vg::core::ExecutionPlan conflicting_plan;
  auto live_raster = raster;
  live_raster.raster_facets.target = while_held;
  CHECK(vg::test_support::assemble_single_user_raster_plan(
      arena, shader, {live_raster}, &conflicting_fixture, &conflicting_plan, &error, options));
  CHECK(conflicting_plan.execution_schedule.transitions.size() == 1);
  CHECK(conflicting_plan.execution_schedule.transitions[0].before_wave ==
        vg::core::kExecutionSchedulePrelude);
  CHECK(conflicting_plan.execution_schedule.transitions[0].representation_operations ==
        std::vector<uint32_t>({0}));
  vg::hal::SubmissionLifetimeHold conflicting_hold;
  CHECK(!conflicting_hold.prepare(conflicting_plan, arena, pool, &error));
  CHECK(error ==
        "a same-submit representation transform would invalidate a sealed Task FacetRef before Stage 7");
  CHECK(arena.lookup(vg::core::PointerRef{source.id, source.generation})->representation_epoch == 0);

  // The SceneRoot schema moves source authority into root.material.albedo.
  // Seal one plan that also uses every optional explicit raster capability so
  // the inventory proves all five Task fields plus the indirect albedo path.
  auto& scene_root = arena.allocate(VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE);
  auto& albedo = arena.allocate(4);
  auto& scene_target = arena.allocate(4);
  auto& scene_vertex = arena.allocate(4);
  auto& scene_index = arena.allocate(6);
  auto& scene_depth = arena.allocate(4);
  vg::core::FacetRef albedo_ref{};
  vg::core::FacetRef scene_target_ref{};
  vg::core::FacetRef scene_vertex_ref{};
  vg::core::FacetRef scene_index_ref{};
  vg::core::FacetRef scene_depth_ref{};
  CHECK(pool.acquire(arena, rgba_view(albedo), vg::core::FacetKind::Sample,
                     &albedo_ref, &error));
  CHECK(pool.acquire(arena, rgba_view(scene_target), vg::core::FacetKind::Attachment,
                     &scene_target_ref, &error));
  CHECK(pool.acquire(arena, rgba_view(scene_vertex), vg::core::FacetKind::Address,
                     &scene_vertex_ref, &error));
  auto index_view = rgba_view(scene_index);
  index_view.format = vg::core::PixelFormat::R16Uint;
  CHECK(pool.acquire(arena, index_view, vg::core::FacetKind::Address,
                     &scene_index_ref, &error));
  auto depth_view = rgba_view(scene_depth);
  depth_view.format = vg::core::PixelFormat::Depth32Float;
  CHECK(pool.acquire(arena, depth_view, vg::core::FacetKind::Attachment,
                     &scene_depth_ref, &error));

  VgSchemaLayout_SceneRootRaster scene_bytes{};
  scene_bytes.camera_clip_from_local[0] = 1.0f;
  scene_bytes.camera_clip_from_local[5] = 1.0f;
  scene_bytes.camera_clip_from_local[10] = 1.0f;
  scene_bytes.camera_clip_from_local[15] = 1.0f;
  scene_bytes.material.base_color[0] = 1.0f;
  scene_bytes.material.base_color[1] = 1.0f;
  scene_bytes.material.base_color[2] = 1.0f;
  scene_bytes.material.base_color[3] = 1.0f;
  scene_bytes.material.albedo.index = albedo_ref.index;
  scene_bytes.material.albedo.generation = albedo_ref.generation;
  std::memcpy(scene_root.bytes.data(), &scene_bytes, sizeof(scene_bytes));

  vg::core::TaskRecord scene_task;
  scene_task.kind = vg::core::TaskKind::Raster;
  scene_task.root_allocation = scene_root.id;
  scene_task.root_generation = scene_root.generation;
  scene_task.raster_facets.target = scene_target_ref;
  scene_task.vertex_buffer_ref = scene_vertex_ref;
  scene_task.index_buffer_ref = scene_index_ref;
  scene_task.index_count = 3;
  scene_task.depth_attachment_ref = scene_depth_ref;
  scene_task.depth_test_enable = true;
  scene_task.depth_write_enable = true;
  const vg::ir::UserRasterShaderContract scene_shader{
      VG_SCHEMA_SCENEROOTRASTER_CONTRACT_NAME, "vertex_main", "fragment_main",
      vg::ir::kRasterVertexAbiXyzuvPackedV1, "kernel source is opaque to core"};
  vg::test_support::AssembledPlanFixture scene_fixture;
  vg::core::ExecutionPlan scene_plan;
  CHECK(vg::test_support::assemble_single_user_raster_plan(
      arena, scene_shader, {scene_task}, &scene_fixture, &scene_plan, &error,
      raster_options));
  CHECK(scene_plan.lifetime_facets.size() == 5);
  CHECK(scene_plan.task_facet_uses.size() == 1);
  CHECK(scene_plan.task_facet_uses[0].size() == 6);  // depth contributes read + write
  CHECK(scene_plan.task_facet_uses[0][0].ref.index == albedo_ref.index);
  CHECK(scene_plan.task_facet_uses[0][0].view.allocation == albedo.id);
  CHECK(std::ranges::any_of(scene_plan.task_effects[0], [&](const auto& effect) {
    return effect.allocation == scene_index.id && effect.size == 6 &&
           effect.access == vg::ir::Access::Read;
  }));
  CHECK(std::ranges::any_of(scene_plan.task_effects[0], [&](const auto& effect) {
    return effect.allocation == scene_depth.id &&
           effect.access == vg::ir::Access::Read;
  }));
  CHECK(std::ranges::any_of(scene_plan.task_effects[0], [&](const auto& effect) {
    return effect.allocation == scene_depth.id &&
           effect.access == vg::ir::Access::Write;
  }));
  const auto contains_lifetime_use = [&](vg::core::FacetRef ref, vg::core::FacetKind kind) {
    return std::ranges::any_of(scene_plan.lifetime_facets, [&](const auto& use) {
      return use.ref.index == ref.index && use.ref.generation == ref.generation && use.kind == kind;
    });
  };
  CHECK(contains_lifetime_use(albedo_ref, vg::core::FacetKind::Sample));
  CHECK(contains_lifetime_use(scene_target_ref, vg::core::FacetKind::Attachment));
  CHECK(contains_lifetime_use(scene_vertex_ref, vg::core::FacetKind::Address));
  CHECK(contains_lifetime_use(scene_index_ref, vg::core::FacetKind::Address));
  CHECK(contains_lifetime_use(scene_depth_ref, vg::core::FacetKind::Attachment));
  vg::hal::SubmissionLifetimeHold scene_hold;
  CHECK(scene_hold.prepare(scene_plan, arena, pool, &error));
  CHECK(scene_hold.acquire({}, &error));
  CHECK(scene_hold.facet_count() == 5);
  CHECK(scene_hold.allocation_count() == 6);  // root plus five distinct facet backings
  scene_hold.release();
  for (const auto ref : {albedo_ref, scene_target_ref, scene_vertex_ref,
                         scene_index_ref, scene_depth_ref})
    CHECK(pool.in_flight(ref) == 0);
}

void test_representation_outputs_join_lifetime_after_physical_stage() {
  vg::core::Arena arena;
  const auto& probe = arena.allocate(32);
  const auto& image = arena.allocate(4);
  const uint64_t image_id = image.id;
  const uint32_t image_generation = image.generation;
  vg::core::FacetPool pool;
  vg::core::RepresentationRequest request;
  request.view = rgba_view(image);
  request.target_kind = vg::core::FacetKind::Sample;
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(assemble_representation_case(arena, probe.id, probe.generation, {request},
                                     pool, &plan, &error));
  CHECK(plan.touched_allocations.size() == 2);  // probe + representation target

  vg::hal::CompiledPlan compiled;
  compiled.representation_operations.push_back({
      vg::hal::CompiledPlan::RepresentationOperation::Identity, 0, "unit physical operation"});
  vg::hal::Submission submission;
  vg::hal::SubmissionLifetimeHold hold;
  CHECK(hold.prepare(plan, arena, pool, &error));
  CHECK(vg::hal::commit_representation_operations(
      plan, compiled.representation_operations, arena, pool,
      [](const vg::core::RepresentationSemanticPlanItem& request,
         const vg::hal::CompiledPlan::PhysicalRepresentationOperation&,
         vg::core::FacetRef, vg::hal::RepresentationTransformCost* cost,
         std::string*) {
        cost->new_backing_bytes = request.view.byte_size();
        return true;
      },
      &submission, &error));
  CHECK(submission.representation_facets.size() == 1);
  const auto target_facet = submission.representation_facets.front();
  CHECK(hold.acquire(submission.representation_facets, &error));
  CHECK(hold.allocation_count() == 2);
  CHECK(hold.facet_count() == 1);
  CHECK(arena.lookup(vg::core::PointerRef{image_id, image_generation})->in_flight == 1);
  CHECK(pool.in_flight(target_facet) == 1);
  uint32_t ignored_epoch = 0;
  CHECK(!arena.transform(image_id, image_generation, &ignored_epoch, &error));
  CHECK(error == "representation epoch is referenced in flight");
  hold.release();
  CHECK(arena.lookup(vg::core::PointerRef{image_id, image_generation})->in_flight == 0);
  CHECK(pool.in_flight(target_facet) == 0);

  // A physical failure happens before hold acquisition. It may have published
  // an epoch (the transform is not transactional), but it must never leak an
  // allocation/facet use count into the next submission.
  const auto& failing_image = arena.allocate(4);
  const uint64_t failing_id = failing_image.id;
  vg::core::RepresentationRequest failing_request;
  failing_request.view = rgba_view(failing_image);
  failing_request.target_kind = vg::core::FacetKind::Storage;
  vg::core::ExecutionPlan failing_plan;
  CHECK(assemble_representation_case(arena, probe.id, probe.generation, {failing_request},
                                     pool, &failing_plan, &error));
  vg::hal::SubmissionLifetimeHold failing_hold;
  CHECK(failing_hold.prepare(failing_plan, arena, pool, &error));
  vg::hal::Submission failed_submission;
  CHECK(!vg::hal::commit_representation_operations(
      failing_plan, compiled.representation_operations, arena, pool,
      [](const vg::core::RepresentationSemanticPlanItem&,
         const vg::hal::CompiledPlan::PhysicalRepresentationOperation&,
         vg::core::FacetRef, vg::hal::RepresentationTransformCost*,
         std::string* physical_error) {
        if (physical_error) *physical_error = "injected physical failure";
        return false;
      },
      &failed_submission, &error));
  CHECK(error.find("injected physical failure") != std::string::npos);
  CHECK(arena.lookup(vg::core::PointerRef{failing_id, failing_image.generation})->in_flight == 0);
  CHECK(!failing_hold.held());
}

void test_reference_submit_releases_holds_on_success_and_repeat() {
  vg::core::Arena arena;
  const auto& root = arena.allocate(32);
  const uint64_t root_id = root.id;
  const uint32_t root_generation = root.generation;
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(vg::test_support::assemble_single_node_plan(
      arena, canonical_module(root_id),
      {vg::test_support::compute_task(root_id, root_generation)}, &fixture, &plan, &error));
  auto device = vg::reference::make_device_hal();
  CHECK(device != nullptr);
  vg::hal::CompiledPlan compiled;
  CHECK(device->compile(plan, &compiled, &error));
  for (int attempt = 0; attempt < 2; ++attempt) {
    vg::hal::Submission submission;
    CHECK(device->submit(compiled, arena, &submission, &error));
    CHECK(submission.result.ok);
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);
  }

  vg::test_support::AssemblyOptions failed_options;
  failed_options.timeline_wait = 1;  // fresh Reference timeline is still 0
  failed_options.timeline_signal = 2;
  vg::test_support::AssembledPlanFixture failed_fixture;
  vg::core::ExecutionPlan failed_plan;
  CHECK(vg::test_support::assemble_single_node_plan(
      arena, canonical_module(root_id),
      {vg::test_support::compute_task(root_id, root_generation)},
      &failed_fixture, &failed_plan, &error, failed_options));
  vg::hal::CompiledPlan failed_compiled;
  CHECK(device->compile(failed_plan, &failed_compiled, &error));
  vg::hal::Submission failed_submission;
  CHECK(device->submit(failed_compiled, arena, &failed_submission, &error));
  CHECK(!failed_submission.result.ok);
  CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);
}

void test_basic_execution_plan_validation() {
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(!plan.validate(&error));
  CHECK(error == "execution plan must be produced by the canonical core assembler");
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

}  // namespace

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
