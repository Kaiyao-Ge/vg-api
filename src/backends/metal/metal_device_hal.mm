#include "backends/metal/metal_device_hal.h"
#include "backends/metal/metal_tier2.h"

#include "backends/reference/reference_executor.h"
#include "compiler/pipeline_classification.h"
#include "ir/sha256.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vg::metal {
namespace {

// Double guard: the selector must both exist (older OS/device combos may
// lack it entirely) and actually hand back a usable object -- some
// devices expose the selector but return nil in practice, which a
// single respondsToSelector: check would silently misreport as "supported".
bool probe_shared_events(id<MTLDevice> device) {
  if (![device respondsToSelector:@selector(newSharedEvent)]) return false;
  id<MTLSharedEvent> probe = [device newSharedEvent];
  return probe != nil;
}

// MTLFeatureSet_macOS_GPUFamily1_v1 (used previously) says nothing about
// indirect command buffer support -- it is unrelated to ICBs and was a
// fabricated signal. Probe the real capability: attempt to create a
// minimal concurrent-dispatch ICB and check it actually comes back non-nil.
bool probe_indirect_command_buffers(id<MTLDevice> device) {
  if (@available(macOS 10.14, *)) {
    MTLIndirectCommandBufferDescriptor* descriptor = [MTLIndirectCommandBufferDescriptor new];
    descriptor.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
    descriptor.inheritBuffers = NO;
    descriptor.maxKernelBufferBindCount = 1;
    id<MTLIndirectCommandBuffer> probe = [device newIndirectCommandBufferWithDescriptor:descriptor
                                                                        maxCommandCount:1
                                                                                 options:MTLResourceStorageModePrivate];
    return probe != nil;
  }
  return false;
}

// Packs a core::TaskRecord as 14 little-endian uint32 words, matching
// compiler::task_ring_metal_source()'s expected buffer layout. Explicit
// word-by-word packing (not a memcpy of the C++ struct) sidesteps any risk
// of MSL/C++ struct-padding divergence between the GPU and host compilers.
constexpr size_t kTaskRingWordsPerRecord = compiler::kTaskRingWordsPerRecord;

void pack_task_record(const core::TaskRecord& task, uint32_t* out) {
  out[0] = task.node_index;
  out[1] = task.node_generation;
  out[2] = static_cast<uint32_t>(task.root_allocation & 0xffffffffu);
  out[3] = static_cast<uint32_t>(task.root_allocation >> 32);
  out[4] = task.root_generation;
  out[5] = task.x;
  out[6] = task.y;
  out[7] = task.z;
  out[8] = task.flags;
  out[9] = task.contract_index;
  out[10] = task.payload_size;
  out[11] = 0;
  out[12] = static_cast<uint32_t>(task.payload_or_offset & 0xffffffffu);
  out[13] = static_cast<uint32_t>(task.payload_or_offset >> 32);
}

core::TaskRecord unpack_task_record(const uint32_t* in) {
  core::TaskRecord task;
  task.node_index = in[0];
  task.node_generation = in[1];
  task.root_allocation = static_cast<uint64_t>(in[2]) | (static_cast<uint64_t>(in[3]) << 32);
  task.root_generation = in[4];
  task.x = in[5];
  task.y = in[6];
  task.z = in[7];
  task.flags = in[8];
  task.contract_index = in[9];
  task.payload_size = in[10];
  task.payload_or_offset = static_cast<uint64_t>(in[12]) | (static_cast<uint64_t>(in[13]) << 32);
  return task;
}

// TASK-D5 / ADR-039: publish only the envelope window. A quota split or
// leftover drain must not GPU-publish the parked suffix (gid == packed
// slot, so a full-graph dispatch would still write leftover).
bool publish_envelope_order(const core::TaskGraph& graph, const std::vector<uint32_t>& order,
                            std::vector<core::TaskRecord>* published, std::string* error) {
  if (published == nullptr) {
    if (error) *error = "envelope published-task output is required";
    return false;
  }
  published->clear();
  if (order.empty()) return true;
  core::PublicationRing ring(static_cast<uint32_t>(order.size()));
  const auto& tasks = graph.tasks();
  published->reserve(order.size());
  for (uint32_t index : order) {
    uint32_t slot = 0;
    std::string task_error;
    if (index >= tasks.size() || !ring.publish_task(tasks[index], &slot, &task_error) ||
        !ring.consume(slot, &task_error)) {
      if (error) *error = task_error.empty() ? "envelope task index is out of range" : task_error;
      return false;
    }
    published->push_back(tasks[index]);
  }
  return true;
}

hal::CapabilitySnapshot make_hal_snapshot(id<MTLDevice> device, DeviceSnapshot* out) {
  hal::CapabilitySnapshot caps{};
  caps.backend = hal::BackendKind::Metal;
  caps.adapter_name = [[device name] UTF8String];
  caps.driver = [[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String];
  caps.max_buffer_size = [device maxBufferLength];
  caps.address_width = 64;
  caps.min_buffer_alignment = 256;
  caps.validation_available = true;
  // Counter sampling is a separate capability query in the full adapter;
  // keep the B2 foundation conservative until the counter path is wired.
  caps.timestamps_available = false;

  const bool shared_events = probe_shared_events(device);
  const bool indirect = probe_indirect_command_buffers(device);
  const bool gpu_addresses = false;
  // Task ring Tier0 (dispatch_task_publish) is a single synchronous dispatch
  // within one submit() call -- it doesn't depend on shared_events/timeline
  // or any other optional feature, just atomic ops on a device buffer, which
  // every Metal device this backend targets supports. So unlike Timeline/
  // IndirectTier1 (genuinely optional hardware features), this bit is set
  // unconditionally, matching EffectDag.
  //
  // Raster, RepresentationTransform and CheckedFacetGeneration join it for the
  // same reason, and are deliberately *not* probed: each is an obligation this
  // adapter now meets in software, not an optional hardware feature. Raster is
  // a real MTLRenderPipelineState draw (06 §1 "render attachment 与基础
  // raster"), RepresentationTransform is a real linear->Private-optimal blit
  // that publishes a new epoch (02 §8), and CheckedFacetGeneration is the
  // in-shader generation guard of 06 §6.4 specialized through an MSL function
  // constant. A device that could not do one of them would have to leave the
  // bit clear, but every Metal device this backend runs on can do all three.
  uint64_t bits = static_cast<uint64_t>(hal::Capability::EffectDag) |
                   static_cast<uint64_t>(hal::Capability::TaskPublication) |
                   static_cast<uint64_t>(hal::Capability::Raster) |
                   static_cast<uint64_t>(hal::Capability::RepresentationTransform) |
                   static_cast<uint64_t>(hal::Capability::CheckedFacetGeneration);
  if (shared_events) bits |= static_cast<uint64_t>(hal::Capability::Timeline);
  if (indirect) bits |= static_cast<uint64_t>(hal::Capability::IndirectTier1);
  if (gpu_addresses) bits |= static_cast<uint64_t>(hal::Capability::LinearAddress);
  caps.capability_bits = bits;

  if (out != nullptr) {
    out->gpu_family = 0;
    out->argument_buffer_tier = static_cast<uint32_t>([device argumentBuffersSupport]);
    out->unified_memory = [device hasUnifiedMemory];
    out->supports_shared_events = shared_events;
    out->supports_indirect_command_buffers = indirect;
    out->supports_gpu_addresses = gpu_addresses;
    out->supports_counter_sampling = false;
  }
  return caps;
}

// Tracks the private GPU-side buffer VG mints for a given core::Allocation.
// Keyed by allocation id; invalidated (recreated) whenever generation or
// required size changes, mirroring the arena's own staleness rules.
struct MetalAllocationRecord {
  id<MTLBuffer> buffer = nil;
  uint64_t allocation_id{};
  uint32_t generation{};
  size_t byte_size{};
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
};

bool same_swizzle(const core::SwizzleChannels& lhs, const core::SwizzleChannels& rhs) {
  return lhs.red == rhs.red && lhs.green == rhs.green && lhs.blue == rhs.blue &&
         lhs.alpha == rhs.alpha;
}

bool same_shape(const MetalFacetRecord& record, const core::CanonicalView& view) {
  return record.width == view.width && record.height == view.height &&
         record.dimension == view.dimension && record.array_layers == view.array_layers &&
         record.mip_levels == view.mip_levels && record.format == view.format &&
         same_swizzle(record.swizzle, view.swizzle);
}

MTLTextureSwizzle to_mtl_swizzle(core::Swizzle swizzle) {
  switch (swizzle) {
    case core::Swizzle::Red: return MTLTextureSwizzleRed;
    case core::Swizzle::Green: return MTLTextureSwizzleGreen;
    case core::Swizzle::Blue: return MTLTextureSwizzleBlue;
    case core::Swizzle::Alpha: return MTLTextureSwizzleAlpha;
    case core::Swizzle::Zero: return MTLTextureSwizzleZero;
    case core::Swizzle::One: return MTLTextureSwizzleOne;
  }
  return MTLTextureSwizzleRed;
}

MTLPixelFormat to_mtl_pixel_format(core::PixelFormat format) {
  return format == core::PixelFormat::RGBA8Unorm ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatR32Float;
}

// A Texture2DArray view lowers to MTLTextureType2DArray even when it names a
// single layer: the shader-side type (texture2d vs texture2d_array) is part of
// the contract the view declares, and silently collapsing a one-layer array to
// a plain 2D texture would bind the wrong kernel.
MTLTextureType to_mtl_texture_type(core::ViewDimension dimension) {
  return dimension == core::ViewDimension::Texture2DArray ? MTLTextureType2DArray : MTLTextureType2D;
}

// Which MTLTextureUsage a facet kind needs. Sample additionally asks for
// PixelFormatView because a SampleFacet is exactly the kind that gets
// reinterpreted: a non-identity swizzle needs a swizzled view, and the raster
// path needs a single-slice 2D view over an array source to reach the
// texture2d<float> the shared raster fragment shader declares. Storage and
// Attachment get it only when the view really asks for a swizzle, so nothing
// pays for a reinterpretation it cannot use.
MTLTextureUsage facet_texture_usage(core::FacetKind kind, const core::CanonicalView& view) {
  MTLTextureUsage usage = 0;
  switch (kind) {
    case core::FacetKind::Sample:
      usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
      break;
    case core::FacetKind::Storage:
      usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
      break;
    case core::FacetKind::Attachment:
      usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      break;
    default:
      return 0;
  }
  if (!view.swizzle.identity()) usage |= MTLTextureUsagePixelFormatView;
  return usage;
}

// One descriptor shape for every texture this backend mints from a
// CanonicalView, so the Shared upload path and the Private transform path
// cannot drift on dimension, mip count or array length.
MTLTextureDescriptor* make_texture_descriptor(const core::CanonicalView& view, core::FacetKind kind,
                                              MTLStorageMode storage_mode) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor new];
  descriptor.textureType = to_mtl_texture_type(view.dimension);
  descriptor.pixelFormat = to_mtl_pixel_format(view.format);
  descriptor.width = view.width;
  descriptor.height = view.height;
  descriptor.depth = 1;
  descriptor.mipmapLevelCount = view.mip_levels;
  descriptor.arrayLength = view.array_layers;
  descriptor.sampleCount = 1;
  descriptor.storageMode = storage_mode;
  descriptor.usage = facet_texture_usage(kind, view);
  return descriptor;
}

// 05 §14: a rejection speaks VG concepts. Every caller that hands a view to
// Metal funnels its shape checks through here so the diagnostics are one text,
// not one per entry point.
bool view_expressible(const core::CanonicalView& view, core::FacetKind kind, uint64_t backing_bytes,
                      std::string* error) {
  if (!view.valid(error)) return false;
  if (kind != core::FacetKind::Sample && kind != core::FacetKind::Storage &&
      kind != core::FacetKind::Attachment) {
    if (error) *error = "Unsupported: Address/Transfer facets have no Metal texture representation";
    return false;
  }
  // Swizzle reinterprets a shader read. Metal applies no such remap to a render
  // target or to an image write, so rather than quietly dropping the channel
  // mapping the caller asked for, those kinds are refused.
  if (!view.swizzle.identity() && kind != core::FacetKind::Sample) {
    if (error) *error = "Unsupported: non-identity swizzle applies to SampleFacet only";
    return false;
  }
  if (backing_bytes < view.byte_size()) {
    if (error)
      *error = "canonical view declares " + std::to_string(view.byte_size()) +
               " bytes of subresources but its allocation holds only " + std::to_string(backing_bytes);
    return false;
  }
  return true;
}

bool subresource_in_range(const core::CanonicalView& view, const AttachmentSubresource& subresource,
                          std::string* error) {
  if (subresource.layer >= view.array_layers || subresource.level >= view.mip_levels) {
    if (error)
      *error = "render pass targets layer " + std::to_string(subresource.layer) + " level " +
               std::to_string(subresource.level) + " of a canonical view declaring " +
               std::to_string(view.array_layers) + " layer(s) and " + std::to_string(view.mip_levels) +
               " mip level(s)";
    return false;
  }
  return true;
}

// Holds a FacetPool GPU-use bracket for as long as a command buffer may still
// reference the slot. Scoped rather than hand-paired because these entry
// points have many early returns, and a leaked use would pin the slot's index
// out of the free list forever.
class FacetUseGuard {
 public:
  FacetUseGuard(core::FacetPool& pool, core::FacetRef ref) : pool_(pool), ref_(ref) {}
  FacetUseGuard(const FacetUseGuard&) = delete;
  FacetUseGuard& operator=(const FacetUseGuard&) = delete;
  ~FacetUseGuard() {
    if (held_) pool_.end_gpu_use(ref_);
  }

  bool begin(const core::Arena& arena, std::string* error) {
    held_ = pool_.begin_gpu_use(arena, ref_, error);
    return held_;
  }

 private:
  core::FacetPool& pool_;
  core::FacetRef ref_;
  bool held_{};
};

hal::LoweringReport make_facet_report() {
  hal::LoweringReport report;
  report.backend = hal::BackendKind::Metal;
  report.supported = true;
  return report;
}

std::array<float, 4> decode_texel(const void* bytes, MTLPixelFormat format) {
  if (format == MTLPixelFormatRGBA8Unorm) {
    const uint8_t* rgba = static_cast<const uint8_t*>(bytes);
    return {rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f};
  }
  float value{};
  std::memcpy(&value, bytes, sizeof(value));
  return {value, 0.0f, 0.0f, 1.0f};
}

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

// TASK-B12: real host-side wall-clock timing and structural counts for one
// dispatch_and_wait()/dispatch_task_publish() call, accumulated by the
// caller into Submission's own fields across however many command buffers a
// single submit() actually issues. Private to this translation unit --
// Submission itself (device_hal.h) is the shared, cross-backend surface;
// this is just the local bookkeeping that feeds it.
struct DispatchStats {
  uint64_t cpu_encode_ns{};
  uint64_t cpu_submit_ns{};
  uint64_t encoder_count{};
  uint64_t command_buffer_count{};
  uint64_t barrier_count{};
  uint64_t queue_wait_count{};

  DispatchStats& operator+=(const DispatchStats& other) {
    cpu_encode_ns += other.cpu_encode_ns;
    cpu_submit_ns += other.cpu_submit_ns;
    encoder_count += other.encoder_count;
    command_buffer_count += other.command_buffer_count;
    barrier_count += other.barrier_count;
    queue_wait_count += other.queue_wait_count;
    return *this;
  }
};

}  // namespace

struct DeviceHal::Impl {
  ~Impl() {
    for (auto& entry : allocation_map) release_buffer(entry.second.buffer);
    for (auto& entry : facet_map) release_facet_textures(entry.second);
  }

  static void release_buffer(id<MTLBuffer>& buffer) {
    if (buffer != nil) {
      [buffer release];
      buffer = nil;
    }
  }

  static void release_facet_textures(MetalFacetRecord& record) {
    if (record.texture != nil && record.texture != record.storage_texture) [record.texture release];
    if (record.storage_texture != nil) [record.storage_texture release];
    record.texture = nil;
    record.storage_texture = nil;
  }

  // Only slots the pool has already stopped resolving, and only after this
  // backend's own waitUntilCompleted (every path here submits-and-waits), so
  // no texture is destroyed under work still in flight (06 §11).
  uint32_t retire_stale_facet_textures(const core::Arena& arena, const core::FacetPool& pool) {
    uint32_t retired = 0;
    for (auto it = facet_map.begin(); it != facet_map.end();) {
      const core::FacetRef ref{it->second.facet_index, it->second.facet_generation};
      if (pool.in_flight(ref) != 0) {
        ++it;
        continue;
      }
      core::FacetStatus status = core::FacetStatus::Ok;
      if (pool.lookup(arena, ref, &status) != nullptr) {
        ++it;
        continue;
      }
      release_facet_textures(it->second);
      it = facet_map.erase(it);
      ++retired;
    }
    return retired;
  }

  // A ConsumeInput has cleared the allocation's host bytes (or the allocation
  // is gone). Leaving the Shared MTLBuffer would mean the peak-memory saving
  // E005 measures never materializes on the device side (06 §11).
  uint64_t release_empty_linear_buffers(const core::Arena& arena) {
    uint64_t released = 0;
    for (auto it = allocation_map.begin(); it != allocation_map.end();) {
      const core::Allocation* allocation = arena.lookup(it->first, it->second.generation);
      if (allocation != nullptr && !allocation->bytes.empty()) {
        ++it;
        continue;
      }
      released += it->second.byte_size;
      release_buffer(it->second.buffer);
      it = allocation_map.erase(it);
    }
    return released;
  }

  void reclaim_released_backing(const core::Arena& arena, const core::FacetPool& pool,
                                uint32_t* retired_textures, uint64_t* released_linear) {
    const uint32_t textures = retire_stale_facet_textures(arena, pool);
    const uint64_t linear = release_empty_linear_buffers(arena);
    if (retired_textures != nullptr) *retired_textures = textures;
    if (released_linear != nullptr) *released_linear = linear;
  }

  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  DeviceSnapshot snapshot{};

  id<MTLLibrary> library = nil;
  id<MTLComputePipelineState> pipeline = nil;
  std::string cached_ir_hash;
  std::unordered_map<uint64_t, MetalAllocationRecord> allocation_map;

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
  std::unordered_map<std::string, id<MTLLibrary>> library_by_hash;
  // Bound only on the path where the in-shader guard is expected to reject the
  // token before the kernel's first sample (see run_sample_facet). It is never
  // read by a shader; it exists because MSL requires a texture argument to be
  // bound even when the specialized function returns before touching it.
  id<MTLTexture> guard_placeholder_texture = nil;
  // TASK-B13: debug/test introspection only, see DeviceHal::last_tier1_indirect_dims().
  std::vector<std::array<uint32_t, 3>> last_tier1_indirect_dims;

  bool ensure_timeline_event(std::string* error) {
    if (timeline_event != nil) return true;
    if (!snapshot.supports_shared_events) {
      if (error) *error = "device does not support MTLSharedEvent";
      return false;
    }
    timeline_event = [device newSharedEvent];
    if (timeline_event == nil) {
      if (error) *error = "failed to create MTLSharedEvent";
      return false;
    }
    return true;
  }

  // Attempts to (re)compile the B4 MSL source into a pipeline, caching by IR
  // hash. Failure here is the sole source of truth for whether this GPU/OS
  // combination can run the module natively -- in particular, a module using
  // the 8-byte atomic_add path fails here if the device/driver lacks native
  // 64-bit atomics, and the caller treats that as a HostAssisted signal
  // rather than guessing at GPU family enums ahead of time.
  bool ensure_pipeline(const std::string& ir_hash, const std::string& msl_source, std::string* error,
                      const std::string& function_name = "vg_linear_compute") {
    if (pipeline != nil && cached_ir_hash == ir_hash) return true;
    pipeline = nil;
    library = nil;
    cached_ir_hash.clear();

    NSError* compile_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    id<MTLLibrary> new_library = [device newLibraryWithSource:[NSString stringWithUTF8String:msl_source.c_str()]
                                                        options:options
                                                          error:&compile_error];
    if (new_library == nil) {
      if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                                : "unknown MSL compile error";
      return false;
    }
    id<MTLFunction> function = [new_library newFunctionWithName:[NSString stringWithUTF8String:function_name.c_str()]];
    if (function == nil) {
      if (error) *error = "MSL library missing " + function_name + " entry point";
      return false;
    }
    NSError* pipeline_error = nil;
    id<MTLComputePipelineState> new_pipeline = [device newComputePipelineStateWithFunction:function
                                                                                       error:&pipeline_error];
    if (new_pipeline == nil) {
      if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                 : "unknown pipeline creation error";
      return false;
    }
    library = new_library;
    pipeline = new_pipeline;
    cached_ir_hash = ir_hash;
    return true;
  }

  // TASK-B14 (E012): per-IR-hash pipeline cache for the Effect DAG path,
  // separate from the single-slot `pipeline`/`cached_ir_hash` above. A
  // multi-pass Effect DAG can have distinct passes with distinct IR hashes
  // that must all coexist within one submission -- the single-slot cache
  // (which evicts on any different hash) cannot serve that, so this is a
  // second, independent cache rather than a generalization of ensure_pipeline.
  std::unordered_map<std::string, std::pair<id<MTLLibrary>, id<MTLComputePipelineState>>> effect_dag_pipelines;

  bool ensure_effect_dag_pipeline(const std::string& ir_hash, const std::string& msl_source,
                                  id<MTLComputePipelineState>* out_pipeline, std::string* error) {
    auto it = effect_dag_pipelines.find(ir_hash);
    if (it != effect_dag_pipelines.end()) { *out_pipeline = it->second.second; return true; }
    NSError* compile_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    id<MTLLibrary> new_library = [device newLibraryWithSource:[NSString stringWithUTF8String:msl_source.c_str()]
                                                        options:options
                                                          error:&compile_error];
    if (new_library == nil) {
      if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                                : "unknown effect DAG pass MSL compile error";
      return false;
    }
    id<MTLFunction> function = [new_library newFunctionWithName:@"vg_linear_compute"];
    if (function == nil) {
      if (error) *error = "effect DAG pass MSL library missing vg_linear_compute entry point";
      return false;
    }
    NSError* pipeline_error = nil;
    id<MTLComputePipelineState> new_pipeline = [device newComputePipelineStateWithFunction:function
                                                                                       error:&pipeline_error];
    if (new_pipeline == nil) {
      if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                 : "unknown effect DAG pass pipeline creation error";
      return false;
    }
    effect_dag_pipelines.emplace(ir_hash, std::make_pair(new_library, new_pipeline));
    *out_pipeline = new_pipeline;
    return true;
  }

  // Creates or reuses a Shared-storage MTLBuffer for `allocation`, uploading
  // its current bytes so the kernel observes the same starting state the
  // reference oracle would. Shared storage keeps this vertical slice on the
  // M1 unified-memory fast path without an explicit blit.
  id<MTLBuffer> ensure_buffer(const core::Allocation& allocation) {
    auto it = allocation_map.find(allocation.id);
    // ConsumeInput has already handed the linear representation back. A dummy
    // 1-byte buffer here would keep a device allocation the host just released
    // and let a later dispatch write into empty host bytes.
    if (allocation.bytes.empty()) {
      if (it != allocation_map.end()) {
        release_buffer(it->second.buffer);
        allocation_map.erase(it);
      }
      return nil;
    }
    const size_t needed = allocation.bytes.size();
    if (it != allocation_map.end() &&
        (it->second.generation != allocation.generation || it->second.byte_size < needed)) {
      release_buffer(it->second.buffer);
      allocation_map.erase(it);
      it = allocation_map.end();
    }
    if (it == allocation_map.end()) {
      id<MTLBuffer> buffer = [device newBufferWithLength:needed options:MTLResourceStorageModeShared];
      if (buffer == nil) return nil;
      MetalAllocationRecord record{buffer, allocation.id, allocation.generation, needed};
      it = allocation_map.emplace(allocation.id, record).first;
    }
    std::memcpy([it->second.buffer contents], allocation.bytes.data(), allocation.bytes.size());
    return it->second.buffer;
  }


  static uint64_t facet_cache_key(core::FacetRef ref) {
    return (uint64_t(ref.index) << 32) | uint64_t(ref.generation);
  }

  // Shared prologue for every facet use: resolve the capability token, reject
  // a kind mismatch, and produce the diagnostic the FacetPool itself
  // classified rather than a generic "stale" string.
  const core::FacetSlot* resolve_facet(const core::Arena& arena, const core::FacetPool& pool,
                                       core::FacetRef ref, core::FacetKind expected_kind,
                                       std::string* error) const {
    core::FacetStatus status = core::FacetStatus::Ok;
    const core::FacetSlot* slot = pool.lookup(arena, ref, &status);
    if (slot == nullptr) {
      if (error) *error = core::to_string(status);
      return nullptr;
    }
    if (slot->kind != expected_kind) {
      if (error) *error = "facet kind mismatch";
      return nullptr;
    }
    return slot;
  }

  // AddressFacet/TransferFacet resolve to the allocation's linear device
  // buffer -- no texture object exists on this path (02 §3.3).
  id<MTLBuffer> ensure_facet_buffer(const core::Arena& arena, const core::FacetPool& pool,
                                    core::FacetRef ref, core::FacetKind expected_kind,
                                    std::string* error) {
    const core::FacetSlot* slot = resolve_facet(arena, pool, ref, expected_kind, error);
    if (slot == nullptr) return nil;
    const core::Allocation* allocation =
        arena.lookup(slot->view.allocation, slot->view.allocation_generation);
    if (allocation == nullptr) {
      if (error) *error = "facet backing allocation not found in arena";
      return nil;
    }
    id<MTLBuffer> buffer = ensure_buffer(*allocation);
    if (buffer == nil && error) *error = "Metal facet buffer allocation failed";
    return buffer;
  }

  // Host readback of a `width` x `height` window at (origin_x, origin_y) of one
  // (slice, level) subresource, decoded to float4 row-major. Once a
  // representation transform has moved a facet to Private storage there is no
  // host-visible mapping left, so that case has to go back through a blit
  // rather than getBytes -- one function for both so the two paths cannot
  // disagree about layout.
  bool read_texture_region(id<MTLTexture> texture, uint32_t slice, uint32_t level, uint32_t origin_x,
                           uint32_t origin_y, uint32_t width, uint32_t height,
                           std::vector<std::array<float, 4>>* out, std::string* error) {
    if (width == 0 || height == 0) { if (error) *error = "facet readback window is empty"; return false; }
    const size_t row_bytes = static_cast<size_t>(width) * kBytesPerTexel;
    const size_t image_bytes = row_bytes * height;
    std::vector<uint8_t> bytes(image_bytes);
    if (texture.storageMode != MTLStorageModePrivate) {
      [texture getBytes:bytes.data()
            bytesPerRow:row_bytes
          bytesPerImage:image_bytes
             fromRegion:MTLRegionMake2D(origin_x, origin_y, width, height)
            mipmapLevel:level
                  slice:slice];
    } else {
      id<MTLBuffer> readback = [device newBufferWithLength:image_bytes options:MTLResourceStorageModeShared];
      if (readback == nil) { if (error) *error = "facet readback buffer allocation failed"; return false; }
      id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
      if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
      id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
      if (blit == nil) { if (error) *error = "failed to create Metal blit encoder"; return false; }
      [blit copyFromTexture:texture
                  sourceSlice:slice
                  sourceLevel:level
                 sourceOrigin:MTLOriginMake(origin_x, origin_y, 0)
                   sourceSize:MTLSizeMake(width, height, 1)
                     toBuffer:readback
            destinationOffset:0
       destinationBytesPerRow:row_bytes
     destinationBytesPerImage:image_bytes];
      [blit endEncoding];
      [command_buffer commit];
      [command_buffer waitUntilCompleted];
      if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
        if (error)
          *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                                : "facet readback blit failed";
        return false;
      }
      std::memcpy(bytes.data(), [readback contents], image_bytes);
    }
    out->resize(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        (*out)[static_cast<size_t>(y) * width + x] =
            decode_texel(bytes.data() + static_cast<size_t>(y) * row_bytes + x * kBytesPerTexel,
                         texture.pixelFormat);
      }
    }
    return true;
  }

  bool read_texel(id<MTLTexture> texture, uint32_t slice, uint32_t level, uint32_t x, uint32_t y,
                  std::array<float, 4>* out, std::string* error) {
    std::vector<std::array<float, 4>> texels;
    if (!read_texture_region(texture, slice, level, x, y, 1, 1, &texels, error)) return false;
    *out = texels[0];
    return true;
  }

  // Publishes `storage_texture` as the backend object behind `ref`, deriving
  // the shader-visible swizzle view when the contract asks for one. Shared by
  // the lazily-created Shared texture path and by the representation
  // transform, so a transformed facet lands in the cache the same shape as
  // any other.
  id<MTLTexture> install_facet_record(core::FacetRef ref, const core::CanonicalView& view,
                                      core::FacetKind kind, uint32_t representation_epoch,
                                      id<MTLTexture> storage_texture, std::string* error) {
    id<MTLTexture> shader_texture = storage_texture;
    if (!view.swizzle.identity()) {
      const MTLTextureSwizzleChannels channels =
          MTLTextureSwizzleChannelsMake(to_mtl_swizzle(view.swizzle.red), to_mtl_swizzle(view.swizzle.green),
                                        to_mtl_swizzle(view.swizzle.blue), to_mtl_swizzle(view.swizzle.alpha));
      // The swizzle applies to the whole view contract, so the view must cover
      // every level and slice the CanonicalView declares. A (0,1)/(0,1) range
      // would silently narrow a mip/array facet to its first subresource.
      shader_texture = [storage_texture newTextureViewWithPixelFormat:to_mtl_pixel_format(view.format)
                                                          textureType:to_mtl_texture_type(view.dimension)
                                                               levels:NSMakeRange(0, view.mip_levels)
                                                               slices:NSMakeRange(0, view.array_layers)
                                                              swizzle:channels];
      if (shader_texture == nil) {
        if (error) *error = "Metal facet swizzle texture view creation failed";
        return nil;
      }
    }
    MetalFacetRecord record;
    record.texture = shader_texture;
    record.storage_texture = storage_texture;
    record.facet_index = ref.index;
    record.facet_generation = ref.generation;
    record.representation_epoch = representation_epoch;
    record.kind = kind;
    record.width = view.width;
    record.height = view.height;
    record.dimension = view.dimension;
    record.array_layers = view.array_layers;
    record.mip_levels = view.mip_levels;
    record.format = view.format;
    record.swizzle = view.swizzle;
    const uint64_t key = facet_cache_key(ref);
    auto existing = facet_map.find(key);
    if (existing != facet_map.end()) {
      release_facet_textures(existing->second);
      facet_map.erase(existing);
    }
    facet_map[key] = record;
    return shader_texture;
  }

  // Uploads every subresource the view declares, decoded through
  // CanonicalView's own linear layout contract (slice-major, then ascending
  // mip level, each level tightly packed at bytes_per_row(level)). This is the
  // same contract reference::sample_facet decodes, so an image comparison
  // between the two backends is comparing sampling, not two disagreeing
  // opinions about byte layout.
  void upload_view_subresources(id<MTLTexture> texture, const core::CanonicalView& view,
                                const core::Allocation& allocation) {
    if (allocation.bytes.empty()) return;
    const uint8_t* base = allocation.bytes.data();
    for (uint32_t layer = 0; layer < view.array_layers; ++layer) {
      for (uint32_t level = 0; level < view.mip_levels; ++level) {
        const uint64_t offset = view.subresource_byte_offset(layer, level);
        const uint64_t row_bytes = view.bytes_per_row(level);
        const MTLRegion region = MTLRegionMake2D(0, 0, view.mip_width(level), view.mip_height(level));
        [texture replaceRegion:region
                   mipmapLevel:level
                         slice:layer
                     withBytes:base + offset
                   bytesPerRow:row_bytes
                 bytesPerImage:0];
      }
    }
  }

  // FacetPool::lookup first; cache keyed by FacetRef index+generation.
  // Returns the shader-visible object; `out_storage` (optional) receives the
  // object that owns the pixels, which is what host upload/readback must use.
  id<MTLTexture> ensure_facet_texture(const core::Arena& arena, const core::FacetPool& pool,
                                      core::FacetRef ref, core::FacetKind expected_kind,
                                      bool* cache_hit, id<MTLTexture>* out_storage,
                                      std::string* error) {
    const core::FacetSlot* slot = resolve_facet(arena, pool, ref, expected_kind, error);
    if (slot == nullptr) return nil;
    const core::CanonicalView& view = slot->view;
    const core::Allocation* allocation = arena.lookup(view.allocation, view.allocation_generation);
    if (allocation == nullptr) {
      if (error) *error = "facet backing allocation not found in arena";
      return nil;
    }
    // The cache is consulted before the backing is examined, and deliberately
    // so. After a Stage 5 ConsumeInput the linear bytes this facet was built
    // from are gone -- that is the whole point of reporting distinct_backing
    // (02 §4.2, 06 §11): the Private texture is independent storage, and the
    // facet the transform published "stays live across a ConsumeInput". Asking
    // the allocation how many bytes it still holds before answering would
    // retire exactly the facet a consume is supposed to leave usable.
    const uint64_t key = facet_cache_key(ref);
    auto it = facet_map.find(key);
    if (it != facet_map.end() &&
        it->second.representation_epoch == slot->representation_epoch &&
        it->second.kind == slot->kind && same_shape(it->second, view) &&
        it->second.facet_index == ref.index &&
        it->second.facet_generation == ref.generation) {
      if (cache_hit) *cache_hit = true;
      if (out_storage) *out_storage = it->second.storage_texture;
      return it->second.texture;
    }
    if (it != facet_map.end()) {
      release_facet_textures(it->second);
      facet_map.erase(it);
    }

    // Creating one, on the other hand, means seeding it from host bytes, so
    // here the backing really does have to cover every subresource the view
    // declares.
    if (!view_expressible(view, expected_kind, allocation->bytes.size(), error)) return nil;

    id<MTLTexture> storage_texture =
        [device newTextureWithDescriptor:make_texture_descriptor(view, expected_kind, MTLStorageModeShared)];
    if (storage_texture == nil) {
      if (error)
        *error = expected_kind == core::FacetKind::Storage
                     ? "Unsupported: pixel format does not support shader write on this device; "
                       "use StorageFacetTarget::LinearBuffer or transform the representation"
                     : "Metal facet texture creation failed";
      return nil;
    }

    // Every kind is seeded from the allocation, including Attachment: a
    // load-action pass must see the bytes the CanonicalView names, which is
    // also what the Private transform path produces for an Attachment target,
    // so the two ways of reaching an attachment texture agree on its initial
    // contents.
    upload_view_subresources(storage_texture, view, *allocation);

    id<MTLTexture> shader_texture = install_facet_record(ref, view, slot->kind, slot->representation_epoch,
                                                         storage_texture, error);
    if (shader_texture == nil) return nil;
    if (cache_hit) *cache_hit = false;
    if (out_storage) *out_storage = storage_texture;
    return shader_texture;
  }

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
  id<MTLSamplerState> ensure_sampler_state(core::FilterMode filter, core::WrapMode wrap,
                                           float lod_min = 0.0f,
                                           float lod_max = std::numeric_limits<float>::max()) {
    std::array<uint32_t, 4> key{static_cast<uint32_t>(filter), static_cast<uint32_t>(wrap), 0, 0};
    std::memcpy(&key[2], &lod_min, sizeof(float));
    std::memcpy(&key[3], &lod_max, sizeof(float));
    auto it = sampler_cache.find(key);
    if (it != sampler_cache.end()) return it->second;
    MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
    const bool nearest = filter == core::FilterMode::Nearest;
    const MTLSamplerMinMagFilter mtl_filter =
        nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    descriptor.minFilter = mtl_filter;
    descriptor.magFilter = mtl_filter;
    descriptor.mipFilter = nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    descriptor.lodMinClamp = lod_min;
    descriptor.lodMaxClamp = lod_max;
    const MTLSamplerAddressMode mtl_wrap =
        wrap == core::WrapMode::Clamp ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
    descriptor.sAddressMode = mtl_wrap;
    descriptor.tAddressMode = mtl_wrap;
    descriptor.rAddressMode = mtl_wrap;
    descriptor.normalizedCoordinates = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    if (sampler != nil) sampler_cache.emplace(key, sampler);
    return sampler;
  }

  // Encodes one command buffer that optionally waits on `wait_value` before
  // dispatching and signals `signal_value` after -- both 0 mean "no
  // timeline involvement," preserving the original B4/B5 synchronous path.
  // When `tasks` is non-empty, dispatches once per task using that task's
  // own x/y/z grid dimensions instead of the hardcoded (1,1,1); this is the
  // B8 Tier0 requirement that dispatch sizing come from real TaskRecord
  // fields, not a placeholder.
  bool dispatch_and_wait(const std::vector<id<MTLBuffer>>& buffers, const std::vector<core::TaskRecord>& tasks,
                        uint64_t wait_value, uint64_t signal_value, DispatchStats* stats, std::string* error) {
    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    if (wait_value != 0) [command_buffer encodeWaitForEvent:timeline_event value:wait_value];
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
    [encoder setComputePipelineState:pipeline];
    for (size_t index = 0; index < buffers.size(); ++index) [encoder setBuffer:buffers[index] offset:0 atIndex:index];
    if (tasks.empty()) {
      [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    } else {
      for (const auto& task : tasks)
        [encoder dispatchThreadgroups:MTLSizeMake(task.x, task.y, task.z) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    }
    [encoder endEncoding];
    if (signal_value != 0) [command_buffer encodeSignalEvent:timeline_event value:signal_value];
    const auto submit_start = std::chrono::steady_clock::now();
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    const auto submit_end = std::chrono::steady_clock::now();
    if (stats != nullptr) {
      stats->cpu_encode_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(submit_start - encode_start).count();
      stats->cpu_submit_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(submit_end - submit_start).count();
      stats->encoder_count += 1;
      stats->command_buffer_count += 1;
      stats->queue_wait_count += 1;
    }
    if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
      if (error)
        *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                              : "Metal command buffer failed";
      return false;
    }
    return true;
  }

  // TASK-B16 (E007): lazily probes whether MTLBuffer.gpuAddress is actually
  // available on this OS/device combination, rather than trusting the
  // capability snapshot's own gpu_addresses bit (hardcoded false in
  // make_hal_snapshot above, never wired to a real probe) -- cached after
  // the first call since this answer cannot change within a process
  // lifetime.
  bool gpu_addresses_probed = false;
  bool gpu_addresses_supported_value = false;
  bool probe_gpu_addresses() {
    if (gpu_addresses_probed) return gpu_addresses_supported_value;
    gpu_addresses_probed = true;
    id<MTLBuffer> probe = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
    gpu_addresses_supported_value = probe != nil && [probe respondsToSelector:@selector(gpuAddress)];
    return gpu_addresses_supported_value;
  }

  // Argument-buffer-style indexed dispatch (TASK-B16/E007): binds exactly
  // one table buffer -- real GPU virtual addresses, one per object in
  // `object_buffers` -- at buffer(0), never each object's own buffer(N)
  // slot. That single-binding shape is the whole point of the contrast
  // against dispatch_and_wait's per-object binding loop above. Every object
  // buffer still needs useResource: for GPU residency even though it is
  // never itself bound at an index -- a real, distinct cost this milestone
  // deliberately reports rather than hides.
  bool dispatch_indexed_and_wait(const std::vector<id<MTLBuffer>>& object_buffers, uint64_t wait_value,
                                uint64_t signal_value, DispatchStats* stats, std::string* error) {
    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    if (wait_value != 0) [command_buffer encodeWaitForEvent:timeline_event value:wait_value];

    const size_t table_bytes = std::max<size_t>(object_buffers.size() * sizeof(uint64_t), sizeof(uint64_t));
    id<MTLBuffer> table_buffer = [device newBufferWithLength:table_bytes options:MTLResourceStorageModeShared];
    if (table_buffer == nil) { if (error) *error = "failed to allocate indexed binding table buffer"; return false; }
    uint64_t* table = static_cast<uint64_t*>([table_buffer contents]);
    for (size_t index = 0; index < object_buffers.size(); ++index) table[index] = [object_buffers[index] gpuAddress];

    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
    [encoder setComputePipelineState:pipeline];
    for (id<MTLBuffer> buffer : object_buffers)
      [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [encoder setBuffer:table_buffer offset:0 atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    if (signal_value != 0) [command_buffer encodeSignalEvent:timeline_event value:signal_value];
    const auto submit_start = std::chrono::steady_clock::now();
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    const auto submit_end = std::chrono::steady_clock::now();
    if (stats != nullptr) {
      stats->cpu_encode_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(submit_start - encode_start).count();
      stats->cpu_submit_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(submit_end - submit_start).count();
      stats->encoder_count += 1;
      stats->command_buffer_count += 1;
      stats->queue_wait_count += 1;
    }
    if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
      if (error)
        *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                              : "Metal indexed command buffer failed";
      return false;
    }
    return true;
  }

  // TASK-B14 (E012): dispatches a general Effect DAG's passes within a single
  // MTLCommandBuffer, choosing the encoder/fence structure from the graph's
  // classified shape (ADR-027). LinearChain: one encoder, sequential
  // dispatches in topological order -- a single encoder's commands already
  // execute in encode order, so no explicit sync is needed. IndependentBranches:
  // one encoder per node, no fences -- relies on Metal's default hazard
  // tracking across encoders within one command buffer, exactly as
  // dispatch_task_tier1_indirect already does for its blit-then-dispatch
  // pair. ForkJoin: a source encoder, then one encoder per middle node each
  // updating its *own* dedicated MTLFence after its dispatch, then a join
  // encoder that waits on every middle branch's fence before its own
  // dispatch -- one fence per branch, not one shared fence, since a single
  // MTLFence's wait only guarantees ordering against the most recent update
  // in encode order, not against every earlier update from independent
  // producers (an ambiguity a dedicated per-branch fence avoids entirely).
  bool dispatch_effect_dag(const std::vector<id<MTLComputePipelineState>>& pipelines,
                           const std::vector<std::vector<id<MTLBuffer>>>& buffers,
                           core::EffectGraphShape shape, const core::EffectGraph& graph,
                           uint32_t node_count, DispatchStats* stats, std::string* error) {
    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }

    auto dispatch_node = [&](id<MTLComputeCommandEncoder> encoder, uint32_t node) {
      [encoder setComputePipelineState:pipelines[node]];
      for (size_t i = 0; i < buffers[node].size(); ++i) [encoder setBuffer:buffers[node][i] offset:0 atIndex:i];
      [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    };

    if (shape == core::EffectGraphShape::LinearChain || shape == core::EffectGraphShape::IndependentBranches) {
      std::vector<uint32_t> order;
      std::string order_error;
      if (!core::effect_graph_deterministic_order(graph, node_count, &order, &order_error)) {
        if (error) *error = order_error;
        return false;
      }
      if (shape == core::EffectGraphShape::LinearChain) {
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
        for (uint32_t node : order) dispatch_node(encoder, node);
        [encoder endEncoding];
        if (stats != nullptr) stats->encoder_count += 1;
      } else {
        for (uint32_t node : order) {
          id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
          if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
          dispatch_node(encoder, node);
          [encoder endEncoding];
          if (stats != nullptr) stats->encoder_count += 1;
        }
      }
    } else if (shape == core::EffectGraphShape::ForkJoin) {
      // classify_effect_graph_shape's exact edge-count invariant
      // (structural_edges == 2*(node_count-1), with the source's n-1 out-
      // edges and join's n-1 in-edges each counted once) always forces
      // exactly one extra edge among the "middle" nodes themselves once
      // node_count > 3 -- a pure diamond with zero middle-to-middle edges
      // only ever produces 2*node_count-3 (or fewer) structural edges, one
      // short of what this classifier requires. So middles are NOT
      // guaranteed mutually independent here even though shape == ForkJoin;
      // rather than assume they are (which would silently drop a real
      // ordering requirement and race on GPU), every node waits on the
      // fence of every node with a direct structural edge into it -- this
      // is exactly right for the textbook diamond (middles have no direct
      // edge, so no extra wait) and for the guaranteed one-extra-edge case
      // alike, with no special-casing of source/join beyond "no
      // predecessors to wait on" / "nothing waits on it further".
      std::vector<uint32_t> order;
      std::string order_error;
      if (!core::effect_graph_deterministic_order(graph, node_count, &order, &order_error)) {
        if (error) *error = order_error;
        return false;
      }
      std::vector<std::vector<uint32_t>> predecessors(node_count);
      for (const auto& edge : graph.edges()) {
        if (edge.kind != core::EffectEdgeKind::Explicit && edge.kind != core::EffectEdgeKind::InferredConflict)
          continue;
        predecessors[edge.after].push_back(edge.before);
      }
      std::vector<id<MTLFence>> fence_by_node(node_count, nil);
      for (uint32_t node : order) {
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
        for (uint32_t pred : predecessors[node]) {
          if (fence_by_node[pred] != nil) [encoder waitForFence:fence_by_node[pred]];
        }
        dispatch_node(encoder, node);
        id<MTLFence> fence = [device newFence];
        if (fence == nil) { if (error) *error = "failed to create Metal fence"; return false; }
        [encoder updateFence:fence];
        fence_by_node[node] = fence;
        [encoder endEncoding];
        if (stats != nullptr) {
          stats->encoder_count += 1;
          if (!predecessors[node].empty()) stats->barrier_count += 1;
        }
      }
    } else {
      if (error) *error = "effect dag shape is unsupported";
      return false;
    }

    const auto submit_start = std::chrono::steady_clock::now();
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    const auto submit_end = std::chrono::steady_clock::now();
    if (stats != nullptr) {
      stats->cpu_encode_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(submit_start - encode_start).count();
      stats->cpu_submit_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(submit_end - submit_start).count();
      stats->command_buffer_count += 1;
      stats->queue_wait_count += 1;
    }
    if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
      if (error)
        *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                              : "Metal effect DAG dispatch failed";
      return false;
    }
    return true;
  }

  // Compiles compiler::task_ring_metal_source() into its own pipeline,
  // separate from the B4 linear-compute pipeline: the publication protocol
  // is backend-private infrastructure, not part of the target-neutral
  // ComputePackage contract.
  bool ensure_task_ring_pipeline(std::string* error) {
    if (task_ring_pipeline != nil) return true;
    NSError* compile_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    const std::string source = compiler::task_ring_metal_source();
    id<MTLLibrary> new_library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                        options:options
                                                          error:&compile_error];
    if (new_library == nil) {
      if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                                : "unknown task ring MSL compile error";
      return false;
    }
    id<MTLFunction> function = [new_library newFunctionWithName:@"vg_task_publish"];
    if (function == nil) {
      if (error) *error = "task ring MSL library missing vg_task_publish entry point";
      return false;
    }
    NSError* pipeline_error = nil;
    id<MTLComputePipelineState> new_pipeline = [device newComputePipelineStateWithFunction:function
                                                                                       error:&pipeline_error];
    if (new_pipeline == nil) {
      if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                 : "unknown task ring pipeline creation error";
      return false;
    }
    task_ring_library = new_library;
    task_ring_pipeline = new_pipeline;
    return true;
  }

  // One thread per task (grid = (count,1,1) threadgroups of (1,1,1)
  // threads), so no two threads ever contend for the same ring slot --
  // each slot's Empty->Writing CAS can only ever be attempted once.
  bool dispatch_task_publish(id<MTLBuffer> state_buffer, id<MTLBuffer> fields_buffer, id<MTLBuffer> inputs_buffer,
                             uint32_t count, DispatchStats* stats, std::string* error) {
    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
    [encoder setComputePipelineState:task_ring_pipeline];
    [encoder setBuffer:state_buffer offset:0 atIndex:0];
    [encoder setBuffer:fields_buffer offset:0 atIndex:1];
    [encoder setBuffer:inputs_buffer offset:0 atIndex:2];
    [encoder dispatchThreadgroups:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    const auto submit_start = std::chrono::steady_clock::now();
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    const auto submit_end = std::chrono::steady_clock::now();
    if (stats != nullptr) {
      stats->cpu_encode_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(submit_start - encode_start).count();
      stats->cpu_submit_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(submit_end - submit_start).count();
      stats->encoder_count += 1;
      stats->command_buffer_count += 1;
      stats->queue_wait_count += 1;
    }
    if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
      if (error)
        *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                              : "Metal task ring dispatch failed";
      return false;
    }
    return true;
  }

  // TASK-B13 (E009): compiles compiler::cull_compact_metal_source() into its
  // own pipeline, mirroring ensure_task_ring_pipeline()'s pattern -- this is
  // backend-private infrastructure, not part of the target-neutral
  // ComputePackage contract.
  bool ensure_cull_compact_pipeline(std::string* error) {
    if (cull_compact_pipeline != nil) return true;
    NSError* compile_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    const std::string source = compiler::cull_compact_metal_source();
    id<MTLLibrary> new_library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                        options:options
                                                          error:&compile_error];
    if (new_library == nil) {
      if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                                : "unknown cull/compact MSL compile error";
      return false;
    }
    id<MTLFunction> function = [new_library newFunctionWithName:@"vg_cull_compact"];
    if (function == nil) {
      if (error) *error = "cull/compact MSL library missing vg_cull_compact entry point";
      return false;
    }
    NSError* pipeline_error = nil;
    id<MTLComputePipelineState> new_pipeline = [device newComputePipelineStateWithFunction:function
                                                                                       error:&pipeline_error];
    if (new_pipeline == nil) {
      if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                 : "unknown cull/compact pipeline creation error";
      return false;
    }
    cull_compact_library = new_library;
    cull_compact_pipeline = new_pipeline;
    return true;
  }

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
  std::string target_identity() const {
    return std::string([[device name] UTF8String]) + "|" +
           [[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String] + "|MSL";
  }

  compiler::PipelineKey make_pipeline_key(const std::string& source, const std::string& entry,
                                          std::vector<std::pair<std::string, uint64_t>> constants,
                                          std::vector<uint32_t> attachment_formats,
                                          uint32_t sample_count) const {
    compiler::PipelineKey key;
    key.code_object_hash = ir::sha256_hex(source);
    key.entry = entry;
    key.function_constants = std::move(constants);
    key.attachment_formats = std::move(attachment_formats);
    key.sample_count = sample_count;
    key.target_identity = target_identity();
    return key;
  }

  // One MTLLibrary per distinct MSL text, so two specializations of the same
  // source share the compiled library and differ only in the function constant
  // values applied to it.
  id<MTLLibrary> ensure_library(const std::string& source, const std::string& hash, std::string* error) {
    auto it = library_by_hash.find(hash);
    if (it != library_by_hash.end()) return it->second;
    NSError* compile_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    id<MTLLibrary> library_object =
        [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                             options:options
                               error:&compile_error];
    if (library_object == nil) {
      if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                                : "unknown MSL compile error";
      return nil;
    }
    library_by_hash.emplace(hash, library_object);
    return library_object;
  }

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
  id<MTLFunction> ensure_function(id<MTLLibrary> library_object, const compiler::PipelineKey& key,
                                  std::string* error) {
    NSString* name = [NSString stringWithUTF8String:key.entry.c_str()];
    MTLFunctionConstantValues* values = [MTLFunctionConstantValues new];
    for (const auto& constant : key.function_constants) {
      // Every function constant this backend specializes on is the MSL `bool`
      // of 06 §6.4; a wider constant would need its own type here rather than
      // being coerced into this one.
      const bool value = constant.second != 0;
      [values setConstantValue:&value
                          type:MTLDataTypeBool
                       atIndex:compiler::kFacetCheckedProfileFunctionConstant];
    }
    NSError* function_error = nil;
    id<MTLFunction> function = [library_object newFunctionWithName:name
                                                    constantValues:values
                                                             error:&function_error];
    if (function == nil && error)
      *error = function_error != nil ? [[function_error localizedDescription] UTF8String]
                                     : "MSL specialization of " + key.entry + " failed";
    return function;
  }

  bool acquire_compute_pipeline(compiler::PipelineClassificationCache& cache,
                                std::unordered_map<uint64_t, id<MTLComputePipelineState>>& objects,
                                const std::string& source, const compiler::PipelineKey& key,
                                const std::string& trigger, id<MTLComputePipelineState>* out,
                                compiler::SpecializationReport* report, std::string* error) {
    const uint64_t digest = key.hash();
    compiler::SpecializationReport local;
    id<MTLComputePipelineState> created = nil;
    const bool ok = cache.acquire(
        key, trigger,
        [&](uint64_t* binary_size, std::string* create_error) {
          id<MTLLibrary> library_object = ensure_library(source, key.code_object_hash, create_error);
          if (library_object == nil) return false;
          id<MTLFunction> function = ensure_function(library_object, key, create_error);
          if (function == nil) return false;
          NSError* pipeline_error = nil;
          created = [device newComputePipelineStateWithFunction:function error:&pipeline_error];
          if (created == nil) {
            if (create_error)
              *create_error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                    : "unknown compute pipeline creation error";
            return false;
          }
          // Metal exposes no compiled binary size for a pipeline built from
          // source, and 10 §12 forbids writing an unobservable cost as a real
          // number, so it stays 0 rather than becoming a guess.
          *binary_size = 0;
          return true;
        },
        &local, error);
    if (!ok) return false;
    if (created != nil) objects[digest] = created;
    auto it = objects.find(digest);
    if (it == objects.end()) {
      if (error) *error = "pipeline cache reported a hit for a Metal object this device never created";
      return false;
    }
    if (out) *out = it->second;
    if (report) *report = local;
    return true;
  }

  bool acquire_render_pipeline(compiler::PipelineClassificationCache& cache,
                               std::unordered_map<uint64_t, id<MTLRenderPipelineState>>& objects,
                               const std::string& source, const compiler::PipelineKey& key,
                               const std::string& vertex_entry, MTLPixelFormat color_format,
                               const std::string& trigger, id<MTLRenderPipelineState>* out,
                               compiler::SpecializationReport* report, std::string* error) {
    const uint64_t digest = key.hash();
    compiler::SpecializationReport local;
    id<MTLRenderPipelineState> created = nil;
    const bool ok = cache.acquire(
        key, trigger,
        [&](uint64_t* binary_size, std::string* create_error) {
          id<MTLLibrary> library_object = ensure_library(source, key.code_object_hash, create_error);
          if (library_object == nil) return false;
          compiler::PipelineKey vertex_key = key;
          vertex_key.entry = vertex_entry;
          id<MTLFunction> vertex_function = ensure_function(library_object, vertex_key, create_error);
          if (vertex_function == nil) return false;
          id<MTLFunction> fragment_function = ensure_function(library_object, key, create_error);
          if (fragment_function == nil) return false;
          MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
          descriptor.vertexFunction = vertex_function;
          descriptor.fragmentFunction = fragment_function;
          descriptor.colorAttachments[0].pixelFormat = color_format;
          descriptor.rasterSampleCount = key.sample_count;
          // No MTLVertexDescriptor on purpose: the vertex stage indexes a
          // `device const VgRasterVertex*` at buffer(0) by [[vertex_id]].
          // Pointer-indexed root data is this project's addressing philosophy
          // (04 §8, 06 §5) and it keeps vertex layout out of the key (06 §7).
          NSError* pipeline_error = nil;
          created = [device newRenderPipelineStateWithDescriptor:descriptor error:&pipeline_error];
          if (created == nil) {
            if (create_error)
              *create_error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                    : "unknown render pipeline creation error";
            return false;
          }
          *binary_size = 0;
          return true;
        },
        &local, error);
    if (!ok) return false;
    if (created != nil) objects[digest] = created;
    auto it = objects.find(digest);
    if (it == objects.end()) {
      if (error) *error = "pipeline cache reported a hit for a Metal object this device never created";
      return false;
    }
    if (out) *out = it->second;
    if (report) *report = local;
    return true;
  }

  // The SampleFacet kernel 06 §6.1 asks for, in the four shapes that actually
  // differ: texture2d vs texture2d_array (a CanonicalView's declared dimension,
  // not a host convenience) crossed with the 06 §6.4 generation guard being
  // compiled in or specialized away. `checked` is the only piece of state that
  // has to enter the pipeline key here; the lod value, the array slices and the
  // sampler are per-use data and bindings that must not.
  bool ensure_sample_facet_pipeline(bool array_dimension, bool checked,
                                    id<MTLComputePipelineState>* out, std::string* error) {
    const std::string source = array_dimension ? compiler::sample_facet_array_metal_source()
                                               : compiler::sample_facet_metal_source();
    const std::string entry = array_dimension ? "vg_sample_facet_array" : "vg_sample_facet";
    std::vector<std::pair<std::string, uint64_t>> constants;
    if (checked) constants.emplace_back("vg_checked_profile", 1);
    const compiler::PipelineKey key = make_pipeline_key(source, entry, constants, {}, 1);
    return acquire_compute_pipeline(pipeline_cache, compute_pipeline_by_key, source, key,
                                    checked ? "checked-profile facet generation guard (06 §6.4)"
                                            : "fast-native SampleFacet kernel",
                                    out, nullptr, error);
  }

  // Real MTLRenderPipelineState for the shared raster pair. Only the attachment
  // format and the sample count reach the key; viewport and tint are dynamic
  // state and shader-visible data respectively, and stay out of it.
  bool ensure_raster_pipeline(core::PixelFormat format, uint32_t sample_count,
                              id<MTLRenderPipelineState>* out, std::string* error) {
    const std::string source = compiler::raster_facet_metal_source();
    const compiler::PipelineKey key = make_pipeline_key(source, "vg_raster_fragment", {},
                                                        {static_cast<uint32_t>(format)}, sample_count);
    return acquire_render_pipeline(pipeline_cache, render_pipeline_by_key, source, key, "vg_raster_vertex",
                                   to_mtl_pixel_format(format),
                                   "basic raster attachment store (05 §9 region.attachment.store)", out,
                                   nullptr, error);
  }

  // 1x1 stand-in bound on the path where the checked-profile guard is expected
  // to reject the token before the kernel's first sample. It carries no facet's
  // pixels and is never read; binding the rejected facet's last-known texture
  // instead would be exactly the "resolve a stale token to its last-known
  // object" behaviour 02 §10 forbids.
  id<MTLTexture> ensure_guard_placeholder_texture(std::string* error) {
    if (guard_placeholder_texture != nil) return guard_placeholder_texture;
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    guard_placeholder_texture = [device newTextureWithDescriptor:descriptor];
    if (guard_placeholder_texture == nil && error)
      *error = "Metal guard placeholder texture creation failed";
    return guard_placeholder_texture;
  }

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
  bool ensure_storage_facet_pipelines(std::string* error) {
    if (storage_facet_pipeline != nil && storage_array_facet_pipeline != nil &&
        storage_buffer_facet_pipeline != nil)
      return true;
    NSError* compile_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    const char* source =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "kernel void vg_storage_facet_write(texture2d<float, access::write> tex [[texture(0)]],\n"
        "                                   constant float4& rgba [[buffer(0)]],\n"
        "                                   constant uint4& target [[buffer(1)]],\n"
        "                                   uint2 gid [[thread_position_in_grid]]) {\n"
        "  if (gid.x != 0 || gid.y != 0) return;\n"
        "  tex.write(rgba, uint2(target.x, target.y), target.w);\n"
        "}\n"
        "kernel void vg_storage_facet_write_array(texture2d_array<float, access::write> tex [[texture(0)]],\n"
        "                                         constant float4& rgba [[buffer(0)]],\n"
        "                                         constant uint4& target [[buffer(1)]],\n"
        "                                         uint2 gid [[thread_position_in_grid]]) {\n"
        "  if (gid.x != 0 || gid.y != 0) return;\n"
        "  tex.write(rgba, uint2(target.x, target.y), target.z, target.w);\n"
        "}\n"
        // format: 0 = RGBA8Unorm, 1 = R32Float, matching core::PixelFormat.
        // The write is encoded in the view's own format so the linear target
        // never silently changes precision (06 §6.2). texel_index is the
        // caller's texel offset in units of core::bytes_per_texel, which is 4
        // for both formats this milestone models.
        "kernel void vg_storage_facet_write_buffer(device uint* texels [[buffer(0)]],\n"
        "                                          constant float4& rgba [[buffer(1)]],\n"
        "                                          constant uint& format [[buffer(2)]],\n"
        "                                          constant uint& texel_index [[buffer(3)]],\n"
        "                                          uint gid [[thread_position_in_grid]]) {\n"
        "  if (gid != 0) return;\n"
        "  if (format == 0) {\n"
        "    texels[texel_index] = pack_float_to_unorm4x8(rgba);\n"
        "  } else {\n"
        "    device float* floats = (device float*)texels;\n"
        "    floats[texel_index] = rgba.x;\n"
        "  }\n"
        "}\n";
    id<MTLLibrary> new_library = [device newLibraryWithSource:@(source)
                                                        options:options
                                                          error:&compile_error];
    if (new_library == nil) {
      if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                                : "unknown storage facet MSL compile error";
      return false;
    }
    id<MTLComputePipelineState> new_pipelines[3] = {nil, nil, nil};
    const char* entry_points[3] = {"vg_storage_facet_write", "vg_storage_facet_write_array",
                                   "vg_storage_facet_write_buffer"};
    for (int i = 0; i < 3; ++i) {
      id<MTLFunction> function = [new_library newFunctionWithName:@(entry_points[i])];
      if (function == nil) {
        if (error) *error = std::string("storage facet MSL library missing ") + entry_points[i] + " entry point";
        return false;
      }
      NSError* pipeline_error = nil;
      new_pipelines[i] = [device newComputePipelineStateWithFunction:function error:&pipeline_error];
      if (new_pipelines[i] == nil) {
        if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                   : "unknown storage facet pipeline creation error";
        return false;
      }
    }
    storage_facet_library = new_library;
    storage_facet_pipeline = new_pipelines[0];
    storage_array_facet_pipeline = new_pipelines[1];
    storage_buffer_facet_pipeline = new_pipelines[2];
    return true;
  }

  // 06 §6.3's load/store/resolve, lowered onto one subresource of `texture`.
  // Shared by the draw-free attachment probe and by the raster draw so the two
  // cannot drift on what a store or a resolve means, and so the multisample
  // rule (a transient MS target resolving into the facet's own texture) is
  // stated once.
  MTLRenderPassDescriptor* make_render_pass(id<MTLTexture> texture, const AttachmentFacetDesc& desc,
                                            const core::CanonicalView& view,
                                            bool* store_traffic_avoided, std::string* error) {
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
    MTLRenderPassColorAttachmentDescriptor* color = rp.colorAttachments[0];
    color.clearColor =
        MTLClearColorMake(desc.clear_rgba[0], desc.clear_rgba[1], desc.clear_rgba[2], desc.clear_rgba[3]);
    switch (desc.load) {
      case AttachmentLoadAction::Clear: color.loadAction = MTLLoadActionClear; break;
      case AttachmentLoadAction::Load: color.loadAction = MTLLoadActionLoad; break;
      case AttachmentLoadAction::DontCare: color.loadAction = MTLLoadActionDontCare; break;
    }
    const uint32_t level = desc.subresource.level;
    if (desc.sample_count > 1) {
      MTLTextureDescriptor* ms = [MTLTextureDescriptor new];
      ms.textureType = MTLTextureType2DMultisample;
      ms.pixelFormat = texture.pixelFormat;
      // The transient target is sized for the subresource being rendered, not
      // for mip 0: rendering into level N of a mip chain is a smaller pass.
      ms.width = view.mip_width(level);
      ms.height = view.mip_height(level);
      ms.sampleCount = desc.sample_count;
      ms.usage = MTLTextureUsageRenderTarget;
      // Memoryless keeps the per-sample data on-tile, so the only external
      // write is the resolved single-sample result. Where that is unavailable
      // the samples really do go to device memory, and the result says so
      // rather than claiming an optimization the device did not perform.
      const bool memoryless = [device supportsFamily:MTLGPUFamilyApple1];
      ms.storageMode = memoryless ? MTLStorageModeMemoryless : MTLStorageModePrivate;
      id<MTLTexture> ms_texture = [device newTextureWithDescriptor:ms];
      if (ms_texture == nil) {
        if (error) *error = "Unsupported: device rejected a multisample render target for this format";
        return nil;
      }
      color.texture = ms_texture;
      color.resolveTexture = texture;
      color.resolveLevel = level;
      color.resolveSlice = desc.subresource.layer;
      color.storeAction = MTLStoreActionMultisampleResolve;
      if (store_traffic_avoided) *store_traffic_avoided = memoryless;
    } else {
      color.texture = texture;
      color.level = level;
      color.slice = desc.subresource.layer;
      color.storeAction =
          desc.store == AttachmentStoreAction::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
      if (store_traffic_avoided) *store_traffic_avoided = desc.store == AttachmentStoreAction::DontCare;
    }
    return rp;
  }

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
                                    core::FacetRef target_facet, TransformCost* cost, std::string* error) {
    const core::Allocation* allocation = arena.lookup(view.allocation, view.allocation_generation);
    if (allocation == nullptr) {
      if (error) *error = "representation transform: backing allocation not found in arena";
      return false;
    }
    if (!view_expressible(view, target_kind, allocation->bytes.size(), error)) return false;

    id<MTLTexture> private_texture =
        [device newTextureWithDescriptor:make_texture_descriptor(view, target_kind, MTLStorageModePrivate)];
    if (private_texture == nil) {
      if (error) *error = "representation transform: Private MTLTexture creation failed";
      return false;
    }

    core::FacetRef transfer_ref{};
    if (!pool.acquire(arena, view, core::FacetKind::Transfer, &transfer_ref, error)) return false;
    bool blit_ok = false;
    {
      FacetUseGuard use(pool, transfer_ref);
      if (!use.begin(arena, error)) return false;
      id<MTLBuffer> source = ensure_facet_buffer(arena, pool, transfer_ref, core::FacetKind::Transfer, error);
      if (source == nil) return false;

      id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
      if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
      id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
      if (blit == nil) { if (error) *error = "failed to create Metal blit encoder"; return false; }
      for (uint32_t layer = 0; layer < view.array_layers; ++layer) {
        for (uint32_t level = 0; level < view.mip_levels; ++level) {
          const uint64_t offset = view.subresource_byte_offset(layer, level);
          const uint64_t row_bytes = view.bytes_per_row(level);
          [blit copyFromBuffer:source
                  sourceOffset:offset
             sourceBytesPerRow:row_bytes
           sourceBytesPerImage:row_bytes * view.mip_height(level)
                    sourceSize:MTLSizeMake(view.mip_width(level), view.mip_height(level), 1)
                     toTexture:private_texture
              destinationSlice:layer
              destinationLevel:level
             destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
      }
      [blit endEncoding];
      [command_buffer commit];
      [command_buffer waitUntilCompleted];
      if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
        if (error)
          *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                                : "representation transform blit failed";
        return false;
      }
      blit_ok = true;
    }
    // The TransferFacet existed only to give the blit a pool-resolved source;
    // its purpose is spent, so its slot goes back rather than being left to
    // linger until some later epoch happens to stale it.
    if (blit_ok) pool.retire(transfer_ref);

    if (install_facet_record(target_facet, view, target_kind, allocation->representation_epoch,
                             private_texture, error) == nil)
      return false;

    if (cost != nullptr) {
      // The device's own accounting for the texture it just created, not the
      // view's logical extent: alignment and tiling padding are real bytes the
      // peak-memory report of 06 §11 has to include.
      const uint64_t allocated = [private_texture allocatedSize];
      cost->new_backing_bytes = allocated != 0 ? allocated : view.byte_size();
      cost->temporary_bytes = 0;
      cost->encoder_count = 1;
    }
    return true;
  }

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
                                    DispatchStats* stats, std::string* error) {
    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    const size_t stride = sizeof(MTLDispatchThreadgroupsIndirectArguments);
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    if (blit == nil) { if (error) *error = "failed to create Metal blit encoder"; return false; }
    for (size_t i = 0; i < order.size(); ++i) {
      const size_t src_offset = (static_cast<size_t>(order[i]) * kTaskRingWordsPerRecord + 5) * sizeof(uint32_t);
      [blit copyFromBuffer:fields_buffer sourceOffset:src_offset
                  toBuffer:indirect_args_buffer destinationOffset:i * stride
                     size:3 * sizeof(uint32_t)];
    }
    [blit endEncoding];
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
    [encoder setComputePipelineState:pipeline];
    for (size_t index = 0; index < buffers.size(); ++index) [encoder setBuffer:buffers[index] offset:0 atIndex:index];
    for (size_t i = 0; i < order.size(); ++i)
      [encoder dispatchThreadgroupsWithIndirectBuffer:indirect_args_buffer
                                  indirectBufferOffset:i * stride
                                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    const auto submit_start = std::chrono::steady_clock::now();
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    const auto submit_end = std::chrono::steady_clock::now();
    if (stats != nullptr) {
      stats->cpu_encode_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(submit_start - encode_start).count();
      stats->cpu_submit_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(submit_end - submit_start).count();
      stats->encoder_count += 2;
      stats->command_buffer_count += 1;
      stats->queue_wait_count += 1;
    }
    if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
      if (error)
        *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                              : "Metal Tier1 indirect dispatch failed";
      return false;
    }
    return true;
  }
};

namespace {
// TASK-B15 (E002): a plan's module is either the linear (load/store/
// atomic_add) subset or the pointer-graph (load_ref/load_via/store_via)
// subset -- ir::verify() already rejects any other op, and neither
// build_*_compute_package() accepts the other's opcodes -- so this dispatch
// is exhaustive and mutually exclusive, never both true for one module.
bool is_pointer_graph_module(const ir::Module& module) {
  return std::any_of(module.instructions.begin(), module.instructions.end(), [](const ir::Instruction& i) {
    return i.op == "load_ref" || i.op == "load_via" || i.op == "store_via";
  });
}

bool has_host_assisted_pipeline(const hal::LoweringReport& report) {
  return std::any_of(report.events.begin(), report.events.end(), [](const hal::LoweringEvent& event) {
    return event.operation == "metal_pipeline" && event.classification == hal::LoweringClass::HostAssisted;
  });
}

bool module_touches_allocation(const ir::Module& module, uint64_t allocation) {
  return std::any_of(module.instructions.begin(), module.instructions.end(),
                     [allocation](const ir::Instruction& instruction) {
                       return instruction.allocation == allocation;
                     });
}

// Stage 5 consume releases the allocation's linear representation before
// Stage 6/7 dispatch (03 §7). A plan that also computes over that same
// allocation is asking for two incompatible things.
bool plan_computes_over_allocation(const hal::ExecutionPlan& plan, uint64_t allocation) {
  if (module_touches_allocation(plan.module, allocation)) return true;
  for (const auto& pass : plan.effect_dag_passes)
    if (module_touches_allocation(pass, allocation)) return true;
  return false;
}

// E004: shared by both the host-assisted and GPU-dispatch submit() paths.
// `touched` is derived from the module's static instruction list rather than
// the dispatch loop's runtime buffer bindings, so this works identically
// regardless of which path executed the submission.
void attach_access_certificate(const hal::CompiledPlan& compiled, const core::Arena& arena, hal::Submission* submission) {
  if (!submission->result.ok || !compiled.plan.requested_certificate_mode.has_value()) return;
  // TASK-D2 / ADR-036: a non-empty seed list already attached the
  // discovered (strict subset) certificate in run_discovery_stage.
  // build_access_certificate here would overwrite it with the B-era
  // full-arena DiscoverThenLease scan (ADR-025 / ADR-035).
  if (!compiled.plan.discovery_seeds.empty()) return;
  std::vector<core::PointerRef> touched;
  for (const auto& instruction : compiled.plan.module.instructions)
    touched.push_back(core::PointerRef{instruction.allocation, instruction.generation});
  core::AccessCertificate certificate;
  std::string cert_error;
  if (core::build_access_certificate(arena, *compiled.plan.requested_certificate_mode, touched, &certificate, &cert_error)) {
    submission->access_certificate = certificate;
    const auto classification = *compiled.plan.requested_certificate_mode == core::AccessCertificateMode::DiscoverThenLease
        ? hal::LoweringClass::HostAssisted : hal::LoweringClass::Direct;
    submission->report.add("access_certificate", classification, certificate.epoch.references().size(),
                           certificate.result_bytes, "Metal arena scan");
  }
}
}  // namespace

DeviceHal::DeviceHal(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DeviceHal::~DeviceHal() = default;

void DeviceHal::reclaim_released_backing(const core::Arena& arena) const {
  impl_->reclaim_released_backing(arena, facet_pool(), nullptr, nullptr);
}

const hal::CapabilitySnapshot& DeviceHal::capabilities() const { return impl_->snapshot.hal; }
const DeviceSnapshot& DeviceHal::snapshot() const { return impl_->snapshot; }

bool DeviceHal::compile(const hal::ExecutionPlan& plan, hal::CompiledPlan* compiled, std::string* error) {
  if (compiled == nullptr) {
    if (error) *error = "compiled plan output is required";
    return false;
  }
  if (!plan.validate(error)) return false;
  if (plan.requested_certificate_mode == core::AccessCertificateMode::SoftwarePaged ||
      plan.requested_certificate_mode == core::AccessCertificateMode::FaultManaged) {
    compiled->abi_version = hal::kDeviceHalAbiVersion;
    compiled->plan = plan;
    compiled->report = {};
    compiled->report.backend = hal::BackendKind::Metal;
    compiled->report.supported = false;
    compiled->report.diagnostic = "requested access certificate mode is not implemented on this backend";
    compiled->report.add("access_certificate", hal::LoweringClass::Unsupported, 1, 0, compiled->report.diagnostic);
    if (error) *error = compiled->report.diagnostic;
    return false;
  }
  // TASK-B15 (E002): pointer-graph modules get a dedicated compute package
  // and are reported CachedObject (ADR-028) -- their load_via/store_via
  // targets are bound by static index, never a real GPU-side pointer chase.
  const bool pointer_graph = is_pointer_graph_module(plan.module);
  // TASK-B16 (E007): request_indexed_binding is meaningless for a
  // pointer-graph module (mutually exclusive by construction: a
  // pointer-graph module's opcodes are never load/store-only, which
  // build_indexed_compute_package requires), so pointer_graph wins the
  // branch outright rather than needing a combined error path.
  const bool indexed_binding = !pointer_graph && plan.request_indexed_binding;
  if (indexed_binding && (!plan.effect_dag_passes.empty() || !plan.task_graph.tasks().empty())) {
    // Out of scope for this milestone -- mirrors ADR-026's "combining Tier1
    // with atomic_add is out of scope" precedent: E007 only needs a
    // standalone binding-cost comparison, not every possible combination.
    compiled->abi_version = hal::kDeviceHalAbiVersion;
    compiled->plan = plan;
    compiled->report = {};
    compiled->report.backend = hal::BackendKind::Metal;
    compiled->report.supported = false;
    compiled->report.diagnostic = "indexed binding combined with effect DAG passes or a task graph is out of scope (TASK-B16)";
    compiled->report.add("compute_package", hal::LoweringClass::Unsupported, 1, 0, compiled->report.diagnostic);
    if (error) *error = compiled->report.diagnostic;
    return false;
  }

  compiler::ComputePackageResult package;
  compiler::IndexedComputePackageResult indexed_package;
  if (indexed_binding) {
    indexed_package = compiler::build_indexed_compute_package(plan.module);
    if (!indexed_package.ok) { if (error) *error = indexed_package.message; return false; }
  } else {
    package = pointer_graph ? compiler::build_pointer_graph_compute_package(plan.module)
                           : compiler::build_linear_compute_package(plan.module);
    if (!package.ok) { if (error) *error = package.message; return false; }
  }

  compiled->abi_version = hal::kDeviceHalAbiVersion;
  compiled->plan = plan;
  compiled->report = {};
  compiled->report.backend = hal::BackendKind::Metal;
  if (indexed_binding) {
    // gpuAddress is queried lazily here rather than trusted from the
    // capability snapshot's own (currently unwired) gpu_addresses bit --
    // this milestone's honest-degradation result depends on a real,
    // just-in-time check of what this OS/device combination actually
    // supports, not a stale capability bit.
    if (!impl_->probe_gpu_addresses()) {
      compiled->report.supported = false;
      compiled->report.diagnostic = "indexed binding requires MTLBuffer.gpuAddress, unavailable on this OS/device";
      compiled->report.add("compute_package", hal::LoweringClass::Unsupported, 1,
                           indexed_package.package.referenced_allocations.size(), compiled->report.diagnostic);
      if (error) *error = compiled->report.diagnostic;
      return false;
    }
    compiled->indexed_compute_package = indexed_package.package;
    // Real device-pointer dereference through one argument-buffer-style
    // table -- Direct, not CachedObject, since every access genuinely goes
    // through a live GPU virtual address, not a statically-resolved index.
    // bytes reuses the existing binding-count convention: 1 (the table),
    // the direct contrast against build_linear_compute_package's N bindings.
    compiled->report.add("compute_package", hal::LoweringClass::Direct, 1, 1,
                         "MSL source generated by B16 (indexed table binding: " +
                         std::to_string(indexed_package.package.referenced_allocations.size()) +
                         " referenced allocations addressed through 1 argument-buffer-style gpuAddress table)");
  } else {
    compiled->compute_package = package.package;
    compiled->report.add("compute_package", pointer_graph ? hal::LoweringClass::CachedObject : hal::LoweringClass::Direct,
                         1, package.package.bindings.size(),
                         pointer_graph ? "MSL source generated by B15 (CachedObject: static index binding, no device pointer chase)"
                                       : "MSL source generated by B4");
  }

  // Stage 5's input (03 §7/§8: "每个 Region 的所需 facet 与 representation
  // version 已固定"). This backend has a real transform -- a linear ->
  // Private-optimal blit that publishes a new RepresentationEpoch (02 §8) --
  // so a request it can express is accepted here and actually performed by
  // submit(). What compile() must not do is perform it: a ConsumeInput is a
  // destructive act on the live arena, and compile() is handed no arena at
  // all. So a request carrying consume_input is recorded as work submit() will
  // do, never consumed at compile time, and never inferred by the adapter on
  // its own (06 §11: "adapter 不自行推断破坏性转换").
  //
  // A request whose shape this device cannot express is reported Unsupported
  // with the VG-concept reason and fails the compile, rather than being dropped
  // while compile() reports success as if none had been asked for (START.md §4
  // invariant 10).
  for (size_t index = 0; index < plan.representation_requests.size(); ++index) {
    const auto& request = plan.representation_requests[index];
    std::string request_error;
    if (!view_expressible(request.view, request.target_kind, request.view.byte_size(), &request_error)) {
      compiled->representation_supported = false;
      compiled->report.supported = false;
      compiled->report.diagnostic = "representation request " + std::to_string(index) +
                                    " is not expressible on this Metal device: " + request_error;
      compiled->report.add("representation_transform", hal::LoweringClass::Unsupported, 1,
                           request.view.byte_size(), compiled->report.diagnostic);
      if (error) *error = compiled->report.diagnostic;
      return false;
    }
    // Same refusal Vulkan's can_lower_representation_requests makes: ConsumeInput
    // releases the linear backing at once, and Stage 5 runs before this
    // submission's compute. Computing over that same allocation would dispatch
    // against a buffer whose bytes were just handed back (START.md §4, invariant 10).
    if (request.consume_input && plan_computes_over_allocation(plan, request.view.allocation)) {
      compiled->representation_supported = false;
      compiled->report.supported = false;
      compiled->report.diagnostic =
          "representation request " + std::to_string(index) +
          " is Unsupported: it asks for ConsumeInput on allocation " +
          std::to_string(request.view.allocation) +
          ", whose linear representation this plan's compute module also reads or writes; the "
          "consume releases that backing before the dispatch could run";
      compiled->report.add("consume_input", hal::LoweringClass::Unsupported, 1, 0,
                           compiled->report.diagnostic);
      if (error) *error = compiled->report.diagnostic;
      return false;
    }
    compiled->report.add("representation_transform", hal::LoweringClass::DevicePass, 1,
                         request.view.byte_size(),
                         "blit every subresource of the linear backing into a Private device-optimal "
                         "MTLTexture and publish a new RepresentationEpoch at submit()");
    if (request.consume_input)
      compiled->report.add("consume_input", hal::LoweringClass::Direct, 1, 0,
                           "recorded for submit(): the Private texture is storage distinct from the linear "
                           "backing it supersedes, so a complete ConsumeProof can release that backing at "
                           "once instead of holding it to command-buffer completion (06 §11)");
  }

  // A timeline wait/signal that this device cannot honor natively must be
  // rejected outright, not silently dropped -- MTLSharedEvent is the only
  // timeline mechanism this backend has (see Impl::timeline_event's doc
  // comment for why no separate host-side mirror exists to fall back to).
  if ((plan.timeline_wait != 0 || plan.timeline_signal != 0) && !impl_->snapshot.supports_shared_events) {
    compiled->report.supported = false;
    compiled->report.diagnostic = "timeline requested but device does not support MTLSharedEvent";
    compiled->report.add("timeline", hal::LoweringClass::Unsupported, 1, 0, compiled->report.diagnostic);
    if (error) *error = compiled->report.diagnostic;
    return false;
  }

  const bool has_atomic = std::any_of(plan.module.instructions.begin(), plan.module.instructions.end(),
                                      [](const ir::Instruction& i) { return i.op == "atomic_add"; });

  std::string pipeline_error;
  const bool pipeline_ok = indexed_binding
      ? impl_->ensure_pipeline(indexed_package.package.canonical_ir_hash, indexed_package.package.metal_source,
                              &pipeline_error, "vg_indexed_compute")
      : impl_->ensure_pipeline(package.package.canonical_ir_hash, package.package.metal_source, &pipeline_error,
                              pointer_graph ? "vg_pointer_graph_compute" : "vg_linear_compute");
  if (pipeline_ok) {
    compiled->report.supported = true;
    compiled->report.add("metal_pipeline", hal::LoweringClass::Direct, 1, 0, "MTLComputePipelineState compiled");
    if (!plan.task_graph.tasks().empty())
      compiled->report.add("task_publication", hal::LoweringClass::Direct, plan.task_graph.tasks().size(), 0,
                           "Metal task ring GPU publication kernel");
    if (plan.timeline_signal != 0)
      compiled->report.add("timeline", hal::LoweringClass::Direct, 1, 0, "MTLSharedEvent wait/signal");

    if (!plan.effect_dag_passes.empty()) {
      // TASK-B14 (E012): compile every pass independently (a fresh
      // build_linear_compute_package call each time, rather than trusting
      // `plan.module`/`package` above to be pass 0 by convention) and build
      // the EffectGraph from each pass's declared_effects plus
      // effect_dag_dependencies, mirroring TaskGraphBuilder's (before,
      // after) dependency-pair shape.
      core::EffectGraphBuilder builder;
      std::vector<compiler::ComputePackage> packages;
      bool passes_ok = true;
      std::string pass_error;
      for (const auto& pass : plan.effect_dag_passes) {
        const auto pass_package = compiler::build_linear_compute_package(pass);
        if (!pass_package.ok) { passes_ok = false; pass_error = pass_package.message; break; }
        std::string pipeline_error_for_pass;
        id<MTLComputePipelineState> pass_pipeline = nil;
        if (!impl_->ensure_effect_dag_pipeline(pass_package.package.canonical_ir_hash,
                                               pass_package.package.metal_source, &pass_pipeline,
                                               &pipeline_error_for_pass)) {
          passes_ok = false;
          pass_error = pipeline_error_for_pass;
          break;
        }
        packages.push_back(pass_package.package);
        builder.add_node(pass.declared_effects);
      }
      if (passes_ok) {
        for (const auto& dependency : plan.effect_dag_dependencies) {
          if (!builder.add_dependency(dependency.first, dependency.second, &pass_error)) { passes_ok = false; break; }
        }
      }
      core::EffectGraph graph;
      uint32_t node_count = 0;
      if (passes_ok && !builder.seal(&graph, &node_count, &pass_error)) passes_ok = false;

      if (!passes_ok) {
        compiled->report.supported = false;
        compiled->report.diagnostic = "effect DAG pass compilation failed: " + pass_error;
        compiled->report.add("effect_dag_lowering", hal::LoweringClass::Unsupported, 1, 0,
                             compiled->report.diagnostic);
        if (error) *error = compiled->report.diagnostic;
        return false;
      }

      const core::EffectGraphShape shape = core::classify_effect_graph_shape(graph, node_count);
      if (shape == core::EffectGraphShape::Unsupported) {
        // Honest scope-boundary result, not a guessed fence placement (ADR-027):
        // "cross queue", representation-transition, and external-present
        // shaped graphs are deliberately reported Unsupported here rather
        // than lowered with an unverified synchronization strategy.
        compiled->report.supported = false;
        compiled->report.diagnostic = "effect DAG graph shape is not one of the 3 in-scope shapes (ADR-027)";
        compiled->report.add("effect_dag_lowering", hal::LoweringClass::Unsupported, node_count, 0,
                             compiled->report.diagnostic);
        if (error) *error = compiled->report.diagnostic;
        return false;
      }

      compiled->effect_dag_packages = std::move(packages);
      compiled->effect_dag_graph = graph;
      compiled->effect_dag_node_count = node_count;
      compiled->effect_dag_shape = shape;
      const char* shape_name = shape == core::EffectGraphShape::LinearChain       ? "LinearChain"
                               : shape == core::EffectGraphShape::IndependentBranches ? "IndependentBranches"
                                                                                       : "ForkJoin";
      compiled->report.add("effect_dag_lowering", hal::LoweringClass::Direct, node_count, 0, shape_name);
    }
    return true;
  }

  if (has_atomic) {
    // Native 64-bit atomics aren't available on this GPU/driver/OS
    // combination. Never silently truncate to a 32-bit atomic -- fall back
    // to an explicit host round trip using the same byte semantics as the
    // reference oracle, and say so in the diagnostic. Task publication and
    // timeline signaling fall back to the same host path: the compute
    // kernel and the task ring kernel are separate pipelines, but once
    // submission already has to leave the GPU for correctness, routing
    // everything through the reference oracle is simpler than -- and
    // exactly as correct as -- keeping only the task ring on GPU.
    compiled->report.supported = true;
    compiled->report.add("metal_pipeline", hal::LoweringClass::HostAssisted, 1, 0,
                         "native 64-bit atomic compile failed, falling back to host execution: " + pipeline_error);
    if (!plan.task_graph.tasks().empty())
      compiled->report.add("task_publication", hal::LoweringClass::HostAssisted, plan.task_graph.tasks().size(), 0,
                           "host-assisted fallback: reference task graph executor");
    if (plan.timeline_signal != 0)
      compiled->report.add("timeline", hal::LoweringClass::HostAssisted, 1, 0,
                           "host-assisted fallback: MTLSharedEvent.signaledValue set from host");
    return true;
  }

  compiled->report.supported = false;
  compiled->report.diagnostic = "Metal pipeline compilation failed: " + pipeline_error;
  compiled->report.add("metal_pipeline", hal::LoweringClass::Unsupported, 1, 0, pipeline_error);
  if (error) *error = compiled->report.diagnostic;
  return false;
}

bool DeviceHal::submit(const hal::CompiledPlan& compiled, core::Arena& arena, hal::Submission* submission,
                       std::string* error) {
  if (submission == nullptr) { if (error) *error = "submission output is required"; return false; }
  if (!compiled.report.supported) { if (error) *error = "compiled plan is unsupported"; return false; }
  if (!compiled.compute_package.has_value() && !compiled.indexed_compute_package.has_value()) {
    if (error) *error = "compiled plan has no compute package";
    return false;
  }
  if (!compiled.plan.graph_epoch_matches(arena, error)) return false;

  submission->abi_version = hal::kDeviceHalAbiVersion;
  submission->report = compiled.report;
  // TASK-D2 / ADR-036 then TASK-D3 / ADR-037: discovery (if seeds are set)
  // then this-submit residency. Empty seeds / unset budget are no-ops.
  if (!hal::run_discovery_stage(compiled.plan, arena, submission, error)) return false;
  if (!hal::apply_working_set_budget(compiled.plan, arena, submission, error)) return false;

  // Stage 5 precedes Stage 6/7 (03 §7): the Region representations a
  // submission depends on are published, and the facets that depend on them
  // acquired, before any dispatch encodes against them. The epoch/facet/consume
  // bookkeeping itself is the shared helper's -- if each backend kept its own,
  // the two could disagree about when a token goes stale and the cross-backend
  // differential of 10 §6 would be comparing two different semantics. Only the
  // physical step below is this backend's, and it is a real device pass.
  //
  // distinct_backing is true because it is true: the Private texture is storage
  // independent of the linear allocation it supersedes, so releasing that
  // backing under a ConsumeInput really does hand memory back (the reference
  // backend reports false for the opposite, equally honest reason -- its
  // transform is the identity). This flag is the whole Metal-side obligation
  // ConsumeInput has, besides genuinely not needing the old linear bytes once
  // the blit has completed, which it does not.
  //
  // A plan carrying no request leaves every Stage 5 field of `submission` at
  // its default and costs nothing, so every pre-Stage-5 caller is unaffected.
  {
    std::string representation_error;
    if (!hal::run_representation_stage(
            compiled.plan.representation_requests, arena, facet_pool(),
            [&](const hal::RepresentationRequest& request, core::FacetRef facet,
                hal::RepresentationTransformCost* cost, std::string* physical_error) {
              Impl::TransformCost transform_cost;
              if (!impl_->transform_into_private_facet(arena, facet_pool(), request.view,
                                                       request.target_kind, facet, &transform_cost,
                                                       physical_error))
                return false;
              cost->new_backing_bytes = transform_cost.new_backing_bytes;
              cost->temporary_bytes = transform_cost.temporary_bytes;
              // No MTLHeap is involved on this path -- the texture comes
              // straight from the device -- so there is no fragmentation this
              // backend can observe, and 0 is the measurement rather than a
              // placeholder.
              cost->heap_fragmentation_bytes = 0;
              cost->used_device_optimal = true;
              cost->distinct_backing = true;
              return true;
            },
            submission, &representation_error)) {
      if (error) *error = representation_error;
      return false;
    }
    // The helper owns the host-side consume; this backend must actually drop
    // the Shared blit source and any MTLTexture whose FacetRef no longer
    // resolves, or released_backing_bytes would describe a saving that never
    // happened on the device (06 §11, Vulkan 07 §14's same follow-through).
    uint32_t retired_textures = 0;
    uint64_t released_linear = 0;
    impl_->reclaim_released_backing(arena, facet_pool(), &retired_textures, &released_linear);
    if (retired_textures != 0) {
      submission->report.add("facet_texture_retire", hal::LoweringClass::Direct, retired_textures, 0,
                             "MTLTextures belonging to retired facet slots or superseded "
                             "RepresentationEpochs destroyed after the stage");
    }
    if (released_linear != 0) {
      submission->report.add("consume_input_backing_release", hal::LoweringClass::Direct, 1, released_linear,
                             "the superseded linear representation's device buffer was destroyed at once "
                             "rather than retained to command-buffer completion (06 §11, E005)");
    }
  }

  const uint64_t wait_value = compiled.plan.timeline_wait;
  const uint64_t signal_value = compiled.plan.timeline_signal;
  // Pre-checked host-side, exactly like reference::execute()'s
  // Timeline::validate_wait/signal: this backend fails fast on an
  // unsatisfied wait or a non-monotonic signal rather than letting the GPU
  // block forever on encodeWaitForEvent: for a value nothing will ever
  // reach in these single-command-buffer submissions.
  if (wait_value != 0 || signal_value != 0) {
    std::string timeline_error;
    if (!impl_->ensure_timeline_event(&timeline_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = timeline_error;
      submission->result.fault.code = "TIMELINE_UNAVAILABLE";
      submission->result.fault.message = timeline_error;
      return true;
    }
    const uint64_t current = impl_->timeline_event.signaledValue;
    if (wait_value != 0 && current < wait_value) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = "timeline wait point is unsatisfied";
      submission->result.fault.code = "TIMELINE_WAIT_UNSATISFIED";
      submission->result.fault.message = submission->result.message;
      return true;
    }
    if (signal_value != 0 && signal_value <= current) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = "timeline signal must be strictly monotonic";
      submission->result.fault.code = "TIMELINE_SIGNAL_NOT_MONOTONIC";
      submission->result.fault.message = submission->result.message;
      return true;
    }
  }

  if (has_host_assisted_pipeline(compiled.report)) {
    const auto host_start = std::chrono::steady_clock::now();
    submission->result = reference::execute(
        compiled.plan.module, arena, compiled.plan.certificate.ranges.empty() ? nullptr : &compiled.plan.certificate);
    if (submission->result.ok && signal_value != 0) impl_->timeline_event.signaledValue = signal_value;
    submission->timeline_value = impl_->timeline_event != nil ? impl_->timeline_event.signaledValue : 0;
    if (submission->result.ok && !compiled.plan.task_graph.tasks().empty()) {
      std::vector<uint32_t> order;
      if (!hal::apply_envelope_continuation(compiled.plan, &envelope_continuations_, submission, &order,
                                            error))
        return false;
      std::string publish_error;
      if (!publish_envelope_order(compiled.plan.task_graph, order, &submission->published_tasks,
                                  &publish_error)) {
        submission->result.ok = false;
        submission->result.message = publish_error;
      }
    }
    const auto host_end = std::chrono::steady_clock::now();
    // No GPU dispatch happens on this path -- it's a host-assisted fallback
    // (see compile()'s "native 64-bit atomic compile failed" branch), so all
    // of the wall-clock time is attributed to cpu_submit_ns and none to
    // cpu_encode_ns/the encoder-and-command-buffer counters, which stay 0.
    submission->cpu_submit_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(host_end - host_start).count();
    attach_access_certificate(compiled, arena, submission);
    return true;
  }

  if (!compiled.plan.certificate.ranges.empty()) {
    const auto verification = ir::verify(compiled.plan.module);
    for (const auto& effect : verification.inferred_effects) {
      if (!compiled.plan.certificate.covers(effect)) {
        submission->result.ok = false;
        submission->result.poison = core::PoisonState::Poisoned;
        submission->result.message = "certificate does not cover inferred effect";
        submission->result.missing_effects.push_back(effect);
        return true;
      }
    }
  }

  if (!compiled.plan.effect_dag_passes.empty()) {
    // TASK-B14 (E012): entirely bypasses the single-module buffer-bind +
    // dispatch_and_wait path below -- plan.module is only pass 0 by
    // convention (kept solely so ExecutionPlan::validate()'s ir::verify()
    // call has something to check), so dispatching it separately here would
    // double-dispatch pass 0's work. task_graph combined with
    // effect_dag_passes is out of scope for this milestone (mirrors ADR-026's
    // "combining Tier1 with atomic_add is out of scope" precedent).
    if (compiled.effect_dag_shape == core::EffectGraphShape::Unsupported ||
        compiled.effect_dag_packages.size() != compiled.plan.effect_dag_passes.size()) {
      submission->result.ok = false;
      submission->result.message = "compiled plan has no usable effect DAG lowering";
      return true;
    }

    std::vector<id<MTLComputePipelineState>> pass_pipelines;
    std::vector<std::vector<id<MTLBuffer>>> pass_buffers;
    std::map<uint64_t, core::Allocation*> touched_by_id;
    std::map<uint64_t, id<MTLBuffer>> buffer_by_allocation_id;

    for (size_t pass_index = 0; pass_index < compiled.plan.effect_dag_passes.size(); ++pass_index) {
      const auto& pass_module = compiled.plan.effect_dag_passes[pass_index];
      const auto& pass_package = compiled.effect_dag_packages[pass_index];

      std::string pipeline_error;
      id<MTLComputePipelineState> pass_pipeline = nil;
      if (!impl_->ensure_effect_dag_pipeline(pass_package.canonical_ir_hash, pass_package.metal_source,
                                             &pass_pipeline, &pipeline_error)) {
        submission->result.ok = false;
        submission->result.message = "Metal effect DAG pipeline compile failed: " + pipeline_error;
        return true;
      }
      pass_pipelines.push_back(pass_pipeline);

      std::map<uint64_t, std::pair<uint32_t, uint32_t>> pass_generation_by_allocation;
      for (const auto& instruction : pass_module.instructions)
        pass_generation_by_allocation.emplace(
            instruction.allocation, std::make_pair(instruction.generation, instruction.representation_epoch));

      std::vector<id<MTLBuffer>> buffers_for_pass;
      for (const auto& binding : pass_package.bindings) {
        auto it = pass_generation_by_allocation.find(binding.allocation);
        core::Allocation* allocation = it == pass_generation_by_allocation.end()
            ? nullptr
            : arena.lookup(binding.allocation, it->second.first, it->second.second);
        if (allocation == nullptr) {
          submission->result.ok = false;
          submission->result.poison = core::PoisonState::Poisoned;
          submission->result.message = "stale generation, representation epoch, or out-of-bounds allocation reference";
          return true;
        }
        id<MTLBuffer> buffer = impl_->ensure_buffer(*allocation);
        if (buffer == nil) {
          submission->result.ok = false;
          submission->result.message = "Metal buffer allocation failed";
          return true;
        }
        buffers_for_pass.push_back(buffer);
        touched_by_id.emplace(allocation->id, allocation);
        buffer_by_allocation_id[allocation->id] = buffer;
      }
      pass_buffers.push_back(std::move(buffers_for_pass));
    }

    DispatchStats stats;
    std::string dispatch_error;
    if (!impl_->dispatch_effect_dag(pass_pipelines, pass_buffers, compiled.effect_dag_shape,
                                    compiled.effect_dag_graph, compiled.effect_dag_node_count, &stats,
                                    &dispatch_error)) {
      submission->result.ok = false;
      submission->result.message = "Metal effect DAG dispatch failed: " + dispatch_error;
      return true;
    }
    submission->timeline_value = impl_->timeline_event != nil ? impl_->timeline_event.signaledValue : 0;

    // Shared storage + dispatch_effect_dag's synchronous waitUntilCompleted
    // mean it is safe to read every touched allocation's bytes back now, once,
    // after all passes have run -- a shared allocation that appears in
    // multiple passes (the ForkJoin conflict case) is deduped by id here so
    // it is only copied back once.
    for (const auto& entry : touched_by_id) {
      id<MTLBuffer> buffer = buffer_by_allocation_id[entry.first];
      std::memcpy(entry.second->bytes.data(), [buffer contents], entry.second->bytes.size());
    }

    uint32_t witness_index = 0;
    for (const auto& pass_module : compiled.plan.effect_dag_passes) {
      for (const auto& instruction : pass_module.instructions) {
        const ir::Access access = instruction.op == "load"       ? ir::Access::Read
                                  : instruction.op == "store"     ? ir::Access::Write
                                  : instruction.op == "atomic_add" ? ir::Access::Atomic
                                                                    : ir::Access::Publish;
        const ir::Effect effect{instruction.allocation, instruction.offset, instruction.size, access,
                                instruction.representation_epoch};
        submission->result.trace.push_back(effect);
        submission->result.witness.record(effect, witness_index++);
      }
    }
    submission->result.ok = true;
    submission->result.poison = core::PoisonState::Valid;
    submission->cpu_encode_ns = stats.cpu_encode_ns;
    submission->cpu_submit_ns = stats.cpu_submit_ns;
    submission->report.encoder_count = stats.encoder_count;
    submission->report.command_buffer_count = stats.command_buffer_count;
    submission->report.barrier_count = stats.barrier_count;
    submission->report.queue_wait_count = stats.queue_wait_count;
    attach_access_certificate(compiled, arena, submission);
    return true;
  }

  if (compiled.indexed_compute_package.has_value()) {
    // TASK-B16 (E007): self-contained -- guaranteed no task_graph/effect_dag
    // interaction to handle here, since compile() rejected that combination
    // outright (mirrors the effect_dag branch above's own early return).
    const auto& indexed_package = *compiled.indexed_compute_package;
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> generation_by_allocation;
    for (const auto& instruction : compiled.plan.module.instructions)
      generation_by_allocation.emplace(instruction.allocation,
                                       std::make_pair(instruction.generation, instruction.representation_epoch));

    std::vector<id<MTLBuffer>> object_buffers;
    std::vector<core::Allocation*> touched;
    for (uint64_t allocation_id : indexed_package.referenced_allocations) {
      auto it = generation_by_allocation.find(allocation_id);
      core::Allocation* allocation = it == generation_by_allocation.end()
          ? nullptr
          : arena.lookup(allocation_id, it->second.first, it->second.second);
      if (allocation == nullptr) {
        submission->result.ok = false;
        submission->result.poison = core::PoisonState::Poisoned;
        submission->result.message = "stale generation, representation epoch, or out-of-bounds allocation reference";
        return true;
      }
      id<MTLBuffer> buffer = impl_->ensure_buffer(*allocation);
      if (buffer == nil) {
        submission->result.ok = false;
        submission->result.message = "Metal buffer allocation failed";
        return true;
      }
      object_buffers.push_back(buffer);
      touched.push_back(allocation);
    }

    DispatchStats stats;
    std::string dispatch_error;
    if (!impl_->dispatch_indexed_and_wait(object_buffers, wait_value, signal_value, &stats, &dispatch_error)) {
      submission->result.ok = false;
      submission->result.message = "Metal indexed dispatch failed: " + dispatch_error;
      return true;
    }
    submission->timeline_value = impl_->timeline_event != nil ? impl_->timeline_event.signaledValue : 0;

    for (size_t index = 0; index < touched.size(); ++index)
      std::memcpy(touched[index]->bytes.data(), [object_buffers[index] contents], touched[index]->bytes.size());

    for (size_t index = 0; index < compiled.plan.module.instructions.size(); ++index) {
      const auto& instruction = compiled.plan.module.instructions[index];
      const ir::Access access = instruction.op == "load" ? ir::Access::Read : ir::Access::Write;
      const ir::Effect effect{instruction.allocation, instruction.offset, instruction.size, access,
                              instruction.representation_epoch};
      submission->result.trace.push_back(effect);
      submission->result.witness.record(effect, static_cast<uint32_t>(index));
    }
    submission->result.ok = true;
    submission->result.poison = core::PoisonState::Valid;
    submission->cpu_encode_ns = stats.cpu_encode_ns;
    submission->cpu_submit_ns = stats.cpu_submit_ns;
    submission->report.encoder_count = stats.encoder_count;
    submission->report.command_buffer_count = stats.command_buffer_count;
    submission->report.barrier_count = stats.barrier_count;
    submission->report.queue_wait_count = stats.queue_wait_count;
    attach_access_certificate(compiled, arena, submission);
    return true;
  }

  const auto& package = *compiled.compute_package;
  std::map<uint64_t, std::pair<uint32_t, uint32_t>> generation_by_allocation;
  for (const auto& instruction : compiled.plan.module.instructions)
    generation_by_allocation.emplace(instruction.allocation,
                                     std::make_pair(instruction.generation, instruction.representation_epoch));

  std::vector<id<MTLBuffer>> buffers;
  std::vector<core::Allocation*> touched;
  for (const auto& binding : package.bindings) {
    auto it = generation_by_allocation.find(binding.allocation);
    core::Allocation* allocation = it == generation_by_allocation.end()
        ? nullptr
        : arena.lookup(binding.allocation, it->second.first, it->second.second);
    if (allocation == nullptr) {
      submission->result.ok = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = "stale generation, representation epoch, or out-of-bounds allocation reference";
      return true;
    }
    id<MTLBuffer> buffer = impl_->ensure_buffer(*allocation);
    if (buffer == nil) {
      submission->result.ok = false;
      submission->result.message = "Metal buffer allocation failed";
      return true;
    }
    buffers.push_back(buffer);
    touched.push_back(allocation);
  }

  DispatchStats stats;
  std::string dispatch_error;
  if (!impl_->dispatch_and_wait(buffers, {}, wait_value, signal_value, &stats, &dispatch_error)) {
    submission->result.ok = false;
    submission->result.message = "Metal dispatch failed: " + dispatch_error;
    return true;
  }
  submission->timeline_value = impl_->timeline_event != nil ? impl_->timeline_event.signaledValue : 0;

  for (size_t index = 0; index < touched.size(); ++index)
    std::memcpy(touched[index]->bytes.data(), [buffers[index] contents], touched[index]->bytes.size());

  for (size_t index = 0; index < compiled.plan.module.instructions.size(); ++index) {
    const auto& instruction = compiled.plan.module.instructions[index];
    const ir::Access access = instruction.op == "load"       ? ir::Access::Read
                              : instruction.op == "store"     ? ir::Access::Write
                              : instruction.op == "atomic_add" ? ir::Access::Atomic
                              : instruction.op == "load_ref"   ? ir::Access::Read
                              : instruction.op == "load_via"   ? ir::Access::Read
                              : instruction.op == "store_via"  ? ir::Access::Write
                                                                : ir::Access::Publish;
    const ir::Effect effect{instruction.allocation, instruction.offset, instruction.size, access,
                            instruction.representation_epoch};
    submission->result.trace.push_back(effect);
    submission->result.witness.record(effect, static_cast<uint32_t>(index));
  }
  submission->result.ok = true;
  submission->result.poison = core::PoisonState::Valid;

  if (!compiled.plan.task_graph.tasks().empty()) {
    // TASK-D5 / ADR-039: apply the envelope window first. Unset quota keeps
    // the pre-D5 full GPU publish. A set cap that does not cover the graph
    // (or a leftover drain) is HostAssisted -- pack/dispatch of the full
    // graph would still write parked tasks because gid == packed slot.
    std::vector<uint32_t> order;
    if (!hal::apply_envelope_continuation(compiled.plan, &envelope_continuations_, submission, &order,
                                          error))
      return false;
    const auto& tasks = compiled.plan.task_graph.tasks();
    const bool host_split =
        order.size() != tasks.size() || submission->envelope_overflow.has_value();
    if (order.empty()) {
      // quota 0, or an empty leftover drain: publish nothing this submit.
    } else if (host_split) {
      std::string publish_error;
      if (!publish_envelope_order(compiled.plan.task_graph, order, &submission->published_tasks,
                                  &publish_error)) {
        submission->result.ok = false;
        submission->result.message = publish_error;
        return true;
      }
    } else {
    // Pack -> dispatch the GPU publish kernel -> read back -> verify every
    // slot reached Published -> unpack, walking slots in the task graph's
    // deterministic dependency order so submission->published_tasks is
    // sequence-identical to reference::execute_task_graph()'s oracle output.
    // Each task writes only its own disjoint slot (gid == its own task
    // index, not a separately-assigned ring position), so the GPU's actual
    // parallel completion order across slots has no effect on correctness
    // -- only the final per-slot state and the host-chosen readback order
    // do. dispatch_task_publish is fully synchronous (waitUntilCompleted),
    // so the Shared-storage buffers are already safe to read from the host
    // by the time it returns; no additional synchronization is needed.
    const uint32_t count = static_cast<uint32_t>(tasks.size());

    id<MTLBuffer> state_buffer = [impl_->device newBufferWithLength:std::max<size_t>(count * sizeof(uint32_t), 1)
                                                              options:MTLResourceStorageModeShared];
    id<MTLBuffer> fields_buffer =
        [impl_->device newBufferWithLength:std::max<size_t>(count * kTaskRingWordsPerRecord * sizeof(uint32_t), 1)
                                    options:MTLResourceStorageModeShared];
    id<MTLBuffer> inputs_buffer =
        [impl_->device newBufferWithLength:std::max<size_t>(count * kTaskRingWordsPerRecord * sizeof(uint32_t), 1)
                                    options:MTLResourceStorageModeShared];
    if (state_buffer == nil || fields_buffer == nil || inputs_buffer == nil) {
      submission->result.ok = false;
      submission->result.message = "Metal task ring buffer allocation failed";
      return true;
    }
    std::memset([state_buffer contents], 0, count * sizeof(uint32_t));
    uint32_t* inputs = static_cast<uint32_t*>([inputs_buffer contents]);
    for (uint32_t i = 0; i < count; ++i) pack_task_record(tasks[i], inputs + i * kTaskRingWordsPerRecord);

    std::string task_pipeline_error;
    if (!impl_->ensure_task_ring_pipeline(&task_pipeline_error)) {
      submission->result.ok = false;
      submission->result.message = "Metal task ring pipeline compile failed: " + task_pipeline_error;
      return true;
    }
    std::string publish_error;
    if (!impl_->dispatch_task_publish(state_buffer, fields_buffer, inputs_buffer, count, &stats, &publish_error)) {
      submission->result.ok = false;
      submission->result.message = "Metal task ring dispatch failed: " + publish_error;
      return true;
    }

    const uint32_t* states = static_cast<const uint32_t*>([state_buffer contents]);
    const uint32_t* fields = static_cast<const uint32_t*>([fields_buffer contents]);
    submission->published_tasks.reserve(count);
    for (uint32_t index : order) {
      if (states[index] != static_cast<uint32_t>(core::PublicationState::Published)) {
        submission->result.ok = false;
        submission->result.message = "task ring slot did not reach Published state";
        return true;
      }
      submission->published_tasks.push_back(unpack_task_record(fields + index * kTaskRingWordsPerRecord));
    }

    impl_->last_tier1_indirect_dims.clear();
    if (compiled.plan.request_tier1_indirect && capabilities().supports(hal::Capability::IndirectTier1)) {
      id<MTLBuffer> indirect_args_buffer = [impl_->device
          newBufferWithLength:std::max<size_t>(count * sizeof(MTLDispatchThreadgroupsIndirectArguments), 1)
                       options:MTLResourceStorageModeShared];
      if (indirect_args_buffer == nil) {
        submission->result.ok = false;
        submission->result.message = "Metal Tier1 indirect-args buffer allocation failed";
        return true;
      }
      std::string tier1_error;
      if (!impl_->dispatch_task_tier1_indirect(buffers, fields_buffer, order, indirect_args_buffer, &stats,
                                               &tier1_error)) {
        submission->result.ok = false;
        submission->result.message = "Metal Tier1 indirect dispatch failed: " + tier1_error;
        return true;
      }
      const uint32_t* args = static_cast<const uint32_t*>([indirect_args_buffer contents]);
      const size_t stride_words = sizeof(MTLDispatchThreadgroupsIndirectArguments) / sizeof(uint32_t);
      impl_->last_tier1_indirect_dims.reserve(order.size());
      for (size_t i = 0; i < order.size(); ++i) {
        const uint32_t* dims = args + i * stride_words;
        impl_->last_tier1_indirect_dims.push_back({dims[0], dims[1], dims[2]});
      }
      submission->report.add("tier1_indirect_dispatch", hal::LoweringClass::Direct, order.size(), 0,
                             "GPU-authored indirect dispatch dims, no host round trip before dispatch");
    }
    // TASK-D4 (E010): isolated behind request_tier2_select so D2/D3/D5
    // submit prologue work does not collide. Implementation lives in
    // metal_tier2.mm -- ICB is not required; default is bucket + per-Node
    // indirect classified EmulatedDevicePass.
    if (compiled.plan.request_tier2_select) {
      std::string tier2_error;
      if (!vg::metal::tier2::apply_select(static_cast<void*>(impl_->device),
                                          static_cast<void*>(impl_->command_queue),
                                          static_cast<void*>(fields_buffer), count, compiled.plan,
                                          submission, &stats.encoder_count, &stats.command_buffer_count,
                                          &stats.queue_wait_count, &tier2_error)) {
        submission->result.ok = false;
        submission->result.message = tier2_error;
        return true;
      }
    }
    }
  }
  submission->cpu_encode_ns = stats.cpu_encode_ns;
  submission->cpu_submit_ns = stats.cpu_submit_ns;
  submission->report.encoder_count = stats.encoder_count;
  submission->report.command_buffer_count = stats.command_buffer_count;
  submission->report.barrier_count = stats.barrier_count;
  submission->report.queue_wait_count = stats.queue_wait_count;
  attach_access_certificate(compiled, arena, submission);
  return true;
}

bool DeviceHal::probe_buffer(size_t length, bool private_storage, BufferSnapshot* result,
                             std::string* error) const {
  if (result == nullptr) {
    if (error) *error = "buffer snapshot output is required";
    return false;
  }
  if (length == 0 || length > [impl_->device maxBufferLength]) {
    if (error) *error = "requested Metal buffer length is outside device limits";
    return false;
  }
  const MTLResourceOptions options = private_storage ? MTLResourceStorageModePrivate
                                                      : MTLResourceStorageModeShared;
  id<MTLBuffer> buffer = [impl_->device newBufferWithLength:length options:options];
  if (buffer == nil) {
    if (error) *error = "Metal rejected buffer allocation";
    return false;
  }
  result->requested_length = length;
  result->allocated_length = [buffer length];
  result->storage_mode = static_cast<uint32_t>([buffer resourceOptions] & MTLResourceStorageModeMask);
  result->gpu_address_available = [buffer respondsToSelector:@selector(gpuAddress)];
  result->gpu_address = result->gpu_address_available ? [buffer gpuAddress] : 0;
  return true;
}

bool DeviceHal::run_cull_compact(const std::vector<uint32_t>& instance_visible,
                                 const std::vector<uint32_t>& instance_ids, CullCompactResult* result,
                                 std::string* error) const {
  if (result == nullptr) { if (error) *error = "cull/compact result output is required"; return false; }
  if (instance_visible.size() != instance_ids.size()) {
    if (error) *error = "instance_visible and instance_ids must be the same size";
    return false;
  }
  const uint32_t count = static_cast<uint32_t>(instance_visible.size());
  std::string pipeline_error;
  if (!impl_->ensure_cull_compact_pipeline(&pipeline_error)) {
    if (error) *error = "Metal cull/compact pipeline compile failed: " + pipeline_error;
    return false;
  }

  id<MTLBuffer> visible_buffer = [impl_->device newBufferWithLength:std::max<size_t>(count * sizeof(uint32_t), 1)
                                                             options:MTLResourceStorageModeShared];
  id<MTLBuffer> ids_buffer = [impl_->device newBufferWithLength:std::max<size_t>(count * sizeof(uint32_t), 1)
                                                        options:MTLResourceStorageModeShared];
  id<MTLBuffer> count_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t)
                                                           options:MTLResourceStorageModeShared];
  id<MTLBuffer> compact_buffer = [impl_->device newBufferWithLength:std::max<size_t>(count * sizeof(uint32_t), 1)
                                                             options:MTLResourceStorageModeShared];
  if (visible_buffer == nil || ids_buffer == nil || count_buffer == nil || compact_buffer == nil) {
    if (error) *error = "Metal cull/compact buffer allocation failed";
    return false;
  }
  if (!instance_visible.empty())
    std::memcpy([visible_buffer contents], instance_visible.data(), instance_visible.size() * sizeof(uint32_t));
  if (!instance_ids.empty())
    std::memcpy([ids_buffer contents], instance_ids.data(), instance_ids.size() * sizeof(uint32_t));
  std::memset([count_buffer contents], 0, sizeof(uint32_t));

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  [encoder setComputePipelineState:impl_->cull_compact_pipeline];
  [encoder setBuffer:visible_buffer offset:0 atIndex:0];
  [encoder setBuffer:ids_buffer offset:0 atIndex:1];
  [encoder setBuffer:count_buffer offset:0 atIndex:2];
  [encoder setBuffer:compact_buffer offset:0 atIndex:3];
  [encoder dispatchThreadgroups:MTLSizeMake(std::max<uint32_t>(count, 1), 1, 1)
          threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal cull/compact dispatch failed";
    return false;
  }

  const uint32_t visible_count = *static_cast<const uint32_t*>([count_buffer contents]);
  const uint32_t* compact = static_cast<const uint32_t*>([compact_buffer contents]);
  result->visible_count = visible_count;
  result->compact_ids.assign(compact, compact + std::min(visible_count, count));
  return true;
}


bool DeviceHal::run_address_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                  AddressFacetResult* result, std::string* error) const {
  if (result == nullptr) { if (error) *error = "address facet result output is required"; return false; }
  FacetUseGuard use(pool, ref);
  if (!use.begin(arena, error)) return false;
  id<MTLBuffer> buffer = impl_->ensure_facet_buffer(arena, pool, ref, core::FacetKind::Address, error);
  if (buffer == nil) return false;

  result->report = make_facet_report();
  result->byte_size = [buffer length];
  result->gpu_address_available = [buffer respondsToSelector:@selector(gpuAddress)];
  result->gpu_address = result->gpu_address_available ? [buffer gpuAddress] : 0;
  result->report.add("address_facet", result->gpu_address_available ? hal::LoweringClass::Direct
                                                                    : hal::LoweringClass::CachedObject,
                     1, result->byte_size,
                     result->gpu_address_available
                         ? "linear device address; no texture object created"
                         : "device exposes no gpuAddress selector, buffer binding only");
  return true;
}

bool DeviceHal::run_sample_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                 core::FilterMode filter, core::WrapMode wrap,
                                 const std::vector<std::array<float, 2>>& uv_coords,
                                 SampleFacetResult* result, std::string* error) const {
  std::vector<SampleCoord> coords;
  coords.reserve(uv_coords.size());
  for (const auto& uv : uv_coords) coords.push_back(SampleCoord{uv[0], uv[1], 0.0f, 0});
  // FastNative, not CheckedNative: this overload names no subresource and no
  // profile, so it means exactly what it meant before the guard existed --
  // level 0 of slice 0, sampled by a pipeline with the guard specialized away,
  // with FacetPool::lookup() as the authority on the token's liveness.
  return run_sample_facet(arena, pool, ref, filter, wrap, coords, core::ValidationProfile::FastNative,
                          result, error);
}

bool DeviceHal::run_sample_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                 core::FilterMode filter, core::WrapMode wrap,
                                 const std::vector<SampleCoord>& coords, core::ValidationProfile profile,
                                 SampleFacetResult* result, std::string* error) const {
  if (result == nullptr) { if (error) *error = "sample facet result output is required"; return false; }
  // 03 §12's profiles change diagnosis and instrumentation, never meaning --
  // but only two of the four have a Metal meaning at all. ReferenceStrict asks
  // for the reference interpreter's own byte-exact judgement and Capture for a
  // canonical capture stream; this adapter is neither, and silently running one
  // of them as CheckedNative would let a caller believe a guarantee it never
  // got (START.md §4 invariant 10).
  if (profile != core::ValidationProfile::CheckedNative &&
      profile != core::ValidationProfile::FastNative) {
    if (error)
      *error = "Unsupported: this Metal adapter implements the CheckedNative and FastNative profiles; "
               "ReferenceStrict and Capture have no Metal lowering and must run on the reference backend";
    return false;
  }
  const bool checked = profile == core::ValidationProfile::CheckedNative;
  const uint32_t count = static_cast<uint32_t>(coords.size());

  // The generation table is a snapshot of what the pool currently resolves
  // (core::FacetPool::snapshot_generations), which is exactly what the kernel's
  // guard compares against. Taken before anything else so the host-side verdict
  // below and the in-shader verdict are formed from the same state.
  std::vector<uint32_t> generations;
  pool.snapshot_generations(&generations);

  std::string lookup_error;
  const core::FacetSlot* slot = impl_->resolve_facet(arena, pool, ref, core::FacetKind::Sample, &lookup_error);

  // A token the host cannot resolve, whose failure the uploaded table *can*
  // encode (a retired or generation-mismatched slot), is precisely the case
  // 06 §6.4's in-shader check exists for: under the checked profile the shader
  // itself rejects it, writes poison and counts the violation, rather than the
  // host inferring the rejection on its behalf. Nothing of the dead facet is
  // resurrected to make that happen -- a 1x1 placeholder is bound purely to
  // satisfy MSL's texture argument, and the guard returns before any sample.
  //
  // A token whose failure the table cannot encode (an epoch that went stale
  // after the snapshot, or a lost allocation) is refused host-side with that
  // reason, because pretending the guard caught it would misreport which check
  // actually fired.
  const bool guard_rejects = checked && slot == nullptr && !pool.generation_valid(ref);
  if (slot == nullptr && !guard_rejects) {
    if (error)
      *error = checked ? lookup_error +
                             " (the checked-profile generation table encodes slot liveness only, so this "
                             "staleness is caught host-side rather than in the shader)"
                       : lookup_error;
    return false;
  }

  const bool array_dimension = slot != nullptr && slot->view.dimension == core::ViewDimension::Texture2DArray;
  if (slot != nullptr) {
    for (uint32_t i = 0; i < count; ++i) {
      // Neither an out-of-range slice nor an out-of-range level is clamped:
      // clamping would turn a caller's indexing bug into a plausible-looking
      // sampled value, and the reference oracle refuses the same coordinates
      // for the same reason.
      if (coords[i].array_slice >= slot->view.array_layers) {
        if (error)
          *error = "sample coordinate " + std::to_string(i) + " names array slice " +
                   std::to_string(coords[i].array_slice) + " of a canonical view declaring " +
                   std::to_string(slot->view.array_layers) + " layer(s)";
        return false;
      }
      // Mixing the two kernels is Unsupported rather than approximated: a
      // Texture2D view has no slice axis, so a non-zero slice on one is a
      // contract error, not something the texture2d_array kernel should be
      // substituted in to satisfy.
      if (!array_dimension && coords[i].array_slice != 0) {
        if (error)
          *error = "Unsupported: sample coordinate " + std::to_string(i) +
                   " names a non-zero array slice on a Texture2D canonical view; declare the view as "
                   "Texture2DArray instead of relying on the array sampling kernel";
        return false;
      }
      if (!(coords[i].lod >= 0.0f) || coords[i].lod > static_cast<float>(slot->view.mip_levels - 1)) {
        if (error)
          *error = "sample coordinate " + std::to_string(i) + " names lod " +
                   std::to_string(coords[i].lod) + " of a canonical view declaring " +
                   std::to_string(slot->view.mip_levels) + " mip level(s)";
        return false;
      }
    }
  }

  std::string pipeline_error;
  id<MTLComputePipelineState> pipeline_state = nil;
  if (!impl_->ensure_sample_facet_pipeline(array_dimension, checked, &pipeline_state, &pipeline_error)) {
    if (error) *error = "Metal sample facet pipeline compile failed: " + pipeline_error;
    return false;
  }

  bool cache_hit = false;
  id<MTLTexture> texture = nil;
  // The GPU-use bracket only exists for a token that resolves; there is nothing
  // to hold out of the free list for a slot the pool has already retired.
  FacetUseGuard use(pool, ref);
  if (slot != nullptr) {
    if (!use.begin(arena, error)) return false;
    std::string tex_error;
    texture = impl_->ensure_facet_texture(arena, pool, ref, core::FacetKind::Sample, &cache_hit, nullptr,
                                          &tex_error);
    if (texture == nil) {
      if (error) *error = tex_error.empty() ? "Metal sample facet texture creation failed" : tex_error;
      return false;
    }
  } else {
    texture = impl_->ensure_guard_placeholder_texture(error);
    if (texture == nil) return false;
  }
  id<MTLSamplerState> sampler = impl_->ensure_sampler_state(filter, wrap);
  if (sampler == nil) { if (error) *error = "Metal sample facet sampler creation failed"; return false; }

  // The kernels take one `constant float& lod` per dispatch, not one per
  // thread, so a batch carrying several distinct levels is genuinely several
  // dispatches. They are grouped by exact lod value and scattered back into
  // the caller's coordinate order afterwards, which keeps every coordinate's
  // own level rather than picking one level for the batch (an approximation
  // START.md §4 invariant 10 forbids). All groups share one encoder and one
  // command buffer.
  std::vector<std::pair<float, std::vector<uint32_t>>> lod_groups;
  for (uint32_t i = 0; i < count; ++i) {
    const float lod = coords[i].lod;
    auto group = std::find_if(lod_groups.begin(), lod_groups.end(),
                              [lod](const std::pair<float, std::vector<uint32_t>>& entry) {
                                return entry.first == lod;
                              });
    if (group == lod_groups.end()) {
      lod_groups.push_back({lod, {i}});
    } else {
      group->second.push_back(i);
    }
  }

  id<MTLBuffer> token_buffer = nil;
  id<MTLBuffer> table_buffer = nil;
  id<MTLBuffer> slot_count_buffer = nil;
  id<MTLBuffer> violation_buffer = nil;
  if (checked) {
    const uint32_t token[2] = {ref.index, ref.generation};
    const uint32_t slot_count = static_cast<uint32_t>(generations.size());
    token_buffer = [impl_->device newBufferWithLength:sizeof(token) options:MTLResourceStorageModeShared];
    table_buffer =
        [impl_->device newBufferWithLength:std::max<size_t>(generations.size() * sizeof(uint32_t), 1)
                                   options:MTLResourceStorageModeShared];
    slot_count_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t)
                                                  options:MTLResourceStorageModeShared];
    violation_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t)
                                                 options:MTLResourceStorageModeShared];
    if (token_buffer == nil || table_buffer == nil || slot_count_buffer == nil || violation_buffer == nil) {
      if (error) *error = "Metal checked-profile facet guard buffer allocation failed";
      return false;
    }
    std::memcpy([token_buffer contents], token, sizeof(token));
    if (!generations.empty())
      std::memcpy([table_buffer contents], generations.data(), generations.size() * sizeof(uint32_t));
    std::memcpy([slot_count_buffer contents], &slot_count, sizeof(slot_count));
    std::memset([violation_buffer contents], 0, sizeof(uint32_t));
  }

  struct SampleGroupBuffers {
    id<MTLBuffer> uv = nil;
    id<MTLBuffer> slices = nil;
    id<MTLBuffer> lod = nil;
    id<MTLBuffer> output = nil;
    const std::vector<uint32_t>* indices = nullptr;
  };
  std::vector<SampleGroupBuffers> group_buffers;
  group_buffers.reserve(lod_groups.size());

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  [encoder setComputePipelineState:pipeline_state];
  [encoder setTexture:texture atIndex:compiler::kSampleFacetTextureIndex];
  [encoder setSamplerState:sampler atIndex:compiler::kSampleFacetSamplerIndex];
  uint32_t descriptor_writes = 2;  // setTexture + setSamplerState
  if (checked) {
    [encoder setBuffer:token_buffer offset:0 atIndex:compiler::kSampleFacetTokenBufferIndex];
    [encoder setBuffer:table_buffer offset:0 atIndex:compiler::kSampleFacetGenerationTableBufferIndex];
    [encoder setBuffer:slot_count_buffer offset:0 atIndex:compiler::kSampleFacetSlotCountBufferIndex];
    [encoder setBuffer:violation_buffer offset:0 atIndex:compiler::kSampleFacetViolationCounterBufferIndex];
    descriptor_writes += 4;
  }

  for (const auto& group : lod_groups) {
    const std::vector<uint32_t>& indices = group.second;
    SampleGroupBuffers buffers;
    buffers.indices = &indices;
    buffers.uv = [impl_->device newBufferWithLength:indices.size() * sizeof(float) * 2
                                            options:MTLResourceStorageModeShared];
    buffers.output = [impl_->device newBufferWithLength:indices.size() * sizeof(float) * 4
                                                options:MTLResourceStorageModeShared];
    buffers.lod = [impl_->device newBufferWithLength:sizeof(float) options:MTLResourceStorageModeShared];
    if (array_dimension)
      buffers.slices = [impl_->device newBufferWithLength:indices.size() * sizeof(uint32_t)
                                                 options:MTLResourceStorageModeShared];
    if (buffers.uv == nil || buffers.output == nil || buffers.lod == nil ||
        (array_dimension && buffers.slices == nil)) {
      if (error) *error = "Metal sample facet buffer allocation failed";
      return false;
    }
    float* uv = static_cast<float*>([buffers.uv contents]);
    uint32_t* slices = array_dimension ? static_cast<uint32_t*>([buffers.slices contents]) : nullptr;
    for (size_t i = 0; i < indices.size(); ++i) {
      uv[i * 2 + 0] = coords[indices[i]].u;
      uv[i * 2 + 1] = coords[indices[i]].v;
      if (slices != nullptr) slices[i] = coords[indices[i]].array_slice;
    }
    const float lod = group.first;
    std::memcpy([buffers.lod contents], &lod, sizeof(lod));

    [encoder setBuffer:buffers.uv offset:0 atIndex:compiler::kSampleFacetUvBufferIndex];
    [encoder setBuffer:buffers.output offset:0 atIndex:compiler::kSampleFacetOutputBufferIndex];
    [encoder setBuffer:buffers.lod offset:0 atIndex:compiler::kSampleFacetLodBufferIndex];
    if (array_dimension)
      [encoder setBuffer:buffers.slices offset:0 atIndex:compiler::kSampleFacetArraySliceBufferIndex];
    descriptor_writes += array_dimension ? 4 : 3;
    [encoder dispatchThreadgroups:MTLSizeMake(indices.size(), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    group_buffers.push_back(buffers);
  }
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal sample facet dispatch failed";
    return false;
  }

  result->sampled_rgba.assign(count, {0.0f, 0.0f, 0.0f, 0.0f});
  for (const auto& buffers : group_buffers) {
    const float* output = static_cast<const float*>([buffers.output contents]);
    for (size_t i = 0; i < buffers.indices->size(); ++i) {
      const uint32_t destination = (*buffers.indices)[i];
      result->sampled_rgba[destination] = {output[i * 4 + 0], output[i * 4 + 1], output[i * 4 + 2],
                                           output[i * 4 + 3]};
    }
  }
  result->generation_violations =
      checked ? *static_cast<const uint32_t*>([violation_buffer contents]) : 0;
  result->checked_profile = checked;
  result->facet_cache_hit = cache_hit;
  result->descriptor_write_count = descriptor_writes;
  result->report = make_facet_report();
  result->report.encoder_count = 1;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  if (guard_rejects) {
    result->report.add("facet_generation_guard", hal::LoweringClass::DevicePass, count, 0,
                       "the checked-profile shader rejected the facet token itself; every output slot is "
                       "poison and no sample was taken");
  } else {
    result->report.add("sample_facet",
                       cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::DevicePass, count,
                       0,
                       cache_hit ? "facet cache hit; no MTLTexture created for this use"
                                 : "facet cache miss; MTLTexture and sampler compiled for this view");
    result->report.add("facet_generation_guard", hal::LoweringClass::Direct, count, 0,
                       checked ? "in-shader generation check compiled in (06 §6.4)"
                               : "fast-native: the guard, its four bindings and its atomic are specialized "
                                 "away entirely");
  }
  if (lod_groups.size() > 1)
    result->report.add("sample_facet_lod_groups", hal::LoweringClass::Direct, lod_groups.size(), 0,
                       "one dispatch per distinct explicit level; the kernel's lod is per-dispatch state");
  return true;
}

bool DeviceHal::run_storage_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                  StorageFacetTarget target, const std::array<float, 4>& write_rgba,
                                  StorageFacetResult* result, std::string* error) const {
  // Texel (0,0) of subresource (layer 0, level 0) is what every pre-mip caller
  // meant, so the defaulted StorageTexel is exactly the old behaviour.
  return run_storage_facet(arena, pool, ref, target, write_rgba, StorageTexel{}, result, error);
}

bool DeviceHal::run_storage_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                  StorageFacetTarget target, const std::array<float, 4>& write_rgba,
                                  StorageTexel texel, StorageFacetResult* result,
                                  std::string* error) const {
  if (result == nullptr) { if (error) *error = "storage facet result output is required"; return false; }
  FacetUseGuard use(pool, ref);
  if (!use.begin(arena, error)) return false;
  const core::FacetSlot* slot = impl_->resolve_facet(arena, pool, ref, core::FacetKind::Storage, error);
  if (slot == nullptr) return false;
  const core::CanonicalView& view = slot->view;
  // Out-of-range coordinates are refused, never clamped: a clamped write lands
  // somewhere real and looks like it worked.
  if (texel.layer >= view.array_layers || texel.level >= view.mip_levels ||
      texel.x >= view.mip_width(texel.level) || texel.y >= view.mip_height(texel.level)) {
    if (error)
      *error = "storage facet target texel (" + std::to_string(texel.x) + ", " + std::to_string(texel.y) +
               ") of layer " + std::to_string(texel.layer) + " level " + std::to_string(texel.level) +
               " lies outside the subresources this canonical view declares";
    return false;
  }
  const uint64_t texel_byte_offset = view.subresource_byte_offset(texel.layer, texel.level) +
                                     static_cast<uint64_t>(texel.y) * view.bytes_per_row(texel.level) +
                                     static_cast<uint64_t>(texel.x) * kBytesPerTexel;
  std::string pipeline_error;
  if (!impl_->ensure_storage_facet_pipelines(&pipeline_error)) {
    if (error) *error = "Metal storage facet pipeline compile failed: " + pipeline_error;
    return false;
  }

  id<MTLBuffer> rgba_buffer = [impl_->device newBufferWithLength:sizeof(float) * 4
                                                          options:MTLResourceStorageModeShared];
  if (rgba_buffer == nil) {
    if (error) *error = "Metal storage facet constant buffer allocation failed";
    return false;
  }
  std::memcpy([rgba_buffer contents], write_rgba.data(), sizeof(float) * 4);

  bool cache_hit = false;
  id<MTLTexture> texture = nil;
  id<MTLBuffer> linear = nil;
  id<MTLBuffer> format_buffer = nil;
  id<MTLBuffer> target_buffer = nil;
  id<MTLBuffer> texel_index_buffer = nil;
  const bool array_dimension = view.dimension == core::ViewDimension::Texture2DArray;
  if (target == StorageFacetTarget::Texture) {
    std::string tex_error;
    texture = impl_->ensure_facet_texture(arena, pool, ref, core::FacetKind::Storage, &cache_hit, nullptr,
                                          &tex_error);
    if (texture == nil) {
      if (error) *error = tex_error.empty() ? "Metal storage facet texture creation failed" : tex_error;
      return false;
    }
    target_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t) * 4
                                                options:MTLResourceStorageModeShared];
    if (target_buffer == nil) {
      if (error) *error = "Metal storage facet target buffer allocation failed";
      return false;
    }
    const uint32_t words[4] = {texel.x, texel.y, texel.layer, texel.level};
    std::memcpy([target_buffer contents], words, sizeof(words));
  } else {
    linear = impl_->ensure_facet_buffer(arena, pool, ref, core::FacetKind::Storage, error);
    if (linear == nil) return false;
    if ([linear length] < texel_byte_offset + kBytesPerTexel) {
      if (error) *error = "storage facet linear backing is smaller than the addressed subresource";
      return false;
    }
    // The write lands in the view's own format, so the buffer path never
    // trades away precision the caller asked for (06 §6.2).
    format_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t)
                                                options:MTLResourceStorageModeShared];
    texel_index_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t)
                                                    options:MTLResourceStorageModeShared];
    if (format_buffer == nil || texel_index_buffer == nil) {
      if (error) *error = "Metal storage facet format buffer allocation failed";
      return false;
    }
    const uint32_t format_code = static_cast<uint32_t>(view.format);
    std::memcpy([format_buffer contents], &format_code, sizeof(format_code));
    // The kernel indexes in texels, not bytes; core::bytes_per_texel is 4 for
    // both formats this milestone models, which is what makes that exact.
    const uint32_t texel_index = static_cast<uint32_t>(texel_byte_offset / kBytesPerTexel);
    std::memcpy([texel_index_buffer contents], &texel_index, sizeof(texel_index));
  }

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  if (target == StorageFacetTarget::Texture) {
    [encoder setComputePipelineState:array_dimension ? impl_->storage_array_facet_pipeline
                                                     : impl_->storage_facet_pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:rgba_buffer offset:0 atIndex:0];
    [encoder setBuffer:target_buffer offset:0 atIndex:1];
    result->descriptor_write_count = 3;  // setTexture + rgba + target subresource
  } else {
    [encoder setComputePipelineState:impl_->storage_buffer_facet_pipeline];
    [encoder setBuffer:linear offset:0 atIndex:0];
    [encoder setBuffer:rgba_buffer offset:0 atIndex:1];
    [encoder setBuffer:format_buffer offset:0 atIndex:2];
    [encoder setBuffer:texel_index_buffer offset:0 atIndex:3];
    result->descriptor_write_count = 4;  // four buffer bindings, no texture
  }
  [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal storage facet dispatch failed";
    return false;
  }

  if (target == StorageFacetTarget::Texture) {
    if (!impl_->read_texel(texture, texel.layer, texel.level, texel.x, texel.y, &result->written_rgba,
                           error))
      return false;
  } else if (view.format == core::PixelFormat::RGBA8Unorm) {
    const uint8_t* bytes = static_cast<const uint8_t*>([linear contents]) + texel_byte_offset;
    result->written_rgba = {bytes[0] / 255.0f, bytes[1] / 255.0f, bytes[2] / 255.0f, bytes[3] / 255.0f};
  } else {
    float value{};
    std::memcpy(&value, static_cast<const uint8_t*>([linear contents]) + texel_byte_offset, sizeof(value));
    result->written_rgba = {value, 0.0f, 0.0f, 0.0f};
  }
  result->target = target;
  result->facet_cache_hit = cache_hit;
  result->encoder_count = 1;
  result->report = make_facet_report();
  result->report.encoder_count = 1;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  if (target == StorageFacetTarget::Texture) {
    result->report.add("storage_facet_texture",
                       cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::DevicePass, 1, 0,
                       "writable MTLTexture representation");
  } else {
    result->report.add("storage_facet_linear_buffer", hal::LoweringClass::Direct, 1, [linear length],
                       "linear MTLBuffer representation; view format preserved on write");
  }
  return true;
}

bool DeviceHal::run_attachment_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                     const AttachmentFacetDesc& desc, AttachmentFacetResult* result,
                                     std::string* error) const {
  if (result == nullptr) { if (error) *error = "attachment facet result output is required"; return false; }
  const bool multisampled = desc.sample_count > 1;
  if (multisampled != (desc.store == AttachmentStoreAction::MultisampleResolve)) {
    if (error)
      *error = "attachment facet: MultisampleResolve and sample_count > 1 must be requested together";
    return false;
  }
  if (multisampled && desc.load == AttachmentLoadAction::Load) {
    if (error) *error = "Unsupported: a transient multisample attachment has no prior contents to load";
    return false;
  }
  FacetUseGuard use(pool, ref);
  if (!use.begin(arena, error)) return false;
  const core::FacetSlot* slot = impl_->resolve_facet(arena, pool, ref, core::FacetKind::Attachment, error);
  if (slot == nullptr) return false;
  if (!subresource_in_range(slot->view, desc.subresource, error)) return false;
  bool cache_hit = false;
  std::string tex_error;
  id<MTLTexture> texture = impl_->ensure_facet_texture(arena, pool, ref, core::FacetKind::Attachment,
                                                       &cache_hit, nullptr, &tex_error);
  if (texture == nil) {
    if (error) *error = tex_error.empty() ? "Metal attachment facet texture creation failed" : tex_error;
    return false;
  }

  // No draw and no fragment shader: this exercises the load/store/resolve
  // lowering itself, which is where 06 §6.3's "not public object state"
  // constraint actually lives. run_raster_triangles() below is the same
  // lowering with a draw in it, built from the same helper.
  bool store_traffic_avoided = false;
  MTLRenderPassDescriptor* rp = impl_->make_render_pass(texture, desc, slot->view,
                                                        &store_traffic_avoided, error);
  if (rp == nil) return false;

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:rp];
  if (encoder == nil) { if (error) *error = "failed to create Metal render encoder"; return false; }
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal attachment facet render pass failed";
    return false;
  }

  if (!impl_->read_texel(texture, desc.subresource.layer, desc.subresource.level, 0, 0,
                         &result->resolved_rgba, error))
    return false;
  result->facet_cache_hit = cache_hit;
  result->encoder_count = 1;
  result->sample_count = desc.sample_count;
  result->store_traffic_avoided = store_traffic_avoided;
  result->report = make_facet_report();
  result->report.encoder_count = 1;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  result->report.add(multisampled ? "attachment_facet_resolve" : "attachment_facet_store",
                     cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::DevicePass, 1, 0,
                     store_traffic_avoided ? "attachment samples never reached device memory"
                                           : "attachment contents written to device memory");
  return true;
}

bool DeviceHal::run_representation_transform(core::Arena& arena, core::FacetPool& pool,
                                             const core::CanonicalView& view, core::FacetKind target_kind,
                                             RepresentationTransformResult* result,
                                             std::string* error) const {
  if (result == nullptr) {
    if (error) *error = "representation transform result output is required";
    return false;
  }
  const core::Allocation* allocation = arena.lookup(view.allocation, view.allocation_generation);
  if (allocation == nullptr) {
    if (error) *error = "representation transform: backing allocation not found in arena";
    return false;
  }
  if (!view_expressible(view, target_kind, allocation->bytes.size(), error)) return false;
  const uint64_t old_bytes = allocation->bytes.size();

  // Real transform pass (02 §8: a transform is not a barrier). Publishing the
  // new epoch first invalidates every facet minted against the old one;
  // retire_stale is what actually returns their slots, and only for slots with
  // no work still in flight.
  uint32_t new_epoch = 0;
  if (!arena.transform(view.allocation, view.allocation_generation, &new_epoch, error)) return false;
  const uint32_t retired = static_cast<uint32_t>(pool.retire_stale(arena));

  core::FacetRef out_facet{};
  if (!pool.acquire(arena, view, target_kind, &out_facet, error)) return false;
  // Same helper submit()'s Stage 5 physical callback uses, so the standalone
  // entry point and the ExecutionPlan path cannot drift on which subresources
  // get copied or on what the transform costs.
  Impl::TransformCost cost;
  if (!impl_->transform_into_private_facet(arena, pool, view, target_kind, out_facet, &cost, error))
    return false;

  result->new_epoch = new_epoch;
  result->old_backing_bytes = old_bytes;
  result->new_backing_bytes = cost.new_backing_bytes;
  result->temporary_bytes = cost.temporary_bytes;
  result->encoder_count = cost.encoder_count;
  result->used_private_optimal = true;
  result->retired_facet_count = retired;
  result->out_facet = out_facet;
  result->report = make_facet_report();
  result->report.encoder_count = cost.encoder_count;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  result->report.add("representation_transform", hal::LoweringClass::DevicePass, view.subresource_count(),
                     cost.new_backing_bytes,
                     "blit every subresource from the TransferFacet's linear buffer into a Private "
                     "optimal texture");
  result->report.add("representation_transform_peak", hal::LoweringClass::Direct, 1,
                     old_bytes + cost.new_backing_bytes,
                     "old linear backing retained alongside the new texture; no staging copy. This entry "
                     "point never consumes it: 02 §4.2 makes ConsumeInput a proven exclusive consume, and "
                     "06 §11 forbids the adapter inferring a destructive transform on its own, so the "
                     "watermark is only reduced through ExecutionPlan::representation_requests");
  result->report.add("facet_retire_stale", hal::LoweringClass::Direct, retired, 0,
                     "facets invalidated by the new RepresentationEpoch");
  const uint32_t retired_textures = impl_->retire_stale_facet_textures(arena, pool);
  if (retired_textures != 0) {
    result->report.add("facet_texture_retire", hal::LoweringClass::Direct, retired_textures, 0,
                       "MTLTextures belonging to retired facet slots destroyed after the transform");
  }
  return true;
}

bool DeviceHal::run_raster_triangles(const core::Arena& arena, core::FacetPool& pool,
                                     core::FacetRef source_ref, core::FacetRef target_ref,
                                     const RasterDesc& desc, const std::vector<RasterVertex>& vertices,
                                     RasterResult* result, std::string* error) const {
  if (result == nullptr) { if (error) *error = "raster result output is required"; return false; }
  if (vertices.empty() || vertices.size() % 3 != 0) {
    if (error) *error = "raster vertex count must be a non-zero multiple of 3 (triangle list)";
    return false;
  }
  const bool multisampled = desc.attachment.sample_count > 1;
  if (multisampled != (desc.attachment.store == AttachmentStoreAction::MultisampleResolve)) {
    if (error) *error = "raster: MultisampleResolve and sample_count > 1 must be requested together";
    return false;
  }
  if (multisampled && desc.attachment.load == AttachmentLoadAction::Load) {
    if (error) *error = "Unsupported: a transient multisample attachment has no prior contents to load";
    return false;
  }

  // Both refs are capability tokens and both are bracketed: a pass reads one
  // facet and writes another, so neither slot may be recycled under work still
  // in flight (06 §6.4, §11).
  FacetUseGuard source_use(pool, source_ref);
  if (!source_use.begin(arena, error)) return false;
  FacetUseGuard target_use(pool, target_ref);
  if (!target_use.begin(arena, error)) return false;

  const core::FacetSlot* source_slot =
      impl_->resolve_facet(arena, pool, source_ref, core::FacetKind::Sample, error);
  if (source_slot == nullptr) return false;
  const core::FacetSlot* target_slot =
      impl_->resolve_facet(arena, pool, target_ref, core::FacetKind::Attachment, error);
  if (target_slot == nullptr) return false;
  const core::CanonicalView& source_view = source_slot->view;
  const core::CanonicalView& target_view = target_slot->view;
  if (!subresource_in_range(target_view, desc.attachment.subresource, error)) return false;
  if (desc.source_array_slice >= source_view.array_layers) {
    if (error)
      *error = "raster source names array slice " + std::to_string(desc.source_array_slice) +
               " of a canonical view declaring " + std::to_string(source_view.array_layers) + " layer(s)";
    return false;
  }
  if (!(desc.source_lod >= 0.0f) || desc.source_lod > static_cast<float>(source_view.mip_levels - 1)) {
    if (error)
      *error = "raster source names lod " + std::to_string(desc.source_lod) +
               " of a canonical view declaring " + std::to_string(source_view.mip_levels) + " mip level(s)";
    return false;
  }
  // Reading the very subresource being written has no defined result, and a
  // pass that returned an order-dependent image for it would be worse than
  // useless as a differential against the oracle, which refuses it for exactly
  // this reason. Sharing an allocation is fine as long as the subresource
  // differs, so generating one mip level from another stays expressible.
  if (source_view.allocation == target_view.allocation &&
      desc.source_array_slice == desc.attachment.subresource.layer &&
      static_cast<uint32_t>(desc.source_lod) == desc.attachment.subresource.level &&
      desc.source_lod == static_cast<float>(static_cast<uint32_t>(desc.source_lod))) {
    if (error)
      *error = "raster source and target name the same subresource of the same allocation; a read of the "
               "surface being written has no defined result";
    return false;
  }

  bool source_cache_hit = false;
  std::string tex_error;
  id<MTLTexture> source_texture = impl_->ensure_facet_texture(arena, pool, source_ref,
                                                              core::FacetKind::Sample, &source_cache_hit,
                                                              nullptr, &tex_error);
  if (source_texture == nil) {
    if (error) *error = tex_error.empty() ? "Metal raster source texture creation failed" : tex_error;
    return false;
  }
  // The shared fragment stage declares `texture2d<float>` and takes no slice or
  // level argument, so an array source reaches it as a single-slice 2D view
  // over the whole mip chain -- a real reinterpretation of the same storage,
  // not a copy and not a silently ignored slice. The requested level is then
  // pinned through the sampler's lod clamps below, which is exact for a
  // fractional lod too because MipFilterLinear blends the two levels the clamp
  // lands between.
  if (source_view.dimension == core::ViewDimension::Texture2DArray) {
    source_texture = [source_texture newTextureViewWithPixelFormat:source_texture.pixelFormat
                                                       textureType:MTLTextureType2D
                                                            levels:NSMakeRange(0, source_view.mip_levels)
                                                            slices:NSMakeRange(desc.source_array_slice, 1)];
    if (source_texture == nil) {
      if (error) *error = "Metal raster source array-slice texture view creation failed";
      return false;
    }
  }
  id<MTLSamplerState> sampler =
      impl_->ensure_sampler_state(desc.filter, desc.wrap, desc.source_lod, desc.source_lod);
  if (sampler == nil) { if (error) *error = "Metal raster sampler creation failed"; return false; }

  bool target_cache_hit = false;
  id<MTLTexture> target_texture = impl_->ensure_facet_texture(arena, pool, target_ref,
                                                              core::FacetKind::Attachment,
                                                              &target_cache_hit, nullptr, &tex_error);
  if (target_texture == nil) {
    if (error) *error = tex_error.empty() ? "Metal raster target texture creation failed" : tex_error;
    return false;
  }

  // Attachment format and sample count are compiled into the pipeline and are
  // therefore key state (06 §7); the viewport set below and the tint bound at
  // fragment buffer(0) are not, and must not enlarge the key.
  std::string pipeline_error;
  id<MTLRenderPipelineState> pipeline_state = nil;
  if (!impl_->ensure_raster_pipeline(target_view.format, desc.attachment.sample_count, &pipeline_state,
                                     &pipeline_error)) {
    if (error) *error = "Metal raster pipeline compile failed: " + pipeline_error;
    return false;
  }

  id<MTLBuffer> vertex_buffer =
      [impl_->device newBufferWithLength:vertices.size() * sizeof(RasterVertex)
                                 options:MTLResourceStorageModeShared];
  id<MTLBuffer> tint_buffer = [impl_->device newBufferWithLength:sizeof(float) * 4
                                                         options:MTLResourceStorageModeShared];
  if (vertex_buffer == nil || tint_buffer == nil) {
    if (error) *error = "Metal raster buffer allocation failed";
    return false;
  }
  // RasterVertex is {x, y, u, v} and the MSL side is
  // `struct VgRasterVertex { float2 position; float2 uv; }`: the same 16 bytes
  // in the same order, so the host array uploads without a repack.
  std::memcpy([vertex_buffer contents], vertices.data(), vertices.size() * sizeof(RasterVertex));
  std::memcpy([tint_buffer contents], desc.tint.data(), sizeof(float) * 4);

  const uint32_t level = desc.attachment.subresource.level;
  const uint32_t width = target_view.mip_width(level);
  const uint32_t height = target_view.mip_height(level);
  bool store_traffic_avoided = false;
  MTLRenderPassDescriptor* rp = impl_->make_render_pass(target_texture, desc.attachment, target_view,
                                                        &store_traffic_avoided, error);
  if (rp == nil) return false;

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:rp];
  if (encoder == nil) { if (error) *error = "failed to create Metal render encoder"; return false; }
  [encoder setRenderPipelineState:pipeline_state];
  // Dynamic state, deliberately: a viewport change must not compile a second
  // pipeline (06 §7's "小的动态状态不应无故扩大 key").
  [encoder setViewport:(MTLViewport){0.0, 0.0, static_cast<double>(width), static_cast<double>(height),
                                     0.0, 1.0}];
  [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:compiler::kRasterVertexBufferIndex];
  [encoder setFragmentTexture:source_texture atIndex:compiler::kRasterTextureIndex];
  [encoder setFragmentSamplerState:sampler atIndex:compiler::kRasterSamplerIndex];
  [encoder setFragmentBuffer:tint_buffer offset:0 atIndex:compiler::kRasterTintBufferIndex];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertices.size()];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal raster pass failed";
    return false;
  }

  const bool stored = desc.attachment.store == AttachmentStoreAction::Store ||
                      desc.attachment.store == AttachmentStoreAction::MultisampleResolve;
  // The whole target subresource, not texel (0,0): an image-correctness
  // differential against reference::raster_triangles needs every pixel, and a
  // single-texel readback would let a coverage or interpolation regression pass
  // unnoticed.
  if (!impl_->read_texture_region(target_texture, desc.attachment.subresource.layer, level, 0, 0, width,
                                  height, &result->resolved_rgba, error))
    return false;

  result->width = width;
  result->height = height;
  result->sample_count = desc.attachment.sample_count;
  result->covered_fragment_count = 0;
  result->stored = stored;
  // A DontCare load leaves the previous bytes visible and a DontCare store
  // leaves memory untouched; in both cases the contract does not define what a
  // reader sees, so the values returned must not be used as an expectation.
  result->contents_defined = stored && desc.attachment.load != AttachmentLoadAction::DontCare;
  result->facet_cache_hit = source_cache_hit && target_cache_hit;
  result->encoder_count = 1;
  result->report = make_facet_report();
  result->report.encoder_count = 1;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  result->report.add("raster_attachment_store", hal::LoweringClass::Direct, vertices.size() / 3, 0,
                     std::string("real MTLRenderPipelineState triangle-list draw into a render "
                                 "attachment; ") +
                         kRasterClipSpaceNote);
  result->report.add("raster_source_sample",
                     source_cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::DevicePass, 1,
                     0,
                     "SampleFacet read through a texture2d view of the requested array slice, level pinned "
                     "by the sampler's lod clamps");
  result->report.add(multisampled ? "raster_resolve" : "raster_store", hal::LoweringClass::Direct, 1, 0,
                     store_traffic_avoided ? "attachment samples never reached device memory"
                                           : "attachment contents written to device memory");
  return true;
}

bool DeviceHal::run_pipeline_classification(PipelineClassificationRun* result, std::string* error) const {
  if (result == nullptr) {
    if (error) *error = "pipeline classification result output is required";
    return false;
  }
  *result = PipelineClassificationRun{};

  // E013's three axes, one per 07 §9 fate that is allowed to exist, plus the
  // two raster parameters E013 names by hand. Every axis carries two values so
  // the matrix is exactly the 2x2x2 the experiment asks for and the expected
  // counts are arithmetic rather than a judgement call.
  //
  //  - checked profile: a function constant. 06 §6.4 compiles the generation
  //    guard in or specializes it away, so it genuinely cannot be anything but
  //    PipelineKey state.
  //  - threadgroup width / viewport: set on the encoder. 06 §7's "小的动态状态
  //    不应无故扩大 key" is precisely about this one.
  //  - sample lod / tint: bytes the shader reads from a binding. Changing them
  //    changes the image, not the program.
  static constexpr std::array<uint64_t, 2> kCheckedValues{0, 1};
  static constexpr std::array<uint64_t, 2> kDynamicValues{32, 64};
  static constexpr std::array<uint64_t, 2> kShaderValues{0, 1};
  static constexpr std::array<core::PixelFormat, 2> kFormats{core::PixelFormat::RGBA8Unorm,
                                                             core::PixelFormat::R32Float};
  static constexpr std::array<uint32_t, 2> kSampleCounts{1, 4};

  const std::string compute_source = compiler::sample_facet_metal_source();
  const std::string raster_source = compiler::raster_facet_metal_source();
  const std::string compute_entry = "vg_sample_facet";
  const std::string raster_fragment_entry = "vg_raster_fragment";
  const std::string raster_vertex_entry = "vg_raster_vertex";

  auto compute_constants = [](uint64_t checked) {
    std::vector<std::pair<std::string, uint64_t>> constants;
    // An unset constant is fast-native (is_function_constant_defined() is
    // false in the kernel), so the guard-off variant names nothing rather than
    // naming a false.
    if (checked != 0) constants.emplace_back("vg_checked_profile", 1);
    return constants;
  };

  // ---- Naive variant -------------------------------------------------------
  //
  // What a backend does when it has no state taxonomy: every permutation of
  // every piece of pipeline-adjacent state is assumed to need its own compiled
  // object, so it compiles one. Nothing here consults a cache, and the count is
  // of MTLComputePipelineState / MTLRenderPipelineState objects this device
  // really created -- not of permutations enumerated.
  std::vector<id<MTLComputePipelineState>> naive_compute;
  std::vector<id<MTLRenderPipelineState>> naive_render;
  const auto release_naive = [&]() {
    for (id<MTLComputePipelineState> pipeline : naive_compute) [pipeline release];
    for (id<MTLRenderPipelineState> pipeline : naive_render) [pipeline release];
  };
  uint64_t naive_ns = 0;
  for (uint64_t checked : kCheckedValues) {
    for (uint64_t dynamic_value : kDynamicValues) {
      for (uint64_t shader_value : kShaderValues) {
        (void)dynamic_value;
        (void)shader_value;
        const auto start = std::chrono::steady_clock::now();
        compiler::PipelineKey key =
            impl_->make_pipeline_key(compute_source, compute_entry, compute_constants(checked), {}, 1);
        id<MTLLibrary> library_object =
            impl_->ensure_library(compute_source, key.code_object_hash, error);
        if (library_object == nil) { release_naive(); return false; }
        id<MTLFunction> function = impl_->ensure_function(library_object, key, error);
        if (function == nil) { release_naive(); return false; }
        NSError* pipeline_error = nil;
        id<MTLComputePipelineState> pipeline =
            [impl_->device newComputePipelineStateWithFunction:function error:&pipeline_error];
        naive_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                .count());
        if (pipeline == nil) {
          if (error)
            *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                           : "naive compute pipeline creation failed";
          release_naive();
          return false;
        }
        naive_compute.push_back(pipeline);
      }
    }
  }
  for (core::PixelFormat format : kFormats) {
    for (uint32_t sample_count : kSampleCounts) {
      for (uint64_t dynamic_value : kDynamicValues) {
        for (uint64_t shader_value : kShaderValues) {
          (void)dynamic_value;
          (void)shader_value;
          const auto start = std::chrono::steady_clock::now();
          const compiler::PipelineKey key =
              impl_->make_pipeline_key(raster_source, raster_fragment_entry, {},
                                       {static_cast<uint32_t>(format)}, sample_count);
          id<MTLLibrary> library_object =
              impl_->ensure_library(raster_source, key.code_object_hash, error);
          if (library_object == nil) { release_naive(); return false; }
          compiler::PipelineKey vertex_key = key;
          vertex_key.entry = raster_vertex_entry;
          id<MTLFunction> vertex_function = impl_->ensure_function(library_object, vertex_key, error);
          id<MTLFunction> fragment_function = impl_->ensure_function(library_object, key, error);
          if (vertex_function == nil || fragment_function == nil) { release_naive(); return false; }
          MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
          descriptor.vertexFunction = vertex_function;
          descriptor.fragmentFunction = fragment_function;
          descriptor.colorAttachments[0].pixelFormat = to_mtl_pixel_format(format);
          descriptor.rasterSampleCount = sample_count;
          NSError* pipeline_error = nil;
          id<MTLRenderPipelineState> pipeline =
              [impl_->device newRenderPipelineStateWithDescriptor:descriptor error:&pipeline_error];
          naive_ns += static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                  .count());
          if (pipeline == nil) {
            if (error)
              *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                             : "naive render pipeline creation failed";
            release_naive();
            return false;
          }
          naive_render.push_back(pipeline);
        }
      }
    }
  }
  result->naive_pipeline_count =
      static_cast<uint32_t>(naive_compute.size() + naive_render.size());
  result->naive_compile_ns = naive_ns;
  release_naive();

  // ---- VG classified variant ----------------------------------------------
  //
  // The cache and the object maps are local to this run so the numbers below
  // describe this experiment only. Reusing impl_->pipeline_cache would fold in
  // every pipeline the rest of the session happened to compile first, and a
  // hit/miss ratio measured against unrelated history is not the ratio E013
  // asks about.
  compiler::PipelineClassificationCache cache;
  std::unordered_map<uint64_t, id<MTLComputePipelineState>> compute_objects;
  std::unordered_map<uint64_t, id<MTLRenderPipelineState>> render_objects;
  const auto release_classified = [&]() {
    for (auto& entry : compute_objects) [entry.second release];
    for (auto& entry : render_objects) [entry.second release];
  };

  for (uint64_t checked : kCheckedValues) {
    for (uint64_t dynamic_value : kDynamicValues) {
      for (uint64_t shader_value : kShaderValues) {
        const compiler::PipelineKey base =
            impl_->make_pipeline_key(compute_source, compute_entry, compute_constants(checked), {}, 1);
        const std::vector<compiler::StateBlock> blocks{
            {"facet_generation_guard", compiler::StateBlockKind::PipelineKey, checked},
            {"threadgroup_width", compiler::StateBlockKind::DynamicState, dynamic_value},
            {"sample_lod", compiler::StateBlockKind::ShaderVisibleData, shader_value},
        };
        const compiler::PipelineClassification classification = classify_pipeline_state(base, blocks);
        if (!classification.ok) {
          if (error) *error = "pipeline classification rejected a supported compute permutation: " +
                              classification.message;
          release_classified();
          return false;
        }
        if (!impl_->acquire_compute_pipeline(cache, compute_objects, compute_source, classification.key,
                                             "E013 classified SampleFacet permutation", nullptr, nullptr,
                                             error)) {
          release_classified();
          return false;
        }
      }
    }
  }
  for (core::PixelFormat format : kFormats) {
    for (uint32_t sample_count : kSampleCounts) {
      for (uint64_t dynamic_value : kDynamicValues) {
        for (uint64_t shader_value : kShaderValues) {
          const compiler::PipelineKey base =
              impl_->make_pipeline_key(raster_source, raster_fragment_entry, {},
                                       {static_cast<uint32_t>(format)}, sample_count);
          const std::vector<compiler::StateBlock> blocks{
              {"viewport", compiler::StateBlockKind::DynamicState, dynamic_value},
              {"tint", compiler::StateBlockKind::ShaderVisibleData, shader_value},
          };
          const compiler::PipelineClassification classification = classify_pipeline_state(base, blocks);
          if (!classification.ok) {
            if (error) *error = "pipeline classification rejected a supported raster permutation: " +
                                classification.message;
            release_classified();
            return false;
          }
          if (!impl_->acquire_render_pipeline(cache, render_objects, raster_source, classification.key,
                                              raster_vertex_entry, to_mtl_pixel_format(format),
                                              "E013 classified raster permutation", nullptr, nullptr,
                                              error)) {
            release_classified();
            // The two raster axes are the only key state that lives late in
            // PipelineKey::canonical(), so a digest that cannot separate them
            // surfaces here first. Say so, rather than letting the caller read
            // a bare "collision" and conclude the classification is wrong: the
            // classification is right, and the key it produced is distinct
            // text. Reporting the failure is the only honest option, since
            // serving one pipeline for two attachment formats would be a
            // silently wrong PSO (START.md invariant 10).
            if (error)
              *error += ". The two keys differ in attachment format or sample count, which 06 §7 makes "
                        "key state; a digest that cannot separate them cannot answer E013";
            return false;
          }
        }
      }
    }
  }

  // A state block this device cannot express must stop the classification, not
  // become another key bit (START.md invariant 10, 06 §6.2). The probe is a
  // real call with a real rejection, so "we reject it" is measured here rather
  // than asserted in a comment.
  const compiler::PipelineKey unsupported_base =
      impl_->make_pipeline_key(raster_source, raster_fragment_entry, {},
                               {static_cast<uint32_t>(core::PixelFormat::RGBA8Unorm)}, 1);
  const compiler::PipelineClassification unsupported = classify_pipeline_state(
      unsupported_base,
      {{"viewport", compiler::StateBlockKind::DynamicState, kDynamicValues[0]},
       {"attachment_format_needs_conversion", compiler::StateBlockKind::UnsupportedNeedsConversion, 0}});
  if (unsupported.ok) {
    if (error)
      *error = "an UnsupportedNeedsConversion state block was folded into a pipeline key instead of "
               "being rejected";
    release_classified();
    return false;
  }
  result->unsupported_rejected = true;

  result->classified_pipeline_count = cache.pipeline_count();
  result->cache_hits = cache.cache_hits();
  result->cache_misses = cache.cache_misses();
  result->reports = cache.reports();
  uint64_t classified_ns = 0;
  for (const compiler::SpecializationReport& report : result->reports) classified_ns += report.compile_ns;
  result->classified_compile_ns = classified_ns;
  release_classified();

  if (result->classified_pipeline_count >= result->naive_pipeline_count) {
    if (error)
      *error = "classification produced " + std::to_string(result->classified_pipeline_count) +
               " pipelines against a naive " + std::to_string(result->naive_pipeline_count) +
               "; E013 only has an answer if keeping dynamic and shader-visible state out of the key "
               "actually removes permutations";
    return false;
  }

  result->report = make_facet_report();
  result->report.add("pipeline_key_state", hal::LoweringClass::Direct,
                     result->classified_pipeline_count, 0,
                     "function constants, attachment format and sample count are compiled in and are the "
                     "only state that reached the key (06 §7)");
  result->report.add("pipeline_dynamic_state", hal::LoweringClass::Direct,
                     static_cast<uint32_t>(kDynamicValues.size()), 0,
                     "threadgroup width and viewport are set on the encoder and compiled no pipeline");
  result->report.add("pipeline_shader_visible_data", hal::LoweringClass::Direct,
                     static_cast<uint32_t>(kShaderValues.size()), 0,
                     "sample lod and tint are bytes the shader reads and compiled no pipeline");
  result->report.add("pipeline_permutations_avoided", hal::LoweringClass::Direct,
                     result->naive_pipeline_count - result->classified_pipeline_count, 0,
                     "permutations a taxonomy-free backend would have compiled");
  result->report.add("pipeline_unsupported_rejected", hal::LoweringClass::Unsupported, 1, 0,
                     "a state block requiring conversion was reported, never folded into a key");
  return true;
}

const std::vector<std::array<uint32_t, 3>>& DeviceHal::last_tier1_indirect_dims() const {
  return impl_->last_tier1_indirect_dims;
}

std::unique_ptr<DeviceHal> make_device_hal() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) return nullptr;
  auto impl = std::make_unique<DeviceHal::Impl>();
  impl->device = device;
  impl->command_queue = [device newCommandQueue];
  if (impl->command_queue == nil) return nullptr;
  impl->snapshot.hal = make_hal_snapshot(device, &impl->snapshot);
  // gpuAddress is a buffer capability, not a device-name heuristic. Probe a
  // real shared buffer and advertise LinearAddress only when it is usable.
  id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
  if (buffer != nil && [buffer respondsToSelector:@selector(gpuAddress)] && [buffer gpuAddress] != 0) {
    impl->snapshot.supports_gpu_addresses = true;
    impl->snapshot.hal.capability_bits |= static_cast<uint64_t>(hal::Capability::LinearAddress);
  }
  return std::unique_ptr<DeviceHal>(new DeviceHal(std::move(impl)));
}

}  // namespace vg::metal
