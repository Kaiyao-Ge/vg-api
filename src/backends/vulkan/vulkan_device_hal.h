#ifndef VG_BACKENDS_VULKAN_DEVICE_HAL_H_
#define VG_BACKENDS_VULKAN_DEVICE_HAL_H_

#include "backends/device_hal.h"
#include "compiler/pipeline_classification.h"

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#if defined(VG_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace vg::vulkan {

// 07 §6 requires a facet use to report the descriptor work it really did:
// "若需要传统 descriptor set update，报告 update 次数/字节/CPU 时间". This
// backend has no descriptor-buffer path, so `used_descriptor_buffer` is always
// false and the counts below are real vkUpdateDescriptorSets accounting rather
// than a descriptor-indexing estimate. Deliberately not a public Bind Group
// (07 §6: "Descriptor indexing 是 facet table 的一种实现，不是公共 Bind
// Group") -- it is a cost report, and no caller can construct or bind one.
struct FacetDescriptorCost {
  uint32_t set_allocation_count{};
  uint32_t descriptor_write_count{};
  uint64_t descriptor_write_bytes{};
  uint64_t cpu_descriptor_ns{};
  bool used_descriptor_buffer{};
};

// 07 §6.2's Metal sibling (06 §6.2, "可读写 texture 或线性 buffer"): the caller
// chooses, and the backend never silently substitutes one for the other, nor
// rewrites the view's format to make a write legal.
enum class StorageFacetTarget : uint32_t { Image, LinearBuffer };

// 07 §9's load/store/resolve, lowered to VkAttachmentLoadOp/VkAttachmentStoreOp
// and VkResolveModeFlagBits. Per-use parameters, never state stored on a facet
// or on a public object (06 §6.3's rule, unchanged here).
enum class AttachmentLoadAction : uint32_t { Clear, Load, DontCare };
enum class AttachmentStoreAction : uint32_t { Store, DontCare, MultisampleResolve };

// SampleFacet use of a FacetRef (07 §6). Callers acquire the ref from
// core::FacetPool; these entry points never accept a raw CanonicalView as a
// capability token, and never hand back a VkImage/VkImageView.
struct SampleFacetResult {
  std::vector<std::array<float, 4>> sampled_rgba;
  bool facet_cache_hit{};
  // True only when the dispatched pipeline really was specialized with
  // constant_id 0 = true and the generation table/token/violation bindings were
  // written -- i.e. when the in-shader guard of 06 §6.4 actually ran. A
  // FastNative submission reports false rather than letting a caller believe a
  // stale token would have been caught.
  bool checked_generation{};
  uint32_t violation_count{};
  FacetDescriptorCost descriptors;
  vg::hal::LoweringReport report;
};

struct StorageFacetResult {
  std::array<float, 4> written_rgba{};
  bool facet_cache_hit{};
  StorageFacetTarget target{StorageFacetTarget::Image};
  FacetDescriptorCost descriptors;
  vg::hal::LoweringReport report;
};

// One vkCmdBeginRendering pass against an AttachmentFacet (07 §9's dynamic
// rendering). `vertices` empty means load/store/resolve only and no draw at
// all -- that is the pure AttachmentFacet lowering; non-empty additionally
// draws them through raster_facet_vulkan_source(), which is the raster
// lowering of 05 §9's `region.attachment.store`. Each vec4 is (xy = clip-space
// position, zw = uv), byte-identical to the two-vec2 MSL struct.
struct RasterPassDesc {
  AttachmentLoadAction load{AttachmentLoadAction::Clear};
  AttachmentStoreAction store{AttachmentStoreAction::Store};
  std::array<float, 4> clear_rgba{};
  // Shader-visible per-draw data, so a UBO rather than a specialization
  // constant: it must not enter the pipeline key (07 §9, 06 §7).
  std::array<float, 4> tint{{1.0f, 1.0f, 1.0f, 1.0f}};
  std::vector<std::array<float, 4>> vertices;
  // >1 renders into a transient multisample VkImage that resolves into the
  // facet's own image through VkRenderingAttachmentInfo::resolveImageView.
  // Pipeline-key state (07 §9), not dynamic state.
  uint32_t sample_count{1};
  // Vulkan dynamic state (VK_DYNAMIC_STATE_VIEWPORT/SCISSOR), so deliberately
  // absent from the pipeline key. 0 means "the attachment's full extent".
  uint32_t viewport_width{};
  uint32_t viewport_height{};
};

struct RasterPassResult {
  std::array<float, 4> resolved_rgba{};
  bool facet_cache_hit{};
  uint32_t sample_count{1};
  uint32_t draw_count{};
  uint64_t pipeline_key_hash{};
  bool pipeline_cache_hit{};
  FacetDescriptorCost descriptors;
  vg::hal::LoweringReport report;
};

// One material/state combination for E013's "naive full permutation vs VG
// classification" comparison. `state` arrives already classified by whoever
// knows this backend's real constraints -- attachment format and sample count
// are structural key inputs (07 §9 fixes both in the Vulkan pipeline), so they
// are named fields rather than blocks a caller could mis-classify as dynamic.
struct RasterPipelineVariant {
  vg::core::PixelFormat attachment_format{vg::core::PixelFormat::RGBA8Unorm};
  uint32_t sample_count{1};
  std::vector<vg::compiler::StateBlock> state;
};

// Both halves of E013's measurement, each a count of VkPipeline objects this
// backend really created: `naive_pipeline_count` folds every StateBlock into
// the key (the "naive full permutation" variant), `classified_pipeline_count`
// folds only PipelineKey-classified ones (07 §9's four-way split). Nothing is
// estimated -- a difference between the two is a difference in pipelines that
// were actually compiled.
struct PipelineClassificationResult {
  uint32_t naive_pipeline_count{};
  uint32_t classified_pipeline_count{};
  uint32_t naive_cache_hits{};
  uint32_t classified_cache_hits{};
  uint64_t naive_compile_ns{};
  uint64_t classified_compile_ns{};
  std::vector<vg::compiler::SpecializationReport> classified_specializations;
  vg::hal::LoweringReport report;
};

// Owns the Vulkan instance/device objects used by the adapter.  Backend handles
// remain private; callers only observe the versioned DeviceHal contract.
class DeviceHal final : public vg::hal::DeviceHal {
 public:
  ~DeviceHal() override;

  DeviceHal(const DeviceHal&) = delete;
  DeviceHal& operator=(const DeviceHal&) = delete;
  DeviceHal(DeviceHal&&) = delete;
  DeviceHal& operator=(DeviceHal&&) = delete;

  const vg::hal::CapabilitySnapshot& capabilities() const override;
  bool compile(const vg::hal::ExecutionPlan& plan,
               vg::hal::CompiledPlan* compiled,
               std::string* error = nullptr) override;
  bool submit(const vg::hal::CompiledPlan& compiled, vg::core::Arena& arena,
              vg::hal::Submission* submission,
              std::string* error = nullptr) override;

  // Phase C facet entry points, the Vulkan siblings of Metal's standalone
  // run_*_facet methods (metal_device_hal.h). Same semantics, different
  // lowering: each resolves `ref` through core::FacetPool::lookup before
  // touching a Vulkan object and brackets its command buffer in
  // begin_gpu_use/end_gpu_use so the slot cannot be recycled under work still
  // in flight (07 §6's step 6, 06 §6.4/§11) -- which is why `pool` is
  // non-const. The backend cache is keyed by FacetRef index+generation (plus
  // epoch/kind/extent/format/swizzle/dimension/layers/mips), never by a
  // VkImage handle exposed on the public API.
  //
  // Every one of these is compile-review-only on this project: no Vulkan
  // hardware is reachable from this machine (permanent constraint, ADR-024),
  // so the vkCmd* sequences below have been written and reviewed against the
  // Vulkan contract, never executed.
  //
  // Sample: dispatches compiler::sample_facet_vulkan_source() (or the
  // sample_facet_array_vulkan_source() sibling when the view's dimension is
  // Texture2DArray) with the combined image sampler bound through a
  // descriptor set -- not BDA, for the reason compiler.h states: a combined
  // image sampler is always descriptor-set bound in Vulkan regardless of BDA
  // use elsewhere. Under core::ValidationProfile::CheckedNative the pipeline
  // is specialized with constant_id 0 = true and the facet token/generation
  // table/slot count/violation counter are bound (06 §6.4); any other profile
  // dispatches the unspecialized fast-native shape and says so through
  // SampleFacetResult::checked_generation.
  bool run_sample_facet(const vg::core::Arena& arena, vg::core::FacetPool& pool, vg::core::FacetRef ref,
                        vg::core::FilterMode filter, vg::core::WrapMode wrap,
                        const std::vector<std::array<float, 2>>& uv_coords, float lod,
                        const std::vector<uint32_t>& array_slices,
                        vg::core::ValidationProfile profile, SampleFacetResult* result,
                        std::string* error = nullptr);

  // Storage: a writable VkImage (StorageFacetTarget::Image) or the
  // allocation's linear BDA buffer (LinearBuffer). A format whose optimal
  // tiling does not advertise VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT is reported
  // Unsupported -- the view's format is never rewritten to make the write
  // legal (06 §6.2, START.md §4 invariant 10).
  bool run_storage_facet(const vg::core::Arena& arena, vg::core::FacetPool& pool, vg::core::FacetRef ref,
                         StorageFacetTarget target, const std::array<float, 4>& write_rgba,
                         StorageFacetResult* result, std::string* error = nullptr);

  // Attachment/raster: one vkCmdBeginRendering pass against `attachment_ref`.
  // `source_ref` is the SampleFacet the fragment stage reads and is required
  // only when desc.vertices is non-empty; an attachment-only pass (no draw)
  // ignores it, allocates no descriptor set and compiles no pipeline.
  bool run_raster_facet(const vg::core::Arena& arena, vg::core::FacetPool& pool,
                        vg::core::FacetRef attachment_ref, vg::core::FacetRef source_ref,
                        const RasterPassDesc& desc, RasterPassResult* result,
                        std::string* error = nullptr);

  // E013: compiles `variants` twice against compiler::PipelineClassificationCache
  // -- once with every StateBlock folded into the key, once honoring 07 §9's
  // four-way split -- creating real VkPipeline objects on each miss. A variant
  // carrying a StateBlockKind::UnsupportedNeedsConversion block is rejected
  // with a VG-concept diagnostic rather than folded into a key that would
  // compile a pipeline meaning something slightly different from what was
  // asked for.
  bool run_pipeline_classification(const std::vector<RasterPipelineVariant>& variants,
                                   PipelineClassificationResult* result, std::string* error = nullptr);

#if defined(VG_HAS_VULKAN)
  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical_device() const { return physical_device_; }
  VkDevice device() const { return device_; }
  uint32_t compute_queue_family() const { return compute_queue_family_; }
#endif

 private:
  DeviceHal() = default;

  // Shared body for both make_device_hal() overloads (ADR-044, F1). uuid ==
  // nullptr keeps the pre-F1 behavior of taking the first enumerated
  // physical device; a non-null uuid must match one vulkan_adapters()
  // produced (vendorID + deviceID + first 8 bytes of pipelineCacheUUID) or
  // this fails with VG-concept "no Vulkan physical device matches the
  // requested adapter uuid" rather than silently falling back to the
  // default (04-public-c-abi.md Sec.17 forbids implicit GPU selection).
  static std::unique_ptr<DeviceHal> create_impl(const uint8_t* uuid, std::string* error);

  vg::hal::CapabilitySnapshot capabilities_;

  // What vkGetPhysicalDeviceFormatProperties reported for one core::PixelFormat
  // under VK_IMAGE_TILING_OPTIMAL, snapshotted once at device creation (07 §1:
  // "所有 extension/feature 均运行时协商"). These are what the Raster /
  // RepresentationTransform / CheckedFacetGeneration capability bits are
  // derived from, and what a per-request rejection cites: a format that cannot
  // be sampled, stored to, or used as an attachment on this device produces an
  // Unsupported LoweringEvent instead of a silently substituted format.
  struct FormatSupport {
    bool sampled_image{};
    bool storage_image{};
    bool color_attachment{};
    bool transfer_dst{};
    bool transfer_src{};
  };
  FormatSupport rgba8_support_;
  FormatSupport r32f_support_;
  // 05 §10 makes a backend binary cache explicitly non-portable, so
  // compiler::PipelineKey::target_identity carries this device's identity
  // (name + API version + driver version) into every pipeline key rather than
  // letting a key look portable across drivers.
  std::string target_identity_;
  const FormatSupport& format_support(vg::core::PixelFormat format) const {
    return format == vg::core::PixelFormat::RGBA8Unorm ? rgba8_support_ : r32f_support_;
  }
#if defined(VG_HAS_VULKAN)
  VkInstance instance_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue compute_queue_{VK_NULL_HANDLE};
  uint32_t compute_queue_family_{UINT32_MAX};
  // VkPhysicalDeviceLimits::framebufferColorSampleCounts, snapshotted at device
  // creation. A RasterPassDesc asking for a sample count outside this mask is
  // reported Unsupported rather than quietly rendered single-sampled, which
  // would silently change the image the caller asked for (07 §9).
  VkSampleCountFlags framebuffer_color_sample_counts_{};
  // VkPhysicalDeviceVulkan13Features::dynamicRendering, as enabled at device
  // creation. Kept as its own flag rather than re-derived from the Raster
  // capability bit: the bit additionally requires a graphics-capable queue and
  // a usable attachment format, so the two are not the same statement.
  bool supports_dynamic_rendering_{};

  // GPU-side buffer VG mints for a given core::Allocation, addressed via
  // buffer device address (BDA) rather than a descriptor set. `mapped` stays
  // valid for the buffer's lifetime: memory is host-visible-coherent, so no
  // explicit flush/invalidate is needed around dispatch (v1 simplification --
  // no staging buffer, mirrors Metal's Shared-storage-mode choice).
  struct AllocationRecord {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceAddress device_address{};
    void* mapped{nullptr};
    uint32_t generation{};
    size_t byte_size{};
  };

  VkShaderModule shader_module_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline compute_pipeline_{VK_NULL_HANDLE};
  VkCommandPool command_pool_{VK_NULL_HANDLE};
  std::string cached_ir_hash_;
  std::unordered_map<uint64_t, AllocationRecord> allocation_map_;

  // VK_SEMAPHORE_TYPE_TIMELINE, initialValue=0; created lazily on first
  // timeline_wait/timeline_signal use (mirrors Metal's lazily-created
  // MTLSharedEvent). Its counter, queried via vkGetSemaphoreCounterValue, is
  // the single source of truth for this device's timeline position -- no
  // separate host-side mirror is kept, so nothing can drift out of sync
  // with what the GPU actually reached.
  VkSemaphore timeline_semaphore_{VK_NULL_HANDLE};

  // Task ring publication kernel (Tier0), compiled from
  // compiler::task_ring_vulkan_source() into its own shader module/layout/
  // pipeline, kept separate from shader_module_/pipeline_layout_/
  // compute_pipeline_ above: the publication protocol is backend-private
  // infrastructure, not part of the target-neutral ComputePackage contract
  // (mirrors Metal's task_ring_library/task_ring_pipeline separation).
  VkShaderModule task_ring_shader_module_{VK_NULL_HANDLE};
  VkPipelineLayout task_ring_pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline task_ring_pipeline_{VK_NULL_HANDLE};

  // Ephemeral GPU-side buffers for one task-graph submission. Sized to the
  // task count and recreated per submit() call rather than cached in
  // allocation_map_, since task_graph size varies call to call and these
  // buffers are backend-private (never exposed through core::Allocation).
  struct TaskRingBuffers {
    VkBuffer state_buffer{VK_NULL_HANDLE};
    VkDeviceMemory state_memory{VK_NULL_HANDLE};
    VkDeviceAddress state_address{};
    void* state_mapped{nullptr};
    VkBuffer fields_buffer{VK_NULL_HANDLE};
    VkDeviceMemory fields_memory{VK_NULL_HANDLE};
    VkDeviceAddress fields_address{};
    void* fields_mapped{nullptr};
    VkBuffer inputs_buffer{VK_NULL_HANDLE};
    VkDeviceMemory inputs_memory{VK_NULL_HANDLE};
    VkDeviceAddress inputs_address{};
    void* inputs_mapped{nullptr};
    // Tier1 conformance floor: one VkDispatchIndirectCommand-sized (12-byte,
    // 4-byte-aligned) slot per task, populated from fields_buffer's x/y/z
    // words via vkCmdCopyBuffer -- see dispatch_task_ring_and_tier1's doc
    // comment for why no host-side repacking is needed.
    VkBuffer indirect_buffer{VK_NULL_HANDLE};
    VkDeviceMemory indirect_memory{VK_NULL_HANDLE};
    uint32_t task_count{};
  };

  // The Vulkan objects one FacetPool slot resolves to (07 §6's steps 2-3:
  // "adapter 选择/创建 compatible image backing" then "创建 view/sampler").
  // Nothing here is reachable from the public API: a GPU-side capability stays
  // a FacetRef index+generation, and every entry point above takes that rather
  // than a VkImage. `layout` is the adapter-maintained backend representation
  // state 07 §7 requires ("adapter 维护当前 backend representation state"), and
  // is what makes a layout transition an event separate from a representation
  // transform. No VkSampler lives here on purpose: filter/wrap are per-use
  // sampler policy rather than part of the CanonicalView contract (06 §6.1),
  // so samplers are cached in `sampler_cache_` keyed by (filter, wrap) and are
  // shared across facets -- storing one per facet record would either
  // duplicate them or make the same view need a new facet to be sampled
  // differently.
  struct VulkanFacetRecord {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkImageViewType view_type{VK_IMAGE_VIEW_TYPE_2D};
    VkExtent3D extent{};
    uint32_t array_layers{1};
    uint32_t mip_levels{1};
    uint32_t facet_index{};
    uint32_t facet_generation{};
    uint32_t representation_epoch{};
    vg::core::FacetKind kind{vg::core::FacetKind::Sample};
    uint64_t backing_bytes{};
    // requirements.size - backing_bytes for this image's dedicated allocation.
    // The only fragmentation this backend can actually observe (it runs no
    // suballocator), so it is what heap_fragmentation_bytes reports rather
    // than a modelled heap number (10 §12: an unobservable cost is not written
    // as a real one).
    uint64_t allocation_padding_bytes{};
  };

  // Every field of 07 §6's facet contract that changes what the image *is*, so
  // two different contracts can never share one cached image. FacetRef
  // index+generation alone would be too weak (a recycled slot at the same
  // generation still names a different view), and the allocation id alone would
  // be too strong (two facets of one allocation are two facets, 02 §3.3).
  struct FacetImageKey {
    uint32_t facet_index{};
    uint32_t facet_generation{};
    uint32_t representation_epoch{};
    uint32_t kind{};
    uint32_t format{};
    uint32_t view_type{};
    uint32_t width{};
    uint32_t height{};
    uint32_t array_layers{};
    uint32_t mip_levels{};
    uint32_t swizzle{};  // four core::Swizzle channels packed one per byte

    bool operator<(const FacetImageKey& other) const {
      return std::tie(facet_index, facet_generation, representation_epoch, kind, format, view_type, width,
                      height, array_layers, mip_levels, swizzle) <
             std::tie(other.facet_index, other.facet_generation, other.representation_epoch, other.kind,
                      other.format, other.view_type, other.width, other.height, other.array_layers,
                      other.mip_levels, other.swizzle);
    }
  };

  // std::map rather than a hashed key: an ordered comparison of the whole
  // contract cannot alias two different contracts the way a 64-bit mix of
  // eleven fields could, and this cache is looked up once per facet use, not
  // per texel.
  std::map<FacetImageKey, VulkanFacetRecord> facet_images_;
  // Only four real combinations (Nearest/Bilinear x Clamp/Repeat) ever occur;
  // keyed by (filter << 1 | wrap) so lookup stays an integer compare.
  std::map<uint32_t, VkSampler> sampler_cache_;

  // Descriptor infrastructure for the Sample/Storage/Raster facet paths.
  // Traditional descriptor set updates, not descriptor indexing and not a
  // descriptor buffer: 07 §6 permits either as long as the cost is reported,
  // and FacetDescriptorCost is where these paths report theirs. The pool is
  // reset (not grown) before each use, which is safe because every entry point
  // here waits on its own VkFence before returning, so no set is ever live
  // across two calls.
  VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
  // Two layouts, not one: sample_facet_vulkan_source() declares bindings 0-3
  // and 5-8, while sample_facet_array_vulkan_source() additionally declares
  // binding 4 (per-coordinate array slices). Declaring binding 4 in both would
  // leave a binding the 2D kernel never writes, which is legal but makes the
  // reported descriptor_write_count stop matching the shader's real inputs.
  VkDescriptorSetLayout sample_set_layout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout sample_array_set_layout_{VK_NULL_HANDLE};
  VkPipelineLayout sample_pipeline_layout_{VK_NULL_HANDLE};
  VkPipelineLayout sample_array_pipeline_layout_{VK_NULL_HANDLE};
  VkShaderModule sample_shader_module_{VK_NULL_HANDLE};
  VkShaderModule sample_array_shader_module_{VK_NULL_HANDLE};
  // Keyed by (array_kernel << 1) | checked_profile: the checked and
  // fast-native shapes are two specializations of one module (constant_id 0),
  // which is exactly what 03 §12 asks for -- a profile changes what is
  // instrumented, never what the program means, so it must not be a second
  // shader.
  std::map<uint32_t, VkPipeline> sample_pipelines_;

  VkDescriptorSetLayout storage_set_layout_{VK_NULL_HANDLE};
  VkPipelineLayout storage_pipeline_layout_{VK_NULL_HANDLE};
  // Keyed by VkFormat: a GLSL storage image needs a format qualifier matching
  // the image's own format, so the two formats this project models are two
  // modules/pipelines rather than one pipeline reinterpreting memory.
  std::map<uint32_t, VkShaderModule> storage_shader_modules_;
  std::map<uint32_t, VkPipeline> storage_pipelines_;

  VkDescriptorSetLayout raster_set_layout_{VK_NULL_HANDLE};
  VkPipelineLayout raster_pipeline_layout_{VK_NULL_HANDLE};
  // raster_facet_vulkan_source() compiled twice, once per stage define
  // (VG_RASTER_VERTEX_STAGE / VG_RASTER_FRAGMENT_STAGE), because a GLSL
  // translation unit has exactly one entry point.
  VkShaderModule raster_vertex_module_{VK_NULL_HANDLE};
  VkShaderModule raster_fragment_module_{VK_NULL_HANDLE};
  // Keyed by compiler::PipelineKey::hash(): format and sample count are in the
  // key (07 §9 fixes both in a Vulkan pipeline), viewport/scissor are not
  // (they are VK_DYNAMIC_STATE_*), and the tint is not (it is a UBO the shader
  // reads).
  std::map<uint64_t, VkPipeline> raster_pipelines_;
  // E013's "naive full permutation" arm keeps its own map so a variant whose
  // naive and classified keys happen to coincide does not have one arm's
  // VkPipeline handed to the other -- the two counts have to be independent
  // measurements to mean anything.
  std::map<uint64_t, VkPipeline> naive_raster_pipelines_;
  vg::compiler::PipelineClassificationCache pipeline_cache_;
  vg::compiler::PipelineClassificationCache naive_pipeline_cache_;

  // (Re)compiles `glsl_source` (GLSL -> SPIR-V via a glslc subprocess -> a
  // VkPipeline bound only by a push-constant BDA-address array, no
  // descriptor sets), caching by IR hash. Failure here is the sole source of
  // truth for whether this device/driver can run the module -- unlike
  // Metal, no HostAssisted fallback is attempted: the target NVIDIA/Linux
  // hardware is expected to support this natively, so failure is reported
  // as Unsupported rather than silently degraded.
  bool ensure_pipeline(const std::string& ir_hash, const std::string& glsl_source, uint32_t binding_count,
                       std::string* error);
  // Creates or reuses a host-visible-coherent VkBuffer for `allocation`,
  // uploading its current bytes. Invalidated (recreated) on generation or
  // required-size mismatch, mirroring Metal's allocation_map policy.
  bool ensure_buffer(const core::Allocation& allocation, AllocationRecord** out, std::string* error);
  bool ensure_timeline_semaphore(std::string* error);
  bool ensure_task_ring_pipeline(std::string* error);
  bool create_task_ring_buffers(uint32_t task_count, TaskRingBuffers* out, std::string* error);
  void destroy_task_ring_buffers(TaskRingBuffers* buffers);
  // Single command buffer covering both B8 tiers: dispatches the Tier0
  // publish kernel (compiler::task_ring_vulkan_source()) over `order.size()`
  // tasks, inserts a vkCmdPipelineBarrier2 making the ring's write visible
  // to a transfer read, copies each published task's x/y/z window directly
  // into that task's indirect slot, inserts a second barrier2 making that
  // transfer write visible to indirect-command reads, then issues one
  // vkCmdDispatchIndirect per task against `compute_pipeline_` (bound with
  // `addresses` as its push-constant BDA array). This is the Tier1
  // conformance floor: dispatch sizing is read back from the GPU-published
  // Task ring, never from host-side TaskRecord.x/y/z (contrast Metal, where
  // Tier1/ICB remains a target rather than a requirement -- see ADR-021 vs
  // ADR-022).
  //
  // TASK-D4 (E010) compile-review-only: Vulkan has no Tier2 execution here
  // (no reachable hardware, ADR-024). The analogue of Metal's default
  // bucket compute + per-Node indirect is a GPU histogram / prefix-sum
  // over authorized node classes followed by one vkCmdDispatchIndirect
  // per class. Metal now prefers a GPU-encoded ICB for the same select.
  // VK_EXT_device_generated_commands (DGC) is the optional
  // capability upgrade matching Metal ICB -- not required, and never the
  // floor. Host-read-counts-then-vkCmdDispatch is Serialized/HostAssisted,
  // never DevicePass. Tier3 (GPU invents a Node / grows the envelope)
  // remains Unsupported.
  bool dispatch_task_ring_and_tier1(const TaskRingBuffers& buffers, const std::vector<uint32_t>& order,
                                    const std::vector<VkDeviceAddress>& addresses, std::string* error);
  // Records and submits a single dispatch on a transient command buffer.
  // wait_value/signal_value of 0 mean "no timeline involvement for this
  // side" -- their fields are omitted from VkTimelineSemaphoreSubmitInfo
  // entirely rather than submitted as a literal 0, matching core's guarantee
  // that a required_value of 0 is rejected before reaching the backend.
  // VkFence + vkWaitForFences remains the host-side completion mechanism;
  // the timeline semaphore is a GPU-ordering primitive layered on top of it,
  // not a replacement for it in this v1 backend.
  bool dispatch_and_wait(const std::vector<VkDeviceAddress>& addresses, uint64_t wait_value, uint64_t signal_value,
                        std::string* error);

  // --- Phase C facet/raster/Stage-5 infrastructure -------------------------
  // 07 §6's step 1 ("Core 固定 CanonicalView + RepresentationEpoch") is core's;
  // everything below is steps 2-6, and each helper stays private because none
  // of them can be expressed without naming a Vulkan type.

  // Shared prologue for every facet use: resolve the capability token and
  // reject a kind mismatch, reporting the status FacetPool itself classified
  // rather than a generic "stale" string (04 §4: every failure programmatically
  // determinable).
  static const vg::core::FacetSlot* resolve_facet(const vg::core::Arena& arena, const vg::core::FacetPool& pool,
                                                  vg::core::FacetRef ref, vg::core::FacetKind expected_kind,
                                                  std::string* error);

  // Creates or reuses the VkImage/VkImageView behind `ref`, uploading every
  // subresource from the backing allocation's bytes using the CanonicalView's
  // own layout contract (subresource_byte_offset / bytes_per_row), and leaves
  // the image in the layout its facet kind is read in. `upload_source`, when
  // not VK_NULL_HANDLE, is copied from directly (that is the Stage 5 case: the
  // allocation's own linear buffer already holds the bytes, so the transform
  // needs no staging buffer and honestly reports 0 temporary bytes); otherwise
  // a transient host-visible staging buffer is created and its size is
  // reported through `temporary_bytes`.
  //
  // A cache hit does not touch the allocation's bytes at all, which is what
  // lets a facet keep resolving after a ConsumeInput released the linear
  // backing the image superseded (02 §4.2).
  bool ensure_facet_image(const vg::core::Arena& arena, const vg::core::FacetPool& pool,
                          vg::core::FacetRef ref, vg::core::FacetKind expected_kind, VkBuffer upload_source,
                          VkDeviceSize upload_source_offset, VulkanFacetRecord** out, bool* cache_hit,
                          uint64_t* temporary_bytes, std::string* error);

  // Records the VkImageMemoryBarrier2 that moves `record` into `new_layout` and
  // updates the adapter's own representation state. Returns true when a barrier
  // was actually recorded, so callers can report a barrier count that matches
  // what the command buffer contains. 07 §7: a layout transition is a separate
  // reported event from a representation transform, never folded into it.
  static bool record_layout_transition(VkCommandBuffer command_buffer, VulkanFacetRecord* record,
                                       VkImageLayout new_layout);

  // Destroys every cached facet image whose FacetRef no longer resolves in
  // `pool` -- i.e. exactly the slots core::FacetPool::retire_stale already
  // retired (07 §6's step 6, "epoch 退休后回收"). Never retires an image whose
  // slot is still resolvable, and never touches the allocation's linear buffer.
  uint32_t retire_stale_facet_images(const vg::core::Arena& arena, const vg::core::FacetPool& pool);

  bool ensure_sampler(vg::core::FilterMode filter, vg::core::WrapMode wrap, VkSampler* out,
                      std::string* error);
  bool ensure_descriptor_pool(std::string* error);
  bool ensure_sample_facet_pipeline(bool array_kernel, bool checked_profile, VkPipeline* pipeline,
                                    VkPipelineLayout* layout, VkDescriptorSetLayout* set_layout,
                                    std::string* error);
  bool ensure_storage_facet_pipeline(VkFormat format, VkPipeline* pipeline, std::string* error);
  bool ensure_raster_shader_modules(std::string* error);
  // Creates (or returns from `pipelines`) the dynamic-rendering graphics
  // pipeline for `key`, with hit/miss and compile time accounted through
  // `cache` -- compiler::PipelineClassificationCache, deliberately not a second
  // pipeline cache invented here, so Metal and Vulkan pipeline counts stay
  // comparable numbers. `binary_size` receives 0 rather than a guess: this
  // backend does not enable VK_KHR_pipeline_executable_properties, so a
  // pipeline's real byte size is not observable here (10 §12).
  //
  // An entry of `raster_state` whose name this backend has no lowering for is
  // rejected rather than ignored: silently dropping a piece of state that was
  // classified as PipelineKey would compile a pipeline meaning something other
  // than what was asked for.
  bool ensure_raster_pipeline(vg::compiler::PipelineClassificationCache& cache,
                              std::map<uint64_t, VkPipeline>& pipelines, const vg::compiler::PipelineKey& key,
                              const std::string& trigger_reason, VkFormat attachment_format,
                              uint32_t sample_count,
                              const std::vector<std::pair<std::string, uint64_t>>& raster_state,
                              VkPipeline* pipeline, bool* cache_hit, uint64_t* binary_size, std::string* error);
  // What Stage 5 actually recorded and submitted, accumulated across a plan's
  // requests. Separate from hal::RepresentationTransformCost (which carries the
  // byte accounting core needs) because these are this backend's structural
  // counts, and submit() has to report counts matching the command buffers it
  // really issued rather than a per-request assumption -- a transform that hit
  // the facet image cache submits nothing and contributes nothing here.
  struct RepresentationStageCounts {
    uint64_t barrier_count{};
    uint64_t command_buffer_count{};
    uint64_t queue_wait_count{};
  };

  // Stage 5's physical step for one request, invoked from submit() through
  // hal::run_representation_stage. Builds the optimal-tiled image for `facet`
  // and copies the allocation's existing linear buffer into it.
  bool transform_representation(const vg::core::Arena& arena, const vg::hal::RepresentationRequest& request,
                                vg::core::FacetRef facet, vg::hal::RepresentationTransformCost* cost,
                                RepresentationStageCounts* counts, std::string* error);
#endif

  // Whether every one of `plan.representation_requests` can actually be
  // carried out by this backend on this device. Returns false with a
  // VG-concept `reason` naming the first request that cannot be, which
  // compile() turns into an Unsupported LoweringEvent -- START.md §4 invariant
  // 10: a semantic this hardware cannot express is rejected, never silently
  // dropped while the rest of the plan compiles as if nothing had been asked.
  bool can_lower_representation_requests(const vg::hal::ExecutionPlan& plan, std::string* reason) const;

  friend std::unique_ptr<DeviceHal> make_device_hal(std::string* error);
  friend std::unique_ptr<DeviceHal> make_device_hal(const uint8_t uuid[16], std::string* error);
};

std::unique_ptr<DeviceHal> make_device_hal(std::string* error = nullptr);

// ADR-044 (F1): honors a caller's openAdapter/createDevice choice instead of
// always taking the first enumerated physical device -- 04-public-c-abi.md
// Sec.17 forbids implicit GPU selection. uuid must match one
// vulkan_adapters() (vulkan_probe.cpp) already produced (vendorID +
// deviceID + first 8 bytes of pipelineCacheUUID). Returns nullptr and fills
// *error when no VkPhysicalDevice matches. Compile-review-only per ADR-024 --
// implemented but not counted as passed hardware evidence.
std::unique_ptr<DeviceHal> make_device_hal(const uint8_t uuid[16], std::string* error);

}  // namespace vg::vulkan

#endif
