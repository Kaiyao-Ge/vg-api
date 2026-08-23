#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "compiler/compiler.h"
#include "core/core.h"
#include "core/task_schema.h"
#include <cassert>

int main() {
  vg::core::Arena arena;
  auto& allocation = arena.allocate(16);
  assert(allocation.id == 1 && allocation.generation == 1);
  assert(arena.topology_epoch() == 1);
  assert(arena.lookup(allocation.id, allocation.generation) != nullptr);
  assert(arena.acquire(allocation.id, allocation.generation));
  std::string transform_error;
  assert(!arena.transform(allocation.id, allocation.generation, nullptr, &transform_error));
  assert(arena.release(allocation.id, allocation.generation));
  uint32_t representation_epoch = 0;
  assert(arena.transform(allocation.id, allocation.generation, &representation_epoch) && representation_epoch == 1);
  assert(arena.lookup(allocation.id, allocation.generation, representation_epoch) != nullptr);
  assert(!arena.transform(allocation.id, allocation.generation, 0, nullptr, &transform_error));
  assert(arena.consume(allocation.id, allocation.generation, representation_epoch, &transform_error));
  assert(arena.lookup(allocation.id, 1) == nullptr);
  auto& second_allocation = arena.allocate(16);


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
    bad_plan.capabilities = reference_device->capabilities();
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
    good_plan.capabilities = reference_device->capabilities();
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
    vg::hal::ExecutionPlan plan;
    plan.capabilities = reference_device->capabilities();
    plan.published = true;
    plan.timeline_signal = 5;

    vg::core::Arena fresh_arena;
    auto& fresh_root = fresh_arena.allocate(64);
    auto module_copy = compiled_module.module;
    module_copy.instructions[0].allocation = fresh_root.id;
    module_copy.declared_effects[0].allocation = fresh_root.id;
    module_copy.canonical_json = vg::ir::serialize_module(module_copy);

    vg::core::TaskGraphBuilder fresh_builder;
    vg::core::TaskRecord fresh_task0{}; fresh_task0.node_index = 0; fresh_task0.root_allocation = fresh_root.id;
    vg::core::TaskRecord fresh_task1{}; fresh_task1.node_index = 1; fresh_task1.root_allocation = fresh_root.id;
    assert(fresh_builder.append(fresh_task0));
    assert(fresh_builder.append(fresh_task1));
    assert(fresh_builder.add_dependency(0, 1));
    vg::core::TaskGraph fresh_graph;
    assert(fresh_builder.seal(&fresh_graph));
    assert(fresh_graph.publish());

    plan.module = module_copy;
    plan.task_graph = fresh_graph;
    plan.graph_epoch = fresh_arena.topology_epoch();

    vg::hal::CompiledPlan compiled;
    std::string submit_error;
    assert(reference_device->compile(plan, &compiled, &submit_error));
    vg::hal::Submission submission;
    assert(reference_device->submit(compiled, fresh_arena, &submission, &submit_error));
    assert(submission.result.ok);
    assert(submission.published_tasks.size() == 2);
    assert(submission.published_tasks[0].node_index == 0);
    assert(submission.published_tasks[1].node_index == 1);
    assert(submission.timeline_value == 5);

    // Next submission's wait is satisfied by the prior signal (real device
    // timeline state, not a plan-local passthrough).
    vg::hal::ExecutionPlan waiting_plan = plan;
    waiting_plan.task_graph = vg::core::TaskGraph{};
    waiting_plan.timeline_wait = 5;
    waiting_plan.timeline_signal = 10;
    vg::hal::CompiledPlan waiting_compiled;
    assert(reference_device->compile(waiting_plan, &waiting_compiled, &submit_error));
    vg::hal::Submission waiting_submission;
    assert(reference_device->submit(waiting_compiled, fresh_arena, &waiting_submission, &submit_error));
    assert(waiting_submission.result.ok);
    assert(waiting_submission.timeline_value == 10);

    // An unsatisfied wait faults honestly (submit() still returns true --
    // matching the Metal/Vulkan convention that submit() reports host-side
    // acceptance, while submission.result.ok reports execution outcome).
    vg::hal::ExecutionPlan stuck_plan = plan;
    stuck_plan.task_graph = vg::core::TaskGraph{};
    stuck_plan.timeline_wait = 999;
    stuck_plan.timeline_signal = 1000;
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

    vg::hal::ExecutionPlan cert_plan;
    cert_plan.capabilities = cert_device->capabilities();
    cert_plan.module = cert_module.module;
    cert_plan.published = true;
    cert_plan.requested_certificate_mode = vg::core::AccessCertificateMode::CertifiedPinned;

    vg::hal::CompiledPlan cert_compiled;
    std::string device_error;
    assert(cert_device->compile(cert_plan, &cert_compiled, &device_error));
    vg::hal::Submission cert_submission;
    assert(cert_device->submit(cert_compiled, cert_arena, &cert_submission, &device_error));
    assert(cert_submission.result.ok);
    assert(cert_submission.access_certificate.has_value());
    assert(cert_submission.access_certificate->mode == vg::core::AccessCertificateMode::CertifiedPinned);
    assert(cert_submission.access_certificate->epoch.references().size() == 1);

    vg::hal::ExecutionPlan unsupported_plan = cert_plan;
    unsupported_plan.requested_certificate_mode = vg::core::AccessCertificateMode::SoftwarePaged;
    vg::hal::CompiledPlan unsupported_compiled;
    std::string unsupported_error;
    assert(!cert_device->compile(unsupported_plan, &unsupported_compiled, &unsupported_error));
    assert(!unsupported_compiled.report.supported);
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

  return 0;
}
