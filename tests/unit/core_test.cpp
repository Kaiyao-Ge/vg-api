#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "capture/capture.h"
#include "compiler/compiler.h"
#include "core/core.h"
#include "core/task_schema.h"
#include "assembled_plan_fixture.h"
#include <cassert>

namespace {

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

}  // namespace

int main() {
  vg::core::Arena arena;
  auto& allocation = arena.allocate(16);
  assert(allocation.id == 1 && allocation.generation == 1);
  assert(arena.topology_epoch() == 1);
  assert(arena.lookup(vg::core::PointerRef{allocation.id, allocation.generation}) != nullptr);
  assert(arena.acquire(allocation.id, allocation.generation));
  std::string transform_error;
  assert(!arena.transform(allocation.id, allocation.generation, nullptr, &transform_error));
  assert(arena.release(allocation.id, allocation.generation));
  uint32_t representation_epoch = 0;
  assert(arena.transform(allocation.id, allocation.generation, &representation_epoch) && representation_epoch == 1);
  assert(arena.lookup(vg::core::RepresentationRef{allocation.id, allocation.generation, representation_epoch}) != nullptr);
  assert(!arena.transform(allocation.id, allocation.generation, 0, nullptr, &transform_error));
  const vg::core::ConsumeProof discharged{true, true, true, true};
  assert(!arena.consume(allocation.id, allocation.generation, representation_epoch,
                        vg::core::ConsumeProof{}, &transform_error) &&
         transform_error.rfind("ConsumeInput proof incomplete", 0) == 0);
  assert(arena.consume(allocation.id, allocation.generation, representation_epoch, discharged,
                       &transform_error));
  assert(arena.lookup(vg::core::PointerRef{allocation.id, 1}) == nullptr);
  auto& second_allocation = arena.allocate(16);

  // F2 (ADR-043 Decision #3, ADR-046): a default-constructed TaskRecord must
  // read as a plain Compute task with F2's fixed raster defaults, so every
  // pre-F2 caller that never touches these fields keeps its old meaning.
  {
    vg::core::TaskRecord default_task{};
    assert(default_task.kind == vg::core::TaskKind::Compute);
    assert(default_task.topology == vg::core::Topology::TriangleList);
    assert(default_task.index_count == 0);
    assert(default_task.raster_filter == vg::core::FilterMode::Bilinear);
    assert(default_task.raster_wrap == vg::core::WrapMode::Clamp);
    assert(default_task.raster_tint[0] == 1.0f && default_task.raster_tint[1] == 1.0f &&
           default_task.raster_tint[2] == 1.0f && default_task.raster_tint[3] == 1.0f);
  }

  vg::core::TaskGraphBuilder builder;
  vg::core::TaskRecord first{}; first.node_index = 1; first.root_allocation = 1;
  vg::core::TaskRecord second{}; second.node_index = 2; second.root_allocation = 1;
  assert(builder.append(first)); assert(builder.append(second));
  assert(builder.add_dependency(0, 1));
  vg::core::TaskGraph graph;
  assert(builder.seal(&graph));
  assert(graph.sealed() && graph.tasks().size() == 2);
  assert(graph.publish());
  assert(graph.published());
  std::string error;
  assert(!builder.append(first, &error) && error == "task graph builder is sealed");

  vg::core::TaskGraphBuilder quota_builder;
  assert(quota_builder.set_quota(1, 4));
  assert(quota_builder.append(first));
  assert(!quota_builder.append(second, &error) && error == "task graph quota overflow");
  vg::core::TaskGraphBuilder payload_quota_builder;
  assert(payload_quota_builder.set_quota(2, 3));
  auto payload_task = first; payload_task.payload_size = 4;
  assert(!payload_quota_builder.append(payload_task, &error) && error == "task payload quota overflow");

  // F4: slot zero is a valid FacetRef index, but a non-zero index with a
  // zero generation is never an issued capability token. Reject it before a
  // backend can disagree on whether depth was requested.
  vg::core::TaskRecord malformed_depth{};
  malformed_depth.kind = vg::core::TaskKind::Raster;
  malformed_depth.depth_attachment_ref = {7, 0};
  malformed_depth.depth_test_enable = true;
  vg::core::TaskGraphBuilder malformed_depth_builder;
  assert(!malformed_depth_builder.append(malformed_depth, &error) &&
         error == "raster depth attachment facet generation must be non-zero");
  vg::core::TaskRecord slot_zero_depth{};
  slot_zero_depth.kind = vg::core::TaskKind::Raster;
  slot_zero_depth.depth_attachment_ref = {0, 1};
  slot_zero_depth.depth_test_enable = true;
  vg::core::TaskGraphBuilder slot_zero_depth_builder;
  assert(slot_zero_depth_builder.append(slot_zero_depth));
  VgSchema_TaskRecord schema_task{};
  schema_task.node.index = 7; schema_task.node.generation = 3; schema_task.root = 11;
  schema_task.shape.x = 2; schema_task.shape.y = 3; schema_task.shape.z = 4;
  schema_task.payload_size = 4; schema_task.payload_or_offset = 99;
  const auto converted = vg::core::task_from_schema(schema_task);
  assert(converted.node_index == 7 && converted.node_generation == 3 && converted.root_allocation == 11);

  vg::core::TaskGraphBuilder effects_builder;
  assert(effects_builder.append(first));
  assert(effects_builder.append(second));
  assert(effects_builder.add_effect(0, {second_allocation.id, 0, 8, vg::ir::Access::Write, 0}));
  assert(effects_builder.add_effect(1, {second_allocation.id, 4, 8, vg::ir::Access::Read, 0}));
  vg::core::TaskGraph effects_graph;
  assert(effects_builder.seal(&effects_graph));
  assert(effects_graph.effect_graph().edges().size() == 1);
  assert(effects_graph.effect_graph().edges().front().kind == vg::core::EffectEdgeKind::InferredConflict);

  // F4: TaskGraphBuilder has no FacetPool to resolve depth attachment backing
  // allocations. It therefore derives a deterministic capability-token write
  // effect; identical depth refs must serialize as a WAW conflict at seal.
  vg::core::TaskRecord depth_writer_a{};
  depth_writer_a.kind = vg::core::TaskKind::Raster;
  depth_writer_a.depth_attachment_ref = {42, 7};
  depth_writer_a.depth_write_enable = true;
  vg::core::TaskRecord depth_writer_b = depth_writer_a;
  vg::core::TaskGraphBuilder depth_effect_builder;
  assert(depth_effect_builder.append(depth_writer_a));
  assert(depth_effect_builder.append(depth_writer_b));
  vg::core::TaskGraph depth_effect_graph;
  assert(depth_effect_builder.seal(&depth_effect_graph));
  assert(depth_effect_graph.effect_graph().edges().size() == 1);
  const auto& depth_edge = depth_effect_graph.effect_graph().edges().front();
  assert(depth_edge.before == 0 && depth_edge.after == 1);
  assert(depth_edge.kind == vg::core::EffectEdgeKind::InferredConflict);

  vg::core::TaskGraphBuilder reads_builder;
  assert(reads_builder.append(first)); assert(reads_builder.append(second));
  assert(reads_builder.add_effect(0, {second_allocation.id, 0, 8, vg::ir::Access::Read, 0}));
  assert(reads_builder.add_effect(1, {second_allocation.id, 0, 8, vg::ir::Access::Read, 0}));
  vg::core::TaskGraph reads_graph;
  assert(reads_builder.seal(&reads_graph));
  assert(reads_graph.effect_graph().edges().empty());
  vg::core::EffectGraph missing_hb;
  std::string hb_error;
  assert(!missing_hb.validate_happens_before(
      {{ {second_allocation.id, 0, 8, vg::ir::Access::Write, 0} },
         { {second_allocation.id, 0, 8, vg::ir::Access::Read, 0} } }, &hb_error));
  assert(hb_error == "conflicting task effects have no happens-before edge");

  vg::core::Timeline timeline;
  assert(timeline.signal(2)); assert(timeline.wait(1));
  assert(timeline.validate_wait(2));
  assert(!timeline.validate_wait(3, &error) && error == "timeline wait point is unsatisfied");
  assert(!timeline.signal(2, &error));

  vg::core::EffectGraph effects;
  assert(effects.add_edge(0, 1)); assert(!effects.add_edge(1, 0) || !effects.valid());
  vg::core::EffectGraph timeline_graph;
  assert(timeline_graph.add_timeline_edge(0, 1, 2, timeline.value()));
  assert(timeline_graph.edges().front().kind == vg::core::EffectEdgeKind::Timeline);
  assert(!timeline_graph.add_timeline_edge(1, 2, 3, timeline.value(), &error));

  vg::core::GraphEpochBuilder epoch_builder(&arena, 7);
  assert(!epoch_builder.add_reference(arena, {allocation.id, allocation.generation}));
  assert(epoch_builder.add_reference(arena, {second_allocation.id, second_allocation.generation}));
  assert(epoch_builder.add_reference(arena, {second_allocation.id, second_allocation.generation}));
  vg::core::GraphEpoch graph_epoch;
  assert(epoch_builder.seal(&graph_epoch));
  assert(graph_epoch.value() == arena.topology_epoch() && graph_epoch.references().size() == 1);
  assert(graph_epoch.contains({second_allocation.id, second_allocation.generation}));
  assert(!epoch_builder.add_reference({2, 1}));

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
    // task_graph must be rejected, not silently accepted.
    vg::hal::ExecutionPlan bad_plan;
    bad_plan.module = compiled_module.module;
    bad_plan.task_graph = sealed_unpublished;
    bad_plan.published = true;
    std::string plan_error;
    assert(!bad_plan.validate(&plan_error));
    assert(plan_error == "task graph must be published before execution");

    // A genuinely sealed+published task graph passes the same cross-check.
    vg::core::TaskGraph published_graph = sealed_unpublished;
    assert(published_graph.publish());
    vg::hal::ExecutionPlan good_plan;
    good_plan.module = compiled_module.module;
    good_plan.task_graph = published_graph;
    good_plan.graph_epoch = task_arena.topology_epoch();
    good_plan.published = true;
    assert(good_plan.validate(&plan_error));

    // graph_epoch_matches(): empty task_graph is exempt regardless of epoch;
    // a non-empty task_graph must match the live arena's topology_epoch.
    vg::hal::ExecutionPlan empty_task_plan;
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
    vg::hal::ExecutionPlan plan;
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
    vg::hal::ExecutionPlan waiting_plan;
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
    vg::hal::ExecutionPlan stuck_plan;
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
    vg::hal::ExecutionPlan cert_plan;
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
    vg::hal::ExecutionPlan unsupported_plan;
    cert_options.certificate_mode = vg::core::AccessCertificateMode::SoftwarePaged;
    std::string unsupported_error;
    assert(!vg::test_support::assemble_single_node_plan(
        cert_arena, cert_module.module,
        {vg::test_support::compute_task(touched_allocation.id, touched_allocation.generation)},
        &unsupported_fixture, &unsupported_plan, &unsupported_error, cert_options));
    assert(unsupported_error.find("Unsupported") != std::string::npos);
  }

  // --- TASK-C1: FacetPool acquire/lookup/retire, and the "facet generation
  // vs epoch = stale token" check (02-principles-and-semantics.md Sec.10). ---
  {
    vg::core::Arena facet_arena;
    auto& backing = facet_arena.allocate(64);

    vg::core::CanonicalView view;
    view.allocation = backing.id;
    view.allocation_generation = backing.generation;
    view.format = vg::core::PixelFormat::RGBA8Unorm;
    view.dimension = vg::core::ViewDimension::Texture2D;
    view.width = 4; view.height = 4;

    vg::core::FacetPool pool;
    vg::core::FacetRef ref;
    std::string facet_error;
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Sample, &ref, &facet_error));
    const auto* slot = pool.lookup(facet_arena, ref);
    assert(slot != nullptr && slot->kind == vg::core::FacetKind::Sample);
    assert(slot->view.allocation == backing.id);

    // A CanonicalView over an allocation the arena doesn't hold is rejected.
    vg::core::CanonicalView bogus_view = view;
    bogus_view.allocation = 999;
    vg::core::FacetRef bogus_ref;
    assert(!pool.acquire(facet_arena, bogus_view, vg::core::FacetKind::Sample, &bogus_ref, &facet_error));

    // Advancing the backing allocation's representation_epoch stales the
    // facet ref -- lookup() must not return the slot's last-known contents.
    uint32_t new_epoch = 0;
    assert(facet_arena.transform(backing.id, backing.generation, &new_epoch));
    assert(pool.lookup(facet_arena, ref) == nullptr);

    // Re-acquiring against the now-current epoch succeeds and yields a
    // fresh, independently valid ref.
    vg::core::FacetRef reacquired;
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Sample, &reacquired, &facet_error));
    assert(pool.lookup(facet_arena, reacquired) != nullptr);

    // retire() frees the index for reuse with a bumped generation; the old
    // ref is rejected afterward, and a re-acquire recycles the same index.
    assert(pool.retire(reacquired, &facet_error));
    assert(pool.lookup(facet_arena, reacquired) == nullptr);
    assert(!pool.retire(reacquired, &facet_error));
    vg::core::FacetRef recycled;
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Storage, &recycled, &facet_error));
    assert(recycled.index == reacquired.index && recycled.generation != reacquired.generation);

    // Sample/Storage/Attachment are distinct kinds from the same CanonicalView
    // (02 §3.3: per-usage facets, not one maximal ViewRecord).
    vg::core::FacetRef sample_ref, storage_ref, attachment_ref;
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Sample, &sample_ref, &facet_error));
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Storage, &storage_ref, &facet_error));
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Attachment, &attachment_ref, &facet_error));
    assert(pool.lookup(facet_arena, sample_ref)->kind == vg::core::FacetKind::Sample);
    assert(pool.lookup(facet_arena, storage_ref)->kind == vg::core::FacetKind::Storage);
    assert(pool.lookup(facet_arena, attachment_ref)->kind == vg::core::FacetKind::Attachment);
    uint32_t transform_epoch = 0;
    assert(facet_arena.transform(backing.id, backing.generation, &transform_epoch));
    assert(pool.lookup(facet_arena, sample_ref) == nullptr);
    assert(pool.lookup(facet_arena, storage_ref) == nullptr);
    assert(pool.lookup(facet_arena, attachment_ref) == nullptr);
    assert(pool.retire_stale(facet_arena) >= 3);
    assert(pool.lookup(facet_arena, sample_ref) == nullptr);

    // Failures say which rule rejected the token, so a caller can distinguish
    // a forged ref from one that merely outlived its epoch.
    vg::core::FacetStatus status = vg::core::FacetStatus::Ok;
    assert(pool.lookup(facet_arena, sample_ref, &status) == nullptr);
    assert(status == vg::core::FacetStatus::Retired);
    vg::core::FacetRef out_of_range{9999, 1};
    assert(pool.lookup(facet_arena, out_of_range, &status) == nullptr);
    assert(status == vg::core::FacetStatus::UnknownIndex);
  }

  // --- A slot referenced by in-flight GPU work is not reusable, even once the
  // token itself is dead (06-backend-macos-metal.md Sec.6.4, Sec.11). ---
  {
    vg::core::Arena arena;
    auto& backing = arena.allocate(64);
    vg::core::CanonicalView view;
    view.allocation = backing.id;
    view.allocation_generation = backing.generation;
    view.width = 4;
    view.height = 4;

    vg::core::FacetPool pool;
    vg::core::FacetRef ref;
    std::string error;
    assert(pool.acquire(arena, view, vg::core::FacetKind::Sample, &ref, &error));
    assert(pool.begin_gpu_use(arena, ref, &error));
    assert(pool.in_flight(ref) == 1);

    // Retiring while in flight kills the token immediately...
    assert(pool.retire(ref, &error));
    assert(pool.lookup(arena, ref) == nullptr);
    // ...but must not hand the index to an unrelated facet underneath the GPU.
    vg::core::FacetRef during_flight;
    assert(pool.acquire(arena, view, vg::core::FacetKind::Sample, &during_flight, &error));
    assert(during_flight.index != ref.index);

    // end_gpu_use matches the generation the use was begun under, not the
    // bumped one, and releasing the last use is what frees the index.
    assert(pool.end_gpu_use(ref, &error));
    assert(!pool.end_gpu_use(ref, &error));
    vg::core::FacetRef recycled;
    assert(pool.acquire(arena, view, vg::core::FacetKind::Sample, &recycled, &error));
    assert(recycled.index == ref.index);

    // An epoch bump takes the same path: retire_stale withholds an in-flight
    // slot's index until the work referencing it is done.
    assert(pool.begin_gpu_use(arena, recycled, &error));
    uint32_t epoch = 0;
    assert(arena.transform(backing.id, backing.generation, &epoch));
    assert(pool.retire_stale(arena) >= 1);
    vg::core::FacetRef after_stale;
    assert(pool.acquire(arena, view, vg::core::FacetKind::Sample, &after_stale, &error));
    assert(after_stale.index != recycled.index);
    assert(pool.end_gpu_use(recycled, &error));

    // A stale ref can never start new work in the first place.
    assert(!pool.begin_gpu_use(arena, recycled, &error));
  }

  // ==========================================================================
  // Phase C: CanonicalView shape contract, RepresentationEpoch, ConsumeInput
  // proofs, E016 backpressure, the checked-profile generation table, and the
  // reference backend's Stage 5. Each block states one documented requirement.
  // ==========================================================================

  // --- CanonicalView::valid()/layout (02 §3.3). Shape validity is a property
  // of the view contract, so every backend gets the same answer; the byte
  // layout is the single contract the Metal upload path and the reference
  // sampling oracle both encode against, so an image comparison between them
  // is only meaningful if it is pinned here. ---
  {
    vg::core::CanonicalView view;
    view.allocation = 1;
    view.allocation_generation = 1;
    view.format = vg::core::PixelFormat::RGBA8Unorm;
    view.dimension = vg::core::ViewDimension::Texture2D;
    view.width = 8;
    view.height = 4;
    std::string shape_error;
    assert(view.valid(&shape_error));

    // Zero extent cannot describe a real image.
    vg::core::CanonicalView zero_width = view;
    zero_width.width = 0;
    assert(!zero_width.valid(&shape_error));
    assert(shape_error == "canonical view extent must be non-zero");
    vg::core::CanonicalView zero_height = view;
    zero_height.height = 0;
    assert(!zero_height.valid(&shape_error));
    vg::core::CanonicalView zero_layers = view;
    zero_layers.array_layers = 0;
    assert(!zero_layers.valid(&shape_error));
    assert(shape_error == "canonical view must name at least one array layer");
    vg::core::CanonicalView zero_levels = view;
    zero_levels.mip_levels = 0;
    assert(!zero_levels.valid(&shape_error));
    assert(shape_error == "canonical view must name at least one mip level");

    // 8x4 supports exactly 4 levels (8x4, 4x2, 2x1, 1x1); a 5th would alias
    // the 1x1 rather than describe new texels, so it is malformed rather than
    // something to clamp.
    vg::core::CanonicalView full_chain = view;
    full_chain.mip_levels = 4;
    assert(full_chain.valid(&shape_error));
    vg::core::CanonicalView over_chain = view;
    over_chain.mip_levels = 5;
    assert(!over_chain.valid(&shape_error));
    assert(shape_error == "canonical view mip chain is longer than its extent supports");

    // array_layers > 1 needs the array dimension.
    vg::core::CanonicalView flat_array = view;
    flat_array.array_layers = 2;
    assert(!flat_array.valid(&shape_error));
    assert(shape_error == "Texture2D canonical view cannot name multiple array layers");
    vg::core::CanonicalView array_view = view;
    array_view.dimension = vg::core::ViewDimension::Texture2DArray;
    array_view.array_layers = 2;
    array_view.mip_levels = 4;
    assert(array_view.valid(&shape_error));
    // A Texture2DArray naming a single layer is legal; only the reverse is not.
    vg::core::CanonicalView single_layer_array = array_view;
    single_layer_array.array_layers = 1;
    assert(single_layer_array.valid(&shape_error));

    // Half-and-clamp, the sizing rule every graphics API's mip chain uses.
    assert(array_view.mip_width(0) == 8 && array_view.mip_height(0) == 4);
    assert(array_view.mip_width(1) == 4 && array_view.mip_height(1) == 2);
    assert(array_view.mip_width(2) == 2 && array_view.mip_height(2) == 1);
    assert(array_view.mip_width(3) == 1 && array_view.mip_height(3) == 1);
    // Clamped, not zero, past the end of the chain.
    assert(array_view.mip_width(9) == 1 && array_view.mip_height(9) == 1);

    assert(vg::core::bytes_per_texel(vg::core::PixelFormat::RGBA8Unorm) == 4);
    assert(vg::core::bytes_per_texel(vg::core::PixelFormat::R32Float) == 4);
    assert(array_view.subresource_count() == 8);
    assert(array_view.bytes_per_row(0) == 32 && array_view.bytes_per_row(2) == 8);
    assert(array_view.subresource_byte_size(0) == 128);
    assert(array_view.subresource_byte_size(3) == 4);

    // byte_size() is exactly the sum of every subresource's size.
    uint64_t summed = 0;
    for (uint32_t layer = 0; layer < array_view.array_layers; ++layer)
      for (uint32_t level = 0; level < array_view.mip_levels; ++level)
        summed += array_view.subresource_byte_size(level);
    assert(array_view.byte_size() == summed);
    assert(array_view.byte_size() == 344);

    // Offsets are slice-major, then ascending mip level, tightly packed.
    assert(array_view.subresource_byte_offset({0, 0}) == 0);
    assert(array_view.subresource_byte_offset({0, 1}) == 128);
    assert(array_view.subresource_byte_offset({0, 2}) == 160);
    assert(array_view.subresource_byte_offset({0, 3}) == 168);
    assert(array_view.subresource_byte_offset({1, 0}) == 172);
    assert(array_view.subresource_byte_offset({1, 3}) == 340);
  }

  // --- RepresentationEpoch/Builder (02 §4.1): a representation transform
  // produces a new frozen interpretation rather than editing this one, which
  // is 02 §8's "transform 不是纯 barrier" stated as a data structure. Mirrors
  // GraphEpochBuilder's shape, so seal() stamps the arena's clock the same way
  // GraphEpochBuilder::seal() stamps topology_epoch(). ---
  {
    vg::core::Arena epoch_arena;
    auto& epoch_backing = epoch_arena.allocate(256);
    const uint64_t backing_id = epoch_backing.id;
    const uint32_t backing_generation = epoch_backing.generation;

    uint32_t published = 0;
    assert(epoch_arena.transform(backing_id, backing_generation, &published) && published == 1);
    assert(epoch_arena.representation_clock() == 1);

    vg::core::CanonicalView view;
    view.allocation = backing_id;
    view.allocation_generation = backing_generation;
    view.width = 4;
    view.height = 4;

    vg::core::FacetPool epoch_pool;
    vg::core::FacetRef sample_ref;
    std::string epoch_error;
    assert(epoch_pool.acquire(epoch_arena, view, vg::core::FacetKind::Sample, &sample_ref, &epoch_error));

    vg::core::RepresentationEpochBuilder epoch_builder(&epoch_arena);
    // add_representation(arena, ...) snapshots the allocation's *current*
    // epoch, so a caller cannot freeze a version the arena is not at.
    assert(epoch_builder.add_representation(epoch_arena, backing_id, backing_generation, &epoch_error));
    assert(epoch_builder.add_facet(epoch_arena, epoch_pool, sample_ref, &epoch_error));
    // Re-adding the same reference is idempotent, not an error.
    assert(epoch_builder.add_representation(epoch_arena, backing_id, backing_generation, &epoch_error));
    // A reference the arena does not hold is refused rather than frozen.
    assert(!epoch_builder.add_representation(epoch_arena, 999, 1, &epoch_error));
    assert(epoch_error == "representation reference is not active in arena");
    assert(!epoch_builder.add_representation({backing_id, 0, published}, &epoch_error));
    assert(epoch_error == "representation reference generation must be non-zero");

    vg::core::RepresentationEpoch representation_epoch;
    assert(!epoch_builder.sealed());
    assert(epoch_builder.seal(&representation_epoch, &epoch_error));
    assert(epoch_builder.sealed());
    assert(representation_epoch.sealed());
    // Stamped from the arena's representation clock, the sibling of the
    // topology_epoch() stamp GraphEpochBuilder::seal() uses.
    assert(representation_epoch.value() == epoch_arena.representation_clock());
    assert(representation_epoch.representations().size() == 1);
    assert(representation_epoch.facets().size() == 1);
    assert(representation_epoch.contains(
        vg::core::RepresentationRef{backing_id, backing_generation, published}));
    assert(representation_epoch.contains(sample_ref));
    assert(!representation_epoch.contains(vg::core::FacetRef{sample_ref.index, sample_ref.generation + 1}));
    assert(!representation_epoch.stale(epoch_arena));
    // Seal once: a sealed builder is immutable (02 §4.1's build -> release ->
    // immutable -> retire).
    assert(!epoch_builder.seal(&representation_epoch, &epoch_error));
    assert(epoch_error == "representation epoch builder is already sealed");
    assert(!epoch_builder.add_representation({backing_id, backing_generation, published}, &epoch_error));
    assert(epoch_error == "representation epoch is sealed");

    // Another transform makes the frozen interpretation stale wholesale: every
    // facet it authorized has to be rebuilt rather than reused (02 §10 at epoch
    // granularity).
    uint32_t superseding = 0;
    assert(epoch_arena.transform(backing_id, backing_generation, &superseding) && superseding == 2);
    assert(representation_epoch.stale(epoch_arena));
    assert(epoch_pool.lookup(epoch_arena, sample_ref) == nullptr);

    // A facet whose slot is already stale cannot be frozen: doing so would
    // authorize a token that is dead on arrival.
    vg::core::RepresentationEpochBuilder stale_builder(&epoch_arena);
    assert(!stale_builder.add_facet(epoch_arena, epoch_pool, sample_ref, &epoch_error));
    assert(epoch_error == vg::core::to_string(vg::core::FacetStatus::EpochStale));

    // Without an arena the builder falls back to the next_epoch it was
    // constructed with, rather than inventing a clock value.
    vg::core::RepresentationEpochBuilder detached(static_cast<uint64_t>(7));
    assert(detached.add_representation({backing_id, backing_generation, superseding}));
    vg::core::RepresentationEpoch detached_epoch;
    assert(detached.seal(&detached_epoch));
    assert(detached_epoch.value() == 7);
  }

  // --- ConsumeProof (02 §4.2): none of the four obligations is observable
  // from arena state, so they are attested rather than inferred, and the
  // rejection names *which* proof failed. 10 §3's "ConsumeInput proof"
  // conformance row and 10 §4's "illegal consume" negative case. ---
  {
    assert(vg::core::ConsumeProof{}.first_unmet() != nullptr);
    assert(std::string(vg::core::ConsumeProof{}.first_unmet()) == "old envelope has not completed");
    assert(std::string(vg::core::ConsumeProof{true, false, false, false}.first_unmet()) ==
           "an external reference to the old representation still exists");
    assert(std::string(vg::core::ConsumeProof{true, true, false, false}.first_unmet()) ==
           "the old representation may still be replayed");
    assert(std::string(vg::core::ConsumeProof{true, true, true, false}.first_unmet()) ==
           "destructive-failure semantics were not accepted");
    assert(discharged.complete());
    assert(discharged.first_unmet() == nullptr);

    vg::core::Arena consume_arena;
    auto& consume_backing = consume_arena.allocate(64);
    const uint64_t consume_id = consume_backing.id;
    const uint32_t consume_generation = consume_backing.generation;
    uint32_t consume_epoch = 0;
    assert(consume_arena.transform(consume_id, consume_generation, &consume_epoch) && consume_epoch == 1);
    assert(consume_arena.lookup(vg::core::PointerRef{consume_id, consume_generation})->live_representations == 2);

    // An incomplete proof is refused before any state is touched, by both
    // destructive operations.
    std::string consume_error;
    uint64_t released = 0;
    assert(!consume_arena.consume_representation(consume_id, consume_generation, consume_epoch,
                                                 vg::core::ConsumeProof{true, true, false, true}, &released,
                                                 &consume_error));
    assert(consume_error ==
           "ConsumeInput proof incomplete: the old representation may still be replayed");
    assert(released == 0);
    assert(!consume_arena.consume(consume_id, consume_generation, consume_epoch,
                                  vg::core::ConsumeProof{true, true, true, false}, &consume_error));
    assert(consume_error ==
           "ConsumeInput proof incomplete: destructive-failure semantics were not accepted");
    assert(consume_arena.lookup(vg::core::RepresentationRef{consume_id, consume_generation, consume_epoch}) != nullptr);

    // consume_representation() is the transform form (06 §11): the object
    // survives -- identity, generation and freshly published epoch all stay
    // live, so facets acquired against the new representation keep resolving --
    // but the superseded backing is handed back at once. That released byte
    // count is E005's watermark reduction.
    assert(consume_arena.consume_representation(consume_id, consume_generation, consume_epoch, discharged,
                                                &released, &consume_error));
    assert(released == 64);
    const auto* survivor = consume_arena.lookup(vg::core::RepresentationRef{consume_id, consume_generation, consume_epoch});
    assert(survivor != nullptr);
    assert(survivor->state == vg::core::ObjectState::Active);
    assert(survivor->generation == consume_generation);
    assert(survivor->representation_epoch == consume_epoch);
    // The old version is no longer retained as an extra live representation.
    assert(survivor->live_representations == 1);
    assert(survivor->bytes.empty());

    // Collapsing the two destructive operations would stale the very facet a
    // transform just published, so consume() -- the retiring form -- is
    // distinct: it retires the allocation and bumps its generation so no old
    // token can ever resolve again.
    vg::core::Arena retire_arena;
    auto& retire_backing = retire_arena.allocate(64);
    const uint64_t retire_id = retire_backing.id;
    const uint32_t retire_generation = retire_backing.generation;
    uint32_t retire_epoch = 0;
    assert(retire_arena.transform(retire_id, retire_generation, &retire_epoch));
    assert(retire_arena.consume(retire_id, retire_generation, retire_epoch, discharged, &consume_error));
    const auto& retired = retire_arena.allocations().at(retire_id);
    assert(retired.state == vg::core::ObjectState::Retired);
    assert(retired.generation == retire_generation + 1);
    assert(retired.live_representations == 0);
    assert(retired.bytes.empty());
    assert(retire_arena.lookup(vg::core::PointerRef{retire_id, retire_generation}) == nullptr);
    // 10 §5: a retired generation is never visible again, and the bumped one
    // was never handed out either.
    assert(retire_arena.lookup(vg::core::PointerRef{retire_id, retire_generation + 1}) == nullptr);
    assert(!retire_arena.consume(retire_id, retire_generation, retire_epoch, discharged, &consume_error));
    assert(consume_error == "stale allocation or representation epoch for consume");

    // 10 §4's "illegal consume": both forms require exclusive ownership, and a
    // superseded epoch token cannot consume the current representation.
    vg::core::Arena exclusive_arena;
    auto& exclusive_backing = exclusive_arena.allocate(32);
    const uint64_t exclusive_id = exclusive_backing.id;
    const uint32_t exclusive_generation = exclusive_backing.generation;
    uint32_t exclusive_epoch = 0;
    assert(exclusive_arena.transform(exclusive_id, exclusive_generation, &exclusive_epoch));
    assert(exclusive_arena.acquire(exclusive_id, exclusive_generation));
    assert(!exclusive_arena.consume_representation(exclusive_id, exclusive_generation, exclusive_epoch,
                                                   discharged, nullptr, &consume_error));
    assert(consume_error == "consume requires exclusive ownership");
    assert(!exclusive_arena.consume(exclusive_id, exclusive_generation, exclusive_epoch, discharged,
                                    &consume_error));
    assert(consume_error == "consume requires exclusive ownership");
    assert(exclusive_arena.release(exclusive_id, exclusive_generation));
    assert(!exclusive_arena.consume(exclusive_id, exclusive_generation, exclusive_epoch - 1, discharged,
                                    &consume_error));
    assert(consume_error == "stale allocation or representation epoch for consume");
    assert(exclusive_arena.consume(exclusive_id, exclusive_generation, exclusive_epoch, discharged,
                                   &consume_error));
  }

  // --- E005 catalog fault-injection, host-visible paths. These observe the
  // existing Stage 5 / consume / capture machinery rather than inventing an
  // Arena-scoped fault injector. ---
  {
    vg::core::Arena stage_arena;
    auto& probe = stage_arena.allocate(16);
    probe.bytes.assign(16, 0);
    auto& backing = stage_arena.allocate(16);
    backing.bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const auto original = backing.bytes;
    const uint32_t generation = backing.generation;
    const uint32_t epoch_before = backing.representation_epoch;
    vg::core::CanonicalView view;
    view.allocation = backing.id;
    view.allocation_generation = generation;
    view.width = 2;
    view.height = 2;
    vg::hal::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = discharged;
    vg::core::FacetPool pool;
    const std::vector<vg::core::RepresentationRequest> requests{request};
    vg::test_support::AssembledPlanFixture fixture;
    vg::core::ExecutionPlan plan;
    vg::hal::Submission submission;
    std::string stage_error;
    assert(assemble_representation_plan(stage_arena, probe, requests, pool, &fixture, &plan,
                                        &stage_error));
    const bool physical_fault_rejected = !vg::hal::commit_representation_operations(
        plan, {{vg::hal::CompiledPlan::RepresentationOperation::CopyToPrivate, 0, "fault harness"}}, stage_arena, pool,
        [](const vg::core::RepresentationSemanticPlanItem&, const vg::hal::CompiledPlan::PhysicalRepresentationOperation&, vg::core::FacetRef, vg::hal::RepresentationTransformCost*,
           std::string* physical_error) {
          if (physical_error) *physical_error = "injected physical transform fault";
          return false;
        },
        &submission, &stage_error);
    assert(physical_fault_rejected);
    assert(stage_error.find("injected physical transform fault") != std::string::npos);
    assert(submission.consumed_allocation_count == 0);
    assert(submission.released_backing_bytes == 0);
    assert(backing.bytes == original);
    assert(backing.generation == generation);
    assert(backing.state == vg::core::ObjectState::Active);
    assert(backing.representation_epoch == epoch_before + 1);
    assert(stage_arena.lookup(vg::core::RepresentationRef{backing.id, generation, epoch_before}) == nullptr);
    assert(stage_arena.lookup(vg::core::RepresentationRef{backing.id, generation, backing.representation_epoch}) != nullptr);
  }

  {
    vg::core::Arena cap_arena;
    auto& backing = cap_arena.allocate(16);
    backing.bytes.assign(16, 0);
    backing.bytes[0] = 255;
    vg::ir::Module module;
    module.version = 1;
    module.root_schema = "vg.test/v1";
    vg::ir::Instruction load;
    load.op = "load";
    load.allocation = backing.id;
    load.generation = backing.generation;
    load.representation_epoch = backing.representation_epoch;
    load.offset = 0;
    load.size = 4;
    module.instructions.push_back(load);
    module.declared_effects.push_back({backing.id, 0, 4, vg::ir::Access::Read, backing.representation_epoch});
    const auto pre = vg::capture::make_capture(module, cap_arena);
    uint32_t new_epoch = 0;
    assert(cap_arena.transform(backing.id, backing.generation, &new_epoch));
    uint64_t released = 0;
    std::string cap_error;
    assert(cap_arena.consume_representation(backing.id, backing.generation, new_epoch, discharged, &released,
                                           &cap_error));
    assert(released == 16);
    assert(backing.bytes.empty());
    vg::capture::ReplayResult pre_replay;
    assert(vg::capture::replay(pre, &pre_replay, &cap_error));
    assert(pre_replay.execution.ok);
    const auto post = vg::capture::make_capture(module, cap_arena);
    assert(post.allocations.size() == 1);
    assert(post.allocations[0].size == 16);
    assert(post.allocations[0].bytes.empty());
    // consume_representation clears bytes but leaves Allocation::size, so the
    // snapshot is not importable (bytes.size() != size). That is the lost
    // replay: the package cannot be reconstituted, not merely executed stale.
    vg::capture::ReplayResult post_replay;
    assert(!vg::capture::replay(post, &post_replay, &cap_error));
    assert(cap_error == "cannot restore a consumed representation");
  }

  {
    vg::core::Arena hold_arena;
    auto& probe = hold_arena.allocate(16);
    probe.bytes.assign(16, 0);
    auto& backing = hold_arena.allocate(16);
    backing.bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const auto original = backing.bytes;
    const uint32_t epoch_before = backing.representation_epoch;
    vg::core::CanonicalView view;
    view.allocation = backing.id;
    view.allocation_generation = backing.generation;
    view.width = 2;
    view.height = 2;
    vg::core::FacetPool pool;
    vg::core::FacetRef live{};
    std::string hold_error;
    assert(pool.acquire(hold_arena, view, vg::core::FacetKind::Sample, &live, &hold_error));
    assert(pool.references(vg::core::RepresentationRef{backing.id, backing.generation, epoch_before}));
    vg::hal::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = discharged;
    const std::vector<vg::core::RepresentationRequest> requests{request};
    vg::hal::Submission submission;
    vg::test_support::AssembledPlanFixture blocked_fixture;
    vg::core::ExecutionPlan blocked_plan;
    const bool live_facet_rejected = !assemble_representation_plan(
        hold_arena, probe, requests, pool, &blocked_fixture, &blocked_plan, &hold_error);
    assert(live_facet_rejected);
    assert(hold_error.find("live FacetRef names its source epoch") != std::string::npos);
    assert(backing.bytes == original);
    assert(backing.representation_epoch == epoch_before);
    assert(pool.lookup(hold_arena, live) != nullptr);
    assert(pool.retire(live, &hold_error));
    assert(!pool.references(vg::core::RepresentationRef{backing.id, backing.generation, epoch_before}));
    vg::test_support::AssembledPlanFixture fixture;
    vg::core::ExecutionPlan plan;
    assert(assemble_representation_plan(hold_arena, probe, requests, pool, &fixture, &plan,
                                        &hold_error));
    const bool consume_after_retire = vg::hal::commit_representation_operations(
        plan, {{vg::hal::CompiledPlan::RepresentationOperation::CopyToPrivate, 0, "fault harness"}}, hold_arena, pool,
        [](const vg::core::RepresentationSemanticPlanItem&, const vg::hal::CompiledPlan::PhysicalRepresentationOperation&, vg::core::FacetRef, vg::hal::RepresentationTransformCost* cost,
           std::string*) {
          cost->distinct_backing = true;
          cost->new_backing_bytes = 16;
          return true;
        },
        &submission, &hold_error);
    assert(consume_after_retire);
    assert(submission.consumed_allocation_count == 1);
    assert(backing.bytes.empty());
  }

  // --- E016 backpressure: "禁止无界创建版本" and "内存不足时可预测失败而非系统
  // 抖动". A non-zero budget makes transform() refuse with an explicit error
  // instead of letting versions accumulate; 0 (the default) is unbounded. ---
  {
    vg::core::Arena budget_arena;
    auto& budget_backing = budget_arena.allocate(32);
    const uint64_t budget_id = budget_backing.id;
    const uint32_t budget_generation = budget_backing.generation;
    assert(budget_arena.max_in_flight_representations() == 0);
    // An allocation starts at 1 live representation -- its own.
    assert(budget_backing.live_representations == 1);

    std::string budget_error;
    budget_arena.set_max_in_flight_representations(1);
    assert(budget_arena.max_in_flight_representations() == 1);
    // A budget of 1 is already exhausted by the initial representation, so the
    // first transform is refused predictably rather than blocking.
    assert(!budget_arena.transform(budget_id, budget_generation, nullptr, &budget_error));
    assert(budget_error == "in-flight representation budget exceeded");
    assert(budget_arena.lookup(vg::core::PointerRef{budget_id, budget_generation})->representation_epoch == 0);

    budget_arena.set_max_in_flight_representations(2);
    uint32_t budget_epoch = 0;
    assert(budget_arena.transform(budget_id, budget_generation, &budget_epoch) && budget_epoch == 1);
    assert(budget_arena.lookup(vg::core::PointerRef{budget_id, budget_generation})->live_representations == 2);
    // A second transform is refused until a version is released.
    assert(!budget_arena.transform(budget_id, budget_generation, nullptr, &budget_error));
    assert(budget_error == "in-flight representation budget exceeded");
    assert(budget_arena.release_representation(budget_id, budget_generation, &budget_error));
    assert(budget_arena.lookup(vg::core::PointerRef{budget_id, budget_generation})->live_representations == 1);
    assert(budget_arena.transform(budget_id, budget_generation, &budget_epoch) && budget_epoch == 2);

    // release_representation() never drops the last version: an Active
    // allocation always retains its current representation.
    assert(budget_arena.release_representation(budget_id, budget_generation, &budget_error));
    assert(budget_arena.lookup(vg::core::PointerRef{budget_id, budget_generation})->live_representations == 1);
    assert(!budget_arena.release_representation(budget_id, budget_generation, &budget_error));
    assert(budget_error == "an active allocation always retains its current representation");
    // A stale token cannot release a version either.
    assert(!budget_arena.release_representation(budget_id, budget_generation + 1, &budget_error));
    assert(budget_error == "stale allocation for representation release");

    // Back to unbounded: the budget is a policy input, not a property of the
    // allocation.
    budget_arena.set_max_in_flight_representations(0);
    assert(budget_arena.transform(budget_id, budget_generation, &budget_epoch) && budget_epoch == 3);
    assert(budget_arena.transform(budget_id, budget_generation, &budget_epoch) && budget_epoch == 4);
    assert(budget_arena.lookup(vg::core::PointerRef{budget_id, budget_generation})->live_representations == 3);
  }

  // --- FacetPool::snapshot_generations()/generation_valid() (06 §6.4): a
  // shader cannot call lookup(), so the checked profile uploads a table and
  // the kernel compares against it. generation_valid() is the host-side mirror
  // of that in-shader comparison and is deliberately *weaker* than lookup():
  // it sees only what the table encodes, so it cannot observe an epoch that
  // went stale, which is why the host-side lookup() stays authoritative. ---
  {
    vg::core::Arena table_arena;
    auto& table_backing = table_arena.allocate(256);
    const uint64_t table_id = table_backing.id;
    const uint32_t table_generation = table_backing.generation;

    vg::core::CanonicalView view;
    view.allocation = table_id;
    view.allocation_generation = table_generation;
    view.width = 4;
    view.height = 4;

    vg::core::FacetPool table_pool;
    vg::core::FacetRef live_ref;
    std::string table_error;
    assert(table_pool.acquire(table_arena, view, vg::core::FacetKind::Sample, &live_ref, &table_error));

    std::vector<uint32_t> table;
    table_pool.snapshot_generations(&table);
    assert(table.size() == table_pool.slot_count());
    assert(table.size() == 1);
    assert(table[live_ref.index] == live_ref.generation);
    assert(table_pool.generation_valid(live_ref));

    // A representation transform stales the token host-side...
    uint32_t table_epoch = 0;
    assert(table_arena.transform(table_id, table_generation, &table_epoch));
    vg::core::FacetStatus status = vg::core::FacetStatus::Ok;
    assert(table_pool.lookup(table_arena, live_ref, &status) == nullptr);
    assert(status == vg::core::FacetStatus::EpochStale);
    // ...but the uploaded table encodes only the slot's generation, so a
    // freshly pulled snapshot still shows the slot live and generation_valid()
    // -- the in-shader verdict -- still accepts the token. This is the exact
    // gap that makes lookup() authoritative rather than redundant.
    table_pool.snapshot_generations(&table);
    assert(table[live_ref.index] == live_ref.generation);
    assert(table_pool.generation_valid(live_ref));

    // Retirement is what the table *can* see: retire_stale() sweeps the
    // epoch-stale slot, and only then does the entry read 0 and the in-shader
    // check reject.
    assert(table_pool.retire_stale(table_arena) == 1);
    table_pool.snapshot_generations(&table);
    assert(table[live_ref.index] == 0);
    assert(!table_pool.generation_valid(live_ref));

    // A recycled index carries a bumped generation, so the old token is
    // rejected by the table while the new one is accepted -- the
    // index+generation discipline the shader relies on.
    vg::core::FacetRef recycled;
    assert(table_pool.acquire(table_arena, view, vg::core::FacetKind::Sample, &recycled, &table_error));
    assert(recycled.index == live_ref.index);
    assert(recycled.generation != live_ref.generation);
    table_pool.snapshot_generations(&table);
    assert(table[recycled.index] == recycled.generation);
    assert(table_pool.generation_valid(recycled));
    assert(!table_pool.generation_valid(live_ref));

    // A forged index is out of the table's range and rejected by both.
    const vg::core::FacetRef forged{9999, 1};
    assert(!table_pool.generation_valid(forged));
    assert(table_pool.lookup(table_arena, forged, &status) == nullptr);
    assert(status == vg::core::FacetStatus::UnknownIndex);
  }

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

    vg::hal::RepresentationRequest request;
    request.view = stage_view;
    request.target_kind = vg::core::FacetKind::Sample;

    const std::vector<vg::core::RepresentationRequest> stage_requests{request};
    vg::test_support::AssembledPlanFixture stage_fixture;
    vg::test_support::AssemblyOptions stage_options;
    stage_options.representation_requests = &stage_requests;
    stage_options.facet_pool = &stage_device->facet_pool();
    vg::hal::ExecutionPlan stage_plan;
    std::string stage_error;
    assert(vg::test_support::assemble_single_node_plan(
        stage_arena, stage_module.module,
        {vg::test_support::compute_task(stage_id, stage_generation)},
        &stage_fixture, &stage_plan, &stage_error, stage_options));

    vg::hal::CompiledPlan stage_compiled;
    assert(stage_device->compile(stage_plan, &stage_compiled, &stage_error));
    assert(stage_compiled.representation_supported);
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

    // ConsumeInput: rejected at compile(), with representation_supported
    // false, an Unsupported lowering event and a reason -- never accepted and
    // quietly not performed.
    auto consume_request = request;
    consume_request.consume_input = true;
    consume_request.consume_proof = discharged;
    const std::vector<vg::core::RepresentationRequest> consume_requests{consume_request};
    vg::test_support::AssembledPlanFixture consume_fixture;
    vg::test_support::AssemblyOptions consume_options;
    consume_options.representation_requests = &consume_requests;
    consume_options.facet_pool = &stage_device->facet_pool();
    vg::hal::ExecutionPlan consume_plan;
    assert(vg::test_support::assemble_single_node_plan(
        stage_arena, stage_module.module,
        {vg::test_support::compute_task(stage_id, stage_generation)},
        &consume_fixture, &consume_plan, &stage_error, consume_options));
    vg::hal::CompiledPlan consume_compiled;
    std::string consume_error;
    assert(!stage_device->compile(consume_plan, &consume_compiled, &consume_error));
    assert(!consume_compiled.representation_supported);
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
    vg::hal::ExecutionPlan unproven_plan;
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
    vg::hal::ExecutionPlan address_plan;
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
    vg::hal::ExecutionPlan racing_plan;
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
    vg::hal::ExecutionPlan plain_plan;
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

    vg::hal::RepresentationRequest stale_request;
    stale_request.view = stage_view;
    stale_request.view.allocation = stale_backing.id;
    stale_request.view.allocation_generation = stale_backing.generation;
    stale_request.target_kind = vg::core::FacetKind::Sample;

    const std::vector<vg::core::RepresentationRequest> stale_requests{stale_request};
    vg::test_support::AssembledPlanFixture stale_fixture;
    vg::test_support::AssemblyOptions stale_options;
    stale_options.representation_requests = &stale_requests;
    stale_options.facet_pool = &stage_device->facet_pool();
    vg::hal::ExecutionPlan stale_plan;
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

  // --- TASK-D1 / ADR-035: lease, budget, overflow are independent types.
  // Budget 0 ≠ unset. A lease cannot cover an unproven allocation.
  // Rejected overflow cannot answer continued(). ---
  {
    const auto unlimited = vg::core::WorkingSetBudget::unlimited();
    assert(!unlimited.has_limit);
    assert(unlimited.allows(0));
    assert(unlimited.allows(1ull << 40));

    const auto zero = vg::core::WorkingSetBudget::limited(0);
    assert(zero.has_limit);
    assert(zero.byte_limit == 0);
    assert(zero.allows(0));
    std::string budget_error;
    assert(!zero.allows(1, &budget_error));
    assert(budget_error == "working-set budget exceeded");
    assert(zero.has_limit != unlimited.has_limit);

    const vg::core::PointerRef proven_a{1, 1};
    const vg::core::PointerRef proven_b{2, 1};
    const vg::core::PointerRef stranger{3, 1};
    const std::vector<vg::core::PointerRef> proven{proven_a, proven_b};

    vg::core::WorkingSetLease lease;
    std::string lease_error;
    assert(lease.add(proven_a, proven, &lease_error));
    assert(lease.covers(proven_a));
    assert(!lease.covers(stranger));
    assert(!lease.add(stranger, proven, &lease_error));
    assert(lease_error == "lease cannot cover an unproven allocation");
    assert(!lease.covers(stranger));
    lease.allocations.push_back(stranger);
    assert(!lease.valid(proven, &lease_error));
    assert(lease_error == "lease cannot cover an unproven allocation");
    lease.allocations.pop_back();
    assert(lease.valid(proven, &lease_error));

    vg::core::EnvelopeOverflow unused;
    assert(unused.valid());
    assert(!unused.continued());
    unused.overflow_task_count = 3;
    assert(!unused.valid(&lease_error));
    assert(lease_error == "an unused overflow record cannot carry leftover work or a continuation token");

    vg::core::EnvelopeOverflow rejected;
    rejected.disposition = vg::core::EnvelopeOverflowDisposition::Rejected;
    rejected.overflow_task_count = 4;
    assert(rejected.valid());
    assert(!rejected.continued());
    rejected.continuation_token = 9;
    assert(!rejected.valid(&lease_error));
    assert(lease_error == "a rejected overflow cannot be marked continued");
    assert(!rejected.continued());

    vg::core::EnvelopeOverflow deferred;
    deferred.disposition = vg::core::EnvelopeOverflowDisposition::Deferred;
    assert(!deferred.valid(&lease_error));
    assert(lease_error == "a deferred overflow requires leftover work and a continuation token");
    deferred.overflow_task_count = 2;
    deferred.continuation_token = 11;
    assert(deferred.valid());
    assert(deferred.continued());

    vg::hal::ExecutionPlan plan;
    assert(!plan.working_set_budget.has_value());
    assert(!plan.working_set_lease.has_value());
    assert(!plan.pending_overflow.has_value());
    vg::hal::Submission submission;
    assert(!submission.envelope_overflow.has_value());

    auto ref_device = vg::reference::make_device_hal();
    assert(ref_device != nullptr);
    auto compiled = vg::compiler::compile_c_like("@node @effects store(1,0,4,7)");
    assert(compiled.ok);
    plan.module = compiled.module;
    plan.published = true;
    assert(plan.validate());

    plan.working_set_budget = vg::core::WorkingSetBudget::limited(16);
    plan.working_set_lease = vg::core::WorkingSetLease{};
    plan.working_set_lease->byte_limit = 32;
    std::string plan_error;
    assert(!plan.validate(&plan_error));
    assert(plan_error == "working-set lease exceeds the plan's working-set budget");
    plan.working_set_lease->byte_limit = 16;
    assert(plan.validate());

    plan.pending_overflow = rejected;
    assert(!plan.validate(&plan_error));
    assert(plan_error == "a rejected overflow cannot be marked continued");
    rejected.continuation_token = 0;
    plan.pending_overflow = rejected;
    assert(plan.validate());
  }

  {
    vg::core::Arena arena;
    auto& left = arena.allocate(32);
    auto& right = arena.allocate(32);
    auto& unused = arena.allocate(32);
    (void)unused;
    vg::core::Certificate parent;
    parent.ranges.push_back({left.id, 0, 16, vg::ir::Access::Read, 0});
    vg::core::Certificate child;
    child.ranges.push_back({left.id, 16, 16, vg::ir::Access::Write, 0});
    vg::core::Certificate composed;
    std::string compose_error;
    assert(vg::core::compose_certificates({parent, child}, &composed, &compose_error));
    assert(composed.covers(parent.ranges.front()));
    assert(composed.covers(child.ranges.front()));
    vg::ir::Effect forged{right.id, 0, 8, vg::ir::Access::Read, 0};
    assert(!composed.covers(forged));
    vg::core::Certificate empty_out;
    assert(!vg::core::compose_certificates({}, &empty_out, &compose_error));
    assert(compose_error == "certificate composition requires at least one child certificate");

    vg::core::GraphEpochBuilder left_builder(&arena);
    assert(left_builder.add_reference(arena, {left.id, left.generation}));
    vg::core::GraphEpoch left_epoch;
    assert(left_builder.seal(&left_epoch));
    vg::core::AccessCertificate left_cert;
    left_cert.mode = vg::core::AccessCertificateMode::DiscoverThenLease;
    left_cert.epoch = left_epoch;

    vg::core::GraphEpochBuilder right_builder(&arena);
    assert(right_builder.add_reference(arena, {right.id, right.generation}));
    vg::core::GraphEpoch right_epoch;
    assert(right_builder.seal(&right_epoch));
    vg::core::AccessCertificate right_cert;
    right_cert.mode = vg::core::AccessCertificateMode::DiscoverThenLease;
    right_cert.epoch = right_epoch;

    vg::core::AccessCertificate union_cert;
    bool exploded = false;
    assert(vg::core::compose_access_certificates(arena, {left_cert, right_cert}, &union_cert, &exploded,
                                                 &compose_error));
    assert(union_cert.epoch.references().size() == 2);
    assert(!exploded);
    assert(vg::core::certificate_covers_discovery_witness(union_cert, left_cert.epoch.references()));
    assert(vg::core::certificate_covers_discovery_witness(union_cert, right_cert.epoch.references()));
    std::vector<vg::core::PointerRef> forged_witness{{unused.id, unused.generation}};
    assert(!vg::core::certificate_covers_discovery_witness(union_cert, forged_witness, &compose_error));
    assert(compose_error == "discovery witness is not covered by the certificate");

    vg::core::GraphEpochBuilder unused_builder(&arena);
    assert(unused_builder.add_reference(arena, {unused.id, unused.generation}));
    vg::core::GraphEpoch unused_epoch;
    assert(unused_builder.seal(&unused_epoch));
    vg::core::AccessCertificate unused_cert;
    unused_cert.mode = vg::core::AccessCertificateMode::DiscoverThenLease;
    unused_cert.epoch = unused_epoch;

    vg::core::AccessCertificate exploded_cert;
    bool did_explode = false;
    assert(vg::core::compose_access_certificates(arena, {left_cert, right_cert, unused_cert}, &exploded_cert,
                                                 &did_explode, &compose_error));
    assert(exploded_cert.epoch.references().size() == 3);
    assert(did_explode);

    auto& extra = arena.allocate(8);
    vg::core::GraphEpochBuilder later_builder(&arena);
    assert(later_builder.add_reference(arena, {extra.id, extra.generation}));
    vg::core::GraphEpoch later_epoch;
    assert(later_builder.seal(&later_epoch));
    vg::core::AccessCertificate later_cert;
    later_cert.mode = vg::core::AccessCertificateMode::DiscoverThenLease;
    later_cert.epoch = later_epoch;
    vg::core::AccessCertificate mixed;
    assert(!vg::core::compose_access_certificates(arena, {left_cert, later_cert}, &mixed, nullptr, &compose_error));
    assert(compose_error == "cannot compose access certificates from different graph epochs");
  }

  return 0;
}
