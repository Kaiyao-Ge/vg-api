#include "backends/device_hal.h"
#include "backends/metal/metal_device_hal.h"
#include "ir/ir.h"
#include "../support/assembled_plan_fixture.h"

#include <array>
#include <cstring>
#include <iostream>
#include <string>

namespace {

vg::ir::Module make_probe_module(vg::core::Arena& arena) {
  const auto& allocation = arena.allocate(64);
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
      {allocation.id, 0, 64, vg::ir::Access::Read, allocation.representation_epoch});
  return module;
}

vg::core::CanonicalView rgba8_view(const vg::core::Allocation& allocation, uint32_t width,
                                   uint32_t height) {
  vg::core::CanonicalView view;
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  view.width = width;
  view.height = height;
  view.format = vg::core::PixelFormat::RGBA8Unorm;
  return view;
}

vg::core::CanonicalView depth_view(const vg::core::Allocation& allocation, uint32_t width,
                                   uint32_t height) {
  auto view = rgba8_view(allocation, width, height);
  view.format = vg::core::PixelFormat::Depth32Float;
  return view;
}

bool has_event(const vg::hal::LoweringReport& report, const char* operation) {
  for (const auto& event : report.events)
    if (event.operation == operation) return true;
  return false;
}

}  // namespace

int main() {
  auto device = vg::metal::make_device_hal();
  if (device == nullptr) {
    std::cerr << "identity-scene-root-cache: no Metal device available on this host\n";
    return 77;  // CTest SKIP_RETURN_CODE: the binary is valid but cannot run here.
  }

  constexpr uint32_t kExtent = 4;
  vg::core::Arena arena;
  auto& source = arena.allocate(kExtent * kExtent * 4);
  auto& target = arena.allocate(kExtent * kExtent * 4);
  auto& depth = arena.allocate(kExtent * kExtent * 4);
  source.bytes.assign(source.bytes.size(), 255);

  std::string error;
  vg::core::FacetRef source_ref, target_ref, depth_ref;
  if (!device->facet_pool().acquire(arena, rgba8_view(source, kExtent, kExtent),
                                    vg::core::FacetKind::Sample, &source_ref, &error) ||
      !device->facet_pool().acquire(arena, rgba8_view(target, kExtent, kExtent),
                                    vg::core::FacetKind::Attachment, &target_ref, &error) ||
      !device->facet_pool().acquire(arena, depth_view(depth, kExtent, kExtent),
                                    vg::core::FacetKind::Attachment, &depth_ref, &error)) {
    std::cerr << "identity-scene-root-cache: facet acquisition failed: " << error << "\n";
    return 1;
  }

  const std::array<vg::metal::RasterVertex, 3> triangle{{
      {-1.0f, 1.0f, 0.5f, 0.0f, 0.0f},
      {3.0f, 1.0f, 0.5f, 2.0f, 0.0f},
      {-1.0f, -3.0f, 0.5f, 0.0f, 2.0f},
  }};
  auto& vertices = arena.allocate(sizeof(triangle));
  std::memcpy(vertices.bytes.data(), triangle.data(), sizeof(triangle));
  vg::core::FacetRef vertex_ref;
  if (!device->facet_pool().acquire(
          arena, rgba8_view(vertices, static_cast<uint32_t>(vertices.bytes.size() / 4), 1),
          vg::core::FacetKind::Address, &vertex_ref, &error)) {
    std::cerr << "identity-scene-root-cache: vertex facet acquisition failed: " << error << "\n";
    return 1;
  }

  vg::core::TaskRecord task;
  task.kind = vg::core::TaskKind::Raster;
  task.raster_facets = {source_ref, target_ref};
  task.vertex_buffer_ref = vertex_ref;
  task.depth_attachment_ref = depth_ref;
  task.depth_test_enable = true;
  task.depth_write_enable = true;
  task.depth_compare_op = vg::core::DepthCompareOp::Less;
  task.raster_filter = vg::core::FilterMode::Nearest;
  task.raster_wrap = vg::core::WrapMode::Clamp;
  vg::hal::ExecutionPlan plan;
  vg::test_support::AssembledPlanFixture fixture;
  if (!vg::test_support::assemble_single_node_plan(arena, make_probe_module(arena), {task},
                                                    &fixture, &plan, &error)) {
    std::cerr << "identity-scene-root-cache: assembly failed: " << error << "\n";
    return 1;
  }
  vg::hal::CompiledPlan compiled;
  if (!device->compile(plan, &compiled, &error)) {
    std::cerr << "identity-scene-root-cache: compile failed: " << error << "\n";
    return 1;
  }

  vg::hal::Submission first;
  vg::hal::Submission second;
  if (!device->submit(compiled, arena, &first, &error) || !first.result.ok ||
      !device->submit(compiled, arena, &second, &error) || !second.result.ok) {
    std::cerr << "identity-scene-root-cache: legacy raster submit failed: " << error << "\n";
    return 1;
  }
  if (!has_event(first.report, "identity_scene_root_buffer_create") ||
      has_event(first.report, "identity_scene_root_buffer_reuse") ||
      !has_event(second.report, "identity_scene_root_buffer_reuse") ||
      has_event(second.report, "identity_scene_root_buffer_create")) {
    std::cerr << "identity-scene-root-cache: expected one creation then reuse in runtime reports\n";
    return 1;
  }

  std::cout << "identity-scene-root-cache: ok\n";
  return 0;
}
