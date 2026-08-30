#include "backends/device_hal.h"

#include "ir/sha256.h"
#include "../support/assembled_plan_fixture.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
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

  // Legacy hints have no Stage 6 meaning unless the assembler translated them
  // into the sealed list.
  auto legacy_only = sealed_requirement_plan({});
  legacy_only.request_tier2_select = true;
  auto no_tier2 = fully_capable_reference_snapshot();
  no_tier2.capability_bits &= ~static_cast<uint64_t>(Capability::IndirectTier2Select);
  vg::hal::CompiledPlan compiled;
  std::string error;
  CHECK(vg::hal::preflight_stage6(legacy_only, no_tier2, vg::hal::BackendKind::Reference, &compiled, &error));
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
    CHECK(vg::hal::preflight_stage6(plan, capabilities, vg::hal::BackendKind::Reference,
                                    &compiled, &error));
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
  compiled.compute_package.emplace();
  compiled.node_compute_packages.emplace_back();
  compiled.effect_dag_packages.emplace_back();
  compiled.effect_dag_node_count = 7;
  CHECK(!vg::hal::preflight_stage6(plan, capabilities, vg::hal::BackendKind::Reference,
                                   &compiled, &error));
  CHECK(!compiled.compute_package.has_value());
  CHECK(compiled.node_compute_packages.empty());
  CHECK(compiled.effect_dag_packages.empty());
  CHECK(compiled.effect_dag_node_count == 0);
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
  duplicate_snapshot.resolved_nodes.push_back(plan.resolved_nodes.front());
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

  // Transitional hints are translated once by assembly, sorted, and de-duped
  // before Stage 6 sees the plan.
  inputs.task_graph = &duplicate_graph;
  inputs.nodes = &nodes;
  inputs.request_tier1_indirect = true;
  inputs.request_tier2_select = false;  // No Node contract class exists yet.
  inputs.request_indexed_binding = true;
  envelope.allowed_nodes = {first_node, second_node};
  CHECK(vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  CHECK((plan.required_capabilities == std::vector<vg::core::CapabilityRequirement>{
      vg::core::CapabilityRequirement::LinearAddress,
      vg::core::CapabilityRequirement::TaskPublication,
      vg::core::CapabilityRequirement::IndirectTier1,
      vg::core::CapabilityRequirement::IndexedBinding,
      vg::core::CapabilityRequirement::CheckedFacetGeneration}));
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
  vg::core::Certificate certificate{{{root.id, 0, 12, vg::ir::Access::Read, 0}}};
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

void test_basic_execution_plan_validation() {
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(!plan.validate(&error));
  CHECK(error == "root schema is required");

  plan.module = {1, "vg.root/v1", {{"store", 1, 0, 4, 7, 1, 0, 0, ""}},
                 {{1, 0, 4, vg::ir::Access::Write, 0}}, {}, "", ""};
  CHECK(plan.validate(&error));
  plan.timeline_wait = 2;
  plan.timeline_signal = 2;
  CHECK(!plan.validate(&error));
  CHECK(error == "timeline signal does not advance past wait");
}

void test_consume_input_proof_rejections() {
  // These are semantic-boundary negatives: no assembled facts are forged and
  // no adapter compile path is involved.  The request itself must carry a
  // complete destructive-transform proof before any lowering is considered.
  vg::core::ExecutionPlan plan;
  plan.module = {1, "vg.root/v1", {{"store", 1, 0, 4, 7, 1, 0, 0, ""}},
                 {{1, 0, 4, vg::ir::Access::Write, 0}}, {}, "", ""};
  vg::core::RepresentationRequest request;
  request.view.allocation = 1;
  request.view.allocation_generation = 1;
  request.view.width = 4;
  request.view.height = 4;
  request.target_kind = vg::core::FacetKind::Sample;
  request.consume_input = true;
  plan.representation_requests = {request};

  std::string error;
  CHECK(!plan.validate(&error));
  CHECK(error.find("ConsumeInput but its proof is incomplete") != std::string::npos);

  request.consume_proof = {true, false, true, true};
  plan.representation_requests = {request};
  CHECK(!plan.validate(&error));
  CHECK(error.find("an external reference to the old representation still exists") != std::string::npos);
}

}  // namespace

int main() {
  static_assert(std::is_same_v<vg::hal::ExecutionPlan, vg::core::ExecutionPlan>);
  test_basic_execution_plan_validation();
  test_consume_input_proof_rejections();
  test_stage6_capability_preflight_rejects_without_weakening();
  test_validation_profile_matrix_and_reset();
  test_effect_conflicts_are_deterministic_or_reject_reverse_cycle();
  test_certificate_and_access_witness_reject_partial_coverage();
  test_bounded_pointer_graph_canonical_identity();
  test_execution_plan_assembler_sound_counterexamples();
  test_execution_plan_assembler_bounded_pointer_graph_access();
  test_execution_plan_assembler_seals_access_planning();
  test_representation_stage5_assembler_boundaries();
  test_representation_semantic_plan_is_sealed();
  return 0;
}
