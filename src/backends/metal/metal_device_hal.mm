#include "backends/metal/metal_device_internal.h"
#include <cstring>

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

}

DeviceHal::DeviceHal(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DeviceHal::~DeviceHal() = default;

const hal::CapabilitySnapshot& DeviceHal::capabilities() const { return impl_->snapshot.hal; }
const DeviceSnapshot& DeviceHal::snapshot() const { return impl_->snapshot; }

bool DeviceHal::compile(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled, std::string* error) {
  return compile_plan(plan, compiled, error);
}
bool DeviceHal::submit(const hal::CompiledPlan& compiled, core::Arena& arena,
                       hal::Submission* submission, std::string* error) {
  return submit_plan(compiled, arena, submission, error);
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
