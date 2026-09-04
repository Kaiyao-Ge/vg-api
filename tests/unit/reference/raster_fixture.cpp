#include "raster_fixture.h"

namespace vg::tests::reference {

Rgba unorm(const Bytes4& bytes) {
  return {static_cast<float>(bytes[0]) / 255.0f, static_cast<float>(bytes[1]) / 255.0f,
          static_cast<float>(bytes[2]) / 255.0f, static_cast<float>(bytes[3]) / 255.0f};
}

// RGBA8Unorm's documented encode step (06 §6.2, mirrored in
// reference_executor.h): round-to-nearest uint8(round(clamp(v,0,1) * 255)).
// Re-derived here so a test can state the exact value a stored float becomes.
Rgba requantize(const Rgba& rgba) {
  Bytes4 bytes{};
  for (size_t c = 0; c < 4; ++c) {
    const float clamped = std::min(1.0f, std::max(0.0f, rgba[c]));
    bytes[c] = static_cast<uint8_t>(std::lround(clamped * 255.0f));
  }
  return unorm(bytes);
}

bool exact_match(const Rgba& lhs, const Rgba& rhs) {
  for (size_t c = 0; c < 4; ++c)
    if (lhs[c] != rhs[c]) return false;
  return true;
}

// Used only where the oracle genuinely averages or blends (trilinear lod,
// multisample resolve): those results are order-of-summation dependent in the
// last bit, so a registered tolerance is the honest comparison (10 §6) rather
// than re-implementing the summation order in the test.
bool close_match(const Rgba& lhs, const Rgba& rhs) {
  for (size_t c = 0; c < 4; ++c)
    if (std::fabs(lhs[c] - rhs[c]) > 1.0e-6f) return false;
  return true;
}

size_t texel_offset(const vg::core::CanonicalView& view, uint32_t layer, uint32_t level, uint32_t x,
                    uint32_t y) {
  return static_cast<size_t>(view.subresource_byte_offset({layer, level}) +
                             static_cast<uint64_t>(y) * view.bytes_per_row(level) +
                             static_cast<uint64_t>(x) * vg::core::bytes_per_texel(view.format));
}

void write_texel(vg::core::Allocation& allocation, const vg::core::CanonicalView& view, uint32_t layer,
                 uint32_t level, uint32_t x, uint32_t y, const Bytes4& bytes) {
  const size_t offset = texel_offset(view, layer, level, x, y);
  assert(offset + 4 <= allocation.bytes.size());
  for (size_t c = 0; c < 4; ++c) allocation.bytes[offset + c] = bytes[c];
}

void fill_subresource(vg::core::Allocation& allocation, const vg::core::CanonicalView& view, uint32_t layer,
                      uint32_t level, const Bytes4& bytes) {
  for (uint32_t y = 0; y < view.mip_height(level); ++y)
    for (uint32_t x = 0; x < view.mip_width(level); ++x)
      write_texel(allocation, view, layer, level, x, y, bytes);
}

// A per-(slice, level) constant colour, distinct in every channel so a wrong
// subresource, a wrong array slice and a wrong swizzle each produce a visibly
// different value rather than a plausible one.
Bytes4 subresource_colour(uint32_t layer, uint32_t level) {
  return {static_cast<uint8_t>(10 + 40 * layer + 10 * level),
          static_cast<uint8_t>(60 + 40 * layer + 10 * level),
          static_cast<uint8_t>(110 + 40 * layer + 10 * level), 200};
}

// A per-texel pattern for the raster source, so a coverage or uv-interpolation
// error moves a pixel to a different colour instead of leaving the image flat.
Bytes4 texel_pattern(uint32_t x, uint32_t y) {
  return {static_cast<uint8_t>(10 + 40 * x), static_cast<uint8_t>(20 + 40 * y),
          static_cast<uint8_t>(30 + 8 * x + 16 * y), 255};
}

vg::core::CanonicalView plain_view(uint64_t allocation, Extent2 extent) {
  vg::core::CanonicalView view;
  view.allocation = allocation;
  view.allocation_generation = 1;
  view.format = vg::core::PixelFormat::RGBA8Unorm;
  view.dimension = vg::core::ViewDimension::Texture2D;
  view.width = extent.width;
  view.height = extent.height;
  return view;
}

// The full-target quad every raster case below draws: two triangles sharing the
// TR->BL diagonal, clip space (-1,-1)..(1,1) with uv (0,0) at the top-left,
// matching reference_executor.h's y-down target convention.
std::vector<vg::reference::RasterVertex> full_target_quad() {
  const vg::reference::RasterVertex top_left{-1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
  const vg::reference::RasterVertex top_right{1.0f, 1.0f, 0.0f, 1.0f, 0.0f};
  const vg::reference::RasterVertex bottom_left{-1.0f, -1.0f, 0.0f, 0.0f, 1.0f};
  const vg::reference::RasterVertex bottom_right{1.0f, -1.0f, 0.0f, 1.0f, 1.0f};
  return {top_left, top_right, bottom_left, top_right, bottom_right, bottom_left};
}

// A minimal canonical module carried by the built-in raster Node. The
// NodeRef-keyed raster package must not execute these instructions as a
// graph-wide compute pre-pass.
vg::ir::Module probe_module(vg::core::Arena& arena) {
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

}  // namespace vg::tests::reference
