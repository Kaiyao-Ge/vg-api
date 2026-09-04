#include "cases.h"
#include <cassert>
#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "compiler/compiler.h"
#include "assembled_plan_fixture.h"

namespace vg::tests::core {

void test_reference_task_timeline() {
  // --- B7/B8 M7: ExecutionPlan cross-validation, graph_epoch, and the
  // reference backend's real task_graph/timeline consumption. ---
  {
    vg::core::Arena task_arena;
    auto& root = task_arena.allocate(64);

    vg::core::TaskGraphBuilder tg_builder;
    vg::core::TaskRecord task0{}; task0.node_index = 0; task0.root_allocation = root.id;
    vg::core::TaskRecord task1{}; task1.node_index = 1; task1.root_allocation = root.id;
    assert(tg_builder.append(task0));
    assert(tg_builder.append(task1));
    assert(tg_builder.add_dependency(0, 1));  // task0 must run before task1
    vg::core::TaskGraph sealed_unpublished;
    assert(tg_builder.seal(&sealed_unpublished));
    assert(sealed_unpublished.sealed() && !sealed_unpublished.published());

    // validate_execution() on a sealed-but-unpublished graph fails honestly.
    std::string tg_error;
    assert(!sealed_unpublished.validate_execution(&tg_error));
    assert(tg_error == "task graph must be published before execution");

    auto compiled_module = vg::compiler::compile_c_like("@node @effects store(1,0,4,7)");
    assert(compiled_module.ok);
    compiled_module.module.instructions[0].allocation = root.id;
    compiled_module.module.declared_effects[0].allocation = root.id;
    compiled_module.module.canonical_json = vg::ir::serialize_module(compiled_module.module);

    auto reference_device = vg::reference::make_device_hal();

    // ExecutionPlan::validate(): published==true with a sealed-but-unpublished
    // task_graph cannot bypass the canonical assembler.
    vg::core::ExecutionPlan bad_plan;
    bad_plan.task_graph = sealed_unpublished;
    bad_plan.published = true;
    std::string plan_error;
    assert(!bad_plan.validate(&plan_error));
    assert(plan_error == "execution plan must be produced by the canonical core assembler");

    // A genuinely sealed+published task graph passes the same cross-check.
    vg::core::TaskGraph published_graph = sealed_unpublished;
    assert(published_graph.publish());
    vg::core::ExecutionPlan good_plan;
    good_plan.task_graph = published_graph;
    good_plan.graph_epoch = task_arena.topology_epoch();
    good_plan.published = true;

    // graph_epoch_matches(): empty task_graph is exempt regardless of epoch;
    // a non-empty task_graph must match the live arena's topology_epoch.
    vg::core::ExecutionPlan empty_task_plan;
    empty_task_plan.graph_epoch = 12345;
    assert(empty_task_plan.graph_epoch_matches(task_arena));
    assert(good_plan.graph_epoch_matches(task_arena));
    task_arena.allocate(8);  // bumps topology_epoch -- good_plan's stamp is now stale
    std::string epoch_error;
    assert(!good_plan.graph_epoch_matches(task_arena, &epoch_error));
    assert(epoch_error == "execution plan graph epoch does not match arena topology");

    // reference::execute_task_graph(): deterministic dependency-respecting order.
    auto task_result = vg::reference::execute_task_graph(published_graph);
    assert(task_result.ok);
    assert(task_result.published_tasks.size() == 2);
    assert(task_result.published_tasks[0].node_index == 0);
    assert(task_result.published_tasks[1].node_index == 1);

    // Full compile()+submit() round trip: task_graph consumption, published_tasks
    // reporting, and real (non-passthrough) timeline wait/signal semantics.
    vg::core::Arena fresh_arena;
    auto& fresh_root = fresh_arena.allocate(64);
    auto module_copy = compiled_module.module;
    module_copy.instructions[0].allocation = fresh_root.id;
    module_copy.declared_effects[0].allocation = fresh_root.id;
    module_copy.canonical_json = vg::ir::serialize_module(module_copy);

    vg::test_support::AssembledPlanFixture fresh_fixture;
    vg::core::ExecutionPlan plan;
    std::string submit_error;
    vg::test_support::AssemblyOptions fresh_options;
    fresh_options.timeline_signal = 5;
    const std::vector<std::pair<uint32_t, uint32_t>> fresh_dependencies{{0, 1}};
    fresh_options.dependencies = &fresh_dependencies;
    assert(vg::test_support::assemble_single_node_plan(
        fresh_arena, module_copy,
        {vg::test_support::compute_task(fresh_root.id), vg::test_support::compute_task(fresh_root.id)},
        &fresh_fixture, &plan, &submit_error, fresh_options));

    vg::hal::CompiledPlan compiled;
    assert(reference_device->compile(plan, &compiled, &submit_error));
    vg::hal::Submission submission;
    assert(reference_device->submit(compiled, fresh_arena, &submission, &submit_error));
    assert(submission.result.ok);
    assert(submission.published_tasks.size() == 2);
    assert(submission.published_tasks[0].node_index == fresh_fixture.node.index);
    assert(submission.published_tasks[1].node_index == fresh_fixture.node.index);
    assert(submission.timeline_value == 5);

    // Next submission's wait is satisfied by the prior signal (real device
    // timeline state, not a plan-local passthrough).
    vg::test_support::AssembledPlanFixture waiting_fixture;
    vg::core::ExecutionPlan waiting_plan;
    vg::test_support::AssemblyOptions waiting_options;
    waiting_options.timeline_wait = 5;
    waiting_options.timeline_signal = 10;
    assert(vg::test_support::assemble_single_node_plan(
        fresh_arena, module_copy, {vg::test_support::compute_task(fresh_root.id)},
        &waiting_fixture, &waiting_plan, &submit_error, waiting_options));
    vg::hal::CompiledPlan waiting_compiled;
    assert(reference_device->compile(waiting_plan, &waiting_compiled, &submit_error));
    vg::hal::Submission waiting_submission;
    assert(reference_device->submit(waiting_compiled, fresh_arena, &waiting_submission, &submit_error));
    assert(waiting_submission.result.ok);
    assert(waiting_submission.timeline_value == 10);

    // An unsatisfied wait faults honestly (submit() still returns true --
    // matching the Metal/Vulkan convention that submit() reports host-side
    // acceptance, while submission.result.ok reports execution outcome).
    vg::test_support::AssembledPlanFixture stuck_fixture;
    vg::core::ExecutionPlan stuck_plan;
    vg::test_support::AssemblyOptions stuck_options;
    stuck_options.timeline_wait = 999;
    stuck_options.timeline_signal = 1000;
    assert(vg::test_support::assemble_single_node_plan(
        fresh_arena, module_copy, {vg::test_support::compute_task(fresh_root.id)},
        &stuck_fixture, &stuck_plan, &submit_error, stuck_options));
    vg::hal::CompiledPlan stuck_compiled;
    assert(reference_device->compile(stuck_plan, &stuck_compiled, &submit_error));
    vg::hal::Submission stuck_submission;
    assert(reference_device->submit(stuck_compiled, fresh_arena, &stuck_submission, &submit_error));
    assert(!stuck_submission.result.ok);
    assert(stuck_submission.result.fault.code == "TIMELINE_WAIT_UNSATISFIED");

    // A stale graph_epoch is rejected at submit() entry.
    fresh_arena.allocate(8);
    vg::hal::Submission stale_submission;
    std::string stale_error;
    assert(!reference_device->submit(compiled, fresh_arena, &stale_submission, &stale_error));
    assert(stale_error == "execution plan graph epoch does not match arena topology");
  }
}

void test_reference_access_certificate() {
  // --- TASK-B11 (E004): core::build_access_certificate direct coverage, plus
  // the reference backend's compile()/submit() handling of
  // requested_certificate_mode -- exercised independently of the Metal-only
  // vertical-slice test so the reference backend's own new code paths have
  // dedicated assertions. ---
  {
    vg::core::Arena cert_arena;
    auto& touched_allocation = cert_arena.allocate(32);
    auto& other_allocation = cert_arena.allocate(16);
    (void)other_allocation;
    std::vector<vg::core::PointerRef> touched{{touched_allocation.id, touched_allocation.generation}};

    vg::core::AccessCertificate pinned_cert;
    std::string cert_error;
    assert(vg::core::build_access_certificate(cert_arena, vg::core::AccessCertificateMode::CertifiedPinned, touched,
                                               &pinned_cert, &cert_error));
    assert(pinned_cert.mode == vg::core::AccessCertificateMode::CertifiedPinned);
    assert(pinned_cert.epoch.references().size() == 1);
    assert(pinned_cert.epoch.contains({touched_allocation.id, touched_allocation.generation}));

    vg::core::AccessCertificate universe_cert;
    assert(vg::core::build_access_certificate(cert_arena, vg::core::AccessCertificateMode::Universe, touched,
                                               &universe_cert, &cert_error));
    assert(universe_cert.epoch.references().size() == 2);

    vg::core::AccessCertificate lease_cert;
    assert(vg::core::build_access_certificate(cert_arena, vg::core::AccessCertificateMode::DiscoverThenLease, touched,
                                               &lease_cert, &cert_error));
    assert(lease_cert.epoch.references().size() == 2);

    vg::core::AccessCertificate unsupported_cert;
    assert(!vg::core::build_access_certificate(cert_arena, vg::core::AccessCertificateMode::SoftwarePaged, touched,
                                                &unsupported_cert, &cert_error));
    assert(!vg::core::build_access_certificate(cert_arena, vg::core::AccessCertificateMode::FaultManaged, touched,
                                                &unsupported_cert, &cert_error));

    // A touched reference the arena doesn't actually hold is rejected, not
    // silently certified.
    vg::core::AccessCertificate bogus_cert;
    std::vector<vg::core::PointerRef> bogus_touched{{999, 1}};
    assert(!vg::core::build_access_certificate(cert_arena, vg::core::AccessCertificateMode::CertifiedPinned,
                                                bogus_touched, &bogus_cert, &cert_error));

    // Reference backend: compile()+submit() with requested_certificate_mode
    // set populates Submission::access_certificate for the 3 real modes, and
    // rejects SoftwarePaged/FaultManaged honestly at compile() time.
    auto cert_device = vg::reference::make_device_hal();
    auto cert_module = vg::compiler::compile_c_like("@node @effects store(1,0,4,7)");
    assert(cert_module.ok);
    cert_module.module.instructions[0].allocation = touched_allocation.id;
    cert_module.module.instructions[0].generation = touched_allocation.generation;
    cert_module.module.declared_effects[0].allocation = touched_allocation.id;
    cert_module.module.canonical_json = vg::ir::serialize_module(cert_module.module);

    vg::test_support::AssembledPlanFixture cert_fixture;
    vg::core::ExecutionPlan cert_plan;
    std::string device_error;
    vg::test_support::AssemblyOptions cert_options;
    cert_options.certificate_mode = vg::core::AccessCertificateMode::CertifiedPinned;
    cert_options.certificate_touched = {{touched_allocation.id, touched_allocation.generation}};
    assert(vg::test_support::assemble_single_node_plan(
        cert_arena, cert_module.module,
        {vg::test_support::compute_task(touched_allocation.id, touched_allocation.generation)},
        &cert_fixture, &cert_plan, &device_error, cert_options));

    vg::hal::CompiledPlan cert_compiled;
    assert(cert_device->compile(cert_plan, &cert_compiled, &device_error));
    vg::hal::Submission cert_submission;
    assert(cert_device->submit(cert_compiled, cert_arena, &cert_submission, &device_error));
    assert(cert_submission.result.ok);
    assert(cert_submission.access_certificate.has_value());
    assert(cert_submission.access_certificate->mode == vg::core::AccessCertificateMode::CertifiedPinned);
    assert(cert_submission.access_certificate->epoch.references().size() == 1);

    vg::test_support::AssembledPlanFixture unsupported_fixture;
    vg::core::ExecutionPlan unsupported_plan;
    cert_options.certificate_mode = vg::core::AccessCertificateMode::SoftwarePaged;
    std::string unsupported_error;
    assert(!vg::test_support::assemble_single_node_plan(
        cert_arena, cert_module.module,
        {vg::test_support::compute_task(touched_allocation.id, touched_allocation.generation)},
        &unsupported_fixture, &unsupported_plan, &unsupported_error, cert_options));
    assert(unsupported_error.find("Unsupported") != std::string::npos);
  }
}

void test_reference_representation_submit(const vg::core::ConsumeProof& discharged) {
  // --- Stage 5 (03 §7) end to end on the reference backend, driven through
  // the device's own FacetPool (06 §2 places the pool inside the adapter).
  //
  // Note the ordering constraint this pins: Stage 5 runs *before* the
  // interpreter, so it publishes a new RepresentationEpoch that the module's
  // own instructions are then resolved against. A module compiled against the
  // superseded epoch is stale afterwards -- 02 §8's "transform 不是纯 barrier"
  // reaching all the way down to instruction resolution -- so the plan here
  // declares the epoch the transform will publish. The stale case is asserted
  // separately below rather than avoided.
  //
  // ConsumeInput is refused at compile() time on this backend, honestly and
  // with a named reason: its transform is the identity, so no backing is
  // superseded and there is nothing a consume could release. ---
  {
    vg::core::Arena stage_arena;
    auto& stage_backing = stage_arena.allocate(256);
    const uint64_t stage_id = stage_backing.id;
    const uint32_t stage_generation = stage_backing.generation;

    auto stage_module = vg::compiler::compile_c_like("@node @effects store(1,0,4,7)");
    assert(stage_module.ok);
    stage_module.module.instructions[0].allocation = stage_id;
    stage_module.module.instructions[0].generation = stage_generation;
    stage_module.module.instructions[0].representation_epoch = 1;
    stage_module.module.declared_effects[0].allocation = stage_id;
    stage_module.module.declared_effects[0].representation_epoch = 1;
    stage_module.module.canonical_json = vg::ir::serialize_module(stage_module.module);

    auto stage_device = vg::reference::make_device_hal();
    assert(stage_device->capabilities().supports(vg::hal::Capability::RepresentationTransform));
    assert(stage_device->capabilities().supports(vg::hal::Capability::CheckedFacetGeneration));

    vg::core::CanonicalView stage_view;
    stage_view.allocation = stage_id;
    stage_view.allocation_generation = stage_generation;
    stage_view.width = 4;
    stage_view.height = 4;
    assert(stage_view.byte_size() == 64);

    vg::core::RepresentationRequest request;
    request.view = stage_view;
    request.target_kind = vg::core::FacetKind::Sample;

    const std::vector<vg::core::RepresentationRequest> stage_requests{request};
    vg::test_support::AssembledPlanFixture stage_fixture;
    vg::test_support::AssemblyOptions stage_options;
    stage_options.representation_requests = &stage_requests;
    stage_options.facet_pool = &stage_device->facet_pool();
    vg::core::ExecutionPlan stage_plan;
    std::string stage_error;
    assert(vg::test_support::assemble_single_node_plan(
        stage_arena, stage_module.module,
        {vg::test_support::compute_task(stage_id, stage_generation)},
        &stage_fixture, &stage_plan, &stage_error, stage_options));

    vg::hal::CompiledPlan stage_compiled;
    assert(stage_device->compile(stage_plan, &stage_compiled, &stage_error));
    assert(stage_compiled.report.supported);

    vg::hal::Submission stage_submission;
    assert(stage_device->submit(stage_compiled, stage_arena, &stage_submission, &stage_error));
    assert(stage_submission.result.ok);
    // Stage 5 sealed a RepresentationEpoch and published exactly one target
    // facet, which still resolves against the epoch the transform published.
    assert(stage_submission.representation_epoch.sealed());
    assert(stage_submission.representation_facets.size() == 1);
    const vg::core::FacetRef stage_facet = stage_submission.representation_facets[0];
    assert(stage_submission.representation_epoch.contains(stage_facet));
    const auto* stage_slot = stage_device->facet_pool().lookup(stage_arena, stage_facet);
    assert(stage_slot != nullptr);
    assert(stage_slot->kind == vg::core::FacetKind::Sample);
    assert(stage_slot->representation_epoch == 1);
    assert(stage_arena.lookup(vg::core::PointerRef{stage_id, stage_generation})->representation_epoch == 1);
    // 06 §11's peak-memory report: the superseded backing is counted whether
    // or not it is released, and nothing was released here.
    assert(stage_submission.old_backing_bytes == 256);
    assert(stage_submission.new_backing_bytes == stage_view.byte_size());
    assert(stage_submission.temporary_bytes == 0);
    assert(stage_submission.released_backing_bytes == 0);
    assert(stage_submission.consumed_allocation_count == 0);
    assert(stage_submission.completion_delay_ns == 0);

    // The source epoch has an intentionally live target token above.  A
    // destructive request must be rejected by assembly until that external
    // reference is retired; after retirement the Reference backend reaches
    // its honest Stage-6 identity/ConsumeInput Unsupported result.
    assert(stage_device->facet_pool().retire(stage_facet, &stage_error));

    // ConsumeInput: rejected at compile() with an Unsupported lowering event
    // and a reason -- never accepted and quietly not performed.
    auto consume_request = request;
    consume_request.consume_input = true;
    consume_request.consume_proof = discharged;
    const std::vector<vg::core::RepresentationRequest> consume_requests{consume_request};
    vg::test_support::AssembledPlanFixture consume_fixture;
    vg::test_support::AssemblyOptions consume_options;
    consume_options.representation_requests = &consume_requests;
    consume_options.facet_pool = &stage_device->facet_pool();
    vg::core::ExecutionPlan consume_plan;
    assert(vg::test_support::assemble_single_node_plan(
        stage_arena, stage_module.module,
        {vg::test_support::compute_task(stage_id, stage_generation)},
        &consume_fixture, &consume_plan, &stage_error, consume_options));
    vg::hal::CompiledPlan consume_compiled;
    std::string consume_error;
    assert(!stage_device->compile(consume_plan, &consume_compiled, &consume_error));
    assert(!consume_compiled.report.supported);
    assert(consume_compiled.report.count(vg::hal::LoweringClass::Unsupported) >= 1);
    assert(consume_error.find("ConsumeInput is not available on the reference backend") == 0);
    // ...and submit() refuses an unsupported compile rather than running it.
    vg::hal::Submission refused_submission;
    std::string refused_error;
    assert(!stage_device->submit(consume_compiled, stage_arena, &refused_submission, &refused_error));

    // An incomplete proof is rejected earlier still, by ExecutionPlan::
    // validate(): the adapter is forbidden from inferring a destructive
    // transform on its own (06 §11).
    auto unproven_request = request;
    unproven_request.consume_input = true;
    const std::vector<vg::core::RepresentationRequest> unproven_requests{unproven_request};
    vg::test_support::AssembledPlanFixture unproven_fixture;
    vg::test_support::AssemblyOptions unproven_options;
    unproven_options.representation_requests = &unproven_requests;
    unproven_options.facet_pool = &stage_device->facet_pool();
    vg::core::ExecutionPlan unproven_plan;
    vg::hal::CompiledPlan unproven_compiled;
    std::string unproven_error;
    assert(!vg::test_support::assemble_single_node_plan(
        stage_arena, stage_module.module,
        {vg::test_support::compute_task(stage_id, stage_generation)},
        &unproven_fixture, &unproven_plan, &unproven_error, unproven_options));
    assert(unproven_error.find("asks for ConsumeInput but its proof is incomplete") != std::string::npos);

    // A Stage 5 target must be a facet a transform can produce: Address and
    // Transfer name how an existing representation is reached.
    auto address_request = request;
    address_request.target_kind = vg::core::FacetKind::Address;
    const std::vector<vg::core::RepresentationRequest> address_requests{address_request};
    vg::test_support::AssembledPlanFixture address_fixture;
    vg::test_support::AssemblyOptions address_options;
    address_options.representation_requests = &address_requests;
    address_options.facet_pool = &stage_device->facet_pool();
    vg::core::ExecutionPlan address_plan;
    vg::hal::CompiledPlan address_compiled;
    std::string address_error;
    assert(!vg::test_support::assemble_single_node_plan(
        stage_arena, stage_module.module,
        {vg::test_support::compute_task(stage_id, stage_generation)},
        &address_fixture, &address_plan, &address_error, address_options));
    assert(address_error.find("must be a Sample, Storage, or Attachment facet") != std::string::npos);

    // Two requests racing one allocation's representation in a single
    // submission is rejected, not serialized behind the caller's back.
    const std::vector<vg::core::RepresentationRequest> racing_requests{request, request};
    vg::test_support::AssembledPlanFixture racing_fixture;
    vg::test_support::AssemblyOptions racing_options;
    racing_options.representation_requests = &racing_requests;
    racing_options.facet_pool = &stage_device->facet_pool();
    vg::core::ExecutionPlan racing_plan;
    vg::hal::CompiledPlan racing_compiled;
    std::string racing_error;
    assert(!vg::test_support::assemble_single_node_plan(
        stage_arena, stage_module.module,
        {vg::test_support::compute_task(stage_id, stage_generation)},
        &racing_fixture, &racing_plan, &racing_error, racing_options));
    assert(racing_error.find("must not race two transforms of a single allocation") != std::string::npos);

    // A plan carrying no request runs no Stage 5 at all, so a caller that
    // never asked cannot be handed a half-filled epoch.
    vg::test_support::AssembledPlanFixture plain_fixture;
    vg::core::ExecutionPlan plain_plan;
    assert(vg::test_support::assemble_single_node_plan(
        stage_arena, stage_module.module,
        {vg::test_support::compute_task(stage_id, stage_generation)},
        &plain_fixture, &plain_plan, &stage_error));
    vg::hal::CompiledPlan plain_compiled;
    assert(stage_device->compile(plain_plan, &plain_compiled, &stage_error));
    vg::hal::Submission plain_submission;
    assert(stage_device->submit(plain_compiled, stage_arena, &plain_submission, &stage_error));
    assert(plain_submission.result.ok);
    assert(!plain_submission.representation_epoch.sealed());
    assert(plain_submission.representation_facets.empty());
    assert(plain_submission.old_backing_bytes == 0);
    // The earlier target token was explicitly retired before the destructive
    // assembly check; this unrelated submission must not recreate it.
    assert(stage_device->facet_pool().lookup(stage_arena, stage_facet) == nullptr);

    // The same plan against a module that still names the pre-transform epoch:
    // Stage 5 really publishes the new epoch, and the interpreter then refuses
    // the instruction as stale rather than resolving it against a version the
    // arena has moved past. submit() still returns true -- host-side
    // acceptance -- while result.ok reports the execution outcome.
    vg::core::Arena stale_arena;
    auto& stale_backing = stale_arena.allocate(256);
    auto stale_module = stage_module;
    stale_module.module.instructions[0].allocation = stale_backing.id;
    stale_module.module.instructions[0].generation = stale_backing.generation;
    stale_module.module.instructions[0].representation_epoch = 0;
    stale_module.module.declared_effects[0].allocation = stale_backing.id;
    stale_module.module.declared_effects[0].representation_epoch = 0;
    stale_module.module.canonical_json = vg::ir::serialize_module(stale_module.module);

    vg::core::RepresentationRequest stale_request;
    stale_request.view = stage_view;
    stale_request.view.allocation = stale_backing.id;
    stale_request.view.allocation_generation = stale_backing.generation;
    stale_request.target_kind = vg::core::FacetKind::Sample;

    const std::vector<vg::core::RepresentationRequest> stale_requests{stale_request};
    vg::test_support::AssembledPlanFixture stale_fixture;
    vg::test_support::AssemblyOptions stale_options;
    stale_options.representation_requests = &stale_requests;
    stale_options.facet_pool = &stage_device->facet_pool();
    vg::core::ExecutionPlan stale_plan;
    assert(vg::test_support::assemble_single_node_plan(
        stale_arena, stale_module.module,
        {vg::test_support::compute_task(stale_backing.id, stale_backing.generation)},
        &stale_fixture, &stale_plan, &stage_error, stale_options));

    vg::hal::CompiledPlan stale_compiled;
    assert(stage_device->compile(stale_plan, &stale_compiled, &stage_error));
    vg::hal::Submission stale_submission;
    assert(stage_device->submit(stale_compiled, stale_arena, &stale_submission, &stage_error));
    assert(!stale_submission.result.ok);
    assert(stale_submission.result.fault.code == "STALE_OR_BOUNDS");
    assert(!stale_submission.result.outputs_valid);
    assert(stale_submission.result.poison == vg::core::PoisonState::Poisoned);
    // 02 §9: a fault is not a transactional rollback. The transform already
    // happened, so the sealed epoch and its facet stay reported rather than
    // being pretended away.
    assert(stale_submission.representation_epoch.sealed());
    assert(stale_submission.representation_facets.size() == 1);
    assert(stale_arena.lookup(vg::core::PointerRef{stale_backing.id, stale_backing.generation})->representation_epoch == 1);
  }
}

}  // namespace vg::tests::core
