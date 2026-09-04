#ifndef VG_BACKENDS_VULKAN_DEVICE_INTERNAL_H_
#define VG_BACKENDS_VULKAN_DEVICE_INTERNAL_H_

#include "backends/vulkan/vulkan_device_hal.h"
#include "backends/vulkan/vulkan_physical_types.h"
#include "backends/vulkan/vulkan_diagnostics.h"
#include "compiler/pipeline_classification.h"
#include <map>
#include <tuple>
#include <unordered_map>
#include <string_view>
#if defined(VG_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace vg::vulkan {

namespace detail {
struct DeviceState {
  explicit DeviceState(DeviceHal& owner);
  ~DeviceState();
  DeviceState(const DeviceState&) = delete;
  DeviceState& operator=(const DeviceState&) = delete;
  DeviceHal& owner_;
  vg::core::FacetPool& facet_pool();
  vg::core::EnvelopeContinuationTable& envelope_continuations();
  const vg::hal::CapabilitySnapshot& capabilities() const;
  bool compile(const vg::core::ExecutionPlan& plan, vg::hal::CompiledPlan* compiled,
               std::string* error);
  bool submit(const vg::hal::CompiledPlan& compiled, vg::core::Arena& arena,
              vg::hal::Submission* submission, std::string* error);
  bool run_raster_pass(const vg::core::Arena& arena, vg::core::FacetPool& pool,
                       vg::core::FacetRef attachment_ref, vg::core::FacetRef source_ref,
                       const RasterPassDesc& desc, RasterPassResult* result, std::string* error);
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

  // Backend-owned Stage-6 objects. The key is derived from the immutable
  // package contents plus the Node entry name; no raw Vulkan handle escapes
  // through hal::CompiledPlan. Entries live until DeviceHal destruction so a
  // later Node compile cannot invalidate a pipeline retained by an earlier
  // CompiledPlan.
  struct ComputePipelineRecord {
    VkShaderModule shader_module{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
    uint32_t binding_count{};
  };
  std::map<std::string, ComputePipelineRecord> compute_pipeline_cache_;
  VkCommandPool command_pool_{VK_NULL_HANDLE};
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
  // pipeline, kept separate from compute_pipeline_cache_ above: the
  // publication protocol is backend-private
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

  // Compiles `glsl_source` (GLSL -> SPIR-V via a glslc subprocess -> a
  // VkPipeline bound only by a push-constant BDA-address array, no descriptor
  // sets), caching by the complete immutable package/entry key. Failure here
  // is the sole source of truth for whether this device/driver can run the module -- unlike
  // Metal, no HostAssisted fallback is attempted: the target NVIDIA/Linux
  // hardware is expected to support this natively, so failure is reported
  // as Unsupported rather than silently degraded.
  bool ensure_pipeline(const std::string& cache_key, const std::string& glsl_source,
                       uint32_t binding_count, const ComputePipelineRecord** out,
                       bool* cache_hit, std::string* error);
  // Creates or reuses a host-visible-coherent VkBuffer for `allocation`,
  // uploading its current bytes. Invalidated (recreated) on generation or
  // required-size mismatch, mirroring Metal's allocation_map policy.
  bool ensure_buffer(const core::Allocation& allocation, AllocationRecord** out, std::string* error);
  bool ensure_timeline_semaphore(std::string* error);
  bool ensure_task_ring_pipeline(std::string* error);
  bool create_task_ring_buffers(uint32_t task_count, TaskRingBuffers* out, std::string* error);
  void destroy_task_ring_buffers(TaskRingBuffers* buffers);
  // Publication-only pass. Canonical program execution is performed by
  // dispatch_task_graph below; the Task ring cannot select a Node pipeline or
  // become a second execution authority.
  struct TaskDispatchCounts;
  bool dispatch_task_ring_publication(const TaskRingBuffers& buffers,
                                       TaskDispatchCounts* counts, std::string* error);

  struct CanonicalTaskDispatch {
    uint32_t task_index{};
    uint32_t x{1};
    uint32_t y{1};
    uint32_t z{1};
    const ComputePipelineRecord* pipeline{};
    std::vector<VkDeviceAddress> addresses;
    // Physical operations admitted by Stage 6 at this wave's first Task.
    std::vector<uint32_t> transitions_before;
  };
  struct TaskDispatchCounts {
    uint64_t dispatch_count{};
    uint64_t barrier_count{};
    uint64_t command_buffer_count{};
    uint64_t queue_wait_count{};
    std::vector<uint32_t> encoded_transitions;
  };
  // Records Core's component/wave schedule in one command buffer. Each Task
  // binds its own NodeRef-keyed Stage-6 pipeline and package bindings. A
  // conservative sync2 barrier is emitted only for Stage-6 wave operations.
  bool dispatch_task_graph(const std::vector<CanonicalTaskDispatch>& dispatches,
                           uint64_t wait_value, uint64_t signal_value,
                           TaskDispatchCounts* counts, std::string* error);

  // wait_value/signal_value of 0 mean "no timeline involvement for this
  // side" -- their fields are omitted from VkTimelineSemaphoreSubmitInfo
  // entirely rather than submitted as a literal 0, matching core's guarantee
  // that a required_value of 0 is rejected before reaching the backend.
  // VkFence + vkWaitForFences remains the host-side completion mechanism;
  // the timeline semaphore is a GPU-ordering primitive layered on top of it,
  // not a replacement for it in this v1 backend.
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
  // Stage-7 representation commit helper. Builds the optimal-tiled image for `facet`
  // and copies the allocation's existing linear buffer into it.
  bool transform_representation(const vg::core::Arena& arena, const vg::core::RepresentationSemanticPlanItem& request,
                                vg::core::FacetRef facet, vg::hal::RepresentationTransformCost* cost,
                                RepresentationStageCounts* counts, std::string* error);
#endif

  // Whether every one of `plan.representation_requests` can actually be
  // carried out by this backend on this device. Returns false with a
  // VG-concept `reason` naming the first request that cannot be, which
  // compile() turns into an Unsupported LoweringEvent -- START.md §4 invariant
  // 10: a semantic this hardware cannot express is rejected, never silently
  // dropped while the rest of the plan compiles as if nothing had been asked.
  bool can_lower_representation_requests(const vg::core::ExecutionPlan& plan, std::string* reason) const;

};

#if defined(VG_HAS_VULKAN)
struct RawBuffer {
  VkBuffer buffer{VK_NULL_HANDLE};
  VkDeviceMemory memory{VK_NULL_HANDLE};
  VkDeviceAddress address{};
  void* mapped{nullptr};
};
class FacetUseGuard {
 public:
  FacetUseGuard(vg::core::FacetPool& pool, vg::core::FacetRef ref);
  FacetUseGuard(const FacetUseGuard&) = delete;
  FacetUseGuard& operator=(const FacetUseGuard&) = delete;
  FacetUseGuard(FacetUseGuard&&) = delete;
  FacetUseGuard& operator=(FacetUseGuard&&) = delete;
  ~FacetUseGuard();
  bool begin(const vg::core::Arena& arena, std::string* error);
 private:
  vg::core::FacetPool& pool_;
  vg::core::FacetRef ref_;
  bool held_{};
};

void append_cache_key_component(std::string* key, std::string_view value);

std::string compute_pipeline_cache_key(const vg::core::ExecutionPlan::ResolvedNode& node,
                                       const vg::compiler::ComputePackage& package);
#endif
bool same_compute_bindings(const std::vector<vg::compiler::ComputeBinding>& left,
                           const std::vector<vg::compiler::ComputeBinding>& right);
bool same_vulkan_compute_package(const vg::compiler::ComputePackage& actual,
                                 const vg::compiler::ComputePackage& expected);
bool node_ref_equal(vg::core::NodeTable::Ref left, vg::core::NodeTable::Ref right);
#if defined(VG_HAS_VULKAN)
void lower_wave_transitions(vg::hal::CompiledPlan* compiled);

bool find_memory_type(VkPhysicalDevice physical_device, uint32_t type_bits, VkMemoryPropertyFlags required,
                      uint32_t* out);

bool compile_glsl_stage(const std::string& glsl_source, const char* shader_stage,
                        const std::vector<std::string>& defines, std::vector<uint32_t>* spirv,
                        std::string* error);

bool compile_glsl_to_spirv(const std::string& glsl_source, std::vector<uint32_t>* spirv, std::string* error);

void destroy_raw_buffer(VkDevice device, RawBuffer* buffer);

bool create_raw_buffer(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size,
                       VkBufferUsageFlags usage, bool want_address, bool want_map, RawBuffer* out,
                       std::string* error);

bool ensure_command_pool(VkDevice device, uint32_t queue_family, VkCommandPool* pool, std::string* error);

bool allocate_command_buffer(VkDevice device, VkCommandPool pool, VkCommandBuffer* out, std::string* error);

bool submit_and_wait(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer command_buffer,
                     const void* submit_pnext, uint32_t wait_count, const VkSemaphore* wait_semaphores,
                     const VkPipelineStageFlags* wait_stage_mask, uint32_t signal_count,
                     const VkSemaphore* signal_semaphores, std::string* error,
                     uint64_t* actual_host_waits = nullptr);

bool submit_and_wait_simple(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer command_buffer,
                            std::string* error);

VkFormat to_vk_format(vg::core::PixelFormat format);

const char* storage_image_format_qualifier(VkFormat format);

VkComponentSwizzle to_vk_swizzle(vg::core::Swizzle swizzle);

VkComponentMapping to_vk_component_mapping(const vg::core::SwizzleChannels& swizzle);

uint32_t packed_swizzle(const vg::core::SwizzleChannels& swizzle);

VkImageViewType to_vk_view_type(vg::core::ViewDimension dimension);

VkImageLayout facet_read_layout(vg::core::FacetKind kind);

VkImageUsageFlags facet_image_usage(vg::core::FacetKind kind);

void layout_sync_scope(VkImageLayout layout, VkPipelineStageFlags2* stage, VkAccessFlags2* access);

void record_image_barrier(VkCommandBuffer command_buffer, VkImage image, VkImageLayout old_layout,
                         VkImageLayout new_layout, uint32_t mip_levels, uint32_t array_layers);

std::array<float, 4> decode_first_texel(const void* bytes, VkFormat format);

std::string storage_facet_glsl_source(const char* format_qualifier);

VkSampleCountFlagBits to_vk_sample_count(uint32_t sample_count);

VkAttachmentLoadOp to_vk_load_op(vg::vulkan::AttachmentLoadAction load);

VkAttachmentStoreOp to_vk_store_op(vg::vulkan::AttachmentStoreAction store);

size_t encode_first_texel(const std::array<float, 4>& rgba, VkFormat format, uint8_t* out);
#endif
}  // namespace detail
}  // namespace vg::vulkan

#endif
