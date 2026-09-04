#include "fixture.h"
#include "metal_adapter_harness.h"

namespace vg::tests::metal {

// E016: four catalog variants over standalone transforms (unbounded growth,
// backpressure reject, ConsumeInput watermark, drop/quality skip). Assert
// unbounded peak at 8 transforms exceeds the ConsumeInput peak, and that
// backpressure triggers at least once. fif==1 accepts 0 transforms because
// the allocation's initial representation already saturates the budget.
bool run_representation_churn(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "representation-churn: no Metal device available on this host\n";
    return false;
  }

  constexpr uint32_t kW = 2;
  constexpr uint32_t kH = 2;
  constexpr uint32_t kAttempts = 8;
  const auto seed = [](vg::core::Arena& arena, vg::core::CanonicalView* view,
                       std::vector<uint8_t>* bytes) -> vg::core::Allocation& {
    auto& allocation = arena.allocate(static_cast<uint64_t>(kW) * kH * 4);
    allocation.bytes = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
    };
    *view = make_rgba8_view(allocation, {.width = kW, .height = kH});
    if (bytes != nullptr) *bytes = allocation.bytes;
    return allocation;
  };

  uint64_t unbounded_peak = 0;
  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& allocation = seed(arena, &view, nullptr);
    arena.set_max_in_flight_representations(0);
    vg::core::FacetPool pool;
    uint64_t accumulated_new = 0;
    uint32_t last_live = allocation.live_representations;
    for (uint32_t i = 0; i < kAttempts; ++i) {
      vg::metal::RepresentationTransformResult result;
      std::string error;
      if (!vg::metal::AdapterHarness(*metal_device).run_representation_transform(arena, pool, view, vg::core::FacetKind::Sample,
                                                      &result, &error)) {
        std::cerr << "representation-churn: unbounded transform " << i << " failed: " << error << "\n";
        return false;
      }
      accumulated_new += result.new_backing_bytes;
      const uint64_t current = result.old_backing_bytes + accumulated_new;
      if (current > unbounded_peak) unbounded_peak = current;
      last_live = allocation.live_representations;
    }
    if (last_live <= 1) {
      std::cerr << "representation-churn: unbounded live_representations did not grow\n";
      return false;
    }
    std::cout << "representation-churn: unbounded ok (live=" << last_live
              << " peak=" << unbounded_peak << ")\n";
  }

  bool backpressure_triggered = false;
  const uint32_t fif_values[] = {1, 2, 4, 8};
  for (uint32_t fif : fif_values) {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    seed(arena, &view, nullptr);
    arena.set_max_in_flight_representations(fif);
    vg::core::FacetPool pool;
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < kAttempts; ++i) {
      vg::metal::RepresentationTransformResult result;
      std::string error;
      if (vg::metal::AdapterHarness(*metal_device).run_representation_transform(arena, pool, view, vg::core::FacetKind::Sample,
                                                     &result, &error)) {
        ++accepted;
        continue;
      }
      if (error.find("in-flight representation budget exceeded") == std::string::npos) {
        std::cerr << "representation-churn: backpressure fif=" << fif
                  << " refused without the budget error: " << error << "\n";
        return false;
      }
      backpressure_triggered = true;
    }
    if (fif == 1 && accepted != 0) {
      std::cerr << "representation-churn: fif=1 must accept 0 transforms, accepted " << accepted
                << "\n";
      return false;
    }
    std::cout << "representation-churn: backpressure fif=" << fif << " accepted " << accepted
              << " / " << kAttempts << "\n";
  }
  if (!backpressure_triggered) {
    std::cerr << "representation-churn: backpressure never triggered\n";
    return false;
  }

  uint64_t consume_peak = 0;
  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    std::vector<uint8_t> original;
    auto& allocation = seed(arena, &view, &original);
    arena.set_max_in_flight_representations(0);
    vg::core::FacetPool pool;
    const auto proof = complete_consume_proof();
    for (uint32_t i = 0; i < kAttempts; ++i) {
      vg::metal::RepresentationTransformResult result;
      std::string error;
      if (!vg::metal::AdapterHarness(*metal_device).run_representation_transform(arena, pool, view, vg::core::FacetKind::Sample,
                                                      &result, &error)) {
        std::cerr << "representation-churn: ConsumeInput transform " << i << " failed: " << error
                  << "\n";
        return false;
      }
      const uint64_t current = result.old_backing_bytes + result.new_backing_bytes;
      if (current > consume_peak) consume_peak = current;
      uint64_t released = 0;
      if (!arena.consume_representation(allocation.id, allocation.generation, result.new_epoch, proof,
                                        &released, &error)) {
        std::cerr << "representation-churn: consume_representation " << i << " failed: " << error
                  << "\n";
        return false;
      }
      // Host consume cannot see this adapter's Shared blit source. Drop it
      // now, before the next frame restores host bytes, so the device
      // watermark matches what consume_representation already handed back.
      vg::metal::AdapterHarness(*metal_device).reclaim_released_backing(arena);
      if (allocation.live_representations > 2) {
        std::cerr << "representation-churn: ConsumeInput live_representations="
                  << allocation.live_representations << " escaped the transform window\n";
        return false;
      }
      // Restore host bytes so the next standalone blit still has a linear
      // source -- consume_representation releases the superseded backing, which
      // is the point of the watermark, but the next frame still has to upload.
      allocation.bytes = original;
    }
    std::cout << "representation-churn: ConsumeInput ok (peak=" << consume_peak << ")\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    seed(arena, &view, nullptr);
    arena.set_max_in_flight_representations(2);
    vg::core::FacetPool pool;
    uint32_t skipped = 0;
    for (uint32_t frame = 0; frame < kAttempts; ++frame) {
      vg::metal::RepresentationTransformResult result;
      std::string error;
      if (vg::metal::AdapterHarness(*metal_device).run_representation_transform(arena, pool, view, vg::core::FacetKind::Sample,
                                                     &result, &error))
        continue;
      // drop/quality: skip the frame. Application policy, not a core API --
      // we do not release_representation to make room, and we do not retry.
      ++skipped;
    }
    if (skipped == 0) {
      std::cerr << "representation-churn: drop/quality never skipped a frame\n";
      return false;
    }
    std::cout << "representation-churn: drop/quality skipped " << skipped << " frames\n";
  }

  if (unbounded_peak <= consume_peak) {
    std::cerr << "representation-churn: unbounded peak at fif=8 (" << unbounded_peak
              << ") must exceed ConsumeInput peak (" << consume_peak << ")\n";
    return false;
  }

  std::cout << "representation-churn: ok\n";
  return true;
}

bool run_consume_fault_during() {
  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_consume_image(arena, &view);
    const auto original = image.bytes;
    const uint32_t generation = image.generation;
    const uint32_t epoch_before = image.representation_epoch;
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);
    vg::core::FacetPool pool;
    const std::vector<vg::core::RepresentationRequest> requests{request};
    vg::test_support::AssemblyOptions options;
    options.representation_requests = &requests;
    // This is an intentionally narrow Stage-7 physical-fault harness, so its
    // explicit pool is the one commit_representation_operations consumes; it
    // is not presented as DeviceHal::compile/submit.
    options.facet_pool = &pool;
    vg::core::ExecutionPlan plan;
    vg::hal::Submission submission;
    std::string error;
    if (!assemble_compute_plan(arena, module, {probe_task(module)}, &plan, &error, options)) {
      std::cerr << "consume-input: fault-during assembly failed: " << error << "\n";
      return false;
    }
    if (vg::hal::commit_representation_operations(
            plan, {{vg::hal::CompiledPlan::RepresentationOperation::CopyToPrivate, 0, "fault harness"}}, arena, pool,
            [](const vg::core::RepresentationSemanticPlanItem&, const vg::hal::CompiledPlan::PhysicalRepresentationOperation&, vg::core::FacetRef,
               vg::hal::RepresentationTransformCost*, std::string* physical_error) {
              if (physical_error) *physical_error = "injected physical transform fault";
              return false;
            },
            &submission, &error)) {
      std::cerr << "consume-input: fault-during must fail the physical step\n";
      return false;
    }
    if (error.find("injected physical transform fault") == std::string::npos) {
      std::cerr << "consume-input: fault-during refused for the wrong reason: " << error << "\n";
      return false;
    }
    // 02 §9: a fault is not transactional rollback. transform() already
    // published the new epoch before the physical step ran; consume must
    // not have happened, and the superseded host bytes must still be here.
    if (submission.consumed_allocation_count != 0 || submission.released_backing_bytes != 0) {
      std::cerr << "consume-input: fault-during must not consume (consumed="
                << submission.consumed_allocation_count
                << " released=" << submission.released_backing_bytes << ")\n";
      return false;
    }
    if (image.bytes != original || image.generation != generation ||
        image.state != vg::core::ObjectState::Active) {
      std::cerr << "consume-input: fault-during must keep the old host backing\n";
      return false;
    }
    if (image.representation_epoch != epoch_before + 1) {
      std::cerr << "consume-input: fault-during rolled back the published epoch (got "
                << image.representation_epoch << ")\n";
      return false;
    }
    if (arena.lookup(vg::core::RepresentationRef{image.id, generation, epoch_before}) != nullptr ||
        arena.lookup(vg::core::RepresentationRef{image.id, generation, image.representation_epoch}) == nullptr) {
      std::cerr << "consume-input: fault-during must leave the new epoch visible and the old one stale\n";
      return false;
    }
    std::cout << "consume-input: fault-during kept old backing (" << image.bytes.size()
              << " bytes), epoch advanced " << epoch_before << "->" << image.representation_epoch
              << ", consume did not run\n";
  }
  return true;
}

bool check_post_consume_sample(vg::metal::DeviceHal* metal_device, vg::core::Arena& arena,
                               const vg::hal::Submission& submission,
                               const std::vector<std::array<float, 2>>& uvs,
                               const vg::reference::SampleFacetResult& expected,
                               std::string& error) {
    vg::metal::SampleFacetResult sampled;
    if (!vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, metal_device->facet_pool(),
                                        submission.representation_facets[0],
                                        vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp, uvs,
                                        &sampled, &error)) {
      std::cerr << "consume-input: new facet must still sample after ConsumeInput: " << error << "\n";
      return false;
    }
    if (!channels_close(sampled.sampled_rgba[0], expected.sampled_rgba[0], kNearestTol, "consume-input",
                        "post-consume sample"))
      return false;
  return true;
}

}  // namespace vg::tests::metal
