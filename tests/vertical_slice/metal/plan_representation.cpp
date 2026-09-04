#include "fixture.h"

namespace vg::tests::metal {

bool compile_and_submit_representation(vg::metal::DeviceHal& metal_device, vg::core::Arena& arena,
                                       const vg::ir::Module& module,
                                       const vg::core::RepresentationRequest& request,
                                       vg::hal::Submission* submission, std::string* error) {
  const std::vector<vg::core::RepresentationRequest> requests{request};
  vg::test_support::AssemblyOptions options;
  options.representation_requests = &requests;
  options.facet_pool = &metal_device.facet_pool();
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  if (!assemble_compute_plan(arena, module, {probe_task(module)}, &plan, error, options)) return false;
  vg::hal::CompiledPlan compiled;
  if (!metal_device.compile(plan, &compiled, error)) return false;
  return metal_device.submit(compiled, arena, submission, error);
}

// E005 via compile()/submit() Stage 5 only. Standalone
// run_representation_transform never consumes (06 §11). multi-version keeps
// the old backing (released_backing_bytes==0); ConsumeInput with a complete
// proof releases it (allocation stays Active, generation unchanged, the new
// facet still samples). An incomplete proof is rejected by
// ExecutionPlan::validate, not inferred by the adapter.
bool run_consume_input(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "consume-input: no Metal device available on this host\n";
    return false;
  }



  const std::vector<std::array<float, 2>> uvs = {{0.25f, 0.25f}};

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    prepare_consume_image(arena, &view);
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);

    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = false;

    vg::hal::Submission submission;
    std::string error;
    if (!compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error)) {
      std::cerr << "consume-input: multi-version compile/submit failed: " << error << "\n";
      return false;
    }
    if (!submission.result.ok) {
      std::cerr << "consume-input: multi-version execution reported failure: "
                << submission.result.message << "\n";
      return false;
    }
    if (!submission.representation_epoch.sealed() || submission.representation_facets.size() != 1) {
      std::cerr << "consume-input: multi-version must seal one RepresentationEpoch facet\n";
      return false;
    }
    if (submission.old_backing_bytes == 0 || submission.released_backing_bytes != 0) {
      std::cerr << "consume-input: multi-version must keep old backing (old="
                << submission.old_backing_bytes << " released=" << submission.released_backing_bytes
                << ")\n";
      return false;
    }
    std::cout << "consume-input: multi-version ok (old=" << submission.old_backing_bytes
              << " new=" << submission.new_backing_bytes << " released=0)\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_consume_image(arena, &view);
    const uint64_t image_id = image.id;
    const uint32_t generation_before = image.generation;
    auto expected = vg::reference::sample_facet(arena, view, vg::core::FilterMode::Nearest,
                                                vg::core::WrapMode::Clamp, uvs);
    if (!expected.ok) {
      std::cerr << "consume-input: pre-submit oracle failed: " << expected.message << "\n";
      return false;
    }
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);

    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();

    vg::hal::Submission submission;
    std::string error;
    if (!compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error)) {
      std::cerr << "consume-input: ConsumeInput compile/submit failed: " << error << "\n";
      return false;
    }
    if (!submission.result.ok) {
      std::cerr << "consume-input: ConsumeInput execution reported failure: "
                << submission.result.message << "\n";
      return false;
    }
    if (submission.released_backing_bytes == 0 || submission.consumed_allocation_count != 1) {
      std::cerr << "consume-input: ConsumeInput must release the superseded backing (released="
                << submission.released_backing_bytes
                << " consumed_allocation_count=" << submission.consumed_allocation_count << ")\n";
      return false;
    }
    if (submission.released_backing_bytes != submission.old_backing_bytes) {
      std::cerr << "consume-input: released_backing_bytes (" << submission.released_backing_bytes
                << ") must equal old_backing_bytes (" << submission.old_backing_bytes << ")\n";
      return false;
    }
    const auto* after = arena.lookup(vg::core::PointerRef{image_id, generation_before});
    if (after == nullptr || after->state != vg::core::ObjectState::Active ||
        after->generation != generation_before) {
      std::cerr << "consume-input: allocation must stay Active at the same generation\n";
      return false;
    }
    if (submission.representation_facets.size() != 1) {
      std::cerr << "consume-input: ConsumeInput must publish exactly one live facet\n";
      return false;
    }
    if (!check_post_consume_sample(metal_device.get(), arena, submission, uvs, expected, error))
      return false;
    bool released_device_linear = false;
    for (const auto& event : submission.report.events) {
      if (event.operation == "consume_input_backing_release" && event.bytes != 0) released_device_linear = true;
    }
    if (!released_device_linear) {
      std::cerr << "consume-input: ConsumeInput must destroy the superseded linear device buffer, "
                   "not only the host bytes\n";
      return false;
    }
    std::cout << "consume-input: ConsumeInput ok (old=" << submission.old_backing_bytes
              << " new=" << submission.new_backing_bytes
              << " released=" << submission.released_backing_bytes << ")\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_consume_image(arena, &view);
    const auto module = make_epoch_probe_module(image, image.representation_epoch);
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    std::string compile_error;
    const std::vector<vg::core::RepresentationRequest> requests{request};
    vg::test_support::AssemblyOptions options;
    options.representation_requests = &requests;
    options.facet_pool = &metal_device->facet_pool();
    vg::core::ExecutionPlan plan;
    if (!assemble_compute_plan(arena, module, {probe_task(module)}, &plan, &compile_error, options)) {
      std::cerr << "consume-input: same-allocation plan assembly failed: " << compile_error << "\n";
      return false;
    }
    vg::hal::CompiledPlan compiled;
    if (metal_device->compile(plan, &compiled, &compile_error)) {
      std::cerr << "consume-input: ConsumeInput of an allocation the module also loads must fail compile\n";
      return false;
    }
    if (compile_error.find("whose linear representation this plan's compute module also reads or writes") ==
        std::string::npos) {
      std::cerr << "consume-input: same-allocation ConsumeInput was refused for the wrong reason: "
                << compile_error << "\n";
      return false;
    }
    std::cout << "consume-input: same-allocation ConsumeInput rejected at compile\n";
  }

  // Catalog fault-injection (09 E005): transform 前/中/后 fault;
  // capture replay request; 外部引用存在. These run the real rejection
  // paths and print what the program actually does -- they do not invent a
  // second Arena fault injector (existing IR poison stays scoped to
  // compile()/submit() instruction execution).
  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_consume_image(arena, &view);
    const auto original = image.bytes;
    const uint32_t generation = image.generation;
    const uint32_t epoch_before = image.representation_epoch;
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    if (!arena.acquire(image.id, generation)) {
      std::cerr << "consume-input: fault-before acquire failed\n";
      return false;
    }
    vg::hal::Submission submission;
    std::string error;
    if (compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error)) {
      std::cerr << "consume-input: fault-before must refuse while the allocation is in flight\n";
      return false;
    }
    if (error.find("representation epoch is referenced in flight") == std::string::npos) {
      std::cerr << "consume-input: fault-before refused for the wrong reason: " << error << "\n";
      return false;
    }
    if (image.bytes != original || image.generation != generation ||
        image.representation_epoch != epoch_before ||
        image.state != vg::core::ObjectState::Active) {
      std::cerr << "consume-input: fault-before must leave the old representation untouched\n";
      return false;
    }
    if (!arena.release(image.id, generation)) {
      std::cerr << "consume-input: fault-before release failed\n";
      return false;
    }
    std::cout << "consume-input: fault-before refused (in-flight), old backing kept ("
              << image.bytes.size() << " bytes, epoch=" << image.representation_epoch << ")\n";
  }

  if (!run_consume_fault_during()) return false;

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_consume_image(arena, &view);
    const uint32_t generation = image.generation;
    const uint32_t epoch_before = image.representation_epoch;
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    vg::hal::Submission submission;
    std::string error;
    if (!compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error) ||
        !submission.result.ok || submission.released_backing_bytes == 0) {
      std::cerr << "consume-input: fault-after setup ConsumeInput failed: " << error << "\n";
      return false;
    }
    const auto stale_module = make_epoch_probe_module(image, epoch_before);
    const auto stale = vg::reference::execute(stale_module, arena);
    if (stale.ok || stale.outputs_valid || stale.poison == vg::core::PoisonState::Valid ||
        stale.fault.code != "STALE_OR_BOUNDS") {
      std::cerr << "consume-input: fault-after old-epoch load must be STALE_OR_BOUNDS (ok="
                << stale.ok << " code=" << stale.fault.code << ")\n";
      return false;
    }
    if (!image.bytes.empty() || image.generation != generation ||
        image.state != vg::core::ObjectState::Active) {
      std::cerr << "consume-input: fault-after must not roll consume back\n";
      return false;
    }
    std::cout << "consume-input: fault-after old-epoch load " << stale.fault.code
              << ", consume not rolled back (released=" << submission.released_backing_bytes << ")\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_consume_image(arena, &view);
    const auto original = image.bytes;
    const uint32_t epoch_before = image.representation_epoch;
    const auto image_module = make_epoch_probe_module(image, epoch_before);
    const auto pre = vg::capture::make_capture(image_module, arena);
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    vg::hal::Submission submission;
    std::string error;
    if (!compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error) ||
        !submission.result.ok) {
      std::cerr << "consume-input: capture-replay setup ConsumeInput failed: " << error << "\n";
      return false;
    }
    vg::capture::ReplayResult pre_replay;
    if (!vg::capture::replay(pre, &pre_replay, &error) || !pre_replay.execution.ok) {
      std::cerr << "consume-input: pre-consume capture must still replay: " << error << " "
                << pre_replay.execution.message << "\n";
      return false;
    }
    const auto post = vg::capture::make_capture(image_module, arena);
    if (post.allocations.size() != 2) {
      std::cerr << "consume-input: post-consume capture allocation count=" << post.allocations.size()
                << "\n";
      return false;
    }
    uint64_t post_image_bytes = 0;
    for (const auto& snapshot : post.allocations) {
      if (snapshot.id == image.id) post_image_bytes = snapshot.bytes.size();
    }
    vg::capture::ReplayResult post_replay;
    if (vg::capture::replay(post, &post_replay, &error)) {
      std::cerr << "consume-input: post-consume capture must not be importable after bytes were released\n";
      return false;
    }
    if (error.find("cannot restore a consumed representation") == std::string::npos) {
      std::cerr << "consume-input: post-consume replay was refused for the wrong reason: " << error << "\n";
      return false;
    }
    std::cout << "consume-input: capture-replay pre-package ok, post-package lost " << original.size()
              << " linear bytes (snapshot now " << post_image_bytes << "), " << error << "\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_consume_image(arena, &view);
    vg::core::FacetRef live{};
    std::string error;
    if (!metal_device->facet_pool().acquire(arena, view, vg::core::FacetKind::Sample, &live, &error)) {
      std::cerr << "consume-input: live-facet acquire failed: " << error << "\n";
      return false;
    }
    if (!metal_device->facet_pool().begin_gpu_use(arena, live, &error)) {
      std::cerr << "consume-input: live-facet begin_gpu_use failed: " << error << "\n";
      return false;
    }
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    vg::hal::Submission submission;
    if (compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error)) {
      std::cerr << "consume-input: live-facet ConsumeInput must be refused while the token is held\n";
      return false;
    }
    // The live-facet snapshot is a Stage-5 semantic fact, so the assembler
    // must reject it before Metal lowering or commit is entered.
    if (error.find("live FacetRef names its source epoch") == std::string::npos) {
      std::cerr << "consume-input: live-facet was refused for the wrong reason: " << error << "\n";
      return false;
    }
    vg::core::FacetStatus status = vg::core::FacetStatus::Ok;
    if (metal_device->facet_pool().lookup(arena, live, &status) == nullptr) {
      std::cerr << "consume-input: live-facet token must still resolve after a refused consume (status="
                << vg::core::to_string(status) << ")\n";
      return false;
    }
    if (image.bytes.size() != 16) {
      std::cerr << "consume-input: live-facet refuse must keep the old backing\n";
      return false;
    }
    if (!metal_device->facet_pool().end_gpu_use(live, &error)) {
      std::cerr << "consume-input: live-facet end_gpu_use failed: " << error << "\n";
      return false;
    }
    std::cout << "consume-input: external live-facet token still live, ConsumeInput refused\n";
  }

  std::cout << "consume-input: ok\n";
  return true;
}

}  // namespace vg::tests::metal
