#include "raster_fixture.h"

namespace vg::tests::reference {

void test_raster_oracle() {
  // --- raster_triangles (05 §9's `region.attachment.store` on this backend):
  // a full-target quad sampling a Sample view into an Attachment view. With
  // Nearest filtering and matching extents the target pixel centres land
  // exactly on source texel centres, so the rendered image must equal what
  // sample_facet returns at those uvs -- the same comparison E008's image
  // check performs, stated as an equality instead of a tolerance. ---
  {
    vg::core::Arena arena;
    const uint64_t source_id = arena.allocate(64).id;
    const uint64_t target_id = arena.allocate(64).id;
    const vg::core::CanonicalView source = plain_view(source_id, {.width = 4, .height = 4});
    const vg::core::CanonicalView target = plain_view(target_id, {.width = 4, .height = 4});

    auto* source_allocation = arena.lookup(vg::core::PointerRef{source_id, 1});
    assert(source_allocation != nullptr);
    for (uint32_t y = 0; y < 4; ++y)
      for (uint32_t x = 0; x < 4; ++x) write_texel(*source_allocation, source, 0, 0, x, y, texel_pattern(x, y));

    vg::reference::RasterDesc desc;
    desc.filter = vg::core::FilterMode::Nearest;
    desc.wrap = vg::core::WrapMode::Clamp;
    desc.attachment.load = vg::reference::AttachmentLoadAction::Clear;
    desc.attachment.store = vg::reference::AttachmentStoreAction::Store;
    desc.attachment.clear_rgba = Rgba{0.0f, 0.0f, 0.0f, 1.0f};

    const auto quad = full_target_quad();
    const auto rendered = vg::reference::raster_triangles(arena, source, target, desc, quad);
    assert(rendered.ok);
    assert(rendered.width == 4 && rendered.height == 4);
    assert(rendered.stored && rendered.contents_defined);
    assert(rendered.sample_count == 1);
    // Every pixel is covered exactly once: the top-left fill rule gives the
    // shared TR->BL diagonal to one triangle only, so a doubled seam would
    // show up here as a count above 16.
    assert(rendered.covered_fragment_count == 16);
    assert(rendered.resolved_rgba.size() == 16);

    std::vector<vg::reference::SampleCoord> pixel_centres;
    for (uint32_t y = 0; y < 4; ++y)
      for (uint32_t x = 0; x < 4; ++x)
        pixel_centres.push_back({(static_cast<float>(x) + 0.5f) / 4.0f,
                                 (static_cast<float>(y) + 0.5f) / 4.0f, 0.0f, 0});
    const auto sampled = vg::reference::sample_facet(arena, source, desc.filter, desc.wrap, pixel_centres);
    assert(sampled.ok);
    for (size_t index = 0; index < 16; ++index) {
      assert(exact_match(rendered.resolved_rgba[index], sampled.sampled_rgba[index]));
      const auto x = static_cast<uint32_t>(index % 4);
      const auto y = static_cast<uint32_t>(index / 4);
      assert(exact_match(rendered.resolved_rgba[index], unorm(texel_pattern(x, y))));
    }

    // The per-draw tint multiplies the sampled fragment, then the store
    // requantizes -- so the expected value is the documented encode step
    // applied to (sample * tint), not the sample itself.
    vg::reference::RasterDesc tinted = desc;
    tinted.tint = Rgba{0.5f, 1.0f, 1.0f, 1.0f};
    const auto tinted_result = vg::reference::raster_triangles(arena, source, target, tinted, quad);
    assert(tinted_result.ok);
    for (size_t index = 0; index < 16; ++index) {
      Rgba expected{};
      for (size_t c = 0; c < 4; ++c) expected[c] = sampled.sampled_rgba[index][c] * tinted.tint[c];
      assert(exact_match(tinted_result.resolved_rgba[index], requantize(expected)));
    }

    // A vertex count that is not a multiple of 3 is not a triangle list.
    const auto dangling = vg::reference::raster_triangles(
        arena, source, target, desc, std::vector<vg::reference::RasterVertex>{quad[0], quad[1]});
    assert(!dangling.ok);
    assert(dangling.message.find("multiple of 3") != std::string::npos);
    // ...and an empty list is legal (zero triangles), not an error.
    const auto empty = vg::reference::raster_triangles(arena, source, target, desc,
                                                      std::vector<vg::reference::RasterVertex>{});
    assert(empty.ok);
    assert(empty.covered_fragment_count == 0);
  }

  // --- Reading the surface being written has no defined result, so it is
  // refused; but sharing an allocation between source and target is fine as
  // long as the slice/level differ, which is what keeps "generate level 1 from
  // level 0" expressible. ---
  {
    vg::core::Arena arena;
    const uint64_t backing_id = arena.allocate(84).id;
    vg::core::CanonicalView view = plain_view(backing_id, {.width = 4, .height = 4});
    view.mip_levels = 3;

    auto* backing = arena.lookup(vg::core::PointerRef{backing_id, 1});
    for (uint32_t y = 0; y < 4; ++y)
      for (uint32_t x = 0; x < 4; ++x) write_texel(*backing, view, 0, 0, x, y, texel_pattern(x, y));

    const auto quad = full_target_quad();

    vg::reference::RasterDesc self_read;
    self_read.filter = vg::core::FilterMode::Nearest;
    self_read.source_lod = 0.0f;
    self_read.attachment.subresource = {0, 0};
    const auto refused = vg::reference::raster_triangles(arena, view, view, self_read, quad);
    assert(!refused.ok);
    assert(refused.message.find("renders into") != std::string::npos);

    // A different mip level of the same allocation is allowed: level 0 ->
    // level 1 with Nearest picks source texel (2i+1, 2j+1) for target (i, j).
    vg::reference::RasterDesc downsample = self_read;
    downsample.attachment.subresource = {0, 1};
    downsample.attachment.load = vg::reference::AttachmentLoadAction::Clear;
    downsample.attachment.store = vg::reference::AttachmentStoreAction::Store;
    const auto generated = vg::reference::raster_triangles(arena, view, view, downsample, quad);
    assert(generated.ok);
    assert(generated.width == 2 && generated.height == 2);
    assert(generated.covered_fragment_count == 4);
    for (uint32_t y = 0; y < 2; ++y)
      for (uint32_t x = 0; x < 2; ++x)
        assert(exact_match(generated.resolved_rgba[y * 2 + x], unorm(texel_pattern(2 * x + 1, 2 * y + 1))));

    // Level 0 was only read, never written.
    const auto untouched = vg::reference::sample_facet(
        arena, view, vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp,
        std::vector<vg::reference::SampleCoord>{{0.125f, 0.125f, 0.0f, 0}});
    assert(untouched.ok);
    assert(exact_match(untouched.sampled_rgba[0], unorm(texel_pattern(0, 0))));
  }
}

void test_depth_oracle() {
  // F4: Depth32Float is attachment-only and the reference rasterizer applies
  // the complete compare-op table against its fixed 1.0 clear value.
  {
    vg::core::Arena arena;
    constexpr uint32_t kExtent = 4;
    auto& source_alloc = arena.allocate(kExtent * kExtent * 4);
    auto& color_alloc = arena.allocate(kExtent * kExtent * 4);
    auto& depth_alloc = arena.allocate(kExtent * kExtent * 4);
    const auto source = plain_view(source_alloc.id, {.width = kExtent, .height = kExtent});
    const auto color = plain_view(color_alloc.id, {.width = kExtent, .height = kExtent});
    auto depth = plain_view(depth_alloc.id, {.width = kExtent, .height = kExtent});
    depth.format = vg::core::PixelFormat::Depth32Float;
    fill_subresource(source_alloc, source, 0, 0, {255, 0, 0, 255});
    const auto quad = full_target_quad();
    std::string error;
    vg::core::FacetPool pool;
    vg::core::FacetRef rejected_depth_sample;
    assert(!pool.acquire(arena, depth, vg::core::FacetKind::Sample, &rejected_depth_sample, &error));
    vg::reference::RasterDesc desc;
    desc.attachment = vg::hal::f2_default_raster_attachment_config<vg::reference::AttachmentFacetDesc>();
    desc.filter = vg::core::FilterMode::Nearest;
    desc.depth_attachment = &depth;
    desc.depth_test_enable = true;
    desc.depth_write_enable = true;
    const std::array<std::pair<vg::core::DepthCompareOp, bool>, 8> table{{
        {vg::core::DepthCompareOp::Never, false}, {vg::core::DepthCompareOp::Less, true},
        {vg::core::DepthCompareOp::Equal, false}, {vg::core::DepthCompareOp::LessEqual, true},
        {vg::core::DepthCompareOp::Greater, false}, {vg::core::DepthCompareOp::NotEqual, true},
        {vg::core::DepthCompareOp::GreaterEqual, false}, {vg::core::DepthCompareOp::Always, true}}};
    for (const auto& [op, should_pass] : table) {
      desc.depth_compare_op = op;
      const auto rendered = vg::reference::raster_triangles(arena, source, color, desc, quad);
      assert(rendered.ok);
      assert((rendered.depth_passed_fragment_count != 0) == should_pass);
      float stored_depth{};
      std::memcpy(&stored_depth, depth_alloc.bytes.data(), sizeof(stored_depth));
      assert(stored_depth == (should_pass ? 0.0f : 1.0f));
    }
    // A disabled test accepts fragments irrespective of compare op; a disabled
    // write still leaves the task's 1.0 clear value observable.
    desc.depth_compare_op = vg::core::DepthCompareOp::Never;
    desc.depth_test_enable = false;
    desc.depth_write_enable = false;
    const auto no_test_no_write = vg::reference::raster_triangles(arena, source, color, desc, quad);
    assert(no_test_no_write.ok && no_test_no_write.depth_passed_fragment_count != 0);
    float unchanged_depth{};
    std::memcpy(&unchanged_depth, depth_alloc.bytes.data(), sizeof(unchanged_depth));
    assert(unchanged_depth == 1.0f);
    auto invalid_z = quad;
    invalid_z[0].z = 1.01f;
    assert(!vg::reference::raster_triangles(arena, source, color, desc, invalid_z).ok);
  }
}

}  // namespace vg::tests::reference
