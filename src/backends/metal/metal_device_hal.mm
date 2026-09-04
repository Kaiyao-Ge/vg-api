#include "backends/metal/metal_device_hal.h"
#include "backends/metal/metal_tier2.h"

#include "backends/reference/reference_executor.h"
#include "compiler/compute_task_ring.h"
#include "compiler/pipeline_classification.h"
#include "core/scene_root.h"
#include "vg_scene_root_layout.h"
#include "ir/sha256.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <string>
#include <string_view>
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
  // Raster, RepresentationTransform, CheckedFacetGeneration and UserShaderImport
  // join it for the same reason, and are deliberately *not* probed: each is an
  // obligation this adapter now meets in software, not an optional hardware
  // feature. Raster is a real MTLRenderPipelineState draw (06 §1 "render
  // attachment 与基础 raster"), RepresentationTransform is a real
  // linear->Private-optimal blit that publishes a new epoch (02 §8),
  // CheckedFacetGeneration is the in-shader generation guard of 06 §6.4
  // specialized through an MSL function constant, and UserShaderImport is the
  // restricted-import user MSL raster path (F3, ADR-043 Decision #4) that
  // compile()/submit()/ensure_raster_pipeline()/run_raster_pass() already
  // implement. A device that could not do one of them would have to leave the
  // bit clear, but every Metal device this backend runs on can do all four.
  uint64_t bits = static_cast<uint64_t>(hal::Capability::EffectDag) |
                   static_cast<uint64_t>(hal::Capability::TaskPublication) |
                   static_cast<uint64_t>(hal::Capability::Raster) |
                   static_cast<uint64_t>(hal::Capability::RepresentationTransform) |
                   static_cast<uint64_t>(hal::Capability::CheckedFacetGeneration) |
                   static_cast<uint64_t>(hal::Capability::UserShaderImport);
  if (shared_events) bits |= static_cast<uint64_t>(hal::Capability::Timeline);
  if (indirect) bits |= static_cast<uint64_t>(hal::Capability::IndirectTier1);
  if (gpu_addresses) {
    bits |= static_cast<uint64_t>(hal::Capability::LinearAddress);
    // The indexed package is the GPU-address argument-table lowering; do
    // not advertise the prerequisite address domain while rejecting the
    // capability that this adapter actually implements with it.
    bits |= static_cast<uint64_t>(hal::Capability::IndexedBinding);
  }
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
  switch (format) {
    case core::PixelFormat::RGBA8Unorm: return MTLPixelFormatRGBA8Unorm;
    case core::PixelFormat::R32Float: return MTLPixelFormatR32Float;
    case core::PixelFormat::Depth32Float: return MTLPixelFormatDepth32Float;
    // Index formats are byte-addressed MTLBuffers, never textures.
    case core::PixelFormat::R16Uint:
    case core::PixelFormat::R32Uint: return MTLPixelFormatInvalid;
  }
  return MTLPixelFormatInvalid;
}

MTLCompareFunction to_mtl_compare_function(core::DepthCompareOp op) {
  switch (op) {
    case core::DepthCompareOp::Never: return MTLCompareFunctionNever;
    case core::DepthCompareOp::Less: return MTLCompareFunctionLess;
    case core::DepthCompareOp::Equal: return MTLCompareFunctionEqual;
    case core::DepthCompareOp::LessEqual: return MTLCompareFunctionLessEqual;
    case core::DepthCompareOp::Greater: return MTLCompareFunctionGreater;
    case core::DepthCompareOp::NotEqual: return MTLCompareFunctionNotEqual;
    case core::DepthCompareOp::GreaterEqual: return MTLCompareFunctionGreaterEqual;
    case core::DepthCompareOp::Always: return MTLCompareFunctionAlways;
  }
  return MTLCompareFunctionAlways;
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
  FacetUseGuard(FacetUseGuard&&) = delete;
  FacetUseGuard& operator=(FacetUseGuard&&) = delete;
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
    const auto* rgba = static_cast<const uint8_t*>(bytes);
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

NSString* ns_utf8(std::string_view text) {
  return [[[NSString alloc] initWithBytes:text.data()
                                   length:text.size()
                                 encoding:NSUTF8StringEncoding] autorelease];
}

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
  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() {
    for (auto& entry : allocation_map) release_buffer(entry.second.buffer);
    release_buffer(identity_scene_root_buffer);
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
      const core::Allocation* allocation = arena.lookup(core::PointerRef{it->first, it->second.generation});
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
  // TASK-B13: debug/test introspection only, see DeviceHal::last_tier1_indirect_dims().
  std::vector<std::array<uint32_t, 3>> last_tier1_indirect_dims;
  // Debug/test introspection only. Entries are appended immediately before
  // the corresponding dispatchThreadgroups call in dispatch_task_graph(), so
  // this observes real encoder arguments rather than Task-ring bytes.
  mutable std::vector<NodeAwareDispatchObservation> last_node_aware_dispatches;

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
                      const std::string& function_name = "vg_linear_compute") {
    const std::string_view ir_hash = compiled_msl.ir_hash;
    const std::string_view msl_source = compiled_msl.source;
    if (pipeline != nil && cached_ir_hash == ir_hash) return true;
    pipeline = nil;
    library = nil;
    cached_ir_hash.clear();

    NSError* compile_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    id<MTLLibrary> new_library = [device newLibraryWithSource:ns_utf8(msl_source)
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

  // Per-program cache: every immutable Node generation in one TaskGraph may
  // name a distinct program and all pipelines must coexist through submit.
  std::unordered_map<std::string, std::pair<id<MTLLibrary>, id<MTLComputePipelineState>>> node_pipelines;

  bool ensure_node_pipeline(const MslModule& compiled_msl,
                            id<MTLComputePipelineState>* out_pipeline, std::string* error,
                            const std::string& function_name = "vg_linear_compute",
                            bool* cache_hit = nullptr) {
    const std::string_view ir_hash = compiled_msl.ir_hash;
    const std::string_view msl_source = compiled_msl.source;
    const std::string cache_key = std::string(ir_hash) + "\n" + function_name;
    auto it = node_pipelines.find(cache_key);
    if (it != node_pipelines.end()) {
      if (cache_hit != nullptr) *cache_hit = true;
      *out_pipeline = it->second.second;
      return true;
    }
    NSError* compile_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    id<MTLLibrary> new_library = [device newLibraryWithSource:ns_utf8(msl_source)
                                                        options:options
                                                          error:&compile_error];
    if (new_library == nil) {
      if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                                : "unknown per-Node MSL compile error";
      return false;
    }
    id<MTLFunction> function =
        [new_library newFunctionWithName:[NSString stringWithUTF8String:function_name.c_str()]];
    if (function == nil) {
      if (error) *error = "per-Node MSL library missing " + function_name + " entry point";
      return false;
    }
    NSError* pipeline_error = nil;
    id<MTLComputePipelineState> new_pipeline = [device newComputePipelineStateWithFunction:function
                                                                                       error:&pipeline_error];
    if (new_pipeline == nil) {
      if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                 : "unknown per-Node pipeline creation error";
      return false;
    }
    node_pipelines.emplace(cache_key, std::make_pair(new_library, new_pipeline));
    if (cache_hit != nullptr) *cache_hit = false;
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
      // Start stale so the common copy below seeds a newly created buffer.
      MetalAllocationRecord record{buffer, allocation.id, allocation.generation, needed, 0};
      it = allocation_map.emplace(allocation.id, record).first;
    }
    if (it->second.content_epoch != allocation.content_epoch) {
      std::memcpy([it->second.buffer contents], allocation.bytes.data(), allocation.bytes.size());
      it->second.content_epoch = allocation.content_epoch;
    }
    return it->second.buffer;
  }

  // `created` is only observability for the submission report: it distinguishes
  // the one device-local allocation from subsequent legacy draws without
  // exposing the Metal object outside this adapter.
  id<MTLBuffer> make_identity_scene_root_buffer(bool* created = nullptr) {
    std::lock_guard<std::mutex> lock(identity_scene_root_mutex);
    if (identity_scene_root_buffer != nil) {
      if (created != nullptr) *created = false;
      return identity_scene_root_buffer;
    }
    identity_scene_root_buffer = [device newBufferWithLength:VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE
                                                     options:MTLResourceStorageModeShared];
    if (identity_scene_root_buffer == nil) return nil;
    std::memset([identity_scene_root_buffer contents], 0, VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE);
    auto* matrix = static_cast<float*>([identity_scene_root_buffer contents]);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    if (created != nullptr) *created = true;
    return identity_scene_root_buffer;
  }

  // Commit a completed GPU buffer write to the canonical F7 byte store and
  // stamp the retained Shared mirror at the same content revision.
  void commit_buffer_write(core::Allocation& allocation, id<MTLBuffer> buffer) {
    if (buffer == nil || allocation.bytes.empty()) return;
    std::memcpy(allocation.bytes.data(), [buffer contents], allocation.bytes.size());
    ++allocation.content_epoch;
    auto it = allocation_map.find(allocation.id);
    if (it != allocation_map.end() && it->second.generation == allocation.generation)
      it->second.content_epoch = allocation.content_epoch;
  }


  static uint64_t facet_cache_key(core::FacetRef ref) {
    return (uint64_t(ref.index) << 32) | uint64_t(ref.generation);
  }

  // Shared prologue for every facet use: resolve the capability token, reject
  // a kind mismatch, and produce the diagnostic the FacetPool itself
  // classified rather than a generic "stale" string.
  static const core::FacetSlot* resolve_facet(const core::Arena& arena, const core::FacetPool& pool,
                                       core::FacetRef ref, core::FacetKind expected_kind,
                                       std::string* error) {
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
        arena.lookup(core::PointerRef{slot->view.allocation, slot->view.allocation_generation});
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
    out->resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        (*out)[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
            decode_texel(bytes.data() + static_cast<size_t>(y) * row_bytes +
                             static_cast<size_t>(x) * kBytesPerTexel,
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
        const uint64_t offset = view.subresource_byte_offset({layer, level});
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
    const core::Allocation* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
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
      // A host write changes bytes, not the facet contract. Refresh the
      // existing Shared texture rather than invalidating the capability.
      if (!allocation->bytes.empty() && it->second.content_epoch != allocation->content_epoch) {
        upload_view_subresources(it->second.storage_texture, view, *allocation);
        it->second.content_epoch = allocation->content_epoch;
      }
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
    auto fresh = facet_map.find(key);
    if (fresh != facet_map.end()) fresh->second.content_epoch = allocation->content_epoch;
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
  id<MTLSamplerState> ensure_sampler_state(core::FilterMode filter, core::WrapMode wrap, LodClamp lod) {
    std::array<uint32_t, 4> key{static_cast<uint32_t>(filter), static_cast<uint32_t>(wrap), 0, 0};
    std::memcpy(&key[2], &lod.min, sizeof(float));
    std::memcpy(&key[3], &lod.max, sizeof(float));
    auto it = sampler_cache.find(key);
    if (it != sampler_cache.end()) return it->second;
    MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
    const bool nearest = filter == core::FilterMode::Nearest;
    const MTLSamplerMinMagFilter mtl_filter =
        nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    descriptor.minFilter = mtl_filter;
    descriptor.magFilter = mtl_filter;
    descriptor.mipFilter = nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    descriptor.lodMinClamp = lod.min;
    descriptor.lodMaxClamp = lod.max;
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
                        core::TimelineGate gate, DispatchStats* stats, std::string* error) const {
    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    if (gate.wait != 0) [command_buffer encodeWaitForEvent:timeline_event value:gate.wait];
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
    if (gate.signal != 0) [command_buffer encodeSignalEvent:timeline_event value:gate.signal];
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
  // capability snapshot's own gpu_addresses bit -- cached after the first
  // call since this answer cannot change within a process lifetime.
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
  bool dispatch_indexed_and_wait(const std::vector<id<MTLBuffer>>& object_buffers, core::TimelineGate gate,
                                DispatchStats* stats, std::string* error) const {
    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    if (gate.wait != 0) [command_buffer encodeWaitForEvent:timeline_event value:gate.wait];

    const size_t table_bytes = std::max<size_t>(object_buffers.size() * sizeof(uint64_t), sizeof(uint64_t));
    id<MTLBuffer> table_buffer = [device newBufferWithLength:table_bytes options:MTLResourceStorageModeShared];
    if (table_buffer == nil) { if (error) *error = "failed to allocate indexed binding table buffer"; return false; }
    auto* table = static_cast<uint64_t*>([table_buffer contents]);
    for (size_t index = 0; index < object_buffers.size(); ++index) table[index] = [object_buffers[index] gpuAddress];

    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
    [encoder setComputePipelineState:pipeline];
    for (id<MTLBuffer> buffer : object_buffers)
      [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [encoder setBuffer:table_buffer offset:0 atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    if (gate.signal != 0) [command_buffer encodeSignalEvent:timeline_event value:gate.signal];
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
                             std::string* error) const {
    if (submitted != nullptr) *submitted = false;
    if (pipeline == nil) {
      if (error) *error = "Metal schedule step has no per-Node compute pipeline";
      return false;
    }
    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
    [encoder setComputePipelineState:pipeline];
    for (size_t index = 0; index < buffers.size(); ++index)
      [encoder setBuffer:buffers[index] offset:0 atIndex:index];
    last_node_aware_dispatches.push_back(
        {task_index, task.node_index, task.node_generation,
         {task.x, task.y, task.z}, pipeline_ordinal});
    [encoder dispatchThreadgroups:MTLSizeMake(task.x, task.y, task.z)
             threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];

    const auto submit_start = std::chrono::steady_clock::now();
    [command_buffer commit];
    if (submitted != nullptr) *submitted = true;
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
                                              : "Metal scheduled compute command buffer failed";
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
  bool dispatch_task_publish(TaskRingBuffers buffers, uint32_t count, DispatchStats* stats, std::string* error) const {
    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
    [encoder setComputePipelineState:task_ring_pipeline];
    [encoder setBuffer:buffers.state offset:0 atIndex:0];
    [encoder setBuffer:buffers.fields offset:0 atIndex:1];
    [encoder setBuffer:buffers.inputs offset:0 atIndex:2];
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
  [[nodiscard]] std::string target_identity() const {
    return std::string([[device name] UTF8String]) + "|" +
           [[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String] + "|MSL";
  }

  [[nodiscard]] compiler::PipelineKey make_pipeline_key(const ShaderEntry& shader,
                                          std::vector<std::pair<std::string, uint64_t>> constants,
                                          std::vector<uint32_t> attachment_formats,
                                          uint32_t sample_count) const {
    compiler::PipelineKey key;
    key.code_object_hash = ir::sha256_hex(shader.source);
    key.entry = std::string(shader.entry);
    key.function_constants = std::move(constants);
    key.attachment_formats = std::move(attachment_formats);
    key.sample_count = sample_count;
    key.target_identity = target_identity();
    return key;
  }

  // One MTLLibrary per distinct MSL text, so two specializations of the same
  // source share the compiled library and differ only in the function constant
  // values applied to it.
  id<MTLLibrary> ensure_library(const LibraryText& text, std::string* error) {
    auto it = library_by_hash.find(std::string(text.hash));
    if (it != library_by_hash.end()) return it->second;
    NSError* compile_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    id<MTLLibrary> library_object =
        [device newLibraryWithSource:ns_utf8(text.source)
                             options:options
                               error:&compile_error];
    if (library_object == nil) {
      if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                                : "unknown MSL compile error";
      return nil;
    }
    library_by_hash.emplace(std::string(text.hash), library_object);
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
  static id<MTLFunction> ensure_function(id<MTLLibrary> library_object, const compiler::PipelineKey& key,
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
          id<MTLLibrary> library_object = ensure_library({source, key.code_object_hash}, create_error);
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
                               MTLPixelFormat depth_format,
                               const std::string& trigger, id<MTLRenderPipelineState>* out,
                               compiler::SpecializationReport* report, std::string* error) {
    const uint64_t digest = key.hash();
    compiler::SpecializationReport local;
    id<MTLRenderPipelineState> created = nil;
    const bool ok = cache.acquire(
        key, trigger,
        [&](uint64_t* binary_size, std::string* create_error) {
          id<MTLLibrary> library_object = ensure_library({source, key.code_object_hash}, create_error);
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
          descriptor.depthAttachmentPixelFormat = depth_format;
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

  bool ensure_depth_stencil_state(const compiler::PipelineKey& key, bool test_enable,
                                  bool write_enable, core::DepthCompareOp compare_op,
                                  id<MTLDepthStencilState>* out, bool* cache_hit,
                                  std::string* error) {
    const uint64_t digest = key.hash();
    auto found = depth_stencil_by_key.find(digest);
    if (found != depth_stencil_by_key.end()) {
      if (out) *out = found->second;
      if (cache_hit) *cache_hit = true;
      return true;
    }
    MTLDepthStencilDescriptor* descriptor = [MTLDepthStencilDescriptor new];
    // Metal ignores compare/write only if there is no depth attachment.  We
    // bind one on every F4 pass, and keep disabled testing semantically exact:
    // Always plus writes disabled means the attachment is not modified.
    descriptor.depthCompareFunction = test_enable ? to_mtl_compare_function(compare_op)
                                                   : MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = write_enable;
    id<MTLDepthStencilState> created = [device newDepthStencilStateWithDescriptor:descriptor];
    if (created == nil) {
      if (error) *error = "Metal depth-stencil state creation failed";
      return false;
    }
    depth_stencil_by_key.emplace(digest, created);
    if (out) *out = created;
    if (cache_hit) *cache_hit = false;
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
    const compiler::PipelineKey key = make_pipeline_key({source, entry}, constants, {}, 1);
    return acquire_compute_pipeline(pipeline_cache, compute_pipeline_by_key, source, key,
                                    checked ? "checked-profile facet generation guard (06 §6.4)"
                                            : "fast-native SampleFacet kernel",
                                    out, nullptr, error);
  }

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
                              const ir::UserRasterShaderContract* user_shader = nullptr) {
    const std::string source =
        user_shader != nullptr ? user_shader->source : compiler::raster_facet_metal_source();
    const std::string vertex_entry = user_shader != nullptr ? user_shader->vertex_entry : "vg_raster_vertex";
    const std::string fragment_entry = user_shader != nullptr ? user_shader->fragment_entry : "vg_raster_fragment";
    const compiler::PipelineKey key = make_pipeline_key(
        {source, fragment_entry}, {},
        has_depth ? std::vector<uint32_t>{static_cast<uint32_t>(format), static_cast<uint32_t>(depth_format)}
                  : std::vector<uint32_t>{static_cast<uint32_t>(format)}, sample_count);
    // These are immutable MTLDepthStencilState choices.  They intentionally
    // participate in the classified key even though the state object is
    // distinct from MTLRenderPipelineState.
    compiler::PipelineKey keyed = key;
    keyed.raster_state = {{"depth_test_enable", depth_test_enable ? 1u : 0u},
                          {"depth_write_enable", depth_write_enable ? 1u : 0u},
                          {"depth_compare_op", static_cast<uint64_t>(depth_compare_op)}};
    if (!acquire_render_pipeline(pipeline_cache, render_pipeline_by_key, source, keyed, vertex_entry,
                                 to_mtl_pixel_format(format),
                                 has_depth ? to_mtl_pixel_format(depth_format) : MTLPixelFormatInvalid,
                                 user_shader != nullptr
                                     ? "restricted-import user MSL raster shader (ADR-043 Decision #4)"
                                     : "F4 depth raster attachment store",
                                 out, nullptr, error))
      return false;
    return !has_depth || ensure_depth_stencil_state(keyed, depth_test_enable, depth_write_enable, depth_compare_op,
                                                    depth_out, depth_cache_hit, error);
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
                       bool* command_submitted = nullptr) {
    if (command_submitted != nullptr) *command_submitted = false;
    if (result == nullptr) { if (error) *error = "raster result output is required"; return false; }
    const uint32_t primitive_count = index_buffer != nil ? index_count : vertex_count;
    if (vertex_count == 0 || primitive_count == 0 || primitive_count % 3 != 0) {
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
    FacetUseGuard source_use(pool, facets.source);
    if (!source_use.begin(arena, error)) return false;
    FacetUseGuard target_use(pool, facets.target);
    if (!target_use.begin(arena, error)) return false;
    // A facet at slot zero is valid. Match core/reference's presence test so
    // a malformed `{nonzero index, zero generation}` never silently disables
    // depth on Metal while other backends reject it as a stale capability.
    const bool has_depth = desc.depth_attachment_ref.index != 0 || desc.depth_attachment_ref.generation != 0;
    std::unique_ptr<FacetUseGuard> depth_use;
    if (has_depth) {
      depth_use = std::make_unique<FacetUseGuard>(pool, desc.depth_attachment_ref);
      if (!depth_use->begin(arena, error)) return false;
    }

    const core::FacetSlot* source_slot =
        resolve_facet(arena, pool, facets.source, core::FacetKind::Sample, error);
    if (source_slot == nullptr) return false;
    const core::FacetSlot* target_slot =
        resolve_facet(arena, pool, facets.target, core::FacetKind::Attachment, error);
    if (target_slot == nullptr) return false;
    const core::FacetSlot* depth_slot = nullptr;
    if (has_depth) {
      depth_slot = resolve_facet(arena, pool, desc.depth_attachment_ref, core::FacetKind::Attachment, error);
      if (depth_slot == nullptr) return false;
    }
    const core::CanonicalView& source_view = source_slot->view;
    const core::CanonicalView& target_view = target_slot->view;
    const core::CanonicalView* depth_view = has_depth ? &depth_slot->view : nullptr;
    // F4's fixed fragment contract samples an RGBA8 source into one RGBA8
    // color attachment. Keep this aligned with the Reference oracle instead
    // of letting Metal's texture2d<float> accept R32Float as a divergent
    // one-channel interpretation.
    if (source_view.format != core::PixelFormat::RGBA8Unorm) {
      if (error) *error = "raster source must use PixelFormat::RGBA8Unorm";
      return false;
    }
    if (has_depth && depth_view->format != core::PixelFormat::Depth32Float) {
      if (error) *error = "raster depth attachment must use PixelFormat::Depth32Float";
      return false;
    }
    if (target_view.format != core::PixelFormat::RGBA8Unorm) {
      if (error) *error = "F4 raster color attachment must use PixelFormat::RGBA8Unorm";
      return false;
    }
    if (has_depth && (target_view.width != depth_view->width || target_view.height != depth_view->height ||
        target_view.array_layers != depth_view->array_layers || target_view.mip_levels != depth_view->mip_levels ||
        target_view.dimension != depth_view->dimension)) {
      if (error) *error = "raster color and depth attachment views must have identical dimensions, layers, and mips";
      return false;
    }
    if (has_depth && desc.attachment.sample_count != 1) {
      if (error) *error = "F4 depth raster supports only single-sample attachments";
      return false;
    }
    if (!subresource_in_range(target_view, desc.attachment.subresource, error) ||
        (has_depth && !subresource_in_range(*depth_view, desc.attachment.subresource, error)))
      return false;
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
    id<MTLTexture> source_texture = ensure_facet_texture(arena, pool, facets.source,
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
        ensure_sampler_state(desc.filter, desc.wrap, {.min = desc.source_lod, .max = desc.source_lod});
    if (sampler == nil) { if (error) *error = "Metal raster sampler creation failed"; return false; }

    bool target_cache_hit = false;
    id<MTLTexture> target_texture = ensure_facet_texture(arena, pool, facets.target,
                                                          core::FacetKind::Attachment,
                                                          &target_cache_hit, nullptr, &tex_error);
    if (target_texture == nil) {
      if (error) *error = tex_error.empty() ? "Metal raster target texture creation failed" : tex_error;
      return false;
    }
    bool depth_cache_hit = false;
    id<MTLTexture> depth_texture = nil;
    if (has_depth)
      depth_texture = ensure_facet_texture(arena, pool, desc.depth_attachment_ref,
                                           core::FacetKind::Attachment,
                                           &depth_cache_hit, nullptr, &tex_error);
    if (has_depth && depth_texture == nil) {
      if (error) *error = tex_error.empty() ? "Metal raster depth texture creation failed" : tex_error;
      return false;
    }

    // Attachment format and sample count are compiled into the pipeline and are
    // therefore key state (06 §7); the viewport set below and the tint bound at
    // fragment buffer(0) are not, and must not enlarge the key.
    std::string pipeline_error;
    id<MTLRenderPipelineState> pipeline_state = nil;
    id<MTLDepthStencilState> depth_state = nil;
    bool depth_state_cache_hit = false;
    if (!ensure_raster_pipeline(target_view.format,
                                has_depth ? depth_view->format : core::PixelFormat::RGBA8Unorm,
                                has_depth,
                                desc.attachment.sample_count,
                                desc.depth_test_enable, desc.depth_write_enable, desc.depth_compare_op,
                                &pipeline_state, &depth_state, &depth_state_cache_hit,
                                &pipeline_error, user_shader)) {
      if (error) *error = "Metal raster pipeline compile failed: " + pipeline_error;
      return false;
    }

    const uint32_t level = desc.attachment.subresource.level;
    const uint32_t width = target_view.mip_width(level);
    const uint32_t height = target_view.mip_height(level);
    bool store_traffic_avoided = false;
    MTLRenderPassDescriptor* rp = make_render_pass(target_texture, desc.attachment, target_view,
                                                   &store_traffic_avoided, error);
    if (rp == nil) return false;
    // F4 has a deliberately fixed depth attachment policy: no load of old
    // depth, clear every task to the far plane, and preserve the resulting
    // depth texture for subsequent inspection/use.
    if (has_depth) {
      MTLRenderPassDepthAttachmentDescriptor* depth = rp.depthAttachment;
      depth.texture = depth_texture;
      depth.level = level;
      depth.slice = desc.attachment.subresource.layer;
      depth.loadAction = MTLLoadActionClear;
      depth.storeAction = MTLStoreActionStore;
      depth.clearDepth = 1.0;
    }

    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:rp];
    if (encoder == nil) { if (error) *error = "failed to create Metal render encoder"; return false; }
    [encoder setRenderPipelineState:pipeline_state];
    if (has_depth) [encoder setDepthStencilState:depth_state];
    // Dynamic state, deliberately: a viewport change must not compile a second
    // pipeline (06 §7's "小的动态状态不应无故扩大 key").
    [encoder setViewport:(MTLViewport){0.0, 0.0, static_cast<double>(width), static_cast<double>(height),
                                       0.0, 1.0}];
    [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:compiler::kRasterVertexBufferIndex];
    [encoder setVertexBuffer:scene_root_buffer offset:0 atIndex:compiler::kRasterSceneRootBufferIndex];
    [encoder setFragmentTexture:source_texture atIndex:compiler::kRasterTextureIndex];
    [encoder setFragmentSamplerState:sampler atIndex:compiler::kRasterSamplerIndex];
    [encoder setFragmentBuffer:tint_buffer offset:0 atIndex:compiler::kRasterTintBufferIndex];
    if (index_buffer != nil) {
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:index_count indexType:index_type
                         indexBuffer:index_buffer indexBufferOffset:0];
    } else {
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertex_count];
    }
    [encoder endEncoding];
    [command_buffer commit];
    if (command_submitted != nullptr) *command_submitted = true;
    [command_buffer waitUntilCompleted];
    result->encoder_count = 1;
    result->report = make_facet_report();
    result->report.encoder_count = 1;
    result->report.command_buffer_count = 1;
    result->report.queue_wait_count = 1;
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
    if (!read_texture_region(target_texture, desc.attachment.subresource.layer, level, 0, 0, width,
                             height, &result->resolved_rgba, error))
      return false;
    if (has_depth) {
      std::vector<std::array<float, 4>> depth_rgba;
      if (!read_texture_region(depth_texture, desc.attachment.subresource.layer, level, 0, 0, width,
                               height, &depth_rgba, error)) return false;
      result->resolved_depth.reserve(depth_rgba.size());
      for (const auto& value : depth_rgba) result->resolved_depth.push_back(value[0]);
    }

    // F7 makes the canonical Arena bytes the public readback source. Commit
    // the completed GPU attachment(s) there before submit returns; cached
    // textures are then stamped with the same content epoch so a later use
    // does not upload stale pre-draw bytes over the result.
    auto commit_rgba = [&](core::FacetRef ref, const core::CanonicalView& view,
                           const std::vector<std::array<float, 4>>& pixels) {
      auto* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
      if (allocation == nullptr || view.format != core::PixelFormat::RGBA8Unorm) return;
      const uint64_t base = view.subresource_byte_offset({desc.attachment.subresource.layer, level});
      for (size_t i = 0; i < pixels.size(); ++i) {
        uint8_t* out = allocation->bytes.data() + base + i * 4;
        for (size_t c = 0; c < 4; ++c)
          out[c] = static_cast<uint8_t>(std::clamp(pixels[i][c], 0.0f, 1.0f) * 255.0f + 0.5f);
      }
      arena.mark_content_modified(*allocation);
      auto record = facet_map.find(facet_cache_key(ref));
      if (record != facet_map.end()) record->second.content_epoch = allocation->content_epoch;
    };
    commit_rgba(facets.target, target_view, result->resolved_rgba);
    if (has_depth) {
      auto* allocation = arena.lookup(core::PointerRef{depth_view->allocation, depth_view->allocation_generation});
      if (allocation != nullptr) {
        const uint64_t base = depth_view->subresource_byte_offset({desc.attachment.subresource.layer, level});
        for (size_t i = 0; i < result->resolved_depth.size(); ++i)
          std::memcpy(allocation->bytes.data() + base + i * sizeof(float), &result->resolved_depth[i], sizeof(float));
        arena.mark_content_modified(*allocation);
        auto record = facet_map.find(facet_cache_key(desc.depth_attachment_ref));
        if (record != facet_map.end()) record->second.content_epoch = allocation->content_epoch;
      }
    }

    result->width = width;
    result->height = height;
    result->sample_count = desc.attachment.sample_count;
    result->covered_fragment_count = 0;
    result->stored = stored;
    // A DontCare load leaves the previous bytes visible and a DontCare store
    // leaves memory untouched; in both cases the contract does not define what a
    // reader sees, so the values returned must not be used as an expectation.
    result->contents_defined = stored && desc.attachment.load != AttachmentLoadAction::DontCare;
    result->facet_cache_hit = source_cache_hit && target_cache_hit && (!has_depth || depth_cache_hit);
    result->report.add("raster_attachment_store", hal::LoweringClass::Direct, vertex_count / 3, 0,
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
    if (has_depth)
      result->report.add("raster_depth_state",
                         depth_state_cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::DevicePass,
                         1, 0, "real MTLDepthStencilState bound with F4 depth compare/write policy");
    return true;
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
                                    core::FacetRef target_facet, TransformCost* cost, std::string* error) {
    const core::Allocation* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
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
          const uint64_t offset = view.subresource_byte_offset({layer, level});
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
      if (cost != nullptr) {
        cost->encoder_count = 1;
        cost->command_buffer_count = 1;
        cost->queue_wait_count = 1;
      }
      if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
        if (error)
          *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                                : "representation transform blit failed";
        return false;
      }
    }
    // The TransferFacet existed only to give the blit a pool-resolved source;
    // its purpose is spent, so its slot goes back rather than being left to
    // linger until some later epoch happens to stale it.
    pool.retire(transfer_ref);

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
      cost->command_buffer_count = 1;
      cost->queue_wait_count = 1;
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
                                    DispatchStats* stats, std::string* error) const {
    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    const size_t stride = sizeof(MTLDispatchThreadgroupsIndirectArguments);
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    if (blit == nil) { if (error) *error = "failed to create Metal blit encoder"; return false; }
    for (size_t i = 0; i < order.size(); ++i) {
      const size_t src_offset =
          (static_cast<size_t>(order[i]) * compiler::kTaskRingWordsPerRecord +
           compiler::kTaskRingDispatchXWord) * sizeof(uint32_t);
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
  return std::ranges::any_of(module.instructions, [](const ir::Instruction& i) {
    return i.op == "load_ref" || i.op == "load_via" || i.op == "store_via";
  });
}

// Stage 5 consume releases the allocation's linear representation before
// Stage 6/7 dispatch (03 §7). A plan that also computes over that same
// allocation is asking for two incompatible things.
bool plan_computes_over_allocation(const core::ExecutionPlan& plan, uint64_t allocation) {
  return std::ranges::any_of(plan.task_effects, [allocation](const auto& effects) {
    return std::ranges::any_of(effects, [allocation](const ir::Effect& effect) {
      return effect.allocation == allocation;
    });
  });
}
}  // namespace

DeviceHal::DeviceHal(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DeviceHal::~DeviceHal() = default;

void DeviceHal::reclaim_released_backing(const core::Arena& arena) const {
  impl_->reclaim_released_backing(arena, facet_pool(), nullptr, nullptr);
}

const hal::CapabilitySnapshot& DeviceHal::capabilities() const { return impl_->snapshot.hal; }
const DeviceSnapshot& DeviceHal::snapshot() const { return impl_->snapshot; }

struct DeviceHal::CompileOps {
  static void init(hal::CompiledPlan* compiled, const core::ExecutionPlan& plan) {
    compiled->abi_version = hal::kDeviceHalAbiVersion;
    compiled->plan = plan;
    compiled->report = {};
    compiled->report.backend = hal::BackendKind::Metal;
  }

  static bool fail(hal::CompiledPlan* compiled, const char* operation, std::string diagnostic,
                   std::string* error, uint64_t count = 1, uint64_t bytes = 0, const char* reason = nullptr) {
    compiled->report.supported = false;
    compiled->report.diagnostic = std::move(diagnostic);
    compiled->report.add(operation, hal::LoweringClass::Unsupported, count, bytes,
                         reason != nullptr ? std::string(reason) : compiled->report.diagnostic);
    if (error) *error = compiled->report.diagnostic;
    return false;
  }

  static bool reject_unsupported(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
                                 std::string* error) {
    if (plan.requested_certificate_mode == core::AccessCertificateMode::SoftwarePaged ||
        plan.requested_certificate_mode == core::AccessCertificateMode::FaultManaged) {
      init(compiled, plan);
      return fail(compiled, "access_certificate",
                  "requested access certificate mode is not implemented on this backend", error);
    }
    return true;
  }

  static bool select_packages(DeviceHal& metal, const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
                              std::string* error) {
    init(compiled, plan);
    for (const auto& node : plan.resolved_nodes) {
      hal::CompiledPlan::PerNodePackage per_node;
      per_node.ref = node.ref;
      const auto task = std::ranges::find_if(plan.task_graph.tasks(), [&](const auto& candidate) {
        return candidate.node_index == node.ref.index &&
               candidate.node_generation == node.ref.generation;
      });
      if (task == plan.task_graph.tasks().end())
        return fail(compiled, "node_package", "resolved Node has no Task", error);
      if (task->kind == core::TaskKind::Raster) {
        per_node.kind = hal::CompiledPlan::NodePackageKind::Raster;
        compiled->per_node_packages.push_back(std::move(per_node));
        if (node.user_raster_shader.has_value()) {
          compiled->report.add("raster_user_shader", hal::LoweringClass::HostAssisted, 1,
                               node.user_raster_shader->source.size(),
                               "caller-declared effect contract accepted; shader logic not independently verified");
        } else {
          compiled->report.add("node_raster_package", hal::LoweringClass::Direct, 1, 0,
                               "canonical NodeRef materialized as the built-in Metal raster contract");
        }
        continue;
      }
      if (!node.module.has_value())
        return fail(compiled, "node_package", "resolved Node has no materialized program", error);
      const bool pointer_graph = is_pointer_graph_module(*node.module);
      auto package = pointer_graph ? compiler::build_pointer_graph_compute_package(*node.module)
                                   : compiler::build_linear_compute_package(*node.module);
      if (!package.ok)
        return fail(compiled, "node_package", "per-Node package compilation failed: " + package.message, error);
      per_node.kind = hal::CompiledPlan::NodePackageKind::CanonicalCompute;
      per_node.package = std::move(package.package);
      id<MTLComputePipelineState> pipeline = nil;
      bool cache_hit = false;
      std::string pipeline_error;
      const char* entry = pointer_graph ? "vg_pointer_graph_compute" : "vg_linear_compute";
      if (!metal.impl_->ensure_node_pipeline({per_node.package->canonical_ir_hash,
                                              per_node.package->metal_source},
                                             &pipeline, &pipeline_error, entry, &cache_hit)) {
        const bool has_atomic = std::ranges::any_of(node.module->instructions,
                                                    [](const ir::Instruction& i) { return i.op == "atomic_add"; });
        if (!has_atomic)
          return fail(compiled, "metal_pipeline", "Metal per-Node pipeline compilation failed: " + pipeline_error,
                      error, 1, 0, pipeline_error.c_str());
        per_node.host_assisted = true;
        compiled->report.add("metal_pipeline", hal::LoweringClass::HostAssisted, 1, 0,
                             "native 64-bit atomic compile failed for Node; host execution: " + pipeline_error);
      } else {
        compiled->report.add("metal_pipeline",
                             cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::Direct,
                             1, 0, cache_hit ? "per-Node MTLComputePipelineState cache hit"
                                             : "per-Node MTLComputePipelineState compiled");
      }
      compiled->report.add("node_compute_package",
                           pointer_graph ? hal::LoweringClass::CachedObject : hal::LoweringClass::Direct,
                           1, per_node.package->bindings.size(),
                           pointer_graph ? "NodeRef-keyed CachedObject package" : "NodeRef-keyed linear package");
      compiled->per_node_packages.push_back(std::move(per_node));
    }
    const bool any_host_assisted = std::ranges::any_of(
        compiled->per_node_packages, [](const auto& package) { return package.host_assisted; });
    const bool any_native_compute = std::ranges::any_of(
        compiled->per_node_packages, [](const auto& package) {
          return package.kind == hal::CompiledPlan::NodePackageKind::CanonicalCompute &&
                 !package.host_assisted;
        });
    if (any_host_assisted && any_native_compute)
      return fail(compiled, "node_compute_package",
                  "Metal mixed native and host-assisted per-Node compute lowering is Unsupported",
                  error);
    const bool has_compute = std::ranges::any_of(plan.task_graph.tasks(), [](const auto& task) {
      return task.kind == core::TaskKind::Compute;
    });
    const bool has_raster = std::ranges::any_of(plan.task_graph.tasks(), [](const auto& task) {
      return task.kind == core::TaskKind::Raster;
    });
    if (has_compute && has_raster &&
        std::ranges::any_of(plan.resolved_nodes, [](const auto& node) {
          return node.execution_domain == core::TaskKind::Raster && node.user_raster_shader.has_value();
        }))
      return fail(compiled, "mixed_domain_user_raster_shader",
                  "Metal Unsupported: restricted user raster shaders cannot participate in a native mixed-domain ExecutionSchedule",
                  error);
    return true;
  }

  static bool representation_requests(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
                                     std::string* error) {
    for (size_t index = 0; index < plan.representation_plan.size(); ++index) {
      const auto& request = plan.representation_plan[index];
      std::string request_error;
      if (!view_expressible(request.view, request.target_kind, request.view.byte_size(), &request_error)) {
        return fail(compiled, "representation_transform",
                    "representation request " + std::to_string(index) +
                        " is not expressible on this Metal device: " + request_error,
                    error, 1, request.view.byte_size());
      }
      if (request.consume_input && plan_computes_over_allocation(plan, request.view.allocation)) {
        return fail(compiled, "consume_input",
                    "representation request " + std::to_string(index) +
                        " is Unsupported: it asks for ConsumeInput on allocation " +
                        std::to_string(request.view.allocation) +
                        ", whose linear representation this plan's compute module also reads or writes; the "
                        "consume releases that backing before the dispatch could run",
                    error);
      }
      compiled->report.add("representation_transform", hal::LoweringClass::DevicePass, 1,
                           request.view.byte_size(),
                           "blit every subresource of the linear backing into a Private device-optimal "
                           "MTLTexture and publish a new RepresentationEpoch at submit()");
      compiled->representation_operations.push_back({hal::CompiledPlan::RepresentationOperation::CopyToPrivate,
                                                     request.transform_order, "Metal private texture copy"});
      if (request.consume_input)
        compiled->report.add("consume_input", hal::LoweringClass::Direct, 1, 0,
                             "recorded for submit(): the Private texture is storage distinct from the linear "
                             "backing it supersedes, so a complete ConsumeProof can release that backing at "
                             "once instead of holding it to command-buffer completion (06 §11)");
    }
    return true;
  }

  static bool timeline(DeviceHal& metal, const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
                       std::string* error) {
    if ((plan.timeline_wait != 0 || plan.timeline_signal != 0) && !metal.impl_->snapshot.supports_shared_events) {
      return fail(compiled, "timeline", "timeline requested but device does not support MTLSharedEvent", error);
    }
    return true;
  }

  static void execution_schedule(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled) {
    uint64_t wave_count = 0;
    for (const auto& component : plan.execution_schedule.components)
      wave_count += component.waves.size();
    const auto& tasks = plan.task_graph.tasks();
    const auto submits_device_command = [&](uint32_t task_index) {
      if (task_index >= tasks.size()) return false;
      const auto& task = tasks[task_index];
      if (task.kind == core::TaskKind::Raster) return true;
      const auto package = std::ranges::find_if(
          compiled->per_node_packages, [&](const auto& candidate) {
            return candidate.ref.index == task.node_index &&
                   candidate.ref.generation == task.node_generation;
          });
      return package != compiled->per_node_packages.end() &&
             package->kind == hal::CompiledPlan::NodePackageKind::CanonicalCompute &&
             !package->host_assisted;
    };
    const bool has_device_commands = std::ranges::any_of(
        plan.execution_schedule.task_order, submits_device_command);
    std::vector<uint8_t> representation_owned(plan.representation_plan.size());
    for (auto& transition : compiled->transition_operations) {
      transition.state = hal::CompiledPlan::TransitionLoweringState::Lowered;
      uint64_t representation_steps = 0;
      for (uint32_t operation : transition.representation_operations) {
        if (operation < representation_owned.size() && representation_owned[operation] == 0) {
          representation_owned[operation] = 1;
          ++representation_steps;
        }
      }
      // MD-4's deliberately conservative implementation completes and host-
      // waits every producer-wave device command before beginning the
      // consumer wave. Host-assisted Compute tasks execute synchronously and
      // therefore contribute no fictional Metal encoder or queue wait.
      transition.encoder_boundary_count = representation_steps;
      transition.host_wait_count = representation_steps;
      if (transition.covers_execution_completion &&
          transition.component < plan.execution_schedule.components.size()) {
        const auto& component =
            plan.execution_schedule.components[transition.component];
        if (transition.before_wave < component.waves.size()) {
          const uint64_t producer_device_commands = std::ranges::count_if(
              component.waves[transition.before_wave].tasks,
              submits_device_command);
          transition.encoder_boundary_count += producer_device_commands;
          transition.host_wait_count += producer_device_commands;
        }
      }
      transition.serialized_fallback = transition.covers_execution_completion;
      compiled->report.transition_encoder_boundary_count += transition.encoder_boundary_count;
      compiled->report.transition_host_wait_count += transition.host_wait_count;
      if (transition.serialized_fallback)
        ++compiled->report.transition_serialized_fallback_count;
    }
    compiled->report.add("execution_schedule", hal::LoweringClass::Serialized,
                         plan.task_graph.tasks().size(), 0,
                         std::string(has_device_commands
                             ? "Metal consumes Core-sealed components/waves and conservatively completes each device command before the next schedule step; "
                             : "Metal consumes Core-sealed components/waves in the host interpreter; ") +
                             std::to_string(plan.execution_schedule.components.size()) + " component(s), " +
                             std::to_string(wave_count) + " wave(s)");
  }

  static bool pipelines(DeviceHal& metal, const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
                        std::string* error) {
    (void)metal;
    (void)error;
    compiled->report.supported = true;
    const uint64_t compute_tasks = std::ranges::count_if(plan.task_graph.tasks(), [](const auto& task) {
      return task.kind == core::TaskKind::Compute;
    });
    const bool compute_only = compute_tasks == plan.task_graph.tasks().size();
    compiled->report.add("task_publication",
                         compute_only ? hal::LoweringClass::Direct : hal::LoweringClass::HostAssisted,
                         compute_only ? compute_tasks : plan.task_graph.tasks().size(), 0,
                         compute_only
                             ? "compute-only Metal task ring publication in canonical schedule order"
                             : "complete cross-domain canonical publication is host-side; Raster Tasks are never packed into the compute ring");
    if (plan.timeline_signal != 0 || plan.timeline_wait != 0)
      compiled->report.add("timeline", hal::LoweringClass::HostAssisted, 1, 0,
                           "submission-wide host observation/signal of MTLSharedEvent around the sealed schedule");
    execution_schedule(plan, compiled);
    return true;
  }
};

bool DeviceHal::compile(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled, std::string* error) {
  if (compiled == nullptr) {
    if (error) *error = "compiled plan output is required";
    return false;
  }
  *compiled = {};
  if (!plan.validate(error)) return false;
  if (!hal::preflight_stage6(plan, capabilities(), hal::BackendKind::Metal, compiled, error)) return false;
  if (!CompileOps::reject_unsupported(plan, compiled, error)) return false;
  if (!CompileOps::select_packages(*this, plan, compiled, error)) return false;
  if (!CompileOps::representation_requests(plan, compiled, error)) return false;
  if (!CompileOps::timeline(*this, plan, compiled, error)) return false;
  return CompileOps::pipelines(*this, plan, compiled, error);
}


struct DeviceHal::SubmitOps {
  enum class Flow { Fail, Finish, Continue };

  static bool take(Flow flow, bool* result) {
    if (flow == Flow::Continue) return false;
    *result = (flow == Flow::Finish);
    return true;
  }

  static void apply_stats(const DispatchStats& stats, hal::Submission* submission) {
    submission->cpu_encode_ns += stats.cpu_encode_ns;
    submission->cpu_submit_ns += stats.cpu_submit_ns;
    submission->report.encoder_count += stats.encoder_count;
    submission->report.command_buffer_count += stats.command_buffer_count;
    submission->report.barrier_count += stats.barrier_count;
    submission->report.queue_wait_count += stats.queue_wait_count;
  }

  static std::map<uint64_t, std::pair<uint32_t, uint32_t>> generations(const ir::Module& module) {
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> map;
    for (const auto& instruction : module.instructions)
      map.emplace(instruction.allocation,
                  std::make_pair(instruction.generation, instruction.representation_epoch));
    return map;
  }

  static bool bind(DeviceHal& metal, core::Arena& arena, uint64_t allocation, uint32_t generation,
                   uint32_t epoch, id<MTLBuffer>* buffer, core::Allocation** touched,
                   hal::Submission* submission) {
    *touched = arena.lookup(core::RepresentationRef{allocation, generation, epoch});
    if (*touched == nullptr) {
      submission->result.ok = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = "stale generation, representation epoch, or out-of-bounds allocation reference";
      return false;
    }
    *buffer = metal.impl_->ensure_buffer(**touched);
    if (*buffer == nil) {
      submission->result.ok = false;
      submission->result.message = "Metal buffer allocation failed";
      return false;
    }
    return true;
  }

  static bool begin(const hal::CompiledPlan& compiled, core::Arena& arena, hal::Submission* submission,
                    std::string* error) {
    if (submission == nullptr) { if (error) *error = "submission output is required"; return false; }
    if (!compiled.report.supported) { if (error) *error = "compiled plan is unsupported"; return false; }
    if (!compiled.plan.validate(error)) return false;
    if (compiled.per_node_packages.size() != compiled.plan.resolved_nodes.size()) {
      if (error) *error = "compiled plan does not contain exactly one package per resolved NodeRef";
      return false;
    }
    for (const auto& node : compiled.plan.resolved_nodes) {
      const size_t matches = std::count_if(compiled.per_node_packages.begin(),
                                           compiled.per_node_packages.end(), [&](const auto& package) {
        return package.ref.index == node.ref.index && package.ref.generation == node.ref.generation;
      });
      if (matches != 1) {
        if (error) *error = "compiled plan contains a duplicate or missing NodeRef package";
        return false;
      }
      const auto package = std::ranges::find_if(compiled.per_node_packages, [&](const auto& candidate) {
        return candidate.ref.index == node.ref.index && candidate.ref.generation == node.ref.generation;
      });
      const auto task = std::ranges::find_if(compiled.plan.task_graph.tasks(), [&](const auto& candidate) {
        return candidate.node_index == node.ref.index && candidate.node_generation == node.ref.generation;
      });
      const bool raster = task != compiled.plan.task_graph.tasks().end() &&
                          task->kind == core::TaskKind::Raster;
      if (package == compiled.per_node_packages.end() ||
          (raster && (package->kind != hal::CompiledPlan::NodePackageKind::Raster ||
                      package->package.has_value())) ||
          (!raster && (package->kind != hal::CompiledPlan::NodePackageKind::CanonicalCompute ||
                       !package->package.has_value()))) {
        if (error) *error = "compiled NodeRef package kind disagrees with the sealed Task domain";
        return false;
      }
    }
    if (!compiled.plan.graph_epoch_matches(arena, error)) return false;
    submission->abi_version = hal::kDeviceHalAbiVersion;
    submission->report = compiled.report;
    submission->report.transition_encoder_boundary_count = 0;
    submission->report.transition_host_wait_count = 0;
    submission->report.transition_serialized_fallback_count = 0;
    if (!hal::run_discovery_stage(compiled.plan, arena, submission, error)) return false;
    if (!hal::apply_working_set_budget(compiled.plan, arena, submission, error)) return false;
    return true;
  }

  static bool stage5(DeviceHal& metal, const hal::CompiledPlan& compiled, core::Arena& arena,
                     hal::Submission* submission, std::string* error) {
    std::string representation_error;
    DispatchStats representation_stats;
    if (!hal::commit_representation_operations(
            compiled.plan, compiled.representation_operations, arena, metal.facet_pool(),
            [&](const core::RepresentationSemanticPlanItem& request, const hal::CompiledPlan::PhysicalRepresentationOperation&, core::FacetRef facet,
                hal::RepresentationTransformCost* cost, std::string* physical_error) {
              Impl::TransformCost transform_cost;
              const bool transformed = metal.impl_->transform_into_private_facet(arena, metal.facet_pool(), request.view,
                                                             request.target_kind, facet, &transform_cost,
                                                             physical_error);
              representation_stats.encoder_count += transform_cost.encoder_count;
              representation_stats.command_buffer_count += transform_cost.command_buffer_count;
              representation_stats.queue_wait_count += transform_cost.queue_wait_count;
              const auto operation_index = static_cast<uint32_t>(
                  &request - compiled.plan.representation_plan.data());
              if (std::ranges::any_of(compiled.transition_operations, [&](const auto& transition) {
                    return std::ranges::find(transition.representation_operations, operation_index) !=
                           transition.representation_operations.end();
                  })) {
                submission->report.transition_encoder_boundary_count += transform_cost.encoder_count;
                submission->report.transition_host_wait_count += transform_cost.queue_wait_count;
              }
              if (!transformed) return false;
              cost->new_backing_bytes = transform_cost.new_backing_bytes;
              cost->temporary_bytes = transform_cost.temporary_bytes;
              cost->heap_fragmentation_bytes = 0;
              cost->used_device_optimal = true;
              cost->distinct_backing = true;
              return true;
            },
            submission, &representation_error)) {
      apply_stats(representation_stats, submission);
      if (error) *error = representation_error;
      return false;
    }
    apply_stats(representation_stats, submission);
    uint32_t retired_textures = 0;
    uint64_t released_linear = 0;
    metal.impl_->reclaim_released_backing(arena, metal.facet_pool(), &retired_textures, &released_linear);
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
    return true;
  }

  // F2 (ADR-043 Decision #3): runs every Raster-kind TaskRecord in the
  // compiled plan's task graph through Impl::run_raster_pass() -- the same
  // facet-acquisition/pipeline/draw/readback code run_raster_triangles() uses
  // -- so the rasterizer is reachable through compile()/submit() by being
  // moved, not rewritten.
  //
  // MD-4 passes only task indices selected from the sealed component/wave
  // schedule. Keeping the physical draw helper here avoids introducing a
  // second raster execution path while allowing compute and raster steps to
  // share one scheduler.
  static bool raster(DeviceHal& metal, const hal::CompiledPlan& compiled, core::Arena& arena,
                     std::span<const uint32_t> task_indices, DispatchStats* stats,
                     hal::Submission* submission, bool* command_submitted,
                     std::string* out_message) {
    if (command_submitted != nullptr) *command_submitted = false;
    const auto& tasks = compiled.plan.task_graph.tasks();
    for (uint32_t task_index : task_indices) {
      if (task_index >= tasks.size()) {
        if (out_message) *out_message = "sealed execution schedule names an out-of-range raster Task";
        return false;
      }
      const core::TaskRecord& task = tasks[task_index];
      if (task.kind != core::TaskKind::Raster) {
        if (out_message) *out_message = "Metal raster schedule step received a non-raster Task";
        return false;
      }

      const core::NodeTable::Ref ref{task.node_index, task.node_generation};
      const auto resolved = std::ranges::find_if(compiled.plan.resolved_nodes, [ref](const auto& node) {
        return node.ref.index == ref.index && node.ref.generation == ref.generation;
      });
      if (resolved == compiled.plan.resolved_nodes.end()) {
        if (out_message) *out_message = "raster task NodeRef is missing from the immutable plan snapshot";
        return false;
      }
      const auto package = std::ranges::find_if(compiled.per_node_packages, [ref](const auto& item) {
        return item.ref.index == ref.index && item.ref.generation == ref.generation;
      });
      if (package == compiled.per_node_packages.end() ||
          package->kind != hal::CompiledPlan::NodePackageKind::Raster ||
          package->package.has_value()) {
        if (out_message) *out_message = "raster Task resolved a non-raster NodeRef package";
        return false;
      }
      const std::string& root_schema = resolved->user_raster_shader.has_value()
          ? resolved->user_raster_shader->root_schema : resolved->module->root_schema;
      const bool uses_scene_root = core::is_scene_root_raster_schema(root_schema);

      std::string task_error;
      core::RasterFacetPair facets = task.raster_facets;
      std::array<float, 4> tint = task.raster_tint;
      std::optional<core::ResolvedSceneRootRaster> scene_root;
      id<MTLBuffer> scene_root_buffer = nil;
      bool identity_buffer_created = false;
      if (uses_scene_root) {
        scene_root.emplace();
        if (!core::resolve_scene_root_raster(arena, task, &*scene_root, &task_error)) {
          if (out_message) *out_message = task_error;
          return false;
        }
        facets.source = scene_root->albedo;
        tint = scene_root->base_color;
        scene_root_buffer = metal.impl_->ensure_buffer(*scene_root->allocation);
      } else {
        // The built-in shader always reads slot 1 after F6. Bind an explicit
        // identity root for every pre-F6 task so its pixels and PSO key stay
        // unchanged rather than relying on an unbound Metal buffer.
        scene_root_buffer = metal.impl_->make_identity_scene_root_buffer(&identity_buffer_created);
      }
      if (scene_root_buffer == nil) {
        if (out_message) *out_message = "Metal SceneRoot buffer allocation failed";
        return false;
      }
      if (!uses_scene_root) {
        submission->report.add(identity_buffer_created ? "identity_scene_root_buffer_create"
                                                       : "identity_scene_root_buffer_reuse",
                               hal::LoweringClass::Direct, 1,
                               identity_buffer_created ? VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE : 0,
                               identity_buffer_created
                                   ? "one immutable device-local legacy SceneRoot buffer created"
                                   : "reused immutable device-local legacy SceneRoot buffer; no draw allocation");
      }
      // Address facets are just as much GPU-visible capabilities as sample
      // and attachment facets. Hold vertex (and, below, index) slots until
      // run_raster_pass has committed and waited for the command buffer.
      FacetUseGuard vertex_use(metal.facet_pool(), task.vertex_buffer_ref);
      if (!vertex_use.begin(arena, &task_error)) {
        if (out_message) *out_message = task_error;
        return false;
      }
      id<MTLBuffer> vertex_buffer = metal.impl_->ensure_facet_buffer(
          arena, metal.facet_pool(), task.vertex_buffer_ref, core::FacetKind::Address, &task_error);
      if (vertex_buffer == nil) {
        if (out_message) *out_message = task_error;
        return false;
      }

      // ensure_facet_buffer() already resolved the facet slot internally to
      // reach the allocation's device buffer; resolving it again here is
      // read-only and cheap, and is the only way to reach the backing
      // Allocation's byte length to derive a vertex count.
      const core::FacetSlot* vertex_slot = metal.impl_->resolve_facet(
          arena, metal.facet_pool(), task.vertex_buffer_ref, core::FacetKind::Address, &task_error);
      if (vertex_slot == nullptr) {
        if (out_message) *out_message = task_error;
        return false;
      }
      const core::Allocation* vertex_allocation = arena.lookup(
          core::PointerRef{vertex_slot->view.allocation, vertex_slot->view.allocation_generation});
      if (vertex_allocation == nullptr) {
        if (out_message) *out_message = "raster task vertex buffer allocation not found in arena";
        return false;
      }
      if (vertex_allocation->bytes.size() % sizeof(RasterVertex) != 0) {
        if (out_message)
          *out_message = "raster task vertex buffer byte size is not a multiple of sizeof(RasterVertex)";
        return false;
      }
      const uint32_t vertex_count =
          static_cast<uint32_t>(vertex_allocation->bytes.size() / sizeof(RasterVertex));
      // Metal clips in homogeneous coordinates, but F4's public contract is
      // normalized window depth. Validate before upload so an old F3 four-float
      // vertex stream cannot be silently reinterpreted as z/u/v data.
      for (uint32_t i = 0; i < vertex_count; ++i) {
        RasterVertex vertex{};
        std::memcpy(&vertex, vertex_allocation->bytes.data() + i * sizeof(RasterVertex), sizeof(vertex));
        if (!std::isfinite(vertex.z) || vertex.z < 0.0f || vertex.z > 1.0f) {
          if (out_message)
            *out_message = "raster task vertex z must be finite and normalized to [0,1] (F4 vertex ABI)";
          return false;
        }
        if (scene_root.has_value() &&
            !core::transform_scene_root_vertex(*scene_root, vertex.x, vertex.y, vertex.z,
                                                &vertex.x, &vertex.y, &vertex.z, &task_error)) {
          if (out_message) *out_message = task_error;
          return false;
        }
      }

      id<MTLBuffer> index_buffer = nil;
      MTLIndexType index_type = MTLIndexTypeUInt16;
      std::unique_ptr<FacetUseGuard> index_use;
      if (task.index_count != 0) {
        index_use = std::make_unique<FacetUseGuard>(metal.facet_pool(), task.index_buffer_ref);
        if (!index_use->begin(arena, &task_error)) { if (out_message) *out_message = task_error; return false; }
        const core::FacetSlot* index_slot = metal.impl_->resolve_facet(
            arena, metal.facet_pool(), task.index_buffer_ref, core::FacetKind::Address, &task_error);
        if (index_slot == nullptr) { if (out_message) *out_message = task_error; return false; }
        const size_t index_stride = index_slot->view.format == core::PixelFormat::R16Uint ? sizeof(uint16_t) :
                                    index_slot->view.format == core::PixelFormat::R32Uint ? sizeof(uint32_t) : 0;
        if (index_stride == 0 || task.index_count % 3 != 0 ||
            task.index_count > std::numeric_limits<size_t>::max() / index_stride) {
          if (out_message) *out_message = "raster task index buffer requires R16Uint/R32Uint and a triangle-list count";
          return false;
        }
        const core::Allocation* index_allocation = arena.lookup(
            core::PointerRef{index_slot->view.allocation, index_slot->view.allocation_generation});
        if (index_allocation == nullptr || index_allocation->bytes.size() < task.index_count * index_stride) {
          if (out_message) *out_message = "raster task index buffer is shorter than index_count";
          return false;
        }
        const uint8_t* indices = index_allocation->bytes.data();
        for (uint32_t i = 0; i < task.index_count; ++i) {
          uint32_t index = 0;
          if (index_stride == sizeof(uint16_t)) { uint16_t value{}; std::memcpy(&value, indices + i * index_stride, sizeof(value)); index = value; }
          else std::memcpy(&index, indices + i * index_stride, sizeof(index));
          if (index >= vertex_count) { if (out_message) *out_message = "raster task index references a vertex outside the vertex buffer"; return false; }
        }
        index_buffer = metal.impl_->ensure_facet_buffer(arena, metal.facet_pool(), task.index_buffer_ref,
                                                          core::FacetKind::Address, &task_error);
        if (index_buffer == nil) { if (out_message) *out_message = task_error; return false; }
        index_type = index_stride == sizeof(uint16_t) ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
      }

      id<MTLBuffer> tint_buffer = [metal.impl_->device newBufferWithLength:sizeof(float) * 4
                                                                  options:MTLResourceStorageModeShared];
      if (tint_buffer == nil) {
        if (out_message) *out_message = "Metal raster buffer allocation failed";
        return false;
      }
      std::memcpy([tint_buffer contents], tint.data(), sizeof(float) * 4);

      // F2 fixed defaults for the backend-private half of RasterDesc; the
      // rest (filter/wrap/tint) comes straight off the TaskRecord.
      RasterDesc desc;
      desc.attachment = hal::f2_default_raster_attachment_config<AttachmentFacetDesc>();
      desc.filter = task.raster_filter;
      desc.wrap = task.raster_wrap;
      desc.tint = tint;
      desc.depth_attachment_ref = task.depth_attachment_ref;
      desc.depth_test_enable = task.depth_test_enable;
      desc.depth_write_enable = task.depth_write_enable;
      desc.depth_compare_op = task.depth_compare_op;

      RasterResult result;
      const ir::UserRasterShaderContract* user_shader =
          resolved->user_raster_shader.has_value() ? &*resolved->user_raster_shader : nullptr;
      bool task_submitted = false;
      const bool raster_ok = metal.impl_->run_raster_pass(arena, metal.facet_pool(), facets, desc, vertex_buffer, scene_root_buffer,
                                        tint_buffer, vertex_count, index_buffer, index_type, task.index_count,
                                        &result, &task_error, user_shader, &task_submitted);
      if (stats != nullptr) {
        stats->encoder_count += result.report.encoder_count;
        stats->command_buffer_count += result.report.command_buffer_count;
        stats->barrier_count += result.report.barrier_count;
        stats->queue_wait_count += result.report.queue_wait_count;
      }
      if (!raster_ok) {
        if (command_submitted != nullptr) *command_submitted = task_submitted;
        if (out_message) *out_message = task_error;
        return false;
      }
      if (command_submitted != nullptr) *command_submitted = true;

      submission->raster_results.push_back(hal::RasterTaskResult{
          .task_index = static_cast<uint32_t>(task_index),
          .resolved_rgba = result.resolved_rgba,
          .resolved_depth = result.resolved_depth,
          .width = result.width,
          .height = result.height,
          .stored = result.stored,
          .contents_defined = result.contents_defined,
      });
      for (const auto& event : result.report.events)
        submission->report.events.push_back(event);
      for (const auto& effect : compiled.plan.task_effects[task_index]) {
        submission->result.trace.push_back(effect);
        submission->result.witness.record(
            effect, static_cast<uint32_t>(submission->result.witness.entries().size()));
      }
    }
    return true;
  }

  static Flow precheck_timeline(DeviceHal& metal, uint64_t wait_value, uint64_t signal_value,
                                hal::Submission* submission) {
    if (wait_value == 0 && signal_value == 0) return Flow::Continue;
    std::string timeline_error;
    if (!metal.impl_->ensure_timeline_event(&timeline_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = timeline_error;
      submission->result.fault.code = "TIMELINE_UNAVAILABLE";
      submission->result.fault.message = timeline_error;
      return Flow::Finish;
    }
    const uint64_t current = metal.impl_->timeline_event.signaledValue;
    if (wait_value != 0 && current < wait_value) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = "timeline wait point is unsatisfied";
      submission->result.fault.code = "TIMELINE_WAIT_UNSATISFIED";
      submission->result.fault.message = submission->result.message;
      return Flow::Finish;
    }
    if (signal_value != 0 && signal_value <= current) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = "timeline signal must be strictly monotonic";
      submission->result.fault.code = "TIMELINE_SIGNAL_NOT_MONOTONIC";
      submission->result.fault.message = submission->result.message;
      return Flow::Finish;
    }
    return Flow::Continue;
  }

  static Flow sealed_effects(const hal::CompiledPlan& compiled, hal::Submission* submission) {
    if (compiled.plan.certificate.ranges.empty()) return Flow::Continue;
    for (const auto& effect : compiled.plan.instantiated_effects) {
      if (!compiled.plan.certificate.covers(effect)) {
        submission->result.ok = false;
        submission->result.poison = core::PoisonState::Poisoned;
        submission->result.message = "certificate does not cover a sealed per-Task effect";
        submission->result.missing_effects.push_back(effect);
        return Flow::Finish;
      }
    }
    return Flow::Continue;
  }

  static Flow execute_schedule(DeviceHal& metal, const hal::CompiledPlan& compiled,
                               core::Arena& arena, uint64_t signal_value,
                               hal::Submission* submission) {
    const auto& tasks = compiled.plan.task_graph.tasks();
    const auto& schedule = compiled.plan.execution_schedule;
    const size_t task_count = tasks.size();
    DispatchStats stats;
    std::vector<uint8_t> cancelled(task_count);
    struct Failure { uint32_t task{}; std::string message; core::FaultRecord fault; };
    std::vector<Failure> failures;
    std::vector<uint64_t> task_encoder_boundaries(task_count), task_host_waits(task_count);
    std::vector<uint8_t> started(task_count);
    std::vector<id<MTLComputePipelineState>> pipeline_ordinals;
    bool produced_output = false;

    submission->result = {};
    submission->result.ok = true;
    metal.impl_->last_node_aware_dispatches.clear();
    metal.impl_->last_node_aware_dispatches.reserve(task_count);

    const auto cancel_descendants = [&](uint32_t failed) {
      std::vector<uint32_t> work{failed};
      for (size_t cursor = 0; cursor < work.size(); ++cursor) {
        for (uint32_t successor : schedule.structural_successors[work[cursor]]) {
          if (cancelled[successor] == 0) {
            cancelled[successor] = 1;
            work.push_back(successor);
          }
        }
      }
    };

    const auto record_effects = [&](uint32_t task_index) {
      for (const auto& effect : compiled.plan.task_effects[task_index]) {
        submission->result.trace.push_back(effect);
        submission->result.witness.record(
            effect, static_cast<uint32_t>(submission->result.witness.entries().size()));
        if (effect.access != ir::Access::Read) produced_output = true;
      }
    };

    const auto run_compute = [&](uint32_t task_index, std::string* task_error) {
      const auto& task = tasks[task_index];
      const core::NodeTable::Ref ref{task.node_index, task.node_generation};
      const auto node = std::ranges::find_if(compiled.plan.resolved_nodes, [ref](const auto& candidate) {
        return candidate.ref.index == ref.index && candidate.ref.generation == ref.generation;
      });
      const auto node_package = std::ranges::find_if(compiled.per_node_packages, [ref](const auto& candidate) {
        return candidate.ref.index == ref.index && candidate.ref.generation == ref.generation;
      });
      if (node == compiled.plan.resolved_nodes.end() || !node->module.has_value() ||
          node_package == compiled.per_node_packages.end() ||
          node_package->kind != hal::CompiledPlan::NodePackageKind::CanonicalCompute ||
          !node_package->package.has_value()) {
        if (task_error) *task_error = "compute Task could not resolve its immutable NodeRef package";
        return false;
      }
      const auto& module = *node->module;
      const auto& package = *node_package->package;
      if (package.canonical_ir_hash != module.hash || package.root_schema != module.root_schema) {
        if (task_error) *task_error = "NodeRef package hash disagrees with its immutable module snapshot";
        return false;
      }
      if (node_package->host_assisted) {
        const auto host_start = std::chrono::steady_clock::now();
        auto result = reference::execute(
            module, arena,
            compiled.plan.certificate.ranges.empty() ? nullptr : &compiled.plan.certificate,
            nullptr, {}, &compiled.plan.task_effects[task_index]);
        submission->cpu_submit_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - host_start).count();
        submission->result.trace.insert(submission->result.trace.end(),
                                        result.trace.begin(), result.trace.end());
        for (const auto& entry : result.witness.entries())
          submission->result.witness.record(
              entry.effect,
              static_cast<uint32_t>(submission->result.witness.entries().size()));
        submission->result.missing_effects.insert(submission->result.missing_effects.end(),
                                                 result.missing_effects.begin(), result.missing_effects.end());
        if (!result.ok) {
          produced_output = produced_output ||
              result.poison == core::PoisonState::PartiallyProduced;
          submission->result.fault = result.fault;
          if (task_error) *task_error = result.message;
          return false;
        }
        for (const auto& effect : compiled.plan.task_effects[task_index])
          if (effect.access != ir::Access::Read) produced_output = true;
        return true;
      }
      id<MTLComputePipelineState> pipeline = nil;
      std::string pipeline_error;
      const bool pointer_graph = is_pointer_graph_module(module);
      if (!metal.impl_->ensure_node_pipeline({package.canonical_ir_hash, package.metal_source},
                                             &pipeline, &pipeline_error,
                                             pointer_graph ? "vg_pointer_graph_compute" : "vg_linear_compute")) {
        if (task_error) *task_error = "Metal per-Node pipeline lookup failed: " + pipeline_error;
        return false;
      }
      auto ordinal = std::ranges::find(pipeline_ordinals, pipeline);
      if (ordinal == pipeline_ordinals.end()) {
        pipeline_ordinals.push_back(pipeline);
        ordinal = std::prev(pipeline_ordinals.end());
      }
      std::vector<id<MTLBuffer>> buffers;
      std::map<uint64_t, core::Allocation*> bound_by_id;
      std::map<uint64_t, id<MTLBuffer>> buffer_by_allocation_id;
      const auto generation_by_allocation = generations(module);
      for (const auto& binding : package.bindings) {
        const auto generation = generation_by_allocation.find(binding.allocation);
        id<MTLBuffer> buffer = nil;
        core::Allocation* allocation = nullptr;
        if (generation == generation_by_allocation.end() ||
            !bind(metal, arena, binding.allocation, generation->second.first,
                  generation->second.second, &buffer, &allocation, submission)) {
          if (task_error && task_error->empty()) *task_error = submission->result.message;
          return false;
        }
        buffers.push_back(buffer);
        bound_by_id.emplace(allocation->id, allocation);
        buffer_by_allocation_id[allocation->id] = buffer;
      }
      bool command_submitted = false;
      if (!metal.impl_->dispatch_compute_task(
              pipeline, buffers, task, task_index,
              static_cast<uint32_t>(std::distance(pipeline_ordinals.begin(), ordinal)),
              &command_submitted, &stats, task_error)) {
        if (command_submitted && std::ranges::any_of(
                compiled.plan.task_effects[task_index], [](const auto& effect) {
                  return effect.access != ir::Access::Read;
                }))
          produced_output = true;
        return false;
      }
      for (const auto& effect : compiled.plan.task_effects[task_index]) {
        if (effect.access == ir::Access::Read) continue;
        const auto allocation = bound_by_id.find(effect.allocation);
        const auto buffer = buffer_by_allocation_id.find(effect.allocation);
        if (allocation != bound_by_id.end() && buffer != buffer_by_allocation_id.end())
          metal.impl_->commit_buffer_write(*allocation->second, buffer->second);
      }
      record_effects(task_index);
      return true;
    };

    for (const auto& component : schedule.components) {
      for (const auto& wave : component.waves) {
        for (uint32_t task_index : wave.tasks) {
          if (cancelled[task_index] != 0) continue;
          started[task_index] = 1;
          submission->result.fault = {};
          const uint64_t encoders_before = stats.encoder_count;
          const uint64_t waits_before = stats.queue_wait_count;
          std::string task_error;
          bool ok = false;
          if (tasks[task_index].kind == core::TaskKind::Compute) {
            ok = run_compute(task_index, &task_error);
          } else {
            const std::array<uint32_t, 1> raster_task{task_index};
            const size_t result_count = submission->raster_results.size();
            bool command_submitted = false;
            ok = raster(metal, compiled, arena, raster_task, &stats, submission,
                        &command_submitted, &task_error);
            if (!ok && command_submitted)
              produced_output = produced_output || std::ranges::any_of(
                  compiled.plan.task_effects[task_index], [](const auto& effect) {
                    return effect.access != ir::Access::Read;
                  });
            if (ok && submission->raster_results.size() == result_count + 1) {
              const auto& result = submission->raster_results.back();
              produced_output = produced_output || (result.stored && result.contents_defined);
            }
          }
          task_encoder_boundaries[task_index] = stats.encoder_count - encoders_before;
          task_host_waits[task_index] = stats.queue_wait_count - waits_before;
          if (!ok) {
            failures.push_back({task_index, std::move(task_error), submission->result.fault});
            cancel_descendants(task_index);
          }
        }
      }
    }

    apply_stats(stats, submission);
    // CompiledPlan records planned lowering. Submission records only the
    // transitions actually reached, including successful representation
    // preludes, never waits for cancelled or pre-command failed producers.
    for (const auto& transition : compiled.transition_operations) {
      if (!transition.covers_execution_completion) continue;
      const auto& component = schedule.components[transition.component];
      for (uint32_t producer : component.waves[transition.before_wave].tasks) {
        submission->report.transition_encoder_boundary_count += task_encoder_boundaries[producer];
        submission->report.transition_host_wait_count += task_host_waits[producer];
      }
      if (std::ranges::any_of(component.waves[transition.after_wave].tasks,
                             [&](uint32_t consumer) { return started[consumer] != 0; }))
        ++submission->report.transition_serialized_fallback_count;
    }
    if (!failures.empty()) {
      std::vector<uint32_t> rank(task_count, UINT32_MAX);
      for (uint32_t index = 0; index < schedule.task_order.size(); ++index)
        rank[schedule.task_order[index]] = index;
      const auto primary = std::ranges::min_element(failures, [&](const auto& left, const auto& right) {
        return rank[left.task] < rank[right.task];
      });
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = produced_output ? core::PoisonState::PartiallyProduced
                                                  : core::PoisonState::Poisoned;
      submission->result.message = primary->message;
      submission->result.fault = primary->fault;
      if (submission->result.fault.code.empty()) submission->result.fault.code = "METAL_TASK_FAILED";
      submission->result.fault.message = primary->message;
      submission->result.fault.task_index = primary->task;
    } else {
      submission->result.ok = true;
      submission->result.poison = core::PoisonState::Valid;
      if (signal_value != 0) metal.impl_->timeline_event.signaledValue = signal_value;
    }
    submission->timeline_value =
        metal.impl_->timeline_event != nil ? metal.impl_->timeline_event.signaledValue : 0;
    return Flow::Finish;
  }

  static Flow publish_tasks(DeviceHal& metal, const hal::CompiledPlan& compiled,
                            const std::vector<uint32_t>& order,
                            DispatchStats* stats, hal::Submission* submission, std::string* error) {
    if (compiled.plan.task_graph.tasks().empty()) return Flow::Continue;
    const auto& tasks = compiled.plan.task_graph.tasks();
    const bool compute_only = std::ranges::all_of(order, [&](uint32_t task_index) {
      return task_index < tasks.size() && tasks[task_index].kind == core::TaskKind::Compute;
    });
    const bool host_split = order.size() != tasks.size() || submission->envelope_overflow.has_value();
    if (order.empty()) return Flow::Continue;
    if (host_split || !compute_only) {
      std::string publish_error;
      if (!publish_envelope_order(compiled.plan.task_graph, order, &submission->published_tasks, &publish_error)) {
        submission->result.ok = false;
        submission->result.message = publish_error;
        return Flow::Finish;
      }
      return Flow::Continue;
    }
    const auto count = static_cast<uint32_t>(tasks.size());
    id<MTLBuffer> state_buffer = [metal.impl_->device newBufferWithLength:std::max<size_t>(count * sizeof(uint32_t), 1)
                                                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> fields_buffer =
        [metal.impl_->device newBufferWithLength:std::max<size_t>(count * compiler::kTaskRingWordsPerRecord * sizeof(uint32_t), 1)
                                         options:MTLResourceStorageModeShared];
    id<MTLBuffer> inputs_buffer =
        [metal.impl_->device newBufferWithLength:std::max<size_t>(count * compiler::kTaskRingWordsPerRecord * sizeof(uint32_t), 1)
                                         options:MTLResourceStorageModeShared];
    if (state_buffer == nil || fields_buffer == nil || inputs_buffer == nil) {
      submission->result.ok = false;
      submission->result.message = "Metal task ring buffer allocation failed";
      return Flow::Finish;
    }
    std::memset([state_buffer contents], 0, count * sizeof(uint32_t));
    auto* inputs = static_cast<uint32_t*>([inputs_buffer contents]);
    for (uint32_t i = 0; i < count; ++i) {
      compiler::ComputeTaskRingRecord record;
      std::string codec_error;
      if (!compiler::make_compute_task_ring_record(tasks[i], &record, &codec_error) ||
          !compiler::pack_compute_task_ring_record(
              record,
              std::span<uint32_t>(inputs + i * compiler::kTaskRingWordsPerRecord,
                                  compiler::kTaskRingWordsPerRecord),
              &codec_error)) {
        submission->result.ok = false;
        submission->result.message = "Metal compute Task ring encode failed: " + codec_error;
        return Flow::Finish;
      }
    }
    std::string task_pipeline_error;
    if (!metal.impl_->ensure_task_ring_pipeline(&task_pipeline_error)) {
      submission->result.ok = false;
      submission->result.message = "Metal task ring pipeline compile failed: " + task_pipeline_error;
      return Flow::Finish;
    }
    std::string publish_error;
    if (!metal.impl_->dispatch_task_publish({.state = state_buffer, .fields = fields_buffer, .inputs = inputs_buffer},
                                            count, stats, &publish_error)) {
      submission->result.ok = false;
      submission->result.message = "Metal task ring dispatch failed: " + publish_error;
      return Flow::Finish;
    }
    const auto* states = static_cast<const uint32_t*>([state_buffer contents]);
    const auto* fields = static_cast<const uint32_t*>([fields_buffer contents]);
    submission->published_tasks.reserve(count);
    for (uint32_t index : order) {
      if (states[index] != static_cast<uint32_t>(core::PublicationState::Published)) {
        submission->result.ok = false;
        submission->result.message = "task ring slot did not reach Published state";
        return Flow::Finish;
      }
      compiler::ComputeTaskRingRecord record;
      std::string codec_error;
      if (!compiler::unpack_compute_task_ring_record(
              std::span<const uint32_t>(fields + index * compiler::kTaskRingWordsPerRecord,
                                        compiler::kTaskRingWordsPerRecord),
              &record, &codec_error)) {
        submission->result.ok = false;
        submission->result.message = "Metal compute Task ring decode failed: " + codec_error;
        return Flow::Finish;
      }
      submission->published_tasks.push_back(compiler::make_task_record(record));
    }
    return Flow::Continue;
  }

  static Flow publish(DeviceHal& metal, const hal::CompiledPlan& compiled,
                      const std::vector<uint32_t>& order,
                      hal::Submission* submission, std::string* error) {
    DispatchStats stats;
    const Flow flow = publish_tasks(metal, compiled, order, &stats, submission, error);
    submission->cpu_encode_ns += stats.cpu_encode_ns;
    submission->cpu_submit_ns += stats.cpu_submit_ns;
    submission->report.encoder_count += stats.encoder_count;
    submission->report.command_buffer_count += stats.command_buffer_count;
    submission->report.barrier_count += stats.barrier_count;
    submission->report.queue_wait_count += stats.queue_wait_count;
    if (flow == Flow::Finish) {
      submission->result.outputs_valid = false;
      submission->result.poison = core::PoisonState::Poisoned;
    }
    return flow;
  }
};

bool DeviceHal::submit(const hal::CompiledPlan& compiled, core::Arena& arena, hal::Submission* submission,
                       std::string* error) {
  if (!hal::validate_stage7_compiled_plan(compiled, hal::BackendKind::Metal, error)) return false;
  if (!SubmitOps::begin(compiled, arena, submission, error)) return false;
  const uint64_t wait_value = compiled.plan.timeline_wait;
  const uint64_t signal_value = compiled.plan.timeline_signal;
  bool result = false;
  if (SubmitOps::take(SubmitOps::precheck_timeline(*this, wait_value, signal_value, submission), &result))
    return result;
  if (SubmitOps::take(SubmitOps::sealed_effects(compiled, submission), &result)) return result;
  // Freeze publication admission before a representation can change epochs,
  // retire facets or submit a physical transform. Publication reuses this order.
  std::vector<uint32_t> publish_order;
  if (!hal::apply_envelope_continuation(compiled.plan, &envelope_continuations(), submission,
                                        &publish_order, error)) return false;
  hal::SubmissionLifetimeHold lifetime_hold;
  if (!lifetime_hold.prepare(compiled.plan, arena, facet_pool(), error)) return false;
  if (!SubmitOps::stage5(*this, compiled, arena, submission, error)) return false;
  if (!lifetime_hold.acquire(submission->representation_facets, error)) return false;

  // Publication is the complete Envelope-filtered semantic graph and precedes
  // execution, as it does on Reference. A later logical Task fault therefore
  // does not make Raster records disappear from the observed submission.
  if (SubmitOps::take(SubmitOps::publish(*this, compiled, publish_order, submission, error), &result)) return result;
  if (SubmitOps::execute_schedule(*this, compiled, arena, signal_value, submission) == SubmitOps::Flow::Fail)
    return false;
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
  const auto count = static_cast<uint32_t>(instance_visible.size());
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
  [encoder setBytes:&count length:sizeof(count) atIndex:4];
  NSUInteger max_tpg = [impl_->cull_compact_pipeline maxTotalThreadsPerThreadgroup];
  uint32_t tpg = 256;
  if (max_tpg > 0 && max_tpg < tpg) tpg = static_cast<uint32_t>(max_tpg);
  if (tpg == 0) tpg = 1;
  const uint32_t groups = count == 0 ? 1u : (count + tpg - 1u) / tpg;
  [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(tpg, 1, 1)];
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
  const auto* compact = static_cast<const uint32_t*>([compact_buffer contents]);
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
  const auto count = static_cast<uint32_t>(coords.size());

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
  id<MTLSamplerState> sampler = impl_->ensure_sampler_state(
      filter, wrap, {0.0f, std::numeric_limits<float>::max()});
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
    auto group = std::ranges::find_if(lod_groups,
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
    const auto slot_count = static_cast<uint32_t>(generations.size());
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
    auto* uv = static_cast<float*>([buffers.uv contents]);
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
    const auto* output = static_cast<const float*>([buffers.output contents]);
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
  const uint64_t texel_byte_offset = view.subresource_byte_offset({texel.layer, texel.level}) +
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
    const auto format_code = static_cast<uint32_t>(view.format);
    std::memcpy([format_buffer contents], &format_code, sizeof(format_code));
    // The kernel indexes in texels, not bytes; core::bytes_per_texel is 4 for
    // both formats this milestone models, which is what makes that exact.
    const auto texel_index = static_cast<uint32_t>(texel_byte_offset / kBytesPerTexel);
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
  const core::Allocation* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
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
  const auto retired = static_cast<uint32_t>(pool.retire_stale(arena));

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

bool DeviceHal::run_raster_triangles(core::Arena& arena, core::FacetPool& pool,
                                     core::RasterFacetPair facets,
                                     const RasterDesc& desc, const std::vector<RasterVertex>& vertices,
                                     RasterResult* result, std::string* error) const {
  for (const RasterVertex& vertex : vertices) {
    if (!std::isfinite(vertex.z) || vertex.z < 0.0f || vertex.z > 1.0f) {
      if (error) *error = "raster vertex z must be finite and normalized to [0,1]";
      return false;
    }
  }
  // Step 3 only ("moved, not rewritten" -- ADR-043 Decision #3, F2): building
  // the host-supplied vertex/tint buffers is the one piece of
  // run_raster_triangles() that a plan-driven raster TaskRecord cannot share,
  // since SubmitOps::raster resolves its vertex buffer straight off a facet
  // instead of a host std::vector<RasterVertex>. Everything else (facet
  // acquisition/validation, pipeline, draw, readback) now lives in
  // Impl::run_raster_pass(), unchanged.
  id<MTLBuffer> vertex_buffer =
      [impl_->device newBufferWithLength:vertices.size() * sizeof(RasterVertex)
                                 options:MTLResourceStorageModeShared];
  id<MTLBuffer> tint_buffer = [impl_->device newBufferWithLength:sizeof(float) * 4
                                                         options:MTLResourceStorageModeShared];
  if (vertex_buffer == nil || tint_buffer == nil) {
    if (error) *error = "Metal raster buffer allocation failed";
    return false;
  }
  // F4 RasterVertex is {x,y,z,u,v}; the MSL side declares
  // `packed_float3 position; packed_float2 uv`, the same 20-byte layout. Do not use
  // float3 here: its Metal alignment would silently change the public stride.
  std::memcpy([vertex_buffer contents], vertices.data(), vertices.size() * sizeof(RasterVertex));
  std::memcpy([tint_buffer contents], desc.tint.data(), sizeof(float) * 4);

  id<MTLBuffer> scene_root_buffer = impl_->make_identity_scene_root_buffer();
  if (scene_root_buffer == nil) {
    if (error) *error = "Metal SceneRoot buffer allocation failed";
    return false;
  }
  return impl_->run_raster_pass(arena, pool, facets, desc, vertex_buffer, scene_root_buffer, tint_buffer,
                                static_cast<uint32_t>(vertices.size()), nil, MTLIndexTypeUInt16, 0,
                                result, error);
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
            impl_->make_pipeline_key({compute_source, compute_entry}, compute_constants(checked), {}, 1);
        id<MTLLibrary> library_object =
            impl_->ensure_library({compute_source, key.code_object_hash}, error);
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
              impl_->make_pipeline_key({raster_source, raster_fragment_entry}, {},
                                       {static_cast<uint32_t>(format)}, sample_count);
          id<MTLLibrary> library_object =
              impl_->ensure_library({raster_source, key.code_object_hash}, error);
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
            impl_->make_pipeline_key({compute_source, compute_entry}, compute_constants(checked), {}, 1);
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
              impl_->make_pipeline_key({raster_source, raster_fragment_entry}, {},
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
                                              MTLPixelFormatInvalid,
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
      impl_->make_pipeline_key({raster_source, raster_fragment_entry}, {},
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

bool DeviceHal::run_task_tier1_indirect_test_harness(
    const ir::Module& module, core::Arena& arena, const std::vector<core::TaskRecord>& tasks,
    hal::Submission* submission, std::string* error) const {
  if (submission == nullptr || tasks.empty()) {
    if (error) *error = "Tier1 physical harness requires tasks and a submission output";
    return false;
  }
  *submission = {};
  submission->abi_version = hal::kDeviceHalAbiVersion;
  const auto package = compiler::build_linear_compute_package(module);
  if (!package.ok) {
    if (error) *error = "Tier1 physical harness package compilation failed: " + package.message;
    return false;
  }
  if (!impl_->ensure_pipeline({package.package.canonical_ir_hash, package.package.metal_source}, error))
    return false;

  std::vector<id<MTLBuffer>> buffers;
  buffers.reserve(package.package.bindings.size());
  for (const auto& binding : package.package.bindings) {
    const auto instruction = std::ranges::find_if(module.instructions, [&](const auto& candidate) {
      return candidate.allocation == binding.allocation;
    });
    if (instruction == module.instructions.end()) {
      if (error) *error = "Tier1 physical harness binding has no immutable IR allocation";
      return false;
    }
    const auto* allocation = arena.lookup(core::RepresentationRef{
        binding.allocation, instruction->generation, instruction->representation_epoch});
    if (allocation == nullptr) {
      if (error) *error = "Tier1 physical harness encountered stale allocation generation or epoch";
      return false;
    }
    id<MTLBuffer> buffer = impl_->ensure_buffer(*allocation);
    if (buffer == nil) {
      if (error) *error = "Tier1 physical harness could not allocate a Metal buffer";
      return false;
    }
    buffers.push_back(buffer);
  }

  const uint32_t count = static_cast<uint32_t>(tasks.size());
  id<MTLBuffer> state = [impl_->device newBufferWithLength:count * sizeof(uint32_t)
                                                options:MTLResourceStorageModeShared];
  id<MTLBuffer> fields = [impl_->device newBufferWithLength:count * compiler::kTaskRingWordsPerRecord * sizeof(uint32_t)
                                                 options:MTLResourceStorageModeShared];
  id<MTLBuffer> inputs = [impl_->device newBufferWithLength:count * compiler::kTaskRingWordsPerRecord * sizeof(uint32_t)
                                                 options:MTLResourceStorageModeShared];
  const size_t indirect_stride = sizeof(MTLDispatchThreadgroupsIndirectArguments);
  id<MTLBuffer> indirect = [impl_->device newBufferWithLength:count * indirect_stride
                                                   options:MTLResourceStorageModeShared];
  if (state == nil || fields == nil || inputs == nil || indirect == nil) {
    if (error) *error = "Tier1 physical harness buffer allocation failed";
    return false;
  }
  std::memset([state contents], 0, count * sizeof(uint32_t));
  auto* packed = static_cast<uint32_t*>([inputs contents]);
  for (uint32_t index = 0; index < count; ++index) {
    compiler::ComputeTaskRingRecord record;
    if (!compiler::make_compute_task_ring_record(tasks[index], &record, error) ||
        !compiler::pack_compute_task_ring_record(
            record,
            std::span<uint32_t>(packed + index * compiler::kTaskRingWordsPerRecord,
                                compiler::kTaskRingWordsPerRecord),
            error))
      return false;
  }
  if (!impl_->ensure_task_ring_pipeline(error)) return false;
  DispatchStats stats;
  if (!impl_->dispatch_task_publish({.state = state, .fields = fields, .inputs = inputs},
                                    count, &stats, error))
    return false;
  std::vector<uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0);
  if (!impl_->dispatch_task_tier1_indirect(buffers, fields, order, indirect, &stats, error)) return false;

  const auto* states = static_cast<const uint32_t*>([state contents]);
  const auto* published = static_cast<const uint32_t*>([fields contents]);
  submission->published_tasks.reserve(count);
  impl_->last_tier1_indirect_dims.clear();
  impl_->last_tier1_indirect_dims.reserve(count);
  const auto* args = static_cast<const uint8_t*>([indirect contents]);
  for (uint32_t index = 0; index < count; ++index) {
    if (states[index] != static_cast<uint32_t>(core::PublicationState::Published)) {
      if (error) *error = "Tier1 physical harness task slot was not published";
      return false;
    }
    compiler::ComputeTaskRingRecord record;
    if (!compiler::unpack_compute_task_ring_record(
            std::span<const uint32_t>(published + index * compiler::kTaskRingWordsPerRecord,
                                      compiler::kTaskRingWordsPerRecord),
            &record, error))
      return false;
    submission->published_tasks.push_back(compiler::make_task_record(record));
    std::array<uint32_t, 3> dims{};
    std::memcpy(dims.data(), args + index * indirect_stride, 3 * sizeof(uint32_t));
    impl_->last_tier1_indirect_dims.push_back(dims);
  }
  submission->result.ok = true;
  submission->result.poison = core::PoisonState::Valid;
  submission->cpu_encode_ns = stats.cpu_encode_ns;
  submission->cpu_submit_ns = stats.cpu_submit_ns;
  submission->report.encoder_count = stats.encoder_count;
  submission->report.command_buffer_count = stats.command_buffer_count;
  submission->report.queue_wait_count = stats.queue_wait_count;
  submission->report.add("task_publication", hal::LoweringClass::Direct, count, 0,
                         "narrow physical harness publication");
  submission->report.add("tier1_indirect_dispatch", hal::LoweringClass::Direct, count, 0,
                         "GPU-authored dispatch dimensions consumed without ExecutionPlan feature flags");
  return true;
}

bool DeviceHal::run_indexed_compute_test_harness(const ir::Module& module, core::Arena& arena,
                                                 IndexedComputeHarnessResult* result,
                                                 hal::Submission* submission,
                                                 std::string* error) const {
  if (result == nullptr || submission == nullptr) {
    if (error) *error = "indexed physical harness requires result and submission outputs";
    return false;
  }
  *result = {};
  *submission = {};
  submission->abi_version = hal::kDeviceHalAbiVersion;
  if (!impl_->probe_gpu_addresses()) {
    if (error) *error = "MTLBuffer.gpuAddress is unavailable";
    return false;
  }
  const auto package = compiler::build_indexed_compute_package(module);
  if (!package.ok) {
    if (error) *error = "indexed physical harness package compilation failed: " + package.message;
    return false;
  }
  if (!impl_->ensure_pipeline({package.package.canonical_ir_hash, package.package.metal_source},
                              error, "vg_indexed_compute"))
    return false;

  std::vector<id<MTLBuffer>> buffers;
  std::map<uint64_t, core::Allocation*> allocations;
  for (uint64_t allocation_id : package.package.referenced_allocations) {
    const auto instruction = std::ranges::find_if(module.instructions, [allocation_id](const auto& candidate) {
      return candidate.allocation == allocation_id;
    });
    if (instruction == module.instructions.end()) {
      if (error) *error = "indexed physical harness allocation has no immutable IR instruction";
      return false;
    }
    auto* allocation = arena.lookup(core::RepresentationRef{
        allocation_id, instruction->generation, instruction->representation_epoch});
    if (allocation == nullptr) {
      if (error) *error = "indexed physical harness encountered stale allocation generation or epoch";
      return false;
    }
    id<MTLBuffer> buffer = impl_->ensure_buffer(*allocation);
    if (buffer == nil || ![buffer respondsToSelector:@selector(gpuAddress)] || [buffer gpuAddress] == 0) {
      if (error) *error = "MTLBuffer.gpuAddress is unavailable for an indexed object";
      return false;
    }
    buffers.push_back(buffer);
    allocations.emplace(allocation_id, allocation);
  }
  DispatchStats stats;
  if (!impl_->dispatch_indexed_and_wait(buffers, {}, &stats, error)) return false;
  for (const auto& effect : module.declared_effects) {
    if (effect.access == ir::Access::Read) continue;
    const auto index = std::ranges::find(package.package.referenced_allocations, effect.allocation);
    if (index != package.package.referenced_allocations.end()) {
      const size_t position = static_cast<size_t>(index - package.package.referenced_allocations.begin());
      impl_->commit_buffer_write(*allocations.at(effect.allocation), buffers[position]);
    }
  }
  submission->result.ok = true;
  submission->result.poison = core::PoisonState::Valid;
  submission->result.trace = module.declared_effects;
  for (uint32_t index = 0; index < module.declared_effects.size(); ++index)
    submission->result.witness.record(module.declared_effects[index], index);
  submission->cpu_encode_ns = stats.cpu_encode_ns;
  submission->cpu_submit_ns = stats.cpu_submit_ns;
  submission->report.encoder_count = stats.encoder_count;
  submission->report.command_buffer_count = stats.command_buffer_count;
  submission->report.queue_wait_count = stats.queue_wait_count;
  submission->report.add("compute_package", hal::LoweringClass::Direct, 1, 1,
                         "narrow indexed-address table physical harness");
  result->referenced_allocation_count =
      static_cast<uint32_t>(package.package.referenced_allocations.size());
  result->report = submission->report;
  return true;
}

const std::vector<std::array<uint32_t, 3>>& DeviceHal::last_tier1_indirect_dims() const {
  return impl_->last_tier1_indirect_dims;
}

const std::vector<NodeAwareDispatchObservation>& DeviceHal::last_node_aware_dispatches() const {
  return impl_->last_node_aware_dispatches;
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
    impl->snapshot.hal.capability_bits |= static_cast<uint64_t>(hal::Capability::LinearAddress) |
                                           static_cast<uint64_t>(hal::Capability::IndexedBinding);
  }
  return std::unique_ptr<DeviceHal>(new DeviceHal(std::move(impl)));
}

std::unique_ptr<DeviceHal> make_device_hal(const uint8_t uuid[16], std::string* error) {
  const uint8_t prefix[8] = {0x56, 0x47, 0x50, 0x30, 0x4d, 0x45, 0x54, 0x4c};
  if (std::memcmp(uuid, prefix, 8) != 0) {
    if (error) *error = "uuid is not a Metal adapter uuid (VGP0METL prefix mismatch)";
    return nullptr;
  }
  uint64_t target_registry_id = 0;
  for (size_t i = 0; i < 8; ++i) target_registry_id |= (static_cast<uint64_t>(uuid[8 + i]) << (i * 8));

  NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
  if (devices.count == 0) {
    id<MTLDevice> default_device = MTLCreateSystemDefaultDevice();
    if (default_device != nil) devices = @[default_device];
  }
  id<MTLDevice> device = nil;
  for (id<MTLDevice> candidate in devices) {
    if ([candidate registryID] == target_registry_id) {
      device = candidate;
      break;
    }
  }
  if (device == nil) {
    if (error) *error = "no MTLDevice matches the requested adapter uuid";
    return nullptr;
  }

  auto impl = std::make_unique<DeviceHal::Impl>();
  impl->device = device;
  impl->command_queue = [device newCommandQueue];
  if (impl->command_queue == nil) {
    if (error) *error = "MTLDevice failed to create a command queue";
    return nullptr;
  }
  impl->snapshot.hal = make_hal_snapshot(device, &impl->snapshot);
  id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
  if (buffer != nil && [buffer respondsToSelector:@selector(gpuAddress)] && [buffer gpuAddress] != 0) {
    impl->snapshot.supports_gpu_addresses = true;
    impl->snapshot.hal.capability_bits |= static_cast<uint64_t>(hal::Capability::LinearAddress) |
                                           static_cast<uint64_t>(hal::Capability::IndexedBinding);
  }
  return std::unique_ptr<DeviceHal>(new DeviceHal(std::move(impl)));
}

}  // namespace vg::metal
