#ifndef VG_TEST_SUPPORT_METAL_ADAPTER_HARNESS_H_
#define VG_TEST_SUPPORT_METAL_ADAPTER_HARNESS_H_
#include "backends/metal/metal_device_hal.h"
#include "backends/metal/metal_physical_types.h"
#include "compiler/pipeline_classification.h"
namespace vg::metal {
// TASK-B13 (E009): result of one run_cull_compact() call. `visible_count` is
// the GPU-authored atomic count of instances that passed the visibility
// check; only compact_ids[0..visible_count) are meaningful. Slot order
// reflects atomic_fetch_add arrival order across GPU threads, not the
// original instance index order -- compare as a set/sorted-multiset against
// a CPU oracle, never by position.
struct CullCompactResult {
  uint32_t visible_count{};
  std::vector<uint32_t> compact_ids;
};

// Narrow observability for the E007 physical adapter experiment.  It is not
// an ExecutionPlan option: production plans obtain packages exclusively from
// resolved Node contracts, while this harness measures one Metal binding
// mechanism in isolation.
struct IndexedComputeHarnessResult {
  uint32_t referenced_allocation_count{};
  hal::LoweringReport report;
};

// AddressFacet use of a FacetRef (02 §3.3): the linear/BDA view of the same
// CanonicalView, resolved to the allocation's device buffer. No texture object
// is involved, and none is exposed -- the caller gets an address and a length.
struct AddressFacetResult {
  uint64_t gpu_address{};
  uint64_t byte_size{};
  bool gpu_address_available{};
  hal::LoweringReport report;
};

// One sampling coordinate of a SampleFacet use. Deliberately mirrors
// reference::SampleCoord field for field (and is deliberately *not* that type:
// this public Metal header must not pull in the reference backend). Carried as
// one struct per coordinate rather than parallel uv/lod/slice vectors so a
// caller cannot silently pair one coordinate's uv with another's level --
// 06 §6.1 names levels/slices among the inputs a SampleFacet compiles from, so
// they are per-use coordinates, not view state.
struct SampleCoord {
  float u{};
  float v{};
  float lod{};
  uint32_t array_slice{};
};

// SampleFacet use of a FacetRef (06 §6.1). Callers acquire the ref from
// core::FacetPool; this method never accepts a raw CanonicalView as a
// capability token.
struct SampleFacetResult {
  std::vector<std::array<float, 4>> sampled_rgba;
  bool facet_cache_hit{};
  uint32_t descriptor_write_count{};
  // Number of threads whose in-shader facet-generation guard (06 §6.4)
  // rejected the bound token. Non-zero only under
  // core::ValidationProfile::CheckedNative, since the fast-native
  // specialization compiles the guard, its bindings and its atomic away
  // entirely. Every rejected thread's slot in `sampled_rgba` holds
  // compiler::kFacetGenerationPoisonValue in all four channels and was never
  // sampled -- poison, not a plausible-looking substitute.
  uint32_t generation_violations{};
  // Whether the pipeline that ran actually had the guard compiled in. False
  // means the sample ran fast-native and `generation_violations` is 0 because
  // nothing checked, not because nothing was wrong.
  bool checked_profile{};
  hal::LoweringReport report;
};

// 06 §6.2: a StorageFacet maps to "可读写 texture 或线性 buffer". The caller
// chooses; the backend never silently substitutes one for the other, and never
// rewrites the view's format to make a write legal -- an unwritable format is
// reported Unsupported so the caller can pick LinearBuffer or transform the
// representation explicitly.
enum class StorageFacetTarget : uint32_t { Texture, LinearBuffer };

// One texel of one subresource, the unit a StorageFacet image write addresses.
// Mirrors reference::StorageTexel field for field so the two backends' write
// oracles address the same thing.
struct StorageTexel {
  uint32_t x{};
  uint32_t y{};
  uint32_t layer{};
  uint32_t level{};
};

struct StorageFacetResult {
  std::array<float, 4> written_rgba{};
  bool facet_cache_hit{};
  uint32_t descriptor_write_count{};
  uint32_t encoder_count{};
  StorageFacetTarget target{StorageFacetTarget::Texture};
  hal::LoweringReport report;
};

// AttachmentFacet use: one render pass against the facet's texture, then host
// readback of texel (0,0) of the targeted subresource. Not a full raster
// workload -- it proves the facet maps to a render-target-capable MTLTexture
// and that load/store/resolve lower without becoming public object state.
// run_raster_triangles() below is the pass that actually draws.
struct AttachmentFacetResult {
  std::array<float, 4> resolved_rgba{};
  bool facet_cache_hit{};
  uint32_t encoder_count{};
  uint32_t sample_count{1};
  // 06 §6.3 requires reporting whether external-memory traffic was avoided.
  // True only when the multisample samples really were never written to
  // device memory (memoryless transient attachment resolved on-tile).
  bool store_traffic_avoided{};
  hal::LoweringReport report;
};

// Linear device buffer -> Private sample/storage/attachment-optimal MTLTexture
// via an explicit blit (02 §8: transform ≠ barrier), then Arena::transform()
// to publish a new RepresentationEpoch. Old FacetRefs become stale; the
// transform retires them itself. The blit source is resolved through a
// TransferFacet over the same CanonicalView, so no capability escapes the
// pool.
struct RepresentationTransformResult {
  uint32_t new_epoch{};
  uint64_t old_backing_bytes{};
  uint64_t new_backing_bytes{};
  uint64_t temporary_bytes{};
  uint32_t encoder_count{};
  bool used_private_optimal{};
  uint32_t retired_facet_count{};
  core::FacetRef out_facet{};
  hal::LoweringReport report;
};

// E013's measurement (06 §7, 05 §11, 07 §9's taxonomy): the same state matrix
// compiled twice, once folding every axis into the Metal specialization and
// once after compiler::classify_pipeline_state() has decided which axes are
// allowed to reach the key at all. `classified_pipeline_count` strictly below
// `naive_pipeline_count` is the whole claim; the counters below are what makes
// it checkable rather than asserted.
struct PipelineClassificationRun {
  uint32_t naive_pipeline_count{};
  uint32_t classified_pipeline_count{};
  uint32_t cache_hits{};
  uint32_t cache_misses{};
  uint64_t naive_compile_ns{};
  uint64_t classified_compile_ns{};
  // 05 §11's per-specialization audit trail for the VG-classified variant --
  // the variant whose key discipline is under test. The naive variant is a
  // baseline whose aggregate is fully described by naive_pipeline_count and
  // naive_compile_ns, so its individual reports are deliberately not mixed in
  // here where they would be indistinguishable.
  std::vector<compiler::SpecializationReport> reports;
  // True once a StateBlockKind::UnsupportedNeedsConversion combination has been
  // put through classify_pipeline_state() and been rejected. It is never folded
  // into a key and never compiled (START.md §4 invariant 10).
  bool unsupported_rejected{};
  hal::LoweringReport report;
};

// Explicit physical-mechanism probes. No ExecutionPlan or compile/submit entry.
// The referenced device must outlive this non-owning test harness.
class AdapterHarness {
 public:
  explicit AdapterHarness(DeviceHal& device);
  bool run_cull_compact(const std::vector<uint32_t>& instance_visible,
                       const std::vector<uint32_t>& instance_ids,
                       CullCompactResult* result, std::string* error = nullptr) const;

  // Explicitly narrow physical-mechanism harnesses.  Neither method accepts
  // or mutates ExecutionPlan, so experimental Tier1/indexed lowering cannot
  // become a second source of production execution semantics.
  bool run_task_tier1_indirect_test_harness(const ir::Module& module, core::Arena& arena,
                                            const std::vector<core::TaskRecord>& tasks,
                                            hal::Submission* submission,
                                            std::string* error = nullptr) const;
  bool run_indexed_compute_test_harness(const ir::Module& module, core::Arena& arena,
                                        IndexedComputeHarnessResult* result,
                                        hal::Submission* submission,
                                        std::string* error = nullptr) const;

  // Every run_*_facet resolves `ref` through FacetPool::lookup before touching
  // a Metal object, and brackets the command buffer in
  // FacetPool::begin_gpu_use/end_gpu_use so the slot cannot be recycled under
  // work still in flight (06 §6.4, §11). `pool` is non-const for that reason.
  // The backend cache is keyed by FacetRef index+generation, never by a host
  // texture pointer exposed on the public API.
  bool run_address_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                        AddressFacetResult* result, std::string* error = nullptr) const;

  // Single-subresource form: every uv is sampled at lod 0 of array slice 0
  // under core::ValidationProfile::FastNative. Kept as its own overload so
  // callers that predate mip/array/checked-profile support keep reading and
  // meaning exactly what they did.
  bool run_sample_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                       core::FilterMode filter, core::WrapMode wrap,
                       const std::vector<std::array<float, 2>>& uv_coords,
                       SampleFacetResult* result, std::string* error = nullptr) const;

  // Full form (06 §6.1 + §6.4). `profile` selects the specialization:
  // CheckedNative compiles in the in-shader generation guard and binds the
  // token/table/slot-count/violation-counter buffers; FastNative leaves the
  // function constant unset, which the kernels read through
  // is_function_constant_defined() as "guard off". ReferenceStrict and Capture
  // have no Metal implementation and are reported Unsupported rather than
  // silently downgraded to one of the two above.
  bool run_sample_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                       core::FilterMode filter, core::WrapMode wrap,
                       const std::vector<SampleCoord>& coords, core::ValidationProfile profile,
                       SampleFacetResult* result, std::string* error = nullptr) const;

  // Writes texel (0,0) of subresource (layer 0, level 0) -- the pre-mip
  // meaning, preserved exactly.
  bool run_storage_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                        StorageFacetTarget target, const std::array<float, 4>& write_rgba,
                        StorageFacetResult* result, std::string* error = nullptr) const;

  // Writes `target` of the view's linear layout: an image write addresses a
  // named (x, y) of a named layer/level, and the linear-buffer form lands at
  // that subresource's byte offset rather than always at texel 0.
  bool run_storage_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                        StorageFacetTarget target, const std::array<float, 4>& write_rgba,
                        StorageTexel texel, StorageFacetResult* result,
                        std::string* error = nullptr) const;

  bool run_attachment_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                           const AttachmentFacetDesc& desc, AttachmentFacetResult* result,
                           std::string* error = nullptr) const;

  // 05 §9's `region.attachment.store` on real raster hardware: a
  // MTLRenderPipelineState built from compiler::raster_facet_metal_source(),
  // one draw of `vertices` as a triangle list sampling `source_ref` and
  // multiplying by `desc.tint`, into `target_ref`'s subresource under
  // `desc.attachment`'s load/store/resolve rules. `source_ref` must be a
  // SampleFacet and `target_ref` an AttachmentFacet; both are resolved through
  // FacetPool::lookup and bracketed in begin_gpu_use/end_gpu_use.
  //
  // F4 adds a single Depth32Float attachment with clear=1.0/store and Metal's
  // eight compare operations. Stencil, blending, face culling and perspective
  // divide remain out of scope.
  bool run_raster_triangles(core::Arena& arena, core::FacetPool& pool, core::RasterFacetPair facets,
                           const RasterDesc& desc,
                           const std::vector<RasterVertex>& vertices, RasterResult* result,
                           std::string* error = nullptr) const;

  // Representation transform: build target-kind MTLTexture from the linear
  // device buffer, Arena::transform to a new epoch, retire the facets the old
  // epoch invalidated, and acquire out_facet into `pool`. Does not
  // ConsumeInput (02 §4.2): a standalone adapter entry point must not infer a
  // destructive transform (06 §11), so the old host bytes remain until a later
  // consume asks for one with a complete proof. The Stage 5 path that *can*
  // consume is ExecutionPlan::representation_requests through submit().
  bool run_representation_transform(core::Arena& arena, core::FacetPool& pool,
                                   const core::CanonicalView& view, core::FacetKind target_kind,
                                   RepresentationTransformResult* result,
                                   std::string* error = nullptr) const;

  // Drops this adapter's copies of linear backing that
  // Arena::consume_representation already cleared, and MTLTextures whose
  // FacetRef no longer resolves. submit() does this after Stage 5. A caller
  // that consumed through core without going through submit() (the standalone
  // transform + consume path) must call it, or the device watermark will keep
  // the copies the host already handed back (06 §11).
  void reclaim_released_backing(const core::Arena& arena) const;

  // E013 (06 §7, 05 §11): compiles a real state matrix twice and reports what
  // each discipline cost. Every pipeline it creates is a real
  // MTLComputePipelineState/MTLRenderPipelineState; the counts are objects that
  // exist, not a model of what compiling would have cost.
  bool run_pipeline_classification(PipelineClassificationRun* result,
                                  std::string* error = nullptr) const;

  [[nodiscard]] const std::vector<std::array<uint32_t, 3>>& last_tier1_indirect_dims() const;
 private:
  using Impl = DeviceHal::Impl;
  DeviceHal& device_;
  Impl* impl_;
};
}  // namespace vg::metal
#endif
