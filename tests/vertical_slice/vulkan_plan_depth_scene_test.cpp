#include "../support/assembled_plan_fixture.h"
#include "backends/vulkan/vulkan_device_hal.h"
#include "vg_scene_root_layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Vertex {
  float x, y, z, u, v;
};
static_assert(sizeof(Vertex) == 5 * sizeof(float));

vg::core::CanonicalView view(const vg::core::Allocation &allocation,
                             vg::core::PixelFormat format, uint32_t width,
                             uint32_t height) {
  vg::core::CanonicalView result;
  result.allocation = allocation.id;
  result.allocation_generation = allocation.generation;
  result.format = format;
  result.dimension = vg::core::ViewDimension::Texture2D;
  result.width = width;
  result.height = height;
  return result;
}

vg::ir::Module scene_root_module(const vg::core::Allocation &root) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = VG_SCHEMA_SCENEROOTRASTER_CONTRACT_NAME;
  module.instructions = {{"load", root.id, 0, 4, 0, root.generation,
                          root.representation_epoch, 0, ""}};
  module.declared_effects = {{root.id, 0, VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE,
                              vg::ir::Access::Read, root.representation_epoch}};
  return module;
}

bool acquire(vg::hal::DeviceHal &device, vg::core::Arena &arena,
             const vg::core::Allocation &allocation,
             vg::core::PixelFormat format, uint32_t width, uint32_t height,
             vg::core::FacetKind kind, vg::core::FacetRef *out,
             std::string *error) {
  return device.facet_pool().acquire(
      arena, view(allocation, format, width, height), kind, out, error);
}

bool has_event(const vg::hal::LoweringReport &report, const char *operation) {
  for (const auto &event : report.events)
    if (event.operation == operation && event.count != 0)
      return true;
  return false;
}

bool run() {
  std::string error;
  auto device = vg::vulkan::make_device_hal(&error);
  if (!device) {
    std::cerr << "plan-depth-scene: Vulkan device required: " << error << "\n";
    return false;
  }

  constexpr uint32_t kExtent = 4;
  constexpr size_t kPixels = kExtent * kExtent;
  vg::core::Arena arena;
  auto &source = arena.allocate(kPixels * 4);
  auto &target = arena.allocate(kPixels * 4);
  auto &depth = arena.allocate(kPixels * sizeof(float));
  auto &vertices = arena.allocate(4 * sizeof(Vertex));
  auto &indices = arena.allocate(6 * sizeof(uint16_t));
  auto &root = arena.allocate(VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE);

  for (size_t pixel = 0; pixel < kPixels; ++pixel) {
    source.bytes[pixel * 4] = 255;
    source.bytes[pixel * 4 + 1] = 64;
    source.bytes[pixel * 4 + 2] = 32;
    source.bytes[pixel * 4 + 3] = 255;
  }
  const std::array<Vertex, 4> fullscreen{{
      {-1.0f, -1.0f, 0.25f, 0.0f, 0.0f},
      {1.0f, -1.0f, 0.25f, 1.0f, 0.0f},
      {-1.0f, 1.0f, 0.25f, 0.0f, 1.0f},
      {1.0f, 1.0f, 0.25f, 1.0f, 1.0f},
  }};
  const std::array<uint16_t, 6> elements{{0, 1, 2, 2, 1, 3}};
  std::memcpy(vertices.bytes.data(), fullscreen.data(), sizeof(fullscreen));
  std::memcpy(indices.bytes.data(), elements.data(), sizeof(elements));
  arena.mark_content_modified(source);
  arena.mark_content_modified(vertices);
  arena.mark_content_modified(indices);

  vg::core::FacetRef source_ref, target_ref, depth_ref, vertex_ref, index_ref;
  if (!acquire(*device, arena, source, vg::core::PixelFormat::RGBA8Unorm,
               kExtent, kExtent, vg::core::FacetKind::Sample, &source_ref,
               &error) ||
      !acquire(*device, arena, target, vg::core::PixelFormat::RGBA8Unorm,
               kExtent, kExtent, vg::core::FacetKind::Attachment, &target_ref,
               &error) ||
      !acquire(*device, arena, depth, vg::core::PixelFormat::Depth32Float,
               kExtent, kExtent, vg::core::FacetKind::Attachment, &depth_ref,
               &error) ||
      !acquire(*device, arena, vertices, vg::core::PixelFormat::RGBA8Unorm,
               sizeof(fullscreen) / 4, 1, vg::core::FacetKind::Address,
               &vertex_ref, &error) ||
      !acquire(*device, arena, indices, vg::core::PixelFormat::R16Uint,
               elements.size(), 1, vg::core::FacetKind::Address, &index_ref,
               &error)) {
    std::cerr << "plan-depth-scene: facet setup failed: " << error << "\n";
    return false;
  }

  VgSchemaLayout_SceneRootRaster scene{};
  scene.camera_clip_from_local[0] = 1.0f;
  scene.camera_clip_from_local[5] = 1.0f;
  scene.camera_clip_from_local[10] = 1.0f;
  scene.camera_clip_from_local[15] = 1.0f;
  scene.material.base_color[0] = 0.5f;
  scene.material.base_color[1] = 0.5f;
  scene.material.base_color[2] = 1.0f;
  scene.material.base_color[3] = 1.0f;
  scene.material.albedo = {source_ref.index, source_ref.generation};
  std::memcpy(root.bytes.data(), &scene, sizeof(scene));
  arena.mark_content_modified(root);

  vg::core::TaskRecord task{};
  task.kind = vg::core::TaskKind::Raster;
  task.root_allocation = root.id;
  task.root_generation = root.generation;
  task.raster_facets.target = target_ref;
  task.vertex_buffer_ref = vertex_ref;
  task.index_buffer_ref = index_ref;
  task.index_count = static_cast<uint32_t>(elements.size());
  task.depth_attachment_ref = depth_ref;
  task.depth_test_enable = true;
  task.depth_write_enable = true;
  task.depth_compare_op = vg::core::DepthCompareOp::Less;
  task.raster_filter = vg::core::FilterMode::Nearest;
  task.raster_wrap = vg::core::WrapMode::Clamp;

  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions options;
  options.facet_pool = &device->facet_pool();
  if (!vg::test_support::assemble_single_node_plan(
          arena, scene_root_module(root), {task}, &fixture, &plan, &error,
          options)) {
    std::cerr << "plan-depth-scene: assembly failed: " << error << "\n";
    return false;
  }

  if (std::find(plan.required_capabilities.begin(),
                plan.required_capabilities.end(),
                vg::core::CapabilityRequirement::IndexedBinding) ==
      plan.required_capabilities.end()) {
    std::cerr
        << "plan-depth-scene: indexed Raster did not seal IndexedBinding\n";
    return false;
  }

  const uint64_t target_epoch = target.content_epoch;
  const uint64_t depth_epoch = depth.content_epoch;
  const auto target_before = target.bytes;
  vg::hal::CompiledPlan compiled;
  if (!device->compile(plan, &compiled, &error)) {
    std::cerr << "plan-depth-scene: compile failed: " << error << "\n";
    return false;
  }
  if (compiled.per_node_packages.size() != 1 ||
      compiled.per_node_packages.front().kind !=
          vg::hal::CompiledPlan::NodePackageKind::Raster) {
    std::cerr
        << "plan-depth-scene: Stage 6 did not preserve the Raster NodeRef\n";
    return false;
  }
  vg::hal::Submission submission;
  if (!device->submit(compiled, arena, &submission, &error) ||
      !submission.result.ok) {
    std::cerr << "plan-depth-scene: submit failed: " << error << " "
              << submission.result.message << "\n";
    return false;
  }
  if (submission.raster_results.size() != 1 ||
      submission.published_tasks.size() != 1 ||
      submission.published_tasks.front().kind != vg::core::TaskKind::Raster ||
      submission.published_tasks.front().node_index != fixture.node.index ||
      submission.published_tasks.front().node_generation !=
          fixture.node.generation ||
      !has_event(submission.report, "vulkan_raster_draw")) {
    std::cerr
        << "plan-depth-scene: Stage 7 did not publish its sealed Raster task\n";
    return false;
  }
  const auto &result = submission.raster_results.front();
  const size_t center = 2 * kExtent + 2;
  const auto close = [](float actual, float expected) {
    return std::fabs(actual - expected) < 0.03f;
  };
  if (!result.stored || !result.contents_defined || result.width != kExtent ||
      result.height != kExtent || result.resolved_rgba.size() != kPixels ||
      result.resolved_depth.size() != kPixels ||
      !close(result.resolved_rgba[center][0], 0.5f) ||
      !close(result.resolved_rgba[center][1], 64.0f / 255.0f * 0.5f) ||
      !close(result.resolved_rgba[center][2], 32.0f / 255.0f) ||
      !close(result.resolved_rgba[center][3], 1.0f) ||
      !close(result.resolved_depth[center], 0.25f) ||
      target.content_epoch <= target_epoch ||
      depth.content_epoch <= depth_epoch || target.bytes == target_before) {
    std::cerr << "plan-depth-scene: color/depth readback or content "
                 "publication mismatch\n";
    return false;
  }
  for (const auto ref :
       {source_ref, target_ref, depth_ref, vertex_ref, index_ref})
    if (device->facet_pool().in_flight(ref) != 0) {
      std::cerr << "plan-depth-scene: submit leaked a facet lifetime hold\n";
      return false;
    }
  for (const auto *allocation :
       {&source, &target, &depth, &vertices, &indices, &root})
    if (allocation->in_flight != 0) {
      std::cerr
          << "plan-depth-scene: submit leaked an allocation lifetime hold\n";
      return false;
    }

  const uint64_t published_target_epoch = target.content_epoch;
  const uint64_t published_depth_epoch = depth.content_epoch;
  const auto published_target_bytes = target.bytes;
  const auto published_depth_bytes = depth.bytes;

  // Invalid triangle-list cardinality is rejected by the sole assembler before
  // Stage 6/7; it cannot publish work or modify existing attachments.
  vg::core::TaskRecord invalid = task;
  invalid.index_count = 5;
  vg::test_support::AssembledPlanFixture invalid_fixture;
  vg::core::ExecutionPlan invalid_plan;
  error.clear();
  if (vg::test_support::assemble_single_node_plan(
          arena, scene_root_module(root), {invalid}, &invalid_fixture,
          &invalid_plan, &error, options) ||
      error.find("triangle-list count") == std::string::npos ||
      target.content_epoch != published_target_epoch ||
      depth.content_epoch != published_depth_epoch ||
      target.bytes != published_target_bytes ||
      depth.bytes != published_depth_bytes) {
    std::cerr << "plan-depth-scene: invalid indexed task was not rejected "
                 "before submit: "
              << error << "\n";
    return false;
  }
  vg::core::TaskRecord bad_topology = task;
  bad_topology.topology = static_cast<vg::core::Topology>(1);
  vg::core::TaskGraphBuilder topology_builder;
  error.clear();
  if (topology_builder.append(bad_topology, &error) ||
      error.find("triangle-list") == std::string::npos) {
    std::cerr << "plan-depth-scene: unsupported topology was accepted: "
              << error << "\n";
    return false;
  }

  auto &layered_target = arena.allocate(kPixels * 4 * 2);
  auto layered_view =
      view(layered_target, vg::core::PixelFormat::RGBA8Unorm, kExtent, kExtent);
  layered_view.dimension = vg::core::ViewDimension::Texture2DArray;
  layered_view.array_layers = 2;
  vg::core::FacetRef layered_target_ref;
  if (!device->facet_pool().acquire(arena, layered_view,
                                    vg::core::FacetKind::Attachment,
                                    &layered_target_ref, &error))
    return false;
  vg::core::TaskRecord layered = task;
  layered.raster_facets.target = layered_target_ref;
  vg::test_support::AssembledPlanFixture layered_fixture;
  vg::core::ExecutionPlan layered_plan;
  error.clear();
  if (!vg::test_support::assemble_single_node_plan(
          arena, scene_root_module(root), {layered}, &layered_fixture,
          &layered_plan, &error, options))
    return false;
  const auto layered_before = layered_target.bytes;
  const auto layered_epoch = layered_target.content_epoch;
  vg::hal::CompiledPlan layered_compiled;
  if (device->compile(layered_plan, &layered_compiled, &error) ||
      error.find("single-mip, single-layer Texture2D") == std::string::npos ||
      layered_target.bytes != layered_before ||
      layered_target.content_epoch != layered_epoch) {
    std::cerr << "plan-depth-scene: layered attachment was not rejected in "
                 "Stage 6 without effects: "
              << error << "\n";
    return false;
  }

  std::cout << "plan-depth-scene: indexed SceneRoot + D32 Raster passed\n";
  return true;
}

} // namespace

int main() { return run() ? 0 : 1; }
