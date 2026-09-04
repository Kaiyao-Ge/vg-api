#include "fixture.h"
#include "metal_adapter_harness.h"

namespace vg::tests::metal {

// Source Sample + target Attachment, full-screen quad, Metal Y-up. Interior
// pixels are compared against reference::raster_triangles at the E008 nearest
// tolerance (edge pixels are where Metal's per-pixel shade and the oracle's
// per-sample shade are allowed to disagree). Wrong kind, a vertex count that
// is not a multiple of 3, and sample_count>1 without MultisampleResolve fail.
bool run_basic_raster(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "basic-raster: no Metal device available on this host\n";
    return false;
  }

  constexpr uint32_t kExtent = 4;
  constexpr uint32_t kBytes = kExtent * kExtent * 4;
  vg::core::Arena arena;
  auto& source_alloc = arena.allocate(kBytes);
  auto& target_alloc = arena.allocate(kBytes);
  auto& depth_alloc = arena.allocate(kBytes);
  for (uint32_t y = 0; y < kExtent; ++y) {
    for (uint32_t x = 0; x < kExtent; ++x) {
      const uint64_t texel = (static_cast<uint64_t>(y) * kExtent + x) * 4;
      source_alloc.bytes[texel + 0] = static_cast<uint8_t>(10 + 40 * x);
      source_alloc.bytes[texel + 1] = static_cast<uint8_t>(20 + 40 * y);
      source_alloc.bytes[texel + 2] = static_cast<uint8_t>(30 + 8 * x + 16 * y);
      source_alloc.bytes[texel + 3] = 255;
    }
  }

  const vg::core::CanonicalView source_view = make_rgba8_view(source_alloc, {.width = kExtent, .height = kExtent});
  const vg::core::CanonicalView target_view = make_rgba8_view(target_alloc, {.width = kExtent, .height = kExtent});
  const vg::core::CanonicalView depth_view = make_depth32_view(depth_alloc, {.width = kExtent, .height = kExtent});

  vg::core::FacetPool pool;
  vg::core::FacetRef source_ref;
  vg::core::FacetRef target_ref;
  vg::core::FacetRef depth_ref;
  std::string error;
  if (!pool.acquire(arena, source_view, vg::core::FacetKind::Sample, &source_ref, &error) ||
      !pool.acquire(arena, target_view, vg::core::FacetKind::Attachment, &target_ref, &error)) {
    std::cerr << "basic-raster: acquire failed: " << error << "\n";
    return false;
  }

  vg::metal::RasterDesc desc;
  desc.filter = vg::core::FilterMode::Nearest;
  desc.wrap = vg::core::WrapMode::Clamp;
  desc.attachment.load = vg::metal::AttachmentLoadAction::Clear;
  desc.attachment.store = vg::metal::AttachmentStoreAction::Store;
  desc.attachment.clear_rgba = {0.0f, 0.0f, 0.0f, 1.0f};
  desc.attachment.sample_count = 1;

  const auto quad = metal_fullscreen_quad();
  vg::metal::RasterResult metal_result;
  if (!vg::metal::AdapterHarness(*metal_device).run_raster_triangles(arena, pool, {.source = source_ref, .target = target_ref}, desc, quad,
                                          &metal_result, &error)) {
    std::cerr << "basic-raster: Metal raster failed: " << error << "\n";
    return false;
  }
  auto oracle = vg::reference::raster_triangles(arena, pool, {.source = source_ref, .target = target_ref},
                                                to_reference_desc(desc), to_reference_vertices(quad));
  if (!oracle.ok) {
    std::cerr << "basic-raster: reference oracle failed: " << oracle.message << "\n";
    return false;
  }
  if (metal_result.resolved_rgba.size() != oracle.resolved_rgba.size() ||
      metal_result.width != kExtent || metal_result.height != kExtent) {
    std::cerr << "basic-raster: resolved image size mismatch\n";
    return false;
  }
  for (uint32_t y = 1; y + 1 < kExtent; ++y) {
    for (uint32_t x = 1; x + 1 < kExtent; ++x) {
      const size_t index = static_cast<size_t>(y) * kExtent + x;
      if (!channels_close(metal_result.resolved_rgba[index], oracle.resolved_rgba[index], kNearestTol,
                          "basic-raster", "interior pixel"))
        return false;
    }
  }

  vg::core::FacetRef wrong_source;
  vg::core::FacetRef wrong_target;
  if (!pool.acquire(arena, source_view, vg::core::FacetKind::Attachment, &wrong_source, &error) ||
      !pool.acquire(arena, target_view, vg::core::FacetKind::Sample, &wrong_target, &error)) {
    std::cerr << "basic-raster: wrong-kind acquire failed: " << error << "\n";
    return false;
  }
  vg::metal::RasterResult unused;
  if (vg::metal::AdapterHarness(*metal_device).run_raster_triangles(arena, pool, {.source = wrong_source, .target = target_ref}, desc, quad, &unused,
                                         &error)) {
    std::cerr << "basic-raster: Attachment source must be rejected\n";
    return false;
  }
  if (vg::metal::AdapterHarness(*metal_device).run_raster_triangles(arena, pool, {.source = source_ref, .target = wrong_target}, desc, quad, &unused,
                                         &error)) {
    std::cerr << "basic-raster: Sample target must be rejected\n";
    return false;
  }

  const std::vector<vg::metal::RasterVertex> dangling{quad[0], quad[1], quad[2], quad[3]};
  if (vg::metal::AdapterHarness(*metal_device).run_raster_triangles(arena, pool, {.source = source_ref, .target = target_ref}, desc, dangling, &unused,
                                         &error)) {
    std::cerr << "basic-raster: vertex count not a multiple of 3 must be rejected\n";
    return false;
  }

  vg::metal::RasterDesc msaa = desc;
  msaa.attachment.sample_count = 4;
  msaa.attachment.store = vg::metal::AttachmentStoreAction::Store;
  if (vg::metal::AdapterHarness(*metal_device).run_raster_triangles(arena, pool, {.source = source_ref, .target = target_ref}, msaa, quad, &unused,
                                         &error)) {
    std::cerr << "basic-raster: sample_count>1 without MultisampleResolve must be rejected\n";
    return false;
  }
  msaa.attachment.store = vg::metal::AttachmentStoreAction::MultisampleResolve;
  vg::metal::RasterResult resolved;
  if (!vg::metal::AdapterHarness(*metal_device).run_raster_triangles(arena, pool, {.source = source_ref, .target = target_ref}, msaa, quad, &resolved,
                                          &error)) {
    std::cerr << "basic-raster: sample_count=4 with MultisampleResolve failed: " << error << "\n";
    return false;
  }
  if (resolved.sample_count != 4) {
    std::cerr << "basic-raster: resolve must report sample_count 4\n";
    return false;
  }

  std::cout << "basic-raster: ok\n";
  return true;
}

}  // namespace vg::tests::metal
