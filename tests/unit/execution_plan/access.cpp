#include "fixture.h"

namespace vg::tests::execution_plan {

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

}  // namespace vg::tests::execution_plan
