#pragma once
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
#include "assembled_plan_fixture.h"
#include "ir/ir.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vg::tests::reference {
using Bytes4 = std::array<uint8_t, 4>;
using Rgba = std::array<float, 4>;
struct Extent2 {
  uint32_t width{};
  uint32_t height{};
};

Rgba unorm(const Bytes4& bytes);
Rgba requantize(const Rgba& rgba);
bool exact_match(const Rgba& lhs, const Rgba& rhs);
bool close_match(const Rgba& lhs, const Rgba& rhs);
size_t texel_offset(const vg::core::CanonicalView& view, uint32_t layer, uint32_t level, uint32_t x,
                    uint32_t y);
void write_texel(vg::core::Allocation& allocation, const vg::core::CanonicalView& view, uint32_t layer,
                 uint32_t level, uint32_t x, uint32_t y, const Bytes4& bytes);
void fill_subresource(vg::core::Allocation& allocation, const vg::core::CanonicalView& view, uint32_t layer,
                      uint32_t level, const Bytes4& bytes);
Bytes4 subresource_colour(uint32_t layer, uint32_t level);
Bytes4 texel_pattern(uint32_t x, uint32_t y);
vg::core::CanonicalView plain_view(uint64_t allocation, Extent2 extent);
std::vector<vg::reference::RasterVertex> full_target_quad();
vg::ir::Module probe_module(vg::core::Arena& arena);
}
