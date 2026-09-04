#ifndef VG_BACKENDS_METAL_PHYSICAL_TYPES_H_
#define VG_BACKENDS_METAL_PHYSICAL_TYPES_H_
#include "backends/device_hal.h"
#include <type_traits>
namespace vg::metal {
// 06 §6.3: load/store/resolve are the lowering of effect and representation
// operations, so they are per-use parameters rather than state stored on the
// facet or on a public object.
enum class AttachmentLoadAction : uint32_t { Clear, Load, DontCare };
enum class AttachmentStoreAction : uint32_t { Store, DontCare, MultisampleResolve };

// The subresource of a CanonicalView a render pass targets. A pass renders into
// exactly one, which is why this is not folded into the view. Mirrors
// reference::AttachmentSubresource.
struct AttachmentSubresource {
  uint32_t layer{};
  uint32_t level{};
};

struct AttachmentFacetDesc {
  AttachmentLoadAction load{AttachmentLoadAction::Clear};
  AttachmentStoreAction store{AttachmentStoreAction::Store};
  std::array<float, 4> clear_rgba{};
  // >1 renders into a transient multisample texture that resolves into the
  // facet's texture. Only meaningful with MultisampleResolve.
  uint32_t sample_count{1};
  // Defaults to (layer 0, level 0), which is exactly what every pre-mip caller
  // meant, so adding this field changes no existing behaviour.
  AttachmentSubresource subresource{};
};

// A rasterizer input vertex: clip-space position in [-1,1] and source uv in
// [0,1]. Mirrors reference::RasterVertex, and matches the MSL
// `struct VgRasterVertex { packed_float3 position; packed_float2 uv; }` that
// compiler::raster_facet_metal_source() reads from vertex buffer(0) byte for
// byte, so the host array is uploaded without a repack.
struct RasterVertex {
  float x{};
  float y{};
  float z{};
  float u{};
  float v{};
};
static_assert(std::is_standard_layout_v<RasterVertex>);
static_assert(sizeof(RasterVertex) == 5 * sizeof(float));

// Per-use parameters of one textured-triangle pass. Mirrors
// reference::RasterDesc so a differential against reference::raster_triangles
// states the same inputs to both backends.
//
// Which of these enter the Metal pipeline cache key is decided by 06 §7, not by
// convenience: `attachment.sample_count` and the target's pixel format are
// compiled into the MTLRenderPipelineState and therefore key state, while the
// viewport is set on the encoder (Metal dynamic state) and `tint` is plain data
// the fragment stage reads from a buffer. Neither of the latter two is allowed
// to enlarge the key ("小的动态状态不应无故扩大 key").
struct RasterDesc {
  AttachmentFacetDesc attachment;
  core::FilterMode filter{core::FilterMode::Bilinear};
  core::WrapMode wrap{core::WrapMode::Clamp};
  float source_lod{};
  uint32_t source_array_slice{};
  std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
  // F4: a Depth32Float AttachmentFacet.  It is deliberately separate from
  // RasterFacetPair's source/color target because it is an additional write
  // capability, not another color attachment.
  core::FacetRef depth_attachment_ref{};
  bool depth_test_enable{};
  bool depth_write_enable{};
  core::DepthCompareOp depth_compare_op{core::DepthCompareOp::Always};
};

struct RasterResult {
  // The whole target subresource, row-major,
  // mip_width(level) * mip_height(level) entries, already decoded to float4 so
  // a caller comparing against reference::raster_triangles never has to
  // re-derive the byte layout and risk disagreeing with the oracle about the
  // very contract under test.
  std::vector<std::array<float, 4>> resolved_rgba;
  std::vector<float> resolved_depth;
  uint32_t width{};
  uint32_t height{};
  uint32_t sample_count{1};
  // Deliberately 0: this backend shades on the GPU and has no honest way to
  // count covered pixel-sample pairs without a counter sample buffer or a
  // fragment-side atomic the shared raster shader does not carry. 10 §12
  // forbids writing an unobservable cost as a real number, so it stays 0
  // rather than becoming a host-side re-rasterization estimate.
  uint64_t covered_fragment_count{};
  bool stored{};
  bool contents_defined{true};
  bool facet_cache_hit{};
  uint32_t encoder_count{};
  hal::LoweringReport report;
};


}  // namespace vg::metal
#endif
