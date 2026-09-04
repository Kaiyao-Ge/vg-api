#include "fixture.h"

namespace vg::tests::metal {

// All compile/submit paths in this vertical slice start from the public
// semantic assembly boundary.  Keep the legacy test body focused on the
// Metal observation it is making; this helper supplies the otherwise
// repetitive CodeObject/NodeTable/TaskGraph/Envelope plumbing without ever
// manufacturing Stage 0--5 sealing facts.
bool assemble_compute_plan(vg::core::Arena& arena, vg::ir::Module module,
                           std::vector<TaskRecord> tasks, vg::core::ExecutionPlan* out,
                           std::string* error, const vg::test_support::AssemblyOptions& options) {
  // The fixture owns the NodeTable only during assembly; the plan carries its
  // immutable resolved-node snapshot afterwards, exactly as production does.
  vg::test_support::AssembledPlanFixture fixture;
  return vg::test_support::assemble_single_node_plan(arena, std::move(module), tasks, &fixture, out, error, options);
}

bool assemble_user_raster_plan(vg::core::Arena& arena, const vg::ir::UserRasterShaderContract& shader,
                               std::vector<TaskRecord> tasks, vg::core::ExecutionPlan* out,
                               std::string* error, const vg::test_support::AssemblyOptions& options) {
  vg::test_support::AssembledPlanFixture fixture;
  return vg::test_support::assemble_single_user_raster_plan(arena, shader, tasks, &fixture, out, error, options);
}

TaskRecord probe_task(const vg::ir::Module& module) {
  TaskRecord task{};
  // Probe modules always contain a real allocation access.  Binding the task
  // root to it is deliberate: a compute Task root is part of assembler-owned
  // authority, rather than a backend fixture shortcut.
  task.root_allocation = module.instructions.front().allocation;
  task.root_generation = module.instructions.front().generation;
  task.x = task.y = task.z = 1;
  return task;
}

// A minimal single-load module. Its only purpose is to give compile()/
// submit() a valid linear compute package to run so the timeline/task-ring
// paths (which don't otherwise touch module semantics) can be exercised
// end to end; the loaded value itself is never inspected.
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
  module.declared_effects.push_back({allocation.id, 0, 64, vg::ir::Access::Read, allocation.representation_epoch});
  return module;
}

bool same_task(const TaskRecord& a, const TaskRecord& b) {
  return a.node_index == b.node_index && a.node_generation == b.node_generation &&
         a.root_allocation == b.root_allocation && a.root_generation == b.root_generation && a.x == b.x &&
         a.y == b.y && a.z == b.z && a.flags == b.flags && a.contract_index == b.contract_index &&
         a.payload_size == b.payload_size && a.payload_or_offset == b.payload_or_offset &&
         a.kind == b.kind && a.topology == b.topology &&
         a.raster_facets.source.index == b.raster_facets.source.index &&
         a.raster_facets.source.generation == b.raster_facets.source.generation &&
         a.raster_facets.target.index == b.raster_facets.target.index &&
         a.raster_facets.target.generation == b.raster_facets.target.generation &&
         a.vertex_buffer_ref.index == b.vertex_buffer_ref.index &&
         a.vertex_buffer_ref.generation == b.vertex_buffer_ref.generation &&
         a.index_buffer_ref.index == b.index_buffer_ref.index &&
         a.index_buffer_ref.generation == b.index_buffer_ref.generation &&
         a.index_count == b.index_count && a.raster_filter == b.raster_filter &&
         a.raster_wrap == b.raster_wrap && a.raster_tint[0] == b.raster_tint[0] &&
         a.raster_tint[1] == b.raster_tint[1] && a.raster_tint[2] == b.raster_tint[2] &&
         a.raster_tint[3] == b.raster_tint[3];
}

bool channels_close(const std::array<float, 4>& got, const std::array<float, 4>& want, float tol,
                    const char* label, const char* what) {
  for (int c = 0; c < 4; ++c) {
    if (std::fabs(got[c] - want[c]) <= tol) continue;
    std::cerr << label << ": " << what << " channel " << c << " got " << got[c] << " expected "
              << want[c] << "\n";
    return false;
  }
  return true;
}

void fill_subresource(vg::core::Allocation& allocation, const vg::core::CanonicalView& view,
                      uint32_t layer, uint32_t level, const std::array<uint8_t, 4>& rgba) {
  const uint64_t offset = view.subresource_byte_offset({layer, level});
  const uint32_t width = view.mip_width(level);
  const uint32_t height = view.mip_height(level);
  const uint64_t row = view.bytes_per_row(level);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint64_t texel = offset + static_cast<uint64_t>(y) * row + static_cast<uint64_t>(x) * 4;
      allocation.bytes[texel + 0] = rgba[0];
      allocation.bytes[texel + 1] = rgba[1];
      allocation.bytes[texel + 2] = rgba[2];
      allocation.bytes[texel + 3] = rgba[3];
    }
  }
}

std::vector<vg::reference::RasterVertex> to_reference_vertices(
    const std::vector<vg::metal::RasterVertex>& vertices) {
  std::vector<vg::reference::RasterVertex> out;
  out.reserve(vertices.size());
  for (const auto& vertex : vertices)
    out.push_back({vertex.x, vertex.y, vertex.z, vertex.u, vertex.v});
  return out;
}

vg::reference::RasterDesc to_reference_desc(const vg::metal::RasterDesc& desc) {
  vg::reference::RasterDesc out;
  out.attachment.load = static_cast<vg::reference::AttachmentLoadAction>(desc.attachment.load);
  out.attachment.store = static_cast<vg::reference::AttachmentStoreAction>(desc.attachment.store);
  out.attachment.clear_rgba = desc.attachment.clear_rgba;
  out.attachment.sample_count = desc.attachment.sample_count;
  out.attachment.subresource = {desc.attachment.subresource.layer, desc.attachment.subresource.level};
  out.filter = desc.filter;
  out.wrap = desc.wrap;
  out.source_lod = desc.source_lod;
  out.source_array_slice = desc.source_array_slice;
  out.tint = desc.tint;
  out.depth_attachment_ref = desc.depth_attachment_ref;
  out.depth_test_enable = desc.depth_test_enable;
  out.depth_write_enable = desc.depth_write_enable;
  out.depth_compare_op = desc.depth_compare_op;
  return out;
}

vg::core::ConsumeProof complete_consume_proof() {
  vg::core::ConsumeProof proof;
  proof.envelope_complete = true;
  proof.no_external_references = true;
  proof.no_replay_required = true;
  proof.failure_semantics_accepted = true;
  return proof;
}

vg::core::CanonicalView make_rgba8_view(const vg::core::Allocation& allocation, Extent2 extent) {
  vg::core::CanonicalView view;
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  view.format = vg::core::PixelFormat::RGBA8Unorm;
  view.dimension = vg::core::ViewDimension::Texture2D;
  view.width = extent.width;
  view.height = extent.height;
  return view;
}

vg::core::CanonicalView make_depth32_view(const vg::core::Allocation& allocation, Extent2 extent) {
  auto view = make_rgba8_view(allocation, extent);
  view.format = vg::core::PixelFormat::Depth32Float;
  return view;
}

// Metal Y-up clip space, uv (0,0) at the top-left of the source -- the same
// full-target quad the reference raster oracle uses, so the two backends
// receive identical vertices.
std::vector<vg::metal::RasterVertex> metal_fullscreen_quad() {
  const vg::metal::RasterVertex top_left{-1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
  const vg::metal::RasterVertex top_right{1.0f, 1.0f, 0.0f, 1.0f, 0.0f};
  const vg::metal::RasterVertex bottom_left{-1.0f, -1.0f, 0.0f, 0.0f, 1.0f};
  const vg::metal::RasterVertex bottom_right{1.0f, -1.0f, 0.0f, 1.0f, 1.0f};
  return {top_left, top_right, bottom_left, top_right, bottom_right, bottom_left};
}

// Builds a load-only module against `allocation` at a caller-chosen
// representation_epoch. Stage 5 runs before the interpreter and bumps the
// transformed allocation's epoch, so a module that still names the
// pre-transform epoch faults STALE_OR_BOUNDS. The consume-input cases below
// load a *separate* probe allocation that Stage 5 does not touch, so the
// module stays valid after the bump (and after ConsumeInput clears the
// image's host bytes).
vg::ir::Module make_epoch_probe_module(const vg::core::Allocation& allocation, uint32_t epoch) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/v1";
  vg::ir::Instruction load;
  load.op = "load";
  load.allocation = allocation.id;
  load.generation = allocation.generation;
  load.representation_epoch = epoch;
  load.offset = 0;
  load.size = 4;
  module.instructions.push_back(load);
  module.declared_effects.push_back({allocation.id, 0, 4, vg::ir::Access::Read, epoch});
  return module;
}

vg::core::Allocation& prepare_consume_image(vg::core::Arena& arena, vg::core::CanonicalView* view) {
    constexpr uint32_t kW = 2;
    constexpr uint32_t kH = 2;
    auto& allocation = arena.allocate(static_cast<uint64_t>(kW) * kH * 4);
    allocation.bytes = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
    };
    *view = make_rgba8_view(allocation, {.width = kW, .height = kH});
    return allocation;
}

}  // namespace vg::tests::metal
