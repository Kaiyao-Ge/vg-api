#ifndef VG_BACKENDS_METAL_DEVICE_INTERNAL_H_
#define VG_BACKENDS_METAL_DEVICE_INTERNAL_H_
#include "backends/metal/metal_device_hal.h"
#include "backends/metal/metal_physical_types.h"
#include "backends/metal/metal_diagnostics.h"
#include "compiler/pipeline_classification.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <map>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace vg::metal {
struct MetalAllocationRecord {
  id<MTLBuffer> buffer = nil;
  uint64_t allocation_id{};
  uint32_t generation{};
  size_t byte_size{};
  uint64_t content_epoch{};
};

// Keyed by FacetRef index+generation (06 §6.4). Invalidated when the live
// FacetPool slot's epoch/kind/size/swizzle no longer match. 06 §6.1 lists
// dimension, levels and slices among the inputs a SampleFacet compiles from, so
// they belong in the invalidation comparison too: two views differing only in
// mip_levels are different contracts and must not share a cached MTLTexture.
struct MetalFacetRecord {
  // Shader-visible object: a swizzled texture view when the view asks for one,
  // otherwise the same object as `storage_texture`.
  id<MTLTexture> texture = nil;
  // Owner of the pixel storage. Uploads and host readback go through this one,
  // since a swizzle must not be applied twice on the host round trip.
  id<MTLTexture> storage_texture = nil;
  uint32_t facet_index{};
  uint32_t facet_generation{};
  uint32_t representation_epoch{};
  core::FacetKind kind{};
  uint32_t width{};
  uint32_t height{};
  core::ViewDimension dimension{};
  uint32_t array_layers{};
  uint32_t mip_levels{};
  core::PixelFormat format{};
  core::SwizzleChannels swizzle{};
  uint64_t content_epoch{};
};

class FacetUseGuard {
 public:
  FacetUseGuard(core::FacetPool& pool, core::FacetRef ref);
  FacetUseGuard(const FacetUseGuard&) = delete;
  FacetUseGuard& operator=(const FacetUseGuard&) = delete;
  FacetUseGuard(FacetUseGuard&&) = delete;
  FacetUseGuard& operator=(FacetUseGuard&&) = delete;
  ~FacetUseGuard();

  bool begin(const core::Arena& arena, std::string* error);

 private:
  core::FacetPool& pool_;
  core::FacetRef ref_;
  bool held_{};
};

bool same_swizzle(const core::SwizzleChannels& lhs, const core::SwizzleChannels& rhs);
bool same_shape(const MetalFacetRecord& record, const core::CanonicalView& view);
MTLTextureSwizzle to_mtl_swizzle(core::Swizzle swizzle);
MTLPixelFormat to_mtl_pixel_format(core::PixelFormat format);
MTLCompareFunction to_mtl_compare_function(core::DepthCompareOp op);
MTLTextureType to_mtl_texture_type(core::ViewDimension dimension);
MTLTextureUsage facet_texture_usage(core::FacetKind kind, const core::CanonicalView& view);
MTLTextureDescriptor* make_texture_descriptor(const core::CanonicalView& view, core::FacetKind kind,
                                              MTLStorageMode storage_mode);
bool view_expressible(const core::CanonicalView& view, core::FacetKind kind, uint64_t backing_bytes,
                      std::string* error);
bool subresource_in_range(const core::CanonicalView& view, const AttachmentSubresource& subresource,
                          std::string* error);
std::array<float, 4> decode_texel(const void* bytes, MTLPixelFormat format);
NSString* ns_utf8(std::string_view text);
bool is_pointer_graph_module(const ir::Module& module);
// Both formats this milestone models are 4 bytes wide, but they reach that
// width differently, so the width is asked for rather than assumed
// (core::bytes_per_texel is the single answer both backends encode against).
constexpr uint32_t kBytesPerTexel = 4;

// Clip-space -> pixel-space is Metal's own convention and the reference
// rasterizer documents matching it exactly: px = (x * 0.5 + 0.5) * width,
// py = (0.5 - y * 0.5) * height, i.e. +Y up in clip space and down in the
// image. Nothing here compensates for anything -- Metal already does this --
// but stating it keeps the two rasterizers' agreement a written contract
// rather than a coincidence.
constexpr const char* kRasterClipSpaceNote =
    "Metal clip space, +Y up: px = (x * 0.5 + 0.5) * width, py = (0.5 - y * 0.5) * height";

struct DeviceHal::Impl {
  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl();

  static void release_buffer(id<MTLBuffer>& buffer);

  static void release_facet_textures(MetalFacetRecord& record);

  // Only slots the pool has already stopped resolving, and only after this
  // backend's own waitUntilCompleted (every path here submits-and-waits), so
  // no texture is destroyed under work still in flight (06 §11).
  uint32_t retire_stale_facet_textures(const core::Arena& arena, const core::FacetPool& pool);

  // A ConsumeInput has cleared the allocation's host bytes (or the allocation
  // is gone). Leaving the Shared MTLBuffer would mean the peak-memory saving
  // E005 measures never materializes on the device side (06 §11).
  uint64_t release_empty_linear_buffers(const core::Arena& arena);

  void reclaim_released_backing(const core::Arena& arena, const core::FacetPool& pool,
                                uint32_t* retired_textures, uint64_t* released_linear);

  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  DeviceSnapshot snapshot{};

  id<MTLLibrary> library = nil;
  id<MTLComputePipelineState> pipeline = nil;
  std::string cached_ir_hash;
  std::unordered_map<uint64_t, MetalAllocationRecord> allocation_map;
  // One device-owned legacy root. It is immutable after creation and is
  // reused for every pre-F6 draw, avoiding a per-draw Metal allocation.
  id<MTLBuffer> identity_scene_root_buffer = nil;
  std::mutex identity_scene_root_mutex;

  // Lazily created on first timeline_wait/timeline_signal use. Its
  // signaledValue is the single source of truth for the device's timeline
  // position -- no separate host-side mirror, so there is nothing that can
  // drift out of sync with what the GPU actually observed.
  id<MTLSharedEvent> timeline_event = nil;
  id<MTLLibrary> task_ring_library = nil;
  id<MTLComputePipelineState> task_ring_pipeline = nil;
  id<MTLLibrary> cull_compact_library = nil;
  id<MTLComputePipelineState> cull_compact_pipeline = nil;
  id<MTLLibrary> storage_facet_library = nil;
  id<MTLComputePipelineState> storage_facet_pipeline = nil;
  id<MTLComputePipelineState> storage_array_facet_pipeline = nil;
  id<MTLComputePipelineState> storage_buffer_facet_pipeline = nil;
  std::unordered_map<uint64_t, MetalFacetRecord> facet_map;
  // Keyed by (filter, wrap, lod_min bits, lod_max bits). The first two are the
  // 4 sampler-policy combinations 06 §6.1 names; the lod clamps exist because
  // the raster fragment stage has no explicit-level sample call, so pinning the
  // level a RasterDesc asks for is a sampler property there rather than a
  // shader argument. Never invalidated -- unlike textures, an MTLSamplerState
  // carries no allocation-derived state to go stale.
  std::map<std::array<uint32_t, 4>, id<MTLSamplerState>> sampler_cache;

  // 06 §7's pipeline cache, driven through the backend-neutral
  // compiler::PipelineClassificationCache so this adapter's hit/miss/compile_ns
  // accounting is the same discipline E013 compares across variants and
  // backends. The cache stores only measurements (vg_compiler cannot name an
  // MTLComputePipelineState), so the objects themselves live in the two maps
  // beside it, keyed by the very same PipelineKey::hash().
  compiler::PipelineClassificationCache pipeline_cache;
  std::unordered_map<uint64_t, id<MTLComputePipelineState>> compute_pipeline_by_key;
  std::unordered_map<uint64_t, id<MTLRenderPipelineState>> render_pipeline_by_key;
  // MTLDepthStencilState is not part of MTLRenderPipelineState, but the F4
  // depth test/write/compare choices are immutable encoder state and are kept
  // under the same classified PipelineKey discipline.  Keeping real objects
  // here makes cache hits observable and prevents recreating a descriptor per
  // draw.
  std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_stencil_by_key;
  std::unordered_map<std::string, id<MTLLibrary>> library_by_hash;
  // Bound only on the path where the in-shader guard is expected to reject the
  // token before the kernel's first sample (see run_sample_facet). It is never
  // read by a shader; it exists because MSL requires a texture argument to be
  // bound even when the specialized function returns before touching it.
  id<MTLTexture> guard_placeholder_texture = nil;
  // TASK-B13: debug/test introspection only, see AdapterHarness::last_tier1_indirect_dims().
  std::vector<std::array<uint32_t, 3>> last_tier1_indirect_dims;
  // Debug/test introspection only. Entries are appended immediately before
  // the corresponding dispatchThreadgroups call in dispatch_compute_task(), so
  // this observes real encoder arguments rather than Task-ring bytes.
  mutable std::vector<NodeAwareDispatchObservation> last_node_aware_dispatches;

  bool ensure_timeline_event(std::string* error);

  struct MslModule {
    std::string_view ir_hash;
    std::string_view source;
  };
  struct ShaderEntry {
    std::string_view source;
    std::string_view entry;
  };
  struct LibraryText {
    std::string_view source;
    std::string_view hash;
  };
  struct LodClamp {
    float min;
    float max;
  };
  struct TaskRingBuffers {
    id<MTLBuffer> state{};
    id<MTLBuffer> fields{};
    id<MTLBuffer> inputs{};
  };

  // Attempts to (re)compile the B4 MSL source into a pipeline, caching by IR
  // hash. Failure here is the sole source of truth for whether this GPU/OS
  // combination can run the module natively -- in particular, a module using
  // the 8-byte atomic_add path fails here if the device/driver lacks native
  // 64-bit atomics, and the caller treats that as a HostAssisted signal
  // rather than guessing at GPU family enums ahead of time.
  bool ensure_pipeline(const MslModule& compiled_msl, std::string* error,
                      const std::string& function_name = "vg_linear_compute");

  // Per-program cache: every immutable Node generation in one TaskGraph may
  // name a distinct program and all pipelines must coexist through submit.
  std::unordered_map<std::string, std::pair<id<MTLLibrary>, id<MTLComputePipelineState>>> node_pipelines;

  bool ensure_node_pipeline(const MslModule& compiled_msl,
                            id<MTLComputePipelineState>* out_pipeline, std::string* error,
                            const std::string& function_name = "vg_linear_compute",
                            bool* cache_hit = nullptr);

  // Creates or reuses a Shared-storage MTLBuffer for `allocation`, uploading
  // its current bytes so the kernel observes the same starting state the
  // reference oracle would. Shared storage keeps this vertical slice on the
  // M1 unified-memory fast path without an explicit blit.
  id<MTLBuffer> ensure_buffer(const core::Allocation& allocation);

  // `created` is only observability for the submission report: it distinguishes
  // the one device-local allocation from subsequent legacy draws without
  // exposing the Metal object outside this adapter.
  id<MTLBuffer> make_identity_scene_root_buffer(bool* created = nullptr);

  // Commit a completed GPU buffer write to the canonical F7 byte store and
  // stamp the retained Shared mirror at the same content revision.
  void commit_buffer_write(core::Allocation& allocation, id<MTLBuffer> buffer);


  static uint64_t facet_cache_key(core::FacetRef ref);

  // Shared prologue for every facet use: resolve the capability token, reject
  // a kind mismatch, and produce the diagnostic the FacetPool itself
  // classified rather than a generic "stale" string.
  static const core::FacetSlot* resolve_facet(const core::Arena& arena, const core::FacetPool& pool,
                                       core::FacetRef ref, core::FacetKind expected_kind,
                                       std::string* error);

  // AddressFacet/TransferFacet resolve to the allocation's linear device
  // buffer -- no texture object exists on this path (02 §3.3).
  id<MTLBuffer> ensure_facet_buffer(const core::Arena& arena, const core::FacetPool& pool,
                                    core::FacetRef ref, core::FacetKind expected_kind,
                                    std::string* error);

  // Host readback of a `width` x `height` window at (origin_x, origin_y) of one
  // (slice, level) subresource, decoded to float4 row-major. Once a
  // representation transform has moved a facet to Private storage there is no
  // host-visible mapping left, so that case has to go back through a blit
  // rather than getBytes -- one function for both so the two paths cannot
  // disagree about layout.
  bool read_texture_region(id<MTLTexture> texture, uint32_t slice, uint32_t level, uint32_t origin_x,
                           uint32_t origin_y, uint32_t width, uint32_t height,
                           std::vector<std::array<float, 4>>* out, std::string* error);

  bool read_texel(id<MTLTexture> texture, uint32_t slice, uint32_t level, uint32_t x, uint32_t y,
                  std::array<float, 4>* out, std::string* error);

  // Publishes `storage_texture` as the backend object behind `ref`, deriving
  // the shader-visible swizzle view when the contract asks for one. Shared by
  // the lazily-created Shared texture path and by the representation
  // transform, so a transformed facet lands in the cache the same shape as
  // any other.
  id<MTLTexture> install_facet_record(core::FacetRef ref, const core::CanonicalView& view,
                                      core::FacetKind kind, uint32_t representation_epoch,
                                      id<MTLTexture> storage_texture, std::string* error);

  // Uploads every subresource the view declares, decoded through
  // CanonicalView's own linear layout contract (slice-major, then ascending
  // mip level, each level tightly packed at bytes_per_row(level)). This is the
  // same contract reference::sample_facet decodes, so an image comparison
  // between the two backends is comparing sampling, not two disagreeing
  // opinions about byte layout.
  void upload_view_subresources(id<MTLTexture> texture, const core::CanonicalView& view,
                                const core::Allocation& allocation);

  // FacetPool::lookup first; cache keyed by FacetRef index+generation.
  // Returns the shader-visible object; `out_storage` (optional) receives the
  // object that owns the pixels, which is what host upload/readback must use.
  id<MTLTexture> ensure_facet_texture(const core::Arena& arena, const core::FacetPool& pool,
                                      core::FacetRef ref, core::FacetKind expected_kind,
                                      bool* cache_hit, id<MTLTexture>* out_storage,
                                      std::string* error);

  // Small permanent cache, never invalidated -- unlike textures, an
  // MTLSamplerState carries no allocation-derived state to go stale.
  //
  // mipFilter follows the same FilterMode the min/mag filters do, which is what
  // makes an explicit level(lod) actually select a mip: with the default
  // MTLSamplerMipFilterNotMipmapped Metal samples level 0 whatever level the
  // shader asks for, i.e. it would silently ignore the coordinate. Nearest maps
  // to MipFilterNearest (the GL/Vulkan round-half-down level rule the reference
  // oracle documents) and Bilinear to MipFilterLinear (full trilinear on a
  // fractional lod, again matching the oracle).
  id<MTLSamplerState> ensure_sampler_state(core::FilterMode filter, core::WrapMode wrap, LodClamp lod);

  // Encodes one command buffer that optionally waits on `wait_value` before
  // dispatching and signals `signal_value` after -- both 0 mean "no
  // timeline involvement," preserving the original B4/B5 synchronous path.
  // When `tasks` is non-empty, dispatches once per task using that task's
  // own x/y/z grid dimensions instead of the hardcoded (1,1,1); this is the
  // B8 Tier0 requirement that dispatch sizing come from real TaskRecord
  // fields, not a placeholder.
  bool dispatch_and_wait(const std::vector<id<MTLBuffer>>& buffers, const std::vector<core::TaskRecord>& tasks,
                        core::TimelineGate gate, DispatchStats* stats, std::string* error) const;

  // TASK-B16 (E007): lazily probes whether MTLBuffer.gpuAddress is actually
  // available on this OS/device combination, rather than trusting the
  // capability snapshot's own gpu_addresses bit -- cached after the first
  // call since this answer cannot change within a process lifetime.
  bool gpu_addresses_probed = false;
  bool gpu_addresses_supported_value = false;
  bool probe_gpu_addresses();

  // Argument-buffer-style indexed dispatch (TASK-B16/E007): binds exactly
  // one table buffer -- real GPU virtual addresses, one per object in
  // `object_buffers` -- at buffer(0), never each object's own buffer(N)
  // slot. That single-binding shape is the whole point of the contrast
  // against dispatch_and_wait's per-object binding loop above. Every object
  // buffer still needs useResource: for GPU residency even though it is
  // never itself bound at an index -- a real, distinct cost this milestone
  // deliberately reports rather than hides.
  bool dispatch_indexed_and_wait(const std::vector<id<MTLBuffer>>& object_buffers, core::TimelineGate gate,
                                DispatchStats* stats, std::string* error) const;

  // One conservative schedule step. MD-4 deliberately gives every logical
  // Task its own command buffer and waits for completion before beginning the
  // next sealed schedule step. This is slower than wave-parallel encoding but
  // makes compute/render visibility domain-neutral and gives Stage 6 exact
  // physical evidence: no backend-local EffectGraph reconstruction, hidden
  // fence, or implicit cross-encoder assumption is involved.
  bool dispatch_compute_task(id<MTLComputePipelineState> pipeline,
                             const std::vector<id<MTLBuffer>>& buffers,
                             const core::TaskRecord& task, uint32_t task_index,
                             uint32_t pipeline_ordinal, bool* submitted,
                             DispatchStats* stats,
                             std::string* error) const;

  // Compiles compiler::task_ring_metal_source() into its own pipeline,
  // separate from the B4 linear-compute pipeline: the publication protocol
  // is backend-private infrastructure, not part of the target-neutral
  // ComputePackage contract.
  bool ensure_task_ring_pipeline(std::string* error);

  // One thread per task (grid = (count,1,1) threadgroups of (1,1,1)
  // threads), so no two threads ever contend for the same ring slot --
  // each slot's Empty->Writing CAS can only ever be attempted once.
  bool dispatch_task_publish(TaskRingBuffers buffers, uint32_t count, DispatchStats* stats, std::string* error) const;

  // TASK-B13 (E009): compiles compiler::cull_compact_metal_source() into its
  // own pipeline, mirroring ensure_task_ring_pipeline()'s pattern -- this is
  // backend-private infrastructure, not part of the target-neutral
  // ComputePackage contract.
  bool ensure_cull_compact_pipeline(std::string* error);

  // ---- 06 §7 pipeline cache -------------------------------------------------
  //
  // "Pipeline cache key 包含：CodeObject hash、entry、function constants、
  // attachment formats/sample count、raster state 中 Metal 必须编译固定的部分、
  // OS/GPU/compiler identity。小的动态状态不应无故扩大 key." Every pipeline this
  // backend builds for a facet or raster use goes through the four helpers
  // below, so no entry point can quietly invent its own key discipline.
  //
  // target_identity is deliberately one opaque string that is only ever hashed:
  // 05 §10 makes a backend binary cache explicitly non-portable, and a key that
  // pretended to interpret the driver/compiler identity would be claiming
  // portability the artifact does not have.
  [[nodiscard]] std::string target_identity() const;

  [[nodiscard]] compiler::PipelineKey make_pipeline_key(const ShaderEntry& shader,
                                          std::vector<std::pair<std::string, uint64_t>> constants,
                                          std::vector<uint32_t> attachment_formats,
                                          uint32_t sample_count) const;

  // One MTLLibrary per distinct MSL text, so two specializations of the same
  // source share the compiled library and differ only in the function constant
  // values applied to it.
  id<MTLLibrary> ensure_library(const LibraryText& text, std::string* error);

  // A function specialized by the function constants a PipelineKey already
  // names. An unset constant is legal and means fast-native: the kernels read
  // it through is_function_constant_defined(), so a pipeline that leaves it
  // undefined compiles the guard away rather than failing to build.
  //
  // Always the constantValues: form, even for an empty constant set. Metal
  // refuses to build a pipeline from a function that declares any function
  // constant unless it was fetched through this selector, so the guard-off
  // specialization is "fetched with no values supplied", not "fetched
  // unspecialized" -- and a source with no constants at all is unaffected by
  // being asked the same way.
  static id<MTLFunction> ensure_function(id<MTLLibrary> library_object, const compiler::PipelineKey& key,
                                  std::string* error);

  bool acquire_compute_pipeline(compiler::PipelineClassificationCache& cache,
                                std::unordered_map<uint64_t, id<MTLComputePipelineState>>& objects,
                                const std::string& source, const compiler::PipelineKey& key,
                                const std::string& trigger, id<MTLComputePipelineState>* out,
                                compiler::SpecializationReport* report, std::string* error);

  bool acquire_render_pipeline(compiler::PipelineClassificationCache& cache,
                               std::unordered_map<uint64_t, id<MTLRenderPipelineState>>& objects,
                               const std::string& source, const compiler::PipelineKey& key,
                               const std::string& vertex_entry, MTLPixelFormat color_format,
                               MTLPixelFormat depth_format,
                               const std::string& trigger, id<MTLRenderPipelineState>* out,
                               compiler::SpecializationReport* report, std::string* error);

  bool ensure_depth_stencil_state(const compiler::PipelineKey& key, bool test_enable,
                                  bool write_enable, core::DepthCompareOp compare_op,
                                  id<MTLDepthStencilState>* out, bool* cache_hit,
                                  std::string* error);

  // The SampleFacet kernel 06 §6.1 asks for, in the four shapes that actually
  // differ: texture2d vs texture2d_array (a CanonicalView's declared dimension,
  // not a host convenience) crossed with the 06 §6.4 generation guard being
  // compiled in or specialized away. `checked` is the only piece of state that
  // has to enter the pipeline key here; the lod value, the array slices and the
  // sampler are per-use data and bindings that must not.
  bool ensure_sample_facet_pipeline(bool array_dimension, bool checked,
                                    id<MTLComputePipelineState>* out, std::string* error);

  // Real MTLRenderPipelineState for the shared raster pair. Only the attachment
  // format and the sample count reach the key; viewport and tint are dynamic
  // state and shader-visible data respectively, and stay out of it.
  //
  // F3 (ADR-043 Decision #4): when `user_shader` is non-null, this builds the
  // pipeline from the caller's restricted-import MSL text and its declared
  // entry-point names instead of the built-in `raster_facet_metal_source()`
  // pair. make_pipeline_key/ensure_library/ensure_function/
  // acquire_render_pipeline are already content-hash-keyed and source/entry-
  // name-agnostic, so nothing below them needs to change -- only what gets
  // passed in does. A missing/malformed entry point surfaces as an ordinary
  // acquire_render_pipeline failure (Metal's own newFunctionWithName:/
  // newRenderPipelineStateWithDescriptor: linking failure), which this
  // returns through `error` exactly like the built-in path: a clean submit-
  // time failure, never a crash or a silent fallback to the built-in shader
  // (docs/START.md invariant 10).
  bool ensure_raster_pipeline(core::PixelFormat format, core::PixelFormat depth_format, bool has_depth,
                              uint32_t sample_count, bool depth_test_enable,
                              bool depth_write_enable, core::DepthCompareOp depth_compare_op,
                              id<MTLRenderPipelineState>* out, id<MTLDepthStencilState>* depth_out,
                              bool* depth_cache_hit, std::string* error,
                              const ir::UserRasterShaderContract* user_shader = nullptr);

  // 1x1 stand-in bound on the path where the checked-profile guard is expected
  // to reject the token before the kernel's first sample. It carries no facet's
  // pixels and is never read; binding the rejected facet's last-known texture
  // instead would be exactly the "resolve a stale token to its last-known
  // object" behaviour 02 §10 forbids.
  id<MTLTexture> ensure_guard_placeholder_texture(std::string* error);

  // StorageFacet write of one texel of one named subresource, in both shapes
  // 06 §6.2 allows: a writable texture and a linear device buffer. All three
  // entry points come from one library so the targets can never drift to
  // different write semantics.
  //
  // `target` is uint4 {x, y, layer, level}. The image forms address (x, y) of a
  // named layer/level rather than always texel 0, and the linear form lands at
  // that subresource's own byte offset -- a write that always hit texel 0 would
  // silently ignore the caller's coordinate, which is the shape of bug
  // START.md §4 invariant 10 exists to forbid.
  bool ensure_storage_facet_pipelines(std::string* error);

  // 06 §6.3's load/store/resolve, lowered onto one subresource of `texture`.
  // Shared by the draw-free attachment probe and by the raster draw so the two
  // cannot drift on what a store or a resolve means, and so the multisample
  // rule (a transient MS target resolving into the facet's own texture) is
  // stated once.
  MTLRenderPassDescriptor* make_render_pass(id<MTLTexture> texture, const AttachmentFacetDesc& desc,
                                            const core::CanonicalView& view,
                                            bool* store_traffic_avoided, std::string* error);

  // F2 (ADR-043 Decision #3): everything run_raster_triangles() does except
  // building the vertex/tint buffers -- moved here (not rewritten) so a
  // plan-driven raster TaskRecord (SubmitOps::raster, whose vertex buffer is
  // resolved through FacetPool rather than handed a host
  // std::vector<RasterVertex>) runs the exact same facet-acquisition /
  // pipeline / draw / readback code as the original hardware-verified path.
  // `vertex_count` stands in for `vertices.size()` since the caller-supplied
  // MTLBuffer alone carries no length.
  bool run_raster_pass(core::Arena& arena, core::FacetPool& pool, core::RasterFacetPair facets,
                       const RasterDesc& desc, id<MTLBuffer> vertex_buffer, id<MTLBuffer> scene_root_buffer,
                       id<MTLBuffer> tint_buffer,
                       uint32_t vertex_count, id<MTLBuffer> index_buffer, MTLIndexType index_type,
                       uint32_t index_count, RasterResult* result, std::string* error,
                       const ir::UserRasterShaderContract* user_shader = nullptr,
                       bool* command_submitted = nullptr);

  // ---- Representation transform, shared by both paths that perform one ------
  //
  // 02 §8: `retile`/format conversion is a representation transform, not a
  // barrier. Both the standalone run_representation_transform() and submit()'s
  // Stage 5 physical callback build their Private optimal texture here, so the
  // two cannot drift on which subresources get copied or on what the transform
  // costs.
  struct TransformCost {
    uint64_t new_backing_bytes{};
    uint64_t temporary_bytes{};
    uint32_t encoder_count{};
    uint32_t command_buffer_count{};
    uint32_t queue_wait_count{};
  };

  // Blits every subresource of `view` out of its linear backing into a fresh
  // Private texture and installs that texture as the backend object behind
  // `target_facet`. The blit source is reached through a TransferFacet over the
  // same CanonicalView, so even the transform's own read is a pool-resolved
  // capability and not a raw buffer handle -- and reusing the existing linear
  // backing means the transform needs no staging copy at all, which is why
  // temporary_bytes is honestly 0 rather than an assumed staging figure.
  bool transform_into_private_facet(core::Arena& arena, core::FacetPool& pool,
                                    const core::CanonicalView& view, core::FacetKind target_kind,
                                    core::FacetRef target_facet, TransformCost* cost, std::string* error);

  // TASK-B13 (E009): follows Tier0 task publication (fields_buffer already
  // holds every task's real x/y/z, GPU-resident from dispatch_task_publish)
  // with a real GPU-indirect dispatch pass, all within one command buffer --
  // Shared storage plus Metal's default automatic hazard tracking across
  // encoders in the same command buffer means no explicit MTLFence is
  // needed between the blit and the compute encoder below. For each task in
  // `order` (host-known count -- the task graph is a static host-authored
  // structure -- but the x/y/z *dims* are never read host-side before
  // dispatching), a blit copies that task's 3 dispatch-dim words from
  // fields_buffer into a dedicated indirect-args MTLBuffer, then
  // dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset: launches
  // the already-compiled module pipeline sized by those GPU-resident bytes.
  // Correctness note: this re-dispatches the same per-submission compute
  // pipeline once per task on top of the single dispatch dispatch_and_wait()
  // already issued, so it is only safe for idempotent instruction sets
  // (load/store) -- combining Tier1 with atomic_add is out of scope for this
  // milestone (see ADR-026).
  bool dispatch_task_tier1_indirect(const std::vector<id<MTLBuffer>>& buffers, id<MTLBuffer> fields_buffer,
                                    const std::vector<uint32_t>& order, id<MTLBuffer> indirect_args_buffer,
                                    DispatchStats* stats, std::string* error) const;
};
}  // namespace vg::metal
#endif
