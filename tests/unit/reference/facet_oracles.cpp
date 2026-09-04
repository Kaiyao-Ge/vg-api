#include "raster_fixture.h"

namespace vg::tests::reference {

void test_sample_oracle() {
  // --- sample_facet with SampleCoord: E008 lists "2D/array/mip" as an input
  // axis, so the oracle has to address any subresource of a CanonicalView, and
  // an out-of-range slice/lod must be a rejection rather than a clamp -- a
  // clamp would turn a caller's indexing bug into a plausible reference value.
  {
    vg::core::Arena arena;
    const uint64_t backing_id = arena.allocate(168).id;

    vg::core::CanonicalView view = plain_view(backing_id, {.width = 4, .height = 4});
    view.dimension = vg::core::ViewDimension::Texture2DArray;
    view.array_layers = 2;
    view.mip_levels = 3;
    assert(view.valid());
    assert(view.byte_size() == 168);

    auto* backing = arena.lookup(vg::core::PointerRef{backing_id, 1});
    assert(backing != nullptr);
    for (uint32_t layer = 0; layer < view.array_layers; ++layer)
      for (uint32_t level = 0; level < view.mip_levels; ++level)
        fill_subresource(*backing, view, layer, level, subresource_colour(layer, level));

    // Nearest mip selection is the GL/Vulkan rule ceil(lod + 0.5) - 1, so an
    // exactly-half lod stays on the finer level.
    const std::vector<vg::reference::SampleCoord> coords{
        {0.5f, 0.5f, 0.0f, 0}, {0.5f, 0.5f, 0.5f, 0}, {0.5f, 0.5f, 1.0f, 0},
        {0.5f, 0.5f, 2.0f, 0}, {0.5f, 0.5f, 0.0f, 1}, {0.5f, 0.5f, 2.0f, 1},
    };
    const auto nearest = vg::reference::sample_facet(arena, view, vg::core::FilterMode::Nearest,
                                                    vg::core::WrapMode::Clamp, coords);
    assert(nearest.ok);
    assert(nearest.sampled_rgba.size() == coords.size());
    assert(exact_match(nearest.sampled_rgba[0], unorm(subresource_colour(0, 0))));
    assert(exact_match(nearest.sampled_rgba[1], unorm(subresource_colour(0, 0))));
    assert(exact_match(nearest.sampled_rgba[2], unorm(subresource_colour(0, 1))));
    assert(exact_match(nearest.sampled_rgba[3], unorm(subresource_colour(0, 2))));
    assert(exact_match(nearest.sampled_rgba[4], unorm(subresource_colour(1, 0))));
    assert(exact_match(nearest.sampled_rgba[5], unorm(subresource_colour(1, 2))));
    // lod 0 and lod 1 really do select different subresources.
    assert(!exact_match(nearest.sampled_rgba[0], nearest.sampled_rgba[2]));
    // ...and so do array slice 0 and slice 1 at the same lod.
    assert(!exact_match(nearest.sampled_rgba[0], nearest.sampled_rgba[4]));

    // A fractional lod under Bilinear is a real trilinear blend of two levels,
    // never silently rounded away (docs/START.md §4 invariant 10).
    const auto trilinear = vg::reference::sample_facet(
        arena, view, vg::core::FilterMode::Bilinear, vg::core::WrapMode::Clamp,
        std::vector<vg::reference::SampleCoord>{{0.5f, 0.5f, 0.5f, 0}});
    assert(trilinear.ok);
    const Rgba fine = unorm(subresource_colour(0, 0));
    const Rgba coarse = unorm(subresource_colour(0, 1));
    Rgba blended{};
    for (size_t c = 0; c < 4; ++c) blended[c] = fine[c] * 0.5f + coarse[c] * 0.5f;
    assert(close_match(trilinear.sampled_rgba[0], blended));
    assert(!close_match(trilinear.sampled_rgba[0], fine));

    // The legacy uv-only overload is exactly (lod 0, slice 0).
    const auto uv_only = vg::reference::sample_facet(
        arena, view, vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp,
        std::vector<std::array<float, 2>>{{0.5f, 0.5f}});
    assert(uv_only.ok);
    assert(exact_match(uv_only.sampled_rgba[0], unorm(subresource_colour(0, 0))));

    // Out-of-range slice/lod: rejected, named, and with no partial output the
    // caller could mistake for data.
    const auto bad_slice = vg::reference::sample_facet(
        arena, view, vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp,
        std::vector<vg::reference::SampleCoord>{{0.5f, 0.5f, 0.0f, 2}});
    assert(!bad_slice.ok);
    assert(bad_slice.sampled_rgba.empty());
    assert(bad_slice.message.find("array slice 2") != std::string::npos);
    assert(bad_slice.message.find("2 layers") != std::string::npos);

    const auto bad_lod = vg::reference::sample_facet(
        arena, view, vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp,
        std::vector<vg::reference::SampleCoord>{{0.5f, 0.5f, 3.0f, 0}});
    assert(!bad_lod.ok);
    assert(bad_lod.sampled_rgba.empty());
    assert(bad_lod.message.find("3 mip levels") != std::string::npos);

    const auto negative_lod = vg::reference::sample_facet(
        arena, view, vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp,
        std::vector<vg::reference::SampleCoord>{{0.5f, 0.5f, -1.0f, 0}});
    assert(!negative_lod.ok);

    // A rejection at coordinate N discards the whole batch rather than
    // returning N valid values and a message.
    const auto partial = vg::reference::sample_facet(
        arena, view, vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp,
        std::vector<vg::reference::SampleCoord>{{0.5f, 0.5f, 0.0f, 0}, {0.5f, 0.5f, 0.0f, 9}});
    assert(!partial.ok);
    assert(partial.sampled_rgba.empty());
    assert(partial.message.find("coordinate 1") != std::string::npos);
  }

  // --- Swizzle is part of the view contract, not sampler policy, and is
  // applied once to the *filtered* result (which is exact, because channel
  // selection commutes with linear filtering). Compared against the same
  // filtered sample taken through an identity-swizzle view, so the assertion
  // pins the permutation without re-deriving the filter. ---
  {
    vg::core::Arena arena;
    const uint64_t backing_id = arena.allocate(8).id;
    vg::core::CanonicalView view = plain_view(backing_id, {.width = 2, .height = 1});
    assert(view.valid() && view.byte_size() == 8);

    auto* backing = arena.lookup(vg::core::PointerRef{backing_id, 1});
    write_texel(*backing, view, 0, 0, 0, 0, Bytes4{0, 128, 255, 255});
    write_texel(*backing, view, 0, 0, 1, 0, Bytes4{255, 0, 64, 255});

    const std::vector<std::array<float, 2>> centre{{0.5f, 0.5f}};
    const auto identity = vg::reference::sample_facet(arena, view, vg::core::FilterMode::Bilinear,
                                                     vg::core::WrapMode::Clamp, centre);
    assert(identity.ok);
    const Rgba filtered = identity.sampled_rgba[0];
    // The two taps really did blend -- otherwise the swizzle assertion below
    // would hold vacuously on a single texel's bytes.
    assert(filtered[0] > 0.0f && filtered[0] < 1.0f);
    assert(filtered[0] != filtered[2]);

    vg::core::CanonicalView swizzled = view;
    swizzled.swizzle = {vg::core::Swizzle::Alpha, vg::core::Swizzle::Blue, vg::core::Swizzle::Red,
                        vg::core::Swizzle::One};
    assert(!swizzled.swizzle.identity());
    const auto permuted = vg::reference::sample_facet(arena, swizzled, vg::core::FilterMode::Bilinear,
                                                      vg::core::WrapMode::Clamp, centre);
    assert(permuted.ok);
    assert(exact_match(permuted.sampled_rgba[0],
                       Rgba{filtered[3], filtered[2], filtered[0], 1.0f}));

    // Zero/One are constants, not channel picks.
    vg::core::CanonicalView constants = view;
    constants.swizzle = {vg::core::Swizzle::Zero, vg::core::Swizzle::One, vg::core::Swizzle::Zero,
                         vg::core::Swizzle::One};
    const auto constant_result = vg::reference::sample_facet(
        arena, constants, vg::core::FilterMode::Bilinear, vg::core::WrapMode::Clamp, centre);
    assert(constant_result.ok);
    assert(exact_match(constant_result.sampled_rgba[0], Rgba{0.0f, 1.0f, 0.0f, 1.0f}));
  }
}

void test_storage_attachment_oracles() {
  // --- storage_facet (06 §6.2): a write, then a readback of that very texel
  // through the sampler, so the test states the value that survived storage
  // rather than the value it asked for. A non-identity swizzle is refused --
  // a swizzle reinterprets a shader *read*, so there is no defined meaning for
  // it on an image write, and inventing one would make this oracle disagree
  // with the Metal backend that refuses for the same reason. ---
  {
    vg::core::Arena arena;
    const uint64_t backing_id = arena.allocate(168).id;
    vg::core::CanonicalView view = plain_view(backing_id, {.width = 4, .height = 4});
    view.dimension = vg::core::ViewDimension::Texture2DArray;
    view.array_layers = 2;
    view.mip_levels = 3;

    auto* backing = arena.lookup(vg::core::PointerRef{backing_id, 1});
    for (uint32_t layer = 0; layer < view.array_layers; ++layer)
      for (uint32_t level = 0; level < view.mip_levels; ++level)
        fill_subresource(*backing, view, layer, level, subresource_colour(layer, level));

    const vg::reference::StorageTexel target{1, 2, 1, 0};
    const Rgba value{0.0f, 1.0f, 100.0f / 255.0f, 1.0f};
    const auto written = vg::reference::storage_facet(arena, view, target, value);
    assert(written.ok);
    assert(exact_match(written.written_rgba, requantize(value)));

    // Read the same texel back through the sampler: level 0 is 4x4, so texel
    // (1, 2)'s centre is uv ((1 + 0.5) / 4, (2 + 0.5) / 4).
    const auto readback = vg::reference::sample_facet(
        arena, view, vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp,
        std::vector<vg::reference::SampleCoord>{{0.375f, 0.625f, 0.0f, 1}});
    assert(readback.ok);
    assert(exact_match(readback.sampled_rgba[0], written.written_rgba));

    // The write touched exactly one texel of exactly one subresource.
    const auto neighbour = vg::reference::sample_facet(
        arena, view, vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp,
        std::vector<vg::reference::SampleCoord>{{0.125f, 0.625f, 0.0f, 1}, {0.375f, 0.625f, 0.0f, 0}});
    assert(neighbour.ok);
    assert(exact_match(neighbour.sampled_rgba[0], unorm(subresource_colour(1, 0))));
    assert(exact_match(neighbour.sampled_rgba[1], unorm(subresource_colour(0, 0))));

    vg::core::CanonicalView swizzled = view;
    swizzled.swizzle.red = vg::core::Swizzle::Alpha;
    const auto refused = vg::reference::storage_facet(arena, swizzled, target, value);
    assert(!refused.ok);
    assert(refused.message.find("non-identity swizzle") != std::string::npos);

    // Out-of-range texel, slice and level are all malformed calls, not clamps.
    assert(!vg::reference::storage_facet(arena, view, vg::reference::StorageTexel{4, 0, 0, 0}, value).ok);
    assert(!vg::reference::storage_facet(arena, view, vg::reference::StorageTexel{0, 0, 2, 0}, value).ok);
    assert(!vg::reference::storage_facet(arena, view, vg::reference::StorageTexel{0, 0, 0, 3}, value).ok);
    // Level 2 is 1x1, so (1, 0) is outside it even though it is inside level 0.
    const auto outside_level = vg::reference::storage_facet(
        arena, view, vg::reference::StorageTexel{1, 0, 0, 2}, value);
    assert(!outside_level.ok);
    assert(outside_level.message.find("outside mip level 2") != std::string::npos);
  }

  // --- attachment_facet (06 §6.3): load/store/resolve are per-use lowering
  // parameters, not public object state. DontCare is modelled explicitly via
  // contents_defined instead of being pretended away, and sample_count > 1
  // without MultisampleResolve is refused because a multisample attachment
  // with a single-sample store has no defined resolution. ---
  {
    vg::core::Arena arena;
    const uint64_t backing_id = arena.allocate(84).id;
    vg::core::CanonicalView view = plain_view(backing_id, {.width = 4, .height = 4});
    view.mip_levels = 3;
    assert(view.valid() && view.byte_size() == 84);

    auto* backing = arena.lookup(vg::core::PointerRef{backing_id, 1});
    const Bytes4 preexisting{11, 22, 33, 44};
    fill_subresource(*backing, view, 0, 1, preexisting);

    vg::reference::AttachmentFacetDesc desc;
    desc.subresource = {0, 1};  // level 1 of a 4x4 view is 2x2

    // A DontCare load leaves the previous bytes visible, but the contract does
    // not define what a reader sees -- so the oracle returns them *and* says
    // they must not be used as an expectation.
    desc.load = vg::reference::AttachmentLoadAction::DontCare;
    desc.store = vg::reference::AttachmentStoreAction::Store;
    const auto dont_care_load = vg::reference::attachment_facet(arena, view, desc);
    assert(dont_care_load.ok);
    assert(dont_care_load.width == 2 && dont_care_load.height == 2);
    assert(dont_care_load.resolved_rgba.size() == 4);
    assert(dont_care_load.stored);
    assert(!dont_care_load.contents_defined);
    assert(exact_match(dont_care_load.resolved_rgba[0], unorm(preexisting)));

    // Clear + Store: the whole subresource becomes the clear colour, and
    // resolved_rgba is decoded back out of the allocation so it carries the
    // format's quantization.
    const Rgba clear_colour{0.0f, 100.0f / 255.0f, 1.0f, 200.0f / 255.0f};
    desc.load = vg::reference::AttachmentLoadAction::Clear;
    desc.clear_rgba = clear_colour;
    const auto cleared = vg::reference::attachment_facet(arena, view, desc);
    assert(cleared.ok && cleared.stored && cleared.contents_defined);
    assert(cleared.sample_count == 1);
    for (const auto& pixel : cleared.resolved_rgba) assert(exact_match(pixel, requantize(clear_colour)));

    // Load reads back exactly what Clear stored.
    desc.load = vg::reference::AttachmentLoadAction::Load;
    const auto loaded = vg::reference::attachment_facet(arena, view, desc);
    assert(loaded.ok && loaded.stored && loaded.contents_defined);
    assert(loaded.resolved_rgba == cleared.resolved_rgba);

    // A DontCare store leaves memory untouched: the in-pass floats come back,
    // contents_defined is false, and a following Load still sees the old bytes.
    vg::reference::AttachmentFacetDesc discarded = desc;
    discarded.load = vg::reference::AttachmentLoadAction::Clear;
    discarded.clear_rgba = Rgba{1.0f, 0.0f, 0.0f, 1.0f};
    discarded.store = vg::reference::AttachmentStoreAction::DontCare;
    const auto not_stored = vg::reference::attachment_facet(arena, view, discarded);
    assert(not_stored.ok);
    assert(!not_stored.stored);
    assert(!not_stored.contents_defined);
    assert(exact_match(not_stored.resolved_rgba[0], discarded.clear_rgba));
    const auto after_discard = vg::reference::attachment_facet(arena, view, desc);
    assert(after_discard.ok);
    assert(after_discard.resolved_rgba == cleared.resolved_rgba);

    // sample_count > 1 requires MultisampleResolve.
    vg::reference::AttachmentFacetDesc msaa = desc;
    msaa.load = vg::reference::AttachmentLoadAction::Clear;
    msaa.clear_rgba = clear_colour;
    msaa.sample_count = 4;
    const auto unresolved = vg::reference::attachment_facet(arena, view, msaa);
    assert(!unresolved.ok);
    assert(unresolved.message.find("MultisampleResolve") != std::string::npos);

    msaa.store = vg::reference::AttachmentStoreAction::MultisampleResolve;
    const auto resolved = vg::reference::attachment_facet(arena, view, msaa);
    assert(resolved.ok && resolved.stored && resolved.contents_defined);
    assert(resolved.sample_count == 4);
    // A 4x clear averages back to the clear colour: a resolve is an average,
    // not a copy of sample 0.
    for (const auto& pixel : resolved.resolved_rgba) assert(close_match(pixel, requantize(clear_colour)));

    // MultisampleResolve at sample_count 1 is the exact identity resolve.
    vg::reference::AttachmentFacetDesc identity_resolve = msaa;
    identity_resolve.sample_count = 1;
    const auto identity = vg::reference::attachment_facet(arena, view, identity_resolve);
    assert(identity.ok && identity.sample_count == 1);
    assert(exact_match(identity.resolved_rgba[0], requantize(clear_colour)));

    // A sample count with no standard pattern is refused, not approximated.
    vg::reference::AttachmentFacetDesc odd = msaa;
    odd.sample_count = 3;
    const auto odd_result = vg::reference::attachment_facet(arena, view, odd);
    assert(!odd_result.ok);
    assert(odd_result.message.find("standard sample pattern") != std::string::npos);

    // An out-of-range subresource is a malformed call.
    vg::reference::AttachmentFacetDesc out_of_range = desc;
    out_of_range.subresource = {0, 3};
    assert(!vg::reference::attachment_facet(arena, view, out_of_range).ok);
    out_of_range.subresource = {1, 0};  // this view declares a single layer
    assert(!vg::reference::attachment_facet(arena, view, out_of_range).ok);
  }
}

}  // namespace vg::tests::reference
