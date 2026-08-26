#ifndef VG_REFERENCE_EXECUTOR_H_
#define VG_REFERENCE_EXECUTOR_H_

#include "core/core.h"

#include <array>
#include <string>
#include <type_traits>
#include <vector>

namespace vg::reference {

// `timeline` is the device's persistent timeline (owned by the caller across
// submissions). When non-null and `gate.wait != 0`, execution is refused
// unless the wait point is already satisfied; on success, `gate.signal`
// (if non-zero) is applied to `*timeline`.
core::ExecutionResult execute(const ir::Module& module, core::Arena& arena,
                              const core::Certificate* certificate = nullptr,
                              core::Timeline* timeline = nullptr,
                              core::TimelineGate gate = {});

struct TaskGraphExecutionResult {
  bool ok{};
  std::string message;
  std::vector<core::TaskRecord> published_tasks;
};

// Publishes every task in `task_graph` through a real core::PublicationRing
// (Empty->Writing->Published->Consumed), in an order consistent with its
// Explicit/InferredConflict dependency edges (deterministic: among ready
// tasks, lowest index first). This is the byte-exact oracle Metal/Vulkan
// Task-ring GPU kernels must match.
TaskGraphExecutionResult execute_task_graph(const core::TaskGraph& task_graph);

// TASK-B13 (E009): reference oracle for GPU cull/compact stream compaction.
// Returns every id whose matching instance_visible flag is nonzero, walked
// in ascending instance-index order. This is the CPU-side *set* the GPU
// kernel must reproduce -- but GPU atomic-append slot assignment is ordered
// by thread arrival, not by instance index, so callers must compare the two
// as sets/sorted-multisets, never by position. `ok` is false only on a
// malformed call (mismatched vector sizes), not on any data-dependent case.
struct CullCompactResult {
  bool ok{};
  std::string message;
  std::vector<uint32_t> compact_ids;
};
CullCompactResult cull_compact(const std::vector<uint32_t>& instance_visible,
                               const std::vector<uint32_t>& instance_ids);

// 06 §6.1 names levels/slices among the inputs a SampleFacet compiles from, so
// the oracle has to be able to address any subresource of a CanonicalView, not
// only (slice 0, level 0). Carried as one struct per coordinate rather than as
// parallel uv/lod/slice vectors: parallel vectors let a caller pair a uv with
// another coordinate's lod whenever the lengths disagree, and a sampling oracle
// that can be silently mis-paired is worthless as a correctness judge.
struct SampleCoord {
  float u{};
  float v{};
  float lod{};
  uint32_t array_slice{};
};

// Software nearest/bilinear/trilinear sampling oracle for SampleFacet
// correctness checks (05 §9: `region.sample` lowers to a software sampler on
// the reference backend). Reads `view`'s backing allocation directly out of
// `arena` (no facet/GPU resource involved) and decodes it through
// CanonicalView's linear layout contract -- slice-major, then ascending mip
// level, each level tightly packed with no row padding -- which is the same
// contract the Metal upload path encodes against, so the two are comparable.
//
// Conventions, all chosen to match what Metal/D3D/Vulkan texture samplers
// agree on:
//   - Bilinear: half-texel convention, i.e. filtering is centred on
//     uv * extent - 0.5.
//   - Nearest texel: floor without an offset, i.e. floor(uv * extent).
//   - Nearest mip level under FilterMode::Nearest: the GL/Vulkan rule
//     ceil(lod + 0.5) - 1 (round-half-down), so lod 0.5 selects level 0.
//   - Fractional lod under FilterMode::Bilinear: full trilinear, i.e. a
//     bilinear tap of floor(lod) and of floor(lod) + 1 blended by the
//     fractional part. Nothing here silently rounds a fractional lod away
//     (docs/START.md §4 invariant 10).
//
// RGBA8Unorm channels are returned as float in [0,1]; R32Float's single channel
// is returned as {value, 0, 0, 1}. The view's swizzle is applied once to the
// filtered result, which is exact because channel selection is a per-component
// pick and therefore commutes with linear filtering.
//
// `ok` is false only for a malformed call -- unknown allocation, a view
// `core::CanonicalView::valid()` rejects, an allocation too small for the
// view's declared subresources, or a coordinate naming a slice/level the view
// does not declare. An out-of-range slice or lod is deliberately *not* clamped:
// clamping would turn a caller's indexing bug into a plausible-looking
// reference value.
struct SampleFacetResult {
  bool ok{};
  std::string message;
  std::vector<std::array<float, 4>> sampled_rgba;
};
SampleFacetResult sample_facet(const core::Arena& arena, const core::CanonicalView& view,
                               core::FilterMode filter, core::WrapMode wrap,
                               const std::vector<SampleCoord>& coords);
// Same oracle reached through a capability token: the reference backend
// enforces the pool's kind and staleness rules before it will sample, so a ref
// that a GPU backend must reject cannot quietly produce a reference value to
// compare against.
SampleFacetResult sample_facet(const core::Arena& arena, const core::FacetPool& pool, core::FacetRef ref,
                               core::FilterMode filter, core::WrapMode wrap,
                               const std::vector<SampleCoord>& coords);
// Convenience for the single-subresource case: every uv is sampled at lod 0 of
// array slice 0. Kept as its own overload so callers that predate mip/array
// support -- notably the Metal vertical-slice differential test -- keep reading
// exactly as they did.
SampleFacetResult sample_facet(const core::Arena& arena, const core::CanonicalView& view,
                               core::FilterMode filter, core::WrapMode wrap,
                               const std::vector<std::array<float, 2>>& uv_coords);
SampleFacetResult sample_facet(const core::Arena& arena, const core::FacetPool& pool, core::FacetRef ref,
                               core::FilterMode filter, core::WrapMode wrap,
                               const std::vector<std::array<float, 2>>& uv_coords);

// One texel of one subresource, the unit a StorageFacet image write addresses.
struct StorageTexel {
  uint32_t x{};
  uint32_t y{};
  uint32_t layer{};
  uint32_t level{};
};

// Software StorageFacet write oracle (06 §6.2). Writes `rgba` into `target` of
// `view`'s backing allocation through the same linear layout contract the
// sampler decodes, then reads the texel back so the caller sees the value that
// actually survived storage rather than the value it asked for.
//
// Format handling: 06 §6.2 requires an unwritable format to yield an explicit
// conversion, an explicit software path, or Unsupported, and forbids silently
// changing format or precision. The reference backend *is* that software path,
// so both formats this milestone models are writable here -- RGBA8Unorm is
// encoded round-to-nearest as uint8(round(clamp(v, 0, 1) * 255)), which is the
// quantization step every RGBA8 comparison tolerance against Metal is derived
// from, and R32Float stores the red channel's float verbatim.
//
// A non-identity swizzle is refused rather than honoured: a swizzle reinterprets
// a shader *read* of a view, so there is no defined meaning for it on an image
// write, and inventing one (applying it forward, or ignoring it) would make the
// oracle disagree with the Metal backend, which refuses for the same reason.
//
// `ok` is false only for a malformed call: unknown allocation, invalid view,
// allocation too small for the view, out-of-range texel/slice/level, or the
// swizzle refusal above.
struct StorageFacetResult {
  bool ok{};
  std::string message;
  std::array<float, 4> written_rgba{};
};
StorageFacetResult storage_facet(core::Arena& arena, const core::CanonicalView& view, StorageTexel target,
                                 const std::array<float, 4>& rgba);
StorageFacetResult storage_facet(core::Arena& arena, const core::FacetPool& pool, core::FacetRef ref,
                                 StorageTexel target, const std::array<float, 4>& rgba);

// 06 §6.3: load/store/resolve are the lowering of effect and representation
// operations and must not become public object state, so they are per-use
// parameters here. Deliberately duplicated from the Metal backend's
// AttachmentLoadAction/AttachmentStoreAction rather than hoisted into
// vg::core: they are lowering parameters of one backend's render pass, and
// promoting a backend's per-use knob into core would be exactly the
// "adapter 特性升级成核心最低能力" that docs/START.md §5 forbids. Each backend
// keeps its own, and the differential test maps between them.
enum class AttachmentLoadAction : uint32_t { Clear, Load, DontCare };
enum class AttachmentStoreAction : uint32_t { Store, DontCare, MultisampleResolve };

// The subresource of a CanonicalView a render pass targets. An attachment pass
// renders into exactly one, which is why this is not folded into the view.
struct AttachmentSubresource {
  uint32_t layer{};
  uint32_t level{};
};

// Per-use parameters of one reference render pass over an attachment
// subresource. `sample_count` > 1 requires MultisampleResolve, because a
// multisample attachment with a single-sample store has no defined resolution
// and guessing one would be a silent downgrade; MultisampleResolve with
// `sample_count` == 1 is allowed and is the exact identity resolve.
struct AttachmentFacetDesc {
  AttachmentLoadAction load{AttachmentLoadAction::Clear};
  AttachmentStoreAction store{AttachmentStoreAction::Store};
  std::array<float, 4> clear_rgba{};
  uint32_t sample_count{1};
  AttachmentSubresource subresource{};
};

// Result of a reference render pass. `resolved_rgba` is the whole target
// subresource, row-major, `mip_width(level) * mip_height(level)` entries,
// already decoded to floats so a caller comparing against a GPU readback never
// has to re-derive the byte layout and risk disagreeing with the oracle about
// the very contract under test.
//
// When the pass stored, `resolved_rgba` is decoded back *out of* the allocation,
// so it carries the format's quantization; when it did not store
// (AttachmentStoreAction::DontCare) it carries the in-pass float values and
// `stored` is false.
//
// `contents_defined` is how DontCare is modelled explicitly instead of being
// pretended away: a DontCare load leaves the previous bytes visible and a
// DontCare store leaves memory untouched, but in both cases the contract does
// not define what a reader sees, so the oracle reports that the values it
// returns must not be used as an expectation.
struct AttachmentFacetResult {
  bool ok{};
  std::string message;
  std::vector<std::array<float, 4>> resolved_rgba;
  uint32_t width{};
  uint32_t height{};
  uint32_t sample_count{1};
  bool stored{};
  bool contents_defined{true};
};

// Software AttachmentFacet oracle (06 §6.3) with no drawing: it exercises load,
// store and resolve alone, which is what makes it usable as the expectation for
// a clear-only Metal render pass. With `sample_count` > 1 the clear/load really
// is materialized into that many per-pixel samples and averaged back down, so a
// resolve is modelled as an average and not as a copy of sample 0.
AttachmentFacetResult attachment_facet(core::Arena& arena, const core::CanonicalView& view,
                                       const AttachmentFacetDesc& desc);
AttachmentFacetResult attachment_facet(core::Arena& arena, const core::FacetPool& pool, core::FacetRef ref,
                                       const AttachmentFacetDesc& desc);

// A rasterizer input vertex: compact {x,y,z,u,v}, where x/y are clip-space,
// z is normalized depth in [0,1], and uv is in [0,1]. This exactly matches
// F4's public vertex-memory contract (and the packed MSL layout).
struct RasterVertex {
  float x{};
  float y{};
  float z{};
  float u{};
  float v{};
};
static_assert(std::is_standard_layout_v<RasterVertex>);
static_assert(sizeof(RasterVertex) == 5 * sizeof(float));

// Per-use parameters of one textured-triangle pass: where it renders (the
// attachment description), how it reads the source view, and the constant tint
// its fragments multiply by.
struct RasterDesc {
  AttachmentFacetDesc attachment;
  core::FilterMode filter{core::FilterMode::Bilinear};
  core::WrapMode wrap{core::WrapMode::Clamp};
  float source_lod{};
  uint32_t source_array_slice{};
  std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
  // An absent depth view is the F3 depth-free path. When supplied, it must be
  // a matching Depth32Float attachment and is cleared to 1.0 for this task.
  const core::CanonicalView* depth_attachment{};
  core::FacetRef depth_attachment_ref{};
  bool depth_test_enable{};
  bool depth_write_enable{};
  core::DepthCompareOp depth_compare_op{core::DepthCompareOp::Always};
};

// `covered_fragment_count` counts shaded pixel-sample pairs, not pixels: this
// rasterizer supersamples (each covered sample is shaded at its own position),
// so with `sample_count` == 1 the number is the covered-pixel count and above
// that it is sample_count times larger on fully covered pixels. Reported so a
// coverage regression shows up as a count difference rather than only as a
// handful of changed pixels.
struct RasterResult {
  bool ok{};
  std::string message;
  std::vector<std::array<float, 4>> resolved_rgba;
  uint32_t width{};
  uint32_t height{};
  uint32_t sample_count{1};
  uint64_t covered_fragment_count{};
  uint64_t depth_passed_fragment_count{};
  bool stored{};
  bool contents_defined{true};
};

// The software raster target 05 §9 assigns to `region.attachment.store` on the
// reference backend, and the CPU judge E008's image-correctness comparison
// needs (10 §11: correctness is zero-tolerance, so the judge has to be exact
// about its own rules). Renders `vertices` as a triangle list, sampling
// `source` through the sample_facet oracle above and multiplying by
// `desc.tint`, into `target`'s subresource under `desc.attachment`'s
// load/store/resolve rules.
//
// Conventions:
//   - Clip space maps to pixel space with the standard half-pixel centre and
//     y pointing *down* in the target image, matching a Metal render target:
//     px = (x * 0.5 + 0.5) * width, py = (0.5 - y * 0.5) * height, and pixel
//     (i, j)'s centre is (i + 0.5, j + 0.5).
//   - Coverage uses edge functions with the top-left fill rule: a sample
//     exactly on an edge counts only for the top and left edges of the
//     triangle. A naive `>= 0` test shades a shared edge from both triangles
//     of a quad, which shows up against a GPU as an intermittent seam.
//   - Winding is normalized (two vertices swapped when the signed area is
//     negative) so front- and back-facing triangles rasterize identically;
//     there is no face culling.
//   - uv interpolation is 2D barycentric with no perspective divide, which is
//     exact for the clip-space input this takes.
//   - Multisampling supersamples at the standard D3D/Metal sample positions for
//     1, 2, 4 and 8 samples; any other count is refused rather than
//     approximated. Note the deliberate difference from a GPU: Metal shades
//     once per covered pixel and replicates to covered samples, this shades
//     each covered sample, so the two agree exactly on interior pixels and on
//     coverage-only workloads, and agree within the registered sampling
//     tolerance (10 §6) on edge pixels of a varying signal.
//
// F4 supplies an optional Depth32Float attachment with fixed clear=1.0/store,
// eight compare ops, and optional testing/writing. Deliberately excluded:
// stencil, blending, clipping against the near/far planes, face culling and
// per-triangle state. Without a depth attachment, later triangles overwrite
// earlier ones at the same sample as in the F3 depth-free path.
//
// `ok` is false only for a malformed call: a vertex count that is not a
// multiple of 3, an invalid source or target view, an unknown allocation, an
// out-of-range subresource, an unsupported sample count/store combination, or a
// pass whose sampled source subresource is the very subresource it renders into
// (a read of the surface being written has no defined result, and an oracle that
// returned an order-dependent image for it would be worse than useless).
// Sharing an allocation between source and target is *not* refused as long as
// the slice/level differ, so generating one mip level from another stays
// expressible.
RasterResult raster_triangles(core::Arena& arena, const core::CanonicalView& source,
                              const core::CanonicalView& target, const RasterDesc& desc,
                              const std::vector<RasterVertex>& vertices);
// Both views reached through capability tokens, with the kinds the pass
// actually uses enforced (Sample for the source, Attachment for the target):
// a token a GPU backend must reject cannot quietly produce a reference image.
RasterResult raster_triangles(core::Arena& arena, const core::FacetPool& pool, core::RasterFacetPair facets,
                              const RasterDesc& desc,
                              const std::vector<RasterVertex>& vertices);

}  // namespace vg::reference

#endif
