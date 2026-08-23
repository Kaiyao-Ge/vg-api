#include "backends/metal/metal_device_hal.h"

#include "backends/reference/reference_executor.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
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
  uint64_t bits = static_cast<uint64_t>(hal::Capability::EffectDag) |
                   static_cast<uint64_t>(hal::Capability::TaskPublication);
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
// FacetPool slot's epoch/kind/size/swizzle no longer match.
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
  core::PixelFormat format{};
  core::SwizzleChannels swizzle{};
};

bool same_swizzle(const core::SwizzleChannels& lhs, const core::SwizzleChannels& rhs) {
  return lhs.red == rhs.red && lhs.green == rhs.green && lhs.blue == rhs.blue &&
         lhs.alpha == rhs.alpha;
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

std::array<float, 4> decode_first_texel(const void* bytes, MTLPixelFormat format) {
  if (format == MTLPixelFormatRGBA8Unorm) {
    const uint8_t* rgba = static_cast<const uint8_t*>(bytes);
    return {rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f};
  }
  float value{};
  std::memcpy(&value, bytes, sizeof(value));
  return {value, 0.0f, 0.0f, 1.0f};
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
  id<MTLLibrary> sample_facet_library = nil;
  id<MTLComputePipelineState> sample_facet_pipeline = nil;
  id<MTLLibrary> storage_facet_library = nil;
  id<MTLComputePipelineState> storage_facet_pipeline = nil;
  id<MTLComputePipelineState> storage_buffer_facet_pipeline = nil;
  std::unordered_map<uint64_t, MetalFacetRecord> facet_map;
  // Only 4 real combinations (Nearest/Bilinear x Clamp/Repeat) ever occur;
  // keyed by (filter << 1 | wrap) so lookup stays a plain integer compare.
  std::unordered_map<uint32_t, id<MTLSamplerState>> sampler_cache;
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
    const size_t needed = std::max<size_t>(allocation.bytes.size(), 1);
    auto it = allocation_map.find(allocation.id);
    if (it != allocation_map.end() &&
        (it->second.generation != allocation.generation || it->second.byte_size < needed)) {
      allocation_map.erase(it);
      it = allocation_map.end();
    }
    if (it == allocation_map.end()) {
      id<MTLBuffer> buffer = [device newBufferWithLength:needed options:MTLResourceStorageModeShared];
      if (buffer == nil) return nil;
      MetalAllocationRecord record{buffer, allocation.id, allocation.generation, needed};
      it = allocation_map.emplace(allocation.id, record).first;
    }
    if (!allocation.bytes.empty())
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

  // Host readback of texel (0,0). Once a representation transform has moved a
  // facet to Private storage there is no host-visible mapping left, so that
  // case has to go back through a blit rather than getBytes.
  bool read_first_texel(id<MTLTexture> texture, std::array<float, 4>* out, std::string* error) {
    const size_t texel_bytes = texture.pixelFormat == MTLPixelFormatRGBA8Unorm ? 4 : sizeof(float);
    if (texture.storageMode != MTLStorageModePrivate) {
      uint8_t bytes[sizeof(float) * 4] = {};
      [texture getBytes:bytes
            bytesPerRow:texel_bytes
             fromRegion:MTLRegionMake2D(0, 0, 1, 1)
            mipmapLevel:0];
      *out = decode_first_texel(bytes, texture.pixelFormat);
      return true;
    }
    id<MTLBuffer> readback = [device newBufferWithLength:texel_bytes options:MTLResourceStorageModeShared];
    if (readback == nil) { if (error) *error = "facet readback buffer allocation failed"; return false; }
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    if (blit == nil) { if (error) *error = "failed to create Metal blit encoder"; return false; }
    [blit copyFromTexture:texture
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(1, 1, 1)
                   toBuffer:readback
          destinationOffset:0
     destinationBytesPerRow:texel_bytes
   destinationBytesPerImage:texel_bytes];
    [blit endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
      if (error)
        *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                              : "facet readback blit failed";
      return false;
    }
    *out = decode_first_texel([readback contents], texture.pixelFormat);
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
      shader_texture = [storage_texture newTextureViewWithPixelFormat:to_mtl_pixel_format(view.format)
                                                          textureType:MTLTextureType2D
                                                               levels:NSMakeRange(0, 1)
                                                               slices:NSMakeRange(0, 1)
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
    record.format = view.format;
    record.swizzle = view.swizzle;
    facet_map[facet_cache_key(ref)] = record;
    return shader_texture;
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
    if (view.dimension != core::ViewDimension::Texture2D || view.array_layers != 1 ||
        view.mip_levels != 1) {
      if (error) *error = "Unsupported for now: only Texture2D with array_layers=1 and mip_levels=1";
      return nil;
    }
    // Swizzle reinterprets a shader read. Metal applies no such remap to a
    // render target or to an image write, so rather than quietly dropping the
    // channel mapping the caller asked for, those kinds are refused.
    if (!view.swizzle.identity() && expected_kind != core::FacetKind::Sample) {
      if (error) *error = "Unsupported: non-identity swizzle applies to SampleFacet only";
      return nil;
    }
    const core::Allocation* allocation = arena.lookup(view.allocation, view.allocation_generation);
    if (allocation == nullptr) {
      if (error) *error = "facet backing allocation not found in arena";
      return nil;
    }

    const uint64_t key = facet_cache_key(ref);
    auto it = facet_map.find(key);
    if (it != facet_map.end() &&
        it->second.representation_epoch == slot->representation_epoch &&
        it->second.kind == slot->kind &&
        it->second.width == view.width && it->second.height == view.height &&
        it->second.format == view.format &&
        same_swizzle(it->second.swizzle, view.swizzle) &&
        it->second.facet_index == ref.index &&
        it->second.facet_generation == ref.generation) {
      if (cache_hit) *cache_hit = true;
      if (out_storage) *out_storage = it->second.storage_texture;
      return it->second.texture;
    }
    if (it != facet_map.end()) facet_map.erase(it);

    const MTLPixelFormat mtl_format = to_mtl_pixel_format(view.format);
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_format
                                                                                            width:view.width
                                                                                           height:view.height
                                                                                        mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    if (expected_kind == core::FacetKind::Sample) {
      descriptor.usage = MTLTextureUsageShaderRead;
    } else if (expected_kind == core::FacetKind::Storage) {
      descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    } else if (expected_kind == core::FacetKind::Attachment) {
      descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    } else {
      if (error) *error = "Unsupported: Address/Transfer facets have no Metal texture representation";
      return nil;
    }
    if (!view.swizzle.identity()) descriptor.usage |= MTLTextureUsagePixelFormatView;

    id<MTLTexture> storage_texture = [device newTextureWithDescriptor:descriptor];
    if (storage_texture == nil) {
      if (error)
        *error = expected_kind == core::FacetKind::Storage
                     ? "Unsupported: pixel format does not support shader write on this device; "
                       "use StorageFacetTarget::LinearBuffer or transform the representation"
                     : "Metal facet texture creation failed";
      return nil;
    }

    if ((expected_kind == core::FacetKind::Sample || expected_kind == core::FacetKind::Storage) &&
        !allocation->bytes.empty()) {
      constexpr size_t kBytesPerPixel = 4;
      const MTLRegion region = MTLRegionMake2D(0, 0, view.width, view.height);
      [storage_texture replaceRegion:region
                         mipmapLevel:0
                           withBytes:allocation->bytes.data()
                         bytesPerRow:view.width * kBytesPerPixel];
    }

    id<MTLTexture> shader_texture = install_facet_record(ref, view, slot->kind, slot->representation_epoch,
                                                         storage_texture, error);
    if (shader_texture == nil) return nil;
    if (cache_hit) *cache_hit = false;
    if (out_storage) *out_storage = storage_texture;
    return shader_texture;
  }

  // Only 4 real (filter, wrap) combinations ever occur, so this is a small
  // permanent cache, never invalidated -- unlike textures,
  // an MTLSamplerState carries no allocation-derived state to go stale.
  id<MTLSamplerState> ensure_sampler_state(core::FilterMode filter, core::WrapMode wrap) {
    const uint32_t key = (static_cast<uint32_t>(filter) << 1) | static_cast<uint32_t>(wrap);
    auto it = sampler_cache.find(key);
    if (it != sampler_cache.end()) return it->second;
    MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
    const MTLSamplerMinMagFilter mtl_filter =
        filter == core::FilterMode::Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    descriptor.minFilter = mtl_filter;
    descriptor.magFilter = mtl_filter;
    const MTLSamplerAddressMode mtl_wrap =
        wrap == core::WrapMode::Clamp ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
    descriptor.sAddressMode = mtl_wrap;
    descriptor.tAddressMode = mtl_wrap;
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

  // Compiles compiler::sample_facet_metal_source() into its own pipeline,
  // mirroring ensure_cull_compact_pipeline()'s pattern exactly.
  bool ensure_sample_facet_pipeline(std::string* error) {
    if (sample_facet_pipeline != nil) return true;
    NSError* compile_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    const std::string source = compiler::sample_facet_metal_source();
    id<MTLLibrary> new_library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                        options:options
                                                          error:&compile_error];
    if (new_library == nil) {
      if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                                : "unknown sample facet MSL compile error";
      return false;
    }
    id<MTLFunction> function = [new_library newFunctionWithName:@"vg_sample_facet"];
    if (function == nil) {
      if (error) *error = "sample facet MSL library missing vg_sample_facet entry point";
      return false;
    }
    NSError* pipeline_error = nil;
    id<MTLComputePipelineState> new_pipeline = [device newComputePipelineStateWithFunction:function
                                                                                       error:&pipeline_error];
    if (new_pipeline == nil) {
      if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                 : "unknown sample facet pipeline creation error";
      return false;
    }
    sample_facet_library = new_library;
    sample_facet_pipeline = new_pipeline;
    return true;
  }

  // StorageFacet write, one texel, in both shapes 06 §6.2 allows: a writable
  // texture2d and a linear device buffer. Both entry points come from one
  // library so the two targets can never drift to different write semantics.
  bool ensure_storage_facet_pipelines(std::string* error) {
    if (storage_facet_pipeline != nil && storage_buffer_facet_pipeline != nil) return true;
    NSError* compile_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    const char* source =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "kernel void vg_storage_facet_write(texture2d<float, access::write> tex [[texture(0)]],\n"
        "                                   constant float4& rgba [[buffer(0)]],\n"
        "                                   uint2 gid [[thread_position_in_grid]]) {\n"
        "  if (gid.x == 0 && gid.y == 0) tex.write(rgba, gid);\n"
        "}\n"
        // format: 0 = RGBA8Unorm, 1 = R32Float, matching core::PixelFormat.
        // The write is encoded in the view's own format so the linear target
        // never silently changes precision (06 §6.2).
        "kernel void vg_storage_facet_write_buffer(device uint* texels [[buffer(0)]],\n"
        "                                          constant float4& rgba [[buffer(1)]],\n"
        "                                          constant uint& format [[buffer(2)]],\n"
        "                                          uint gid [[thread_position_in_grid]]) {\n"
        "  if (gid != 0) return;\n"
        "  if (format == 0) {\n"
        "    texels[0] = pack_float_to_unorm4x8(rgba);\n"
        "  } else {\n"
        "    device float* floats = (device float*)texels;\n"
        "    floats[0] = rgba.x;\n"
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
    id<MTLComputePipelineState> new_pipelines[2] = {nil, nil};
    const char* entry_points[2] = {"vg_storage_facet_write", "vg_storage_facet_write_buffer"};
    for (int i = 0; i < 2; ++i) {
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
    storage_buffer_facet_pipeline = new_pipelines[1];
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

// E004: shared by both the host-assisted and GPU-dispatch submit() paths.
// `touched` is derived from the module's static instruction list rather than
// the dispatch loop's runtime buffer bindings, so this works identically
// regardless of which path executed the submission.
void attach_access_certificate(const hal::CompiledPlan& compiled, const core::Arena& arena, hal::Submission* submission) {
  if (!submission->result.ok || !compiled.plan.requested_certificate_mode.has_value()) return;
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
      auto task_result = reference::execute_task_graph(compiled.plan.task_graph);
      if (!task_result.ok) {
        submission->result.ok = false;
        submission->result.message = task_result.message;
      } else {
        submission->published_tasks = std::move(task_result.published_tasks);
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
    std::vector<uint32_t> order;
    std::string order_error;
    if (!compiled.plan.task_graph.deterministic_order(&order, &order_error)) {
      submission->result.ok = false;
      submission->result.message = order_error;
      return true;
    }
    const auto& tasks = compiled.plan.task_graph.tasks();
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
  if (result == nullptr) { if (error) *error = "sample facet result output is required"; return false; }
  FacetUseGuard use(pool, ref);
  if (!use.begin(arena, error)) return false;
  std::string pipeline_error;
  if (!impl_->ensure_sample_facet_pipeline(&pipeline_error)) {
    if (error) *error = "Metal sample facet pipeline compile failed: " + pipeline_error;
    return false;
  }
  bool cache_hit = false;
  std::string tex_error;
  id<MTLTexture> texture =
      impl_->ensure_facet_texture(arena, pool, ref, core::FacetKind::Sample, &cache_hit, nullptr, &tex_error);
  if (texture == nil) {
    if (error) *error = tex_error.empty() ? "Metal sample facet texture creation failed" : tex_error;
    return false;
  }
  id<MTLSamplerState> sampler = impl_->ensure_sampler_state(filter, wrap);
  if (sampler == nil) { if (error) *error = "Metal sample facet sampler creation failed"; return false; }

  const uint32_t count = static_cast<uint32_t>(uv_coords.size());
  id<MTLBuffer> uv_buffer = [impl_->device newBufferWithLength:std::max<size_t>(count * sizeof(float) * 2, 1)
                                                        options:MTLResourceStorageModeShared];
  id<MTLBuffer> output_buffer = [impl_->device newBufferWithLength:std::max<size_t>(count * sizeof(float) * 4, 1)
                                                            options:MTLResourceStorageModeShared];
  if (uv_buffer == nil || output_buffer == nil) {
    if (error) *error = "Metal sample facet buffer allocation failed";
    return false;
  }
  if (!uv_coords.empty())
    std::memcpy([uv_buffer contents], uv_coords.data(), uv_coords.size() * sizeof(float) * 2);

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  [encoder setComputePipelineState:impl_->sample_facet_pipeline];
  [encoder setTexture:texture atIndex:0];
  [encoder setSamplerState:sampler atIndex:0];
  [encoder setBuffer:uv_buffer offset:0 atIndex:0];
  [encoder setBuffer:output_buffer offset:0 atIndex:1];
  [encoder dispatchThreadgroups:MTLSizeMake(std::max<uint32_t>(count, 1), 1, 1)
          threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal sample facet dispatch failed";
    return false;
  }

  const float* output = static_cast<const float*>([output_buffer contents]);
  result->sampled_rgba.resize(count);
  for (uint32_t i = 0; i < count; ++i)
    result->sampled_rgba[i] = {output[i * 4 + 0], output[i * 4 + 1], output[i * 4 + 2], output[i * 4 + 3]};
  result->facet_cache_hit = cache_hit;
  result->descriptor_write_count = 2;  // setTexture + setSamplerState
  result->report = make_facet_report();
  result->report.encoder_count = 1;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  result->report.add("sample_facet",
                     cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::DevicePass, count, 0,
                     cache_hit ? "facet cache hit; no MTLTexture created for this use"
                               : "facet cache miss; MTLTexture and sampler compiled for this view");
  return true;
}

bool DeviceHal::run_storage_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                  StorageFacetTarget target, const std::array<float, 4>& write_rgba,
                                  StorageFacetResult* result, std::string* error) const {
  if (result == nullptr) { if (error) *error = "storage facet result output is required"; return false; }
  FacetUseGuard use(pool, ref);
  if (!use.begin(arena, error)) return false;
  const core::FacetSlot* slot = impl_->resolve_facet(arena, pool, ref, core::FacetKind::Storage, error);
  if (slot == nullptr) return false;
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
  if (target == StorageFacetTarget::Texture) {
    std::string tex_error;
    texture = impl_->ensure_facet_texture(arena, pool, ref, core::FacetKind::Storage, &cache_hit, nullptr,
                                          &tex_error);
    if (texture == nil) {
      if (error) *error = tex_error.empty() ? "Metal storage facet texture creation failed" : tex_error;
      return false;
    }
  } else {
    linear = impl_->ensure_facet_buffer(arena, pool, ref, core::FacetKind::Storage, error);
    if (linear == nil) return false;
    // The write lands in the view's own format, so the buffer path never
    // trades away precision the caller asked for (06 §6.2).
    format_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t)
                                                options:MTLResourceStorageModeShared];
    if (format_buffer == nil) {
      if (error) *error = "Metal storage facet format buffer allocation failed";
      return false;
    }
    const uint32_t format_code = static_cast<uint32_t>(slot->view.format);
    std::memcpy([format_buffer contents], &format_code, sizeof(format_code));
  }

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  if (target == StorageFacetTarget::Texture) {
    [encoder setComputePipelineState:impl_->storage_facet_pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:rgba_buffer offset:0 atIndex:0];
    result->descriptor_write_count = 2;  // setTexture + setBuffer
  } else {
    [encoder setComputePipelineState:impl_->storage_buffer_facet_pipeline];
    [encoder setBuffer:linear offset:0 atIndex:0];
    [encoder setBuffer:rgba_buffer offset:0 atIndex:1];
    [encoder setBuffer:format_buffer offset:0 atIndex:2];
    result->descriptor_write_count = 3;  // three buffer bindings, no texture
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
    if (!impl_->read_first_texel(texture, &result->written_rgba, error)) return false;
  } else if (slot->view.format == core::PixelFormat::RGBA8Unorm) {
    const uint8_t* bytes = static_cast<const uint8_t*>([linear contents]);
    result->written_rgba = {bytes[0] / 255.0f, bytes[1] / 255.0f, bytes[2] / 255.0f, bytes[3] / 255.0f};
  } else {
    const float* texel = static_cast<const float*>([linear contents]);
    result->written_rgba = {texel[0], 0.0f, 0.0f, 0.0f};
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
  // constraint actually lives.
  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
  MTLRenderPassColorAttachmentDescriptor* color = rp.colorAttachments[0];
  color.clearColor =
      MTLClearColorMake(desc.clear_rgba[0], desc.clear_rgba[1], desc.clear_rgba[2], desc.clear_rgba[3]);
  switch (desc.load) {
    case AttachmentLoadAction::Clear: color.loadAction = MTLLoadActionClear; break;
    case AttachmentLoadAction::Load: color.loadAction = MTLLoadActionLoad; break;
    case AttachmentLoadAction::DontCare: color.loadAction = MTLLoadActionDontCare; break;
  }

  bool store_traffic_avoided = false;
  if (multisampled) {
    MTLTextureDescriptor* ms = [MTLTextureDescriptor new];
    ms.textureType = MTLTextureType2DMultisample;
    ms.pixelFormat = texture.pixelFormat;
    ms.width = texture.width;
    ms.height = texture.height;
    ms.sampleCount = desc.sample_count;
    ms.usage = MTLTextureUsageRenderTarget;
    // Memoryless keeps the per-sample data on-tile, so the only external
    // write is the resolved single-sample result. Where that is unavailable
    // the samples really do go to device memory, and the result says so
    // rather than claiming an optimization the device did not perform.
    const bool memoryless = [impl_->device supportsFamily:MTLGPUFamilyApple1];
    ms.storageMode = memoryless ? MTLStorageModeMemoryless : MTLStorageModePrivate;
    id<MTLTexture> ms_texture = [impl_->device newTextureWithDescriptor:ms];
    if (ms_texture == nil) {
      if (error) *error = "Unsupported: device rejected a multisample render target for this format";
      return false;
    }
    color.texture = ms_texture;
    color.resolveTexture = texture;
    color.storeAction = MTLStoreActionMultisampleResolve;
    store_traffic_avoided = memoryless;
  } else {
    color.texture = texture;
    color.storeAction =
        desc.store == AttachmentStoreAction::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
    store_traffic_avoided = desc.store == AttachmentStoreAction::DontCare;
  }

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

  if (!impl_->read_first_texel(texture, &result->resolved_rgba, error)) return false;
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
  if (target_kind != core::FacetKind::Sample && target_kind != core::FacetKind::Storage &&
      target_kind != core::FacetKind::Attachment) {
    if (error) *error = "representation transform target_kind must be Sample, Storage, or Attachment";
    return false;
  }
  if (view.dimension != core::ViewDimension::Texture2D || view.array_layers != 1 || view.mip_levels != 1) {
    if (error) *error = "Unsupported: representation transform currently requires Texture2D 1x1 layers/mips";
    return false;
  }
  auto* allocation = arena.lookup(view.allocation, view.allocation_generation);
  if (allocation == nullptr) {
    if (error) *error = "representation transform: backing allocation not found in arena";
    return false;
  }
  const uint64_t old_bytes = allocation->bytes.size();
  constexpr uint64_t kBytesPerPixel = 4;
  const uint64_t new_bytes = static_cast<uint64_t>(view.width) * view.height * kBytesPerPixel;
  if (old_bytes < new_bytes) {
    if (error) *error = "representation transform: linear backing smaller than view extent";
    return false;
  }
  if (!view.swizzle.identity() && target_kind != core::FacetKind::Sample) {
    if (error) *error = "Unsupported: non-identity swizzle applies to SampleFacet only";
    return false;
  }

  const MTLPixelFormat mtl_format = to_mtl_pixel_format(view.format);
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_format
                                                                                          width:view.width
                                                                                         height:view.height
                                                                                      mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate;
  if (target_kind == core::FacetKind::Sample) {
    descriptor.usage = MTLTextureUsageShaderRead;
  } else if (target_kind == core::FacetKind::Storage) {
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  } else {
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  }
  if (!view.swizzle.identity()) descriptor.usage |= MTLTextureUsagePixelFormatView;
  id<MTLTexture> private_texture = [impl_->device newTextureWithDescriptor:descriptor];
  if (private_texture == nil) {
    if (error) *error = "representation transform: Private MTLTexture creation failed";
    return false;
  }

  // Real transform pass (02 §8: a transform is not a barrier). The blit source
  // is the linear representation reached through a TransferFacet over the same
  // CanonicalView, so even the transform's own read is a pool-resolved
  // capability and not a raw buffer handle -- and reusing the existing linear
  // backing means the transform needs no staging copy at all.
  core::FacetRef transfer_ref{};
  if (!pool.acquire(arena, view, core::FacetKind::Transfer, &transfer_ref, error)) return false;
  {
    FacetUseGuard use(pool, transfer_ref);
    if (!use.begin(arena, error)) return false;
    id<MTLBuffer> source = impl_->ensure_facet_buffer(arena, pool, transfer_ref, core::FacetKind::Transfer, error);
    if (source == nil) return false;

    id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    if (blit == nil) { if (error) *error = "failed to create Metal blit encoder"; return false; }
    [blit copyFromBuffer:source
            sourceOffset:0
           sourceBytesPerRow:view.width * kBytesPerPixel
         sourceBytesPerImage:new_bytes
                  sourceSize:MTLSizeMake(view.width, view.height, 1)
                   toTexture:private_texture
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
      if (error)
        *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                              : "representation transform blit failed";
      return false;
    }
  }

  uint32_t new_epoch = 0;
  if (!arena.transform(view.allocation, view.allocation_generation, &new_epoch, error)) return false;
  // Publishing the new epoch invalidates every facet minted against the old
  // one, including the TransferFacet above; retire_stale is what actually
  // returns their slots, and only for slots with no work still in flight.
  const uint32_t retired = static_cast<uint32_t>(pool.retire_stale(arena));

  core::FacetRef out_facet{};
  if (!pool.acquire(arena, view, target_kind, &out_facet, error)) return false;
  // Install the Private texture so subsequent facet uses resolve through
  // FacetPool onto the optimal representation instead of recreating a Shared
  // texture from host bytes.
  if (impl_->install_facet_record(out_facet, view, target_kind, new_epoch, private_texture, error) == nil)
    return false;

  result->new_epoch = new_epoch;
  result->old_backing_bytes = old_bytes;
  result->new_backing_bytes = new_bytes;
  result->temporary_bytes = 0;
  result->encoder_count = 1;
  result->used_private_optimal = true;
  result->retired_facet_count = retired;
  result->out_facet = out_facet;
  result->report = make_facet_report();
  result->report.encoder_count = 1;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  result->report.add("representation_transform", hal::LoweringClass::DevicePass, 1, new_bytes,
                     "blit from the TransferFacet's linear buffer into a Private optimal texture");
  result->report.add("representation_transform_peak", hal::LoweringClass::Direct, 1, old_bytes + new_bytes,
                     "old linear backing retained alongside the new texture; no staging copy");
  result->report.add("facet_retire_stale", hal::LoweringClass::Direct, retired, 0,
                     "facets invalidated by the new RepresentationEpoch");
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
