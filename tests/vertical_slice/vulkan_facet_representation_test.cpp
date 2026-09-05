#include "../support/assembled_plan_fixture.h"
#include "../support/vulkan_adapter_harness.h"
#include "backends/reference/reference_executor.h"
#include "backends/vulkan/vulkan_device_hal.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr float kTol = 1.0f / 255.0f + 1e-4f;

bool close(const std::array<float, 4> &a, const std::array<float, 4> &b) {
  for (size_t i = 0; i != a.size(); ++i)
    if (!std::isfinite(a[i]) || !std::isfinite(b[i]) ||
        std::fabs(a[i] - b[i]) > kTol)
      return false;
  return true;
}

vg::core::CanonicalView image_view(const vg::core::Allocation &allocation) {
  vg::core::CanonicalView view;
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  view.format = vg::core::PixelFormat::RGBA8Unorm;
  view.dimension = vg::core::ViewDimension::Texture2D;
  view.width = 2;
  view.height = 2;
  return view;
}

vg::core::Allocation &image(vg::core::Arena &arena,
                            vg::core::CanonicalView *view) {
  auto &allocation = arena.allocate(16);
  allocation.bytes = {255, 0, 0,   255, 0,   255, 0,   255,
                      0,   0, 255, 255, 255, 255, 255, 255};
  *view = image_view(allocation);
  return allocation;
}

vg::ir::Module probe(const vg::core::Allocation &allocation) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.vulkan.d/probe";
  vg::ir::Instruction load;
  load.op = "load";
  load.allocation = allocation.id;
  load.generation = allocation.generation;
  load.representation_epoch = allocation.representation_epoch;
  load.size = 4;
  module.instructions.push_back(load);
  module.declared_effects.push_back({allocation.id, 0, 4, vg::ir::Access::Read,
                                     allocation.representation_epoch});
  return module;
}

vg::core::TaskRecord task(const vg::ir::Module &module) {
  vg::core::TaskRecord out{};
  out.root_allocation = module.instructions.front().allocation;
  out.root_generation = module.instructions.front().generation;
  out.x = out.y = out.z = 1;
  return out;
}

vg::core::ConsumeProof complete_proof() { return {true, true, true, true}; }

bool assemble_submit(vg::vulkan::DeviceHal &device, vg::core::Arena &arena,
                     const vg::ir::Module &module,
                     const vg::core::RepresentationRequest &request,
                     vg::hal::Submission *submission, std::string *error) {
  const std::vector<vg::core::RepresentationRequest> requests{request};
  vg::test_support::AssemblyOptions options;
  options.representation_requests = &requests;
  options.facet_pool = &device.facet_pool();
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  if (!vg::test_support::assemble_single_node_plan(
          arena, module, {task(module)}, &fixture, &plan, error, options))
    return false;
  vg::hal::CompiledPlan compiled;
  return device.compile(plan, &compiled, error) &&
         device.submit(compiled, arena, submission, error);
}

bool cpu_fixture() {
  vg::core::Arena arena;
  vg::core::CanonicalView view;
  auto &allocation = image(arena, &view);
  vg::core::FacetPool pool;
  vg::core::FacetRef ref{};
  std::string error;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &ref, &error))
    return false;
  const std::vector<std::array<float, 2>> uvs{
      {0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f}};
  const auto sampled = vg::reference::sample_facet(
      arena, pool, ref, vg::core::FilterMode::Nearest,
      vg::core::WrapMode::Clamp, uvs);
  if (!sampled.ok || sampled.sampled_rgba.size() != 3 ||
      !close(sampled.sampled_rgba[0], {1, 0, 0, 1}) ||
      !close(sampled.sampled_rgba[1], {0, 1, 0, 1}) ||
      !close(sampled.sampled_rgba[2], {0, 0, 1, 1})) {
    std::cerr << "cpu-fixture: Reference archive sampling mismatch\n";
    return false;
  }
  uint32_t epoch = 0;
  if (!arena.transform(allocation.id, allocation.generation, &epoch, &error) ||
      pool.retire_stale(arena) == 0) {
    std::cerr << "cpu-fixture: epoch fixture failed: " << error << "\n";
    return false;
  }
  vg::core::FacetStatus status = vg::core::FacetStatus::Ok;
  if (pool.lookup(arena, ref, &status) != nullptr ||
      status != vg::core::FacetStatus::Retired) {
    std::cerr << "cpu-fixture: retired stale facet token was not classified "
                 "Retired\n";
    return false;
  }
  // Actual Core assembler negatives: incomplete ConsumeInput proof and a
  // source-epoch FacetRef are both refused before any backend lowering.
  auto &probe_allocation = arena.allocate(64);
  vg::core::RepresentationRequest request;
  request.view = view;
  request.target_kind = vg::core::FacetKind::Sample;
  request.consume_input = true;
  const std::vector<vg::core::RepresentationRequest> requests{request};
  vg::test_support::AssemblyOptions options;
  options.representation_requests = &requests;
  options.facet_pool = &pool;
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  if (vg::test_support::assemble_single_node_plan(
          arena, probe(probe_allocation), {task(probe(probe_allocation))},
          &fixture, &plan, &error, options) ||
      error.find("proof is incomplete") == std::string::npos) {
    std::cerr
        << "cpu-fixture: assembler accepted incomplete ConsumeInput proof\n";
    return false;
  }
  request.consume_proof = complete_proof();
  vg::core::FacetRef live{};
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &live, &error))
    return false;
  const std::vector<vg::core::RepresentationRequest> live_requests{request};
  options.representation_requests = &live_requests;
  error.clear();
  if (vg::test_support::assemble_single_node_plan(
          arena, probe(probe_allocation), {task(probe(probe_allocation))},
          &fixture, &plan, &error, options) ||
      error.find("live FacetRef") == std::string::npos) {
    std::cerr << "cpu-fixture: assembler accepted live source facet\n";
    return false;
  }
  if (!pool.retire(live, &error) ||
      !arena.acquire(allocation.id, allocation.generation))
    return false;
  uint32_t rejected_epoch = 0;
  if (arena.transform(allocation.id, allocation.generation, &rejected_epoch,
                      &error) ||
      error.find("referenced in flight") == std::string::npos ||
      !arena.release(allocation.id, allocation.generation)) {
    std::cerr << "cpu-fixture: in-flight Core transform did not fail/release "
                 "correctly\n";
    return false;
  }
  if (allocation.in_flight != 0 || probe_allocation.in_flight != 0) {
    std::cerr
        << "cpu-fixture: a failed fixture left an allocation hold behind\n";
    return false;
  }
  std::cout << "cpu-fixture: Reference sampling + Core assembler "
               "proof/live-facet/in-flight negatives ok\n";
  return true;
}

bool facets() {
  auto device = vg::vulkan::make_device_hal();
  if (!device) {
    std::cerr << "Vulkan device required; no fallback\n";
    return false;
  }
  vg::core::Arena arena;
  vg::core::CanonicalView view;
  image(arena, &view);
  auto &pool = device->facet_pool();
  vg::core::FacetRef sample{};
  vg::core::FacetRef storage{};
  std::string error;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &sample,
                    &error) ||
      !pool.acquire(arena, view, vg::core::FacetKind::Storage, &storage,
                    &error))
    return false;
  const std::vector<std::array<float, 2>> uvs{{0.25f, 0.25f}, {0.75f, 0.75f}};
  const auto expected = vg::reference::sample_facet(
      arena, pool, sample, vg::core::FilterMode::Nearest,
      vg::core::WrapMode::Clamp, uvs);
  vg::vulkan::AdapterHarness harness(*device);
  vg::vulkan::SampleFacetResult first, second;
  if (!expected.ok ||
      !harness.run_sample_facet(
          arena, pool, sample, vg::core::FilterMode::Nearest,
          vg::core::WrapMode::Clamp, uvs, 0.0f, {},
          vg::core::ValidationProfile::CheckedNative, &first, &error) ||
      !harness.run_sample_facet(
          arena, pool, sample, vg::core::FilterMode::Nearest,
          vg::core::WrapMode::Clamp, uvs, 0.0f, {},
          vg::core::ValidationProfile::CheckedNative, &second, &error) ||
      !first.checked_generation || first.violation_count != 0 ||
      !second.facet_cache_hit ||
      first.sampled_rgba.size() != expected.sampled_rgba.size() ||
      !close(first.sampled_rgba[0], expected.sampled_rgba[0]) ||
      !close(first.sampled_rgba[1], expected.sampled_rgba[1])) {
    std::cerr << "facets: checked SampleFacet/cache/oracle failed: " << error
              << "\n";
    return false;
  }
  vg::vulkan::StorageFacetResult image_write, linear_write;
  const std::array<float, 4> value{0.25f, 0.5f, 0.75f, 1.0f};
  if (!harness.run_storage_facet(arena, pool, storage,
                                 vg::vulkan::StorageFacetTarget::Image, value,
                                 &image_write, &error) ||
      !harness.run_storage_facet(arena, pool, storage,
                                 vg::vulkan::StorageFacetTarget::LinearBuffer,
                                 value, &linear_write, &error) ||
      !close(image_write.written_rgba, value) ||
      !close(linear_write.written_rgba, value)) {
    std::cerr << "facets: StorageFacet image/linear failed: " << error << "\n";
    return false;
  }
  vg::vulkan::SampleFacetResult unsupported;
  if (harness.run_sample_facet(
          arena, pool, sample, vg::core::FilterMode::Nearest,
          vg::core::WrapMode::Clamp, uvs, 0.0f, {},
          vg::core::ValidationProfile::ReferenceStrict, &unsupported, &error)) {
    std::cerr << "facets: unsupported profile was silently lowered\n";
    return false;
  }
  return true;
}

bool representation(bool consume) {
  auto device = vg::vulkan::make_device_hal();
  if (!device) {
    std::cerr << "Vulkan device required; no fallback\n";
    return false;
  }
  vg::core::Arena arena;
  vg::core::CanonicalView view;
  auto &allocation = image(arena, &view);
  const std::vector<std::array<float, 2>> center{{0.25f, 0.25f}};
  const uint32_t epoch_before = allocation.representation_epoch;
  const auto expected =
      vg::reference::sample_facet(arena, view, vg::core::FilterMode::Nearest,
                                  vg::core::WrapMode::Clamp, center);
  auto &probe_allocation = arena.allocate(64);
  vg::core::RepresentationRequest request;
  request.view = view;
  request.target_kind = vg::core::FacetKind::Sample;
  request.consume_input = consume;
  request.consume_proof = complete_proof();
  vg::hal::Submission submission;
  std::string error;
  if (!expected.ok ||
      !assemble_submit(*device, arena, probe(probe_allocation), request,
                       &submission, &error) ||
      !submission.result.ok || !submission.representation_epoch.sealed() ||
      submission.representation_facets.size() != 1 ||
      allocation.representation_epoch <= epoch_before ||
      submission.new_backing_bytes == 0 ||
      (consume && submission.released_backing_bytes == 0) ||
      (!consume && submission.released_backing_bytes != 0)) {
    std::cerr << "representation: assembler/compile/submit transform failed: "
              << error << "\n";
    return false;
  }
  vg::vulkan::AdapterHarness harness(*device);
  vg::vulkan::SampleFacetResult sampled;
  if (!harness.run_sample_facet(
          arena, device->facet_pool(), submission.representation_facets.front(),
          vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp, center,
          0.0f, {}, vg::core::ValidationProfile::CheckedNative, &sampled,
          &error) ||
      !close(sampled.sampled_rgba.front(), expected.sampled_rgba.front())) {
    std::cerr
        << "representation: retained facet did not sample expected pixels: "
        << error << "\n";
    return false;
  }
  if (!sampled.facet_cache_hit) {
    std::cerr << "representation: post-transform sample did not reuse the "
                 "Stage-5 facet image cache\n";
    return false;
  }
  vg::vulkan::RepresentationPhysicalObservation physical;
  if (!harness.observe_representation_backing(
          arena, device->facet_pool(), submission.representation_facets.front(),
          &physical, &error) ||
      physical.retained_facet_backing_bytes == 0 ||
      physical.cached_facet_image_count == 0 ||
      (consume && physical.cached_linear_backing_bytes != 0) ||
      (!consume && physical.cached_linear_backing_bytes == 0)) {
    std::cerr << "representation: physical old/new backing observation failed: "
              << error << "\n";
    return false;
  }
  if (allocation.in_flight != 0 || probe_allocation.in_flight != 0)
    return false;
  if (consume && !allocation.bytes.empty()) {
    std::cerr << "consume-input: host linear backing was retained\n";
    return false;
  }
  // Failure paths must leave the source representation and holds intact. These
  // are assembled plans too: no test stamps an ExecutionPlan by hand.
  if (consume) {
    vg::core::Arena negative_arena;
    vg::core::CanonicalView negative_view;
    auto &negative_image = image(negative_arena, &negative_view);
    const uint32_t negative_epoch = negative_image.representation_epoch;
    const auto original = negative_image.bytes;
    auto &negative_probe = negative_arena.allocate(64);
    vg::core::RepresentationRequest incomplete;
    incomplete.view = negative_view;
    incomplete.target_kind = vg::core::FacetKind::Sample;
    incomplete.consume_input = true;
    std::string negative_error;
    vg::hal::Submission ignored;
    if (assemble_submit(*device, negative_arena, probe(negative_probe),
                        incomplete, &ignored, &negative_error) ||
        negative_image.bytes != original ||
        negative_image.representation_epoch != negative_epoch ||
        negative_error.find("proof is incomplete") == std::string::npos ||
        negative_image.in_flight != 0) {
      std::cerr << "consume-input: incomplete proof was accepted or changed "
                   "source state\n";
      return false;
    }
    // The external in-flight hold is a semantic refusal before physical Stage
    // 5. Releasing it afterwards proves failed work did not strand the hold.
    if (!negative_arena.acquire(negative_image.id, negative_image.generation))
      return false;
    vg::core::RepresentationRequest held = incomplete;
    held.consume_proof = complete_proof();
    negative_error.clear();
    if (assemble_submit(*device, negative_arena, probe(negative_probe), held,
                        &ignored, &negative_error) ||
        negative_image.bytes != original ||
        negative_image.representation_epoch != negative_epoch ||
        negative_error.find("referenced in flight") == std::string::npos ||
        negative_image.in_flight != 1 ||
        !negative_arena.release(negative_image.id, negative_image.generation)) {
      std::cerr << "consume-input: in-flight source was consumed or its hold "
                   "was not cleanly released\n";
      return false;
    }
    if (negative_image.in_flight != 0 || negative_probe.in_flight != 0)
      return false;
    // A module touching the same old representation is refused at compile,
    // before a partial physical transform can occur.
    vg::core::Arena overlap_arena;
    vg::core::CanonicalView overlap_view;
    auto &overlap = image(overlap_arena, &overlap_view);
    const uint32_t overlap_epoch = overlap.representation_epoch;
    vg::core::RepresentationRequest overlap_request;
    overlap_request.view = overlap_view;
    overlap_request.target_kind = vg::core::FacetKind::Sample;
    overlap_request.consume_input = true;
    overlap_request.consume_proof = complete_proof();
    vg::hal::Submission overlap_submission;
    negative_error.clear();
    if (assemble_submit(*device, overlap_arena, probe(overlap), overlap_request,
                        &overlap_submission, &negative_error) ||
        overlap.bytes.size() != 16 ||
        overlap.representation_epoch != overlap_epoch ||
        negative_error.find("also reads or writes") == std::string::npos) {
      std::cerr << "consume-input: same-allocation plan did not fail closed "
                   "before transform: "
                << negative_error << "\n";
      return false;
    }
  }
  return true;
}

bool raster() {
  auto device = vg::vulkan::make_device_hal();
  if (!device) {
    std::cerr << "Vulkan device required; no fallback\n";
    return false;
  }
  vg::core::Arena arena;
  vg::core::CanonicalView source_view, target_view;
  image(arena, &source_view);
  image(arena, &target_view);
  vg::core::FacetRef source{}, target{};
  std::string error;
  auto &pool = device->facet_pool();
  if (!pool.acquire(arena, source_view, vg::core::FacetKind::Sample, &source,
                    &error) ||
      !pool.acquire(arena, target_view, vg::core::FacetKind::Attachment,
                    &target, &error))
    return false;
  vg::vulkan::RasterPassDesc desc;
  desc.clear_rgba = {0.125f, 0.25f, 0.5f, 1.0f};
  vg::vulkan::RasterPassResult clear;
  if (!vg::vulkan::AdapterHarness(*device).run_raster_facet(
          arena, pool, target, source, desc, &clear, &error) ||
      clear.draw_count != 0 || !close(clear.resolved_rgba, desc.clear_rgba)) {
    std::cerr << "facet-raster: physical clear/readback failed: " << error
              << "\n";
    return false;
  }
  // Existing narrow harness vertex shape: xyuv floats, one triangle, no
  // SceneRoot/depth expansion. Constant UV makes the Reference SampleFacet
  // oracle an exact prediction of the first physical readback.
  desc.vertices = {{-1.0f, -1.0f, 0.25f, 0.25f},
                   {3.0f, -1.0f, 0.25f, 0.25f},
                   {-1.0f, 3.0f, 0.25f, 0.25f}};
  desc.tint = {0.5f, 1.0f, 1.0f, 1.0f};
  desc.clear_rgba = {0, 0, 0, 1};
  const std::vector<std::array<float, 2>> raster_uv{{0.25f, 0.25f}};
  const auto sampled = vg::reference::sample_facet(
      arena, pool, source, vg::core::FilterMode::Bilinear,
      vg::core::WrapMode::Clamp, raster_uv);
  if (!sampled.ok || sampled.sampled_rgba.empty()) {
    std::cerr << "facet-raster: Reference SampleFacet oracle failed\n";
    return false;
  }
  const std::array<float, 4> expected{
      sampled.sampled_rgba.front()[0] * desc.tint[0],
      sampled.sampled_rgba.front()[1] * desc.tint[1],
      sampled.sampled_rgba.front()[2] * desc.tint[2],
      sampled.sampled_rgba.front()[3] * desc.tint[3]};
  vg::vulkan::RasterPassResult result;
  if (!vg::vulkan::AdapterHarness(*device).run_raster_facet(
          arena, pool, target, source, desc, &result, &error) ||
      result.draw_count != 1 || !close(result.resolved_rgba, expected)) {
    std::cerr << "facet-raster: physical triangle/readback did not match "
                 "Reference SampleFacet oracle: "
              << error << "\n";
    return false;
  }
  return true;
}

bool classification() {
  auto device = vg::vulkan::make_device_hal();
  if (!device) {
    std::cerr << "Vulkan device required; no fallback\n";
    return false;
  }
  vg::vulkan::RasterPipelineVariant a, b;
  a.state = {{"viewport", vg::compiler::StateBlockKind::DynamicState, 1},
             {"tint", vg::compiler::StateBlockKind::ShaderVisibleData, 4}};
  b.state = {{"viewport", vg::compiler::StateBlockKind::DynamicState, 2},
             {"tint", vg::compiler::StateBlockKind::ShaderVisibleData, 7}};
  vg::vulkan::PipelineClassificationResult result;
  std::string error;
  if (!vg::vulkan::AdapterHarness(*device).run_pipeline_classification(
          {a, b}, &result, &error) ||
      result.naive_pipeline_count <= result.classified_pipeline_count ||
      result.classified_specializations.empty()) {
    std::cerr << "pipeline-classification: physical VkPipeline classification "
                 "failed: "
              << error << "\n";
    return false;
  }
  return true;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  const std::string mode = argv[1];
  if (mode == "cpu-fixture")
    return cpu_fixture() ? 0 : 1;
  if (mode == "facets")
    return facets() ? 0 : 1;
  if (mode == "representation")
    return representation(false) ? 0 : 1;
  if (mode == "consume-input")
    return representation(true) ? 0 : 1;
  if (mode == "facet-raster")
    return raster() ? 0 : 1;
  if (mode == "pipeline-classification")
    return classification() ? 0 : 1;
  return 2;
}
