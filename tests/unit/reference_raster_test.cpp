// Unit coverage for the reference backend's Phase C facet oracles:
// sample_facet (mip/array/lod/swizzle), storage_facet, attachment_facet and
// raster_triangles. These are the correctness oracles E008's SampleFacet row
// and E013's image comparison are judged against (09-experiment-catalog.md;
// 10 §11 makes correctness zero-tolerance, so the judge has to be exact about
// its own rules), and 05 §9 is what assigns `region.sample` /
// `region.attachment.store` a software sampler and software raster target on
// this backend.
//
// Allocation bytes are filled here through CanonicalView's own layout contract
// (02 §3.3, slice-major then ascending mip, tightly packed) so every expected
// colour is exact rather than approximated: RGBA8Unorm decodes as byte/255.0f,
// which is bit-identical on both sides of the comparison.
//
// Assert-based like tests/unit/core_test.cpp -- no test framework.
#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "core/core.h"
#include "ir/ir.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Bytes4 = std::array<uint8_t, 4>;
using Rgba = std::array<float, 4>;

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

struct Extent2 {
  uint32_t width{};
  uint32_t height{};
};

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
  const vg::reference::RasterVertex top_left{-1.0f, 1.0f, 0.0f, 0.0f};
  const vg::reference::RasterVertex top_right{1.0f, 1.0f, 1.0f, 0.0f};
  const vg::reference::RasterVertex bottom_left{-1.0f, -1.0f, 0.0f, 1.0f};
  const vg::reference::RasterVertex bottom_right{1.0f, -1.0f, 1.0f, 1.0f};
  return {top_left, top_right, bottom_left, top_right, bottom_right, bottom_left};
}

// A minimal single-load module: compile()'s build_linear_compute_package()
// requires a valid module regardless of whether the task graph it accompanies
// is raster-only, so this gives it one without the test caring about what it
// computes.
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

}  // namespace

int main() {
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

  // --- The FacetRef overloads: 03 §10 makes a facet an index+generation
  // capability token, never a raw backend resource, so the reference backend
  // enforces the pool's kind and staleness rules before it will produce a
  // value. A token a GPU backend must reject cannot quietly produce a
  // reference image to compare against. ---
  {
    vg::core::Arena arena;
    const uint64_t source_id = arena.allocate(64).id;
    const uint64_t target_id = arena.allocate(64).id;
    const vg::core::CanonicalView source = plain_view(source_id, {.width = 4, .height = 4});
    const vg::core::CanonicalView target = plain_view(target_id, {.width = 4, .height = 4});

    auto* source_allocation = arena.lookup(vg::core::PointerRef{source_id, 1});
    for (uint32_t y = 0; y < 4; ++y)
      for (uint32_t x = 0; x < 4; ++x) write_texel(*source_allocation, source, 0, 0, x, y, texel_pattern(x, y));

    vg::core::FacetPool pool;
    std::string acquire_error;
    vg::core::FacetRef sample_ref, storage_ref, attachment_ref, address_ref;
    assert(pool.acquire(arena, source, vg::core::FacetKind::Sample, &sample_ref, &acquire_error));
    assert(pool.acquire(arena, source, vg::core::FacetKind::Storage, &storage_ref, &acquire_error));
    assert(pool.acquire(arena, source, vg::core::FacetKind::Address, &address_ref, &acquire_error));
    assert(pool.acquire(arena, target, vg::core::FacetKind::Attachment, &attachment_ref, &acquire_error));

    const std::vector<vg::reference::SampleCoord> centre{{0.125f, 0.125f, 0.0f, 0}};

    // Reached through the right kind of token, the oracle agrees with the
    // direct-view call exactly.
    const auto by_view = vg::reference::sample_facet(arena, source, vg::core::FilterMode::Nearest,
                                                    vg::core::WrapMode::Clamp, centre);
    const auto by_token = vg::reference::sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                                     vg::core::WrapMode::Clamp, centre);
    assert(by_view.ok && by_token.ok);
    assert(exact_match(by_view.sampled_rgba[0], by_token.sampled_rgba[0]));

    // An AddressFacet names how an existing representation is reached; it is
    // not a sampleable facet, so it is refused rather than reinterpreted.
    const auto wrong_kind = vg::reference::sample_facet(arena, pool, address_ref, vg::core::FilterMode::Nearest,
                                                       vg::core::WrapMode::Clamp, centre);
    assert(!wrong_kind.ok);
    assert(wrong_kind.message == "facet kind mismatch");
    assert(wrong_kind.sampled_rgba.empty());

    const auto wrong_storage = vg::reference::storage_facet(arena, pool, sample_ref,
                                                            vg::reference::StorageTexel{0, 0, 0, 0},
                                                            Rgba{1.0f, 1.0f, 1.0f, 1.0f});
    assert(!wrong_storage.ok && wrong_storage.message == "facet kind mismatch");

    const auto wrong_attachment =
        vg::reference::attachment_facet(arena, pool, sample_ref, vg::reference::AttachmentFacetDesc{});
    assert(!wrong_attachment.ok && wrong_attachment.message == "facet kind mismatch");

    // A StorageFacet token does write, and the sampler sees it.
    const auto stored = vg::reference::storage_facet(arena, pool, storage_ref,
                                                     vg::reference::StorageTexel{0, 0, 0, 0},
                                                     Rgba{1.0f, 0.0f, 0.0f, 1.0f});
    assert(stored.ok);
    const auto stored_readback = vg::reference::sample_facet(
        arena, pool, sample_ref, vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp, centre);
    assert(stored_readback.ok);
    assert(exact_match(stored_readback.sampled_rgba[0], Rgba{1.0f, 0.0f, 0.0f, 1.0f}));

    // raster_triangles enforces both kinds independently, and says which one
    // was wrong (05 §14: the diagnostic names the VG concept that failed).
    vg::reference::RasterDesc desc;
    desc.filter = vg::core::FilterMode::Nearest;
    desc.attachment.load = vg::reference::AttachmentLoadAction::Clear;
    desc.attachment.store = vg::reference::AttachmentStoreAction::Store;
    const auto quad = full_target_quad();

    const auto bad_source = vg::reference::raster_triangles(arena, pool, {.source = attachment_ref, .target = attachment_ref}, desc, quad);
    assert(!bad_source.ok);
    assert(bad_source.message == "raster source: facet kind mismatch");
    const auto bad_target = vg::reference::raster_triangles(arena, pool, {.source = sample_ref, .target = sample_ref}, desc, quad);
    assert(!bad_target.ok);
    assert(bad_target.message == "raster target: facet kind mismatch");

    const auto via_tokens = vg::reference::raster_triangles(arena, pool, {.source = sample_ref, .target = attachment_ref}, desc, quad);
    assert(via_tokens.ok);
    assert(via_tokens.covered_fragment_count == 16);

    // A representation transform stales every token acquired against the old
    // epoch (02 §10), and none of the four entry points will honour one.
    uint32_t new_epoch = 0;
    assert(arena.transform(source_id, 1, &new_epoch) && new_epoch == 1);
    const auto stale_sample = vg::reference::sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                                         vg::core::WrapMode::Clamp, centre);
    assert(!stale_sample.ok);
    assert(stale_sample.message == vg::core::to_string(vg::core::FacetStatus::EpochStale));
    const auto stale_storage = vg::reference::storage_facet(arena, pool, storage_ref,
                                                            vg::reference::StorageTexel{0, 0, 0, 0},
                                                            Rgba{0.0f, 1.0f, 0.0f, 1.0f});
    assert(!stale_storage.ok);
    assert(stale_storage.message == vg::core::to_string(vg::core::FacetStatus::EpochStale));
    const auto stale_raster = vg::reference::raster_triangles(arena, pool, {.source = sample_ref, .target = attachment_ref}, desc, quad);
    assert(!stale_raster.ok);
    assert(stale_raster.message ==
           std::string("raster source: ") + vg::core::to_string(vg::core::FacetStatus::EpochStale));

    // The target token, whose own allocation did not transform, is still live:
    // staleness is per-allocation, not per-pool.
    assert(pool.lookup(arena, attachment_ref) != nullptr);
    const auto still_live =
        vg::reference::attachment_facet(arena, pool, attachment_ref, vg::reference::AttachmentFacetDesc{});
    assert(still_live.ok);

    // A retired token is refused with the retirement reason, not the epoch one.
    std::string retire_error;
    assert(pool.retire(attachment_ref, &retire_error));
    const auto retired =
        vg::reference::attachment_facet(arena, pool, attachment_ref, vg::reference::AttachmentFacetDesc{});
    assert(!retired.ok);
    assert(retired.message == vg::core::to_string(vg::core::FacetStatus::Retired));
  }

  // --- F2 (ADR-043 Decision #3, ADR-046): rasterization is a shape of
  // TaskRecord/ExecutionPlan, not a parallel API. A Raster-kind task driven
  // through TaskGraphBuilder -> seal -> publish -> ExecutionPlan ->
  // compile() -> submit() must land in submission.raster_results with the
  // exact same result raster_triangles() would produce called directly.
  // Facets are acquired against the device's own facet_pool() (not a local
  // one), since submit() resolves task.raster_facets/vertex_buffer_ref
  // against that pool, not a caller-supplied one. ---
  {
    auto device = vg::reference::make_device_hal();
    assert(device != nullptr);

    vg::core::Arena arena;
    constexpr uint32_t kExtent = 4;
    const uint64_t source_id = arena.allocate(64).id;
    const uint64_t target_id = arena.allocate(64).id;
    const vg::core::CanonicalView source = plain_view(source_id, {.width = kExtent, .height = kExtent});
    const vg::core::CanonicalView target = plain_view(target_id, {.width = kExtent, .height = kExtent});

    auto* source_allocation = arena.lookup(vg::core::PointerRef{source_id, 1});
    for (uint32_t y = 0; y < kExtent; ++y)
      for (uint32_t x = 0; x < kExtent; ++x) write_texel(*source_allocation, source, 0, 0, x, y, texel_pattern(x, y));

    std::string error;
    vg::core::FacetRef source_ref, target_ref;
    assert(device->facet_pool().acquire(arena, source, vg::core::FacetKind::Sample, &source_ref, &error));
    assert(device->facet_pool().acquire(arena, target, vg::core::FacetKind::Attachment, &target_ref, &error));

    const auto quad = full_target_quad();
    const uint64_t vertex_bytes = quad.size() * sizeof(vg::reference::RasterVertex);
    auto& vertex_alloc = arena.allocate(vertex_bytes);
    std::memcpy(vertex_alloc.bytes.data(), quad.data(), vertex_bytes);
    const vg::core::CanonicalView vertex_view =
        plain_view(vertex_alloc.id, {.width = static_cast<uint32_t>(vertex_bytes / 4), .height = 1});
    vg::core::FacetRef vertex_ref;
    assert(device->facet_pool().acquire(arena, vertex_view, vg::core::FacetKind::Address, &vertex_ref, &error));

    vg::core::TaskRecord raster_task{};
    raster_task.kind = vg::core::TaskKind::Raster;
    raster_task.raster_facets = {.source = source_ref, .target = target_ref};
    raster_task.vertex_buffer_ref = vertex_ref;
    raster_task.raster_filter = vg::core::FilterMode::Nearest;
    raster_task.raster_wrap = vg::core::WrapMode::Clamp;

    vg::core::TaskGraphBuilder builder;
    assert(builder.append(raster_task));
    vg::core::TaskGraph graph;
    assert(builder.seal(&graph) && graph.publish());

    const auto module = probe_module(arena);
    vg::hal::ExecutionPlan plan;
    plan.capabilities = device->capabilities();
    plan.module = module;
    plan.published = true;
    plan.task_graph = graph;
    plan.graph_epoch = arena.topology_epoch();

    vg::hal::CompiledPlan compiled;
    assert(device->compile(plan, &compiled, &error));

    vg::hal::Submission submission;
    assert(device->submit(compiled, arena, &submission, &error));
    assert(submission.result.ok);
    assert(submission.raster_results.size() == 1);
    const auto& task_result = submission.raster_results[0];
    assert(task_result.task_index == 0);
    assert(task_result.width == kExtent);
    assert(task_result.height == kExtent);

    // F2's fixed attachment defaults are hard-coded inside submit(); mirror
    // them here so the direct call matches exactly what submit() ran.
    vg::reference::RasterDesc oracle_desc;
    oracle_desc.filter = raster_task.raster_filter;
    oracle_desc.wrap = raster_task.raster_wrap;
    oracle_desc.attachment =
        vg::hal::f2_default_raster_attachment_config<vg::reference::AttachmentFacetDesc>();
    const auto oracle = vg::reference::raster_triangles(arena, device->facet_pool(), {.source = source_ref, .target = target_ref},
                                                        oracle_desc, quad);
    assert(oracle.ok);
    assert(task_result.resolved_rgba.size() == oracle.resolved_rgba.size());
    assert(task_result.stored == oracle.stored);
    assert(task_result.contents_defined == oracle.contents_defined);
    for (size_t i = 0; i < oracle.resolved_rgba.size(); ++i) assert(exact_match(task_result.resolved_rgba[i], oracle.resolved_rgba[i]));

    // Second sub-case: an indexed raster draw is deferred to F5 and must be
    // rejected at compile() time. TaskGraph::validate_execution() -- run
    // ahead of the index_count check inside compile() -- only requires the
    // graph to be sealed/published with non-zero node/root generation; it
    // never inspects FacetRef contents, so an otherwise-default TaskRecord
    // (node_generation/root_generation both default to 1) already reaches
    // that rejection.
    vg::core::TaskRecord indexed_task{};
    indexed_task.kind = vg::core::TaskKind::Raster;
    indexed_task.index_count = 3;
    vg::core::TaskGraphBuilder indexed_builder;
    assert(indexed_builder.append(indexed_task));
    vg::core::TaskGraph indexed_graph;
    assert(indexed_builder.seal(&indexed_graph) && indexed_graph.publish());

    vg::hal::ExecutionPlan indexed_plan;
    indexed_plan.capabilities = device->capabilities();
    indexed_plan.module = module;
    indexed_plan.published = true;
    indexed_plan.task_graph = indexed_graph;
    indexed_plan.graph_epoch = arena.topology_epoch();
    vg::hal::CompiledPlan indexed_compiled;
    std::string indexed_error;
    assert(!device->compile(indexed_plan, &indexed_compiled, &indexed_error));
    assert(indexed_error == "indexed raster draws deferred to F5");
  }

  // --- F3 (ADR-043 Decision #4): a restricted-import "vg.msl.raster/v1"
  // submission -- plan.user_raster_shader set, plan.module left default --
  // must still drive a Raster-kind task exactly like F2's plain path above.
  // This backend never interprets the supplied MSL text (raster_triangles()
  // is completely unchanged), so the pixel output must match F2's fixed
  // C++-shading formula exactly, regardless of what the custom fragment
  // shader source below claims to compute -- a disclosed, intentional
  // limitation (ADR-018: this backend is not a pixel-correctness oracle for
  // user shading logic), asserted here as documented behaviour. compile()
  // must also record the "raster_user_shader" HostAssisted disclosure event
  // (docs/START.md invariant 10: no silent degradation to "verified"). ---
  {
    auto device = vg::reference::make_device_hal();
    assert(device != nullptr);
    const auto& caps = device->capabilities();
    assert(caps.supports(vg::hal::Capability::UserShaderImport));

    vg::core::Arena arena;
    constexpr uint32_t kExtent = 4;
    const uint64_t source_id = arena.allocate(64).id;
    const uint64_t target_id = arena.allocate(64).id;
    const vg::core::CanonicalView source = plain_view(source_id, {.width = kExtent, .height = kExtent});
    const vg::core::CanonicalView target = plain_view(target_id, {.width = kExtent, .height = kExtent});

    auto* source_allocation = arena.lookup(vg::core::PointerRef{source_id, 1});
    for (uint32_t y = 0; y < kExtent; ++y)
      for (uint32_t x = 0; x < kExtent; ++x) write_texel(*source_allocation, source, 0, 0, x, y, texel_pattern(x, y));

    std::string error;
    vg::core::FacetRef source_ref, target_ref;
    assert(device->facet_pool().acquire(arena, source, vg::core::FacetKind::Sample, &source_ref, &error));
    assert(device->facet_pool().acquire(arena, target, vg::core::FacetKind::Attachment, &target_ref, &error));

    const auto quad = full_target_quad();
    const uint64_t vertex_bytes = quad.size() * sizeof(vg::reference::RasterVertex);
    auto& vertex_alloc = arena.allocate(vertex_bytes);
    std::memcpy(vertex_alloc.bytes.data(), quad.data(), vertex_bytes);
    const vg::core::CanonicalView vertex_view =
        plain_view(vertex_alloc.id, {.width = static_cast<uint32_t>(vertex_bytes / 4), .height = 1});
    vg::core::FacetRef vertex_ref;
    assert(device->facet_pool().acquire(arena, vertex_view, vg::core::FacetKind::Address, &vertex_ref, &error));

    vg::core::TaskRecord raster_task{};
    raster_task.kind = vg::core::TaskKind::Raster;
    raster_task.raster_facets = {.source = source_ref, .target = target_ref};
    raster_task.vertex_buffer_ref = vertex_ref;
    raster_task.raster_filter = vg::core::FilterMode::Nearest;
    raster_task.raster_wrap = vg::core::WrapMode::Clamp;

    vg::core::TaskGraphBuilder builder;
    assert(builder.append(raster_task));
    vg::core::TaskGraph graph;
    assert(builder.seal(&graph) && graph.publish());

    vg::hal::ExecutionPlan plan;
    plan.capabilities = device->capabilities();
    // plan.module stays default: a "vg.msl.raster/v1" submission never
    // carries linear IR (vg_api_execution.cpp's submit()); validate() skips
    // ir::verify(module) whenever user_raster_shader is set.
    plan.user_raster_shader = vg::ir::UserRasterShaderContract{
        "vg.test.raster/v1", "vg_test_vertex", "vg_test_fragment",
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct VgRasterVertex { float2 position; float2 uv; };\n"
        "struct VgRasterVaryings { float4 position [[position]]; float2 uv; };\n"
        "vertex VgRasterVaryings vg_test_vertex(device const VgRasterVertex* vertices [[buffer(0)]],\n"
        "                                       uint vid [[vertex_id]]) {\n"
        "  VgRasterVaryings varyings;\n"
        "  varyings.position = float4(vertices[vid].position, 0.0f, 1.0f);\n"
        "  varyings.uv = vertices[vid].uv;\n"
        "  return varyings;\n"
        "}\n"
        "fragment float4 vg_test_fragment(VgRasterVaryings varyings [[stage_in]]) {\n"
        "  return float4(1.0f, 0.0f, 0.0f, 1.0f);\n"
        "}\n"};
    plan.published = true;
    plan.task_graph = graph;
    plan.graph_epoch = arena.topology_epoch();

    vg::hal::CompiledPlan compiled;
    assert(device->compile(plan, &compiled, &error));
    assert(compiled.report.supported);
    bool found_user_shader_event = false;
    for (const auto& event : compiled.report.events) {
      if (event.operation == "raster_user_shader" && event.classification == vg::hal::LoweringClass::HostAssisted)
        found_user_shader_event = true;
    }
    assert(found_user_shader_event);

    vg::hal::Submission submission;
    assert(device->submit(compiled, arena, &submission, &error));
    assert(submission.result.ok);
    assert(submission.raster_results.size() == 1);
    const auto& task_result = submission.raster_results[0];
    assert(task_result.width == kExtent && task_result.height == kExtent);

    // The reference backend never interprets the supplied MSL text above (it
    // always runs raster_triangles()'s fixed C++ shading), so the pixel
    // output must equal F2's plain fixed-shading oracle exactly -- the same
    // oracle construction as the plain task-graph-driven raster case earlier
    // in this file -- not the solid-red colour the custom fragment shader
    // source claims to produce.
    vg::reference::RasterDesc oracle_desc;
    oracle_desc.filter = raster_task.raster_filter;
    oracle_desc.wrap = raster_task.raster_wrap;
    oracle_desc.attachment =
        vg::hal::f2_default_raster_attachment_config<vg::reference::AttachmentFacetDesc>();
    const auto oracle = vg::reference::raster_triangles(arena, device->facet_pool(), {.source = source_ref, .target = target_ref},
                                                        oracle_desc, quad);
    assert(oracle.ok);
    assert(task_result.resolved_rgba.size() == oracle.resolved_rgba.size());
    for (size_t i = 0; i < oracle.resolved_rgba.size(); ++i)
      assert(exact_match(task_result.resolved_rgba[i], oracle.resolved_rgba[i]));
  }

  return 0;
}
