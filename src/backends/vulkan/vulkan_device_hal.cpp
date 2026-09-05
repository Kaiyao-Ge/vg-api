#include "backends/vulkan/vulkan_device_internal.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace vg::vulkan {
using detail::set_error;

DeviceHal::DeviceHal() : state_(std::make_unique<detail::DeviceState>(*this)) {}
DeviceHal::~DeviceHal() = default;
detail::DeviceState::DeviceState(DeviceHal& owner) : owner_(owner) {}
vg::core::FacetPool& detail::DeviceState::facet_pool() { return owner_.facet_pool(); }
vg::core::EnvelopeContinuationTable& detail::DeviceState::envelope_continuations() { return owner_.envelope_continuations(); }
const vg::hal::CapabilitySnapshot& detail::DeviceState::capabilities() const { return capabilities_; }
bool DeviceHal::compile(const vg::core::ExecutionPlan& plan, vg::hal::CompiledPlan* compiled, std::string* error) {
  return state_->compile(plan, compiled, error);
}
bool DeviceHal::submit(const vg::hal::CompiledPlan& compiled, vg::core::Arena& arena, vg::hal::Submission* submission, std::string* error) {
  return state_->submit(compiled, arena, submission, error);
}

const vg::hal::CapabilitySnapshot& DeviceHal::capabilities() const {
  return state_->capabilities_;
}

std::unique_ptr<DeviceHal> DeviceHal::create_impl(const uint8_t* uuid, std::string* error) {
  auto adapter = std::unique_ptr<DeviceHal>(new DeviceHal());
#if !defined(VG_HAS_VULKAN)
  (void)uuid;
  adapter->state_->capabilities_.backend = vg::hal::BackendKind::Vulkan;
  adapter->state_->capabilities_.adapter_name = "Vulkan unavailable (build without VG_HAS_VULKAN)";
  set_error(error, "Vulkan adapter is unavailable in this build");
  return nullptr;
#else
  // Request instance-level 1.3: any Vulkan loader on the target Linux/NVIDIA
  // servers this backend ships to has supported 1.3 for years. The specific
  // physical device's own reported apiVersion (checked below) is what
  // actually gates which promoted-core feature structs are legal to chain.
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "VG Vulkan DeviceHAL";
  app.applicationVersion = 1;
  app.pEngineName = "VG";
  app.engineVersion = 1;
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app;
  if (vkCreateInstance(&instance_info, nullptr, &adapter->state_->instance_) != VK_SUCCESS) {
    set_error(error, "failed to create Vulkan instance");
    return nullptr;
  }

  uint32_t device_count = 0;
  if (vkEnumeratePhysicalDevices(adapter->state_->instance_, &device_count, nullptr) != VK_SUCCESS ||
      device_count == 0) {
    set_error(error, "no Vulkan physical device available");
    return nullptr;
  }
  std::vector<VkPhysicalDevice> devices(device_count);
  vkEnumeratePhysicalDevices(adapter->state_->instance_, &device_count, devices.data());
  if (uuid == nullptr) {
    adapter->state_->physical_device_ = devices.front();
  } else {
    bool matched = false;
    for (VkPhysicalDevice candidate : devices) {
      VkPhysicalDeviceProperties candidate_properties{};
      vkGetPhysicalDeviceProperties(candidate, &candidate_properties);
      uint8_t candidate_uuid[16] = {};
      std::memcpy(candidate_uuid, &candidate_properties.vendorID, sizeof(candidate_properties.vendorID));
      std::memcpy(candidate_uuid + 4, &candidate_properties.deviceID, sizeof(candidate_properties.deviceID));
      std::memcpy(candidate_uuid + 8, candidate_properties.pipelineCacheUUID, 8);
      if (std::memcmp(candidate_uuid, uuid, 16) == 0) {
        adapter->state_->physical_device_ = candidate;
        matched = true;
        break;
      }
    }
    if (!matched) {
      set_error(error, "no Vulkan physical device matches the requested adapter uuid");
      return nullptr;
    }
  }
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(adapter->state_->physical_device_, &properties);

  // BufferDeviceAddress and 64-bit shader buffer atomics are both promoted
  // into core Vulkan 1.2 (VkPhysicalDeviceVulkan12Features::bufferDeviceAddress
  // / shaderBufferInt64Atomics) -- this backend requires that baseline and
  // enables both purely through that core-promoted feature struct, with no
  // VkDeviceCreateInfo extension strings needed for either.
  const bool device_supports_1_2 =
      VK_API_VERSION_MAJOR(properties.apiVersion) > 1 ||
      (VK_API_VERSION_MAJOR(properties.apiVersion) == 1 && VK_API_VERSION_MINOR(properties.apiVersion) >= 2);
  if (!device_supports_1_2) {
    set_error(error, "Vulkan physical device does not support API version 1.2, "
                     "required for buffer device address and 64-bit shader atomics");
    return nullptr;
  }
  const bool device_supports_1_3 =
      VK_API_VERSION_MAJOR(properties.apiVersion) > 1 ||
      (VK_API_VERSION_MAJOR(properties.apiVersion) == 1 && VK_API_VERSION_MINOR(properties.apiVersion) >= 3);

  adapter->state_->capabilities_.backend = vg::hal::BackendKind::Vulkan;
  adapter->state_->capabilities_.adapter_name = properties.deviceName;
  adapter->state_->capabilities_.driver = "Vulkan " + std::to_string(VK_API_VERSION_MAJOR(properties.apiVersion)) +
      "." + std::to_string(VK_API_VERSION_MINOR(properties.apiVersion));
  adapter->state_->capabilities_.max_buffer_size = properties.limits.maxStorageBufferRange;
  adapter->state_->capabilities_.address_width = 0;
  adapter->state_->capabilities_.min_buffer_alignment = static_cast<uint32_t>(
      std::min<VkDeviceSize>(properties.limits.minStorageBufferOffsetAlignment,
                             std::numeric_limits<uint32_t>::max()));

  uint32_t queue_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(adapter->state_->physical_device_, &queue_count, nullptr);
  std::vector<VkQueueFamilyProperties> queues(queue_count);
  vkGetPhysicalDeviceQueueFamilyProperties(adapter->state_->physical_device_, &queue_count, queues.data());
  // One queue family for everything, and preferably one that can also draw:
  // 07 §9's raster lowering records vkCmdBeginRendering/vkCmdDraw into the same
  // command pool the compute and Stage-5 transfer paths use, and 07 §4 permits
  // an adapter to serialize on a single queue. Preferring a graphics-capable
  // family (rather than requiring one) keeps a compute-only device working
  // exactly as it did before -- it simply does not get the Raster bit below,
  // which is the honest report of what it can do.
  uint32_t graphics_and_compute_family = UINT32_MAX;
  uint32_t compute_only_family = UINT32_MAX;
  for (uint32_t i = 0; i < queue_count; ++i) {
    if ((queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) continue;
    if (compute_only_family == UINT32_MAX) compute_only_family = i;
    if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && graphics_and_compute_family == UINT32_MAX)
      graphics_and_compute_family = i;
  }
  const bool graphics_capable = graphics_and_compute_family != UINT32_MAX;
  adapter->state_->compute_queue_family_ = graphics_capable ? graphics_and_compute_family : compute_only_family;
  if (adapter->state_->compute_queue_family_ == UINT32_MAX) {
    set_error(error, "Vulkan device has no compute queue family");
    return nullptr;
  }
  // A queue family that cannot do transfers cannot carry Stage 5's
  // vkCmdCopyBufferToImage. In practice VK_QUEUE_COMPUTE_BIT implies transfer
  // support, but the bit is read rather than assumed, because the whole point
  // of the capability probe is that this backend claims only what the device
  // actually reported (07 §1).
  const bool transfer_capable =
      (queues[adapter->state_->compute_queue_family_].queueFlags &
       (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;

  // 07 §6's facet lowering rests on what the device says each format can do
  // under VK_IMAGE_TILING_OPTIMAL, so it is queried once here and never
  // guessed at a use site. A format missing a feature makes the corresponding
  // request Unsupported later; it never causes a substitute format to be
  // chosen quietly (06 §6.2).
  const auto probe_format = [&](VkFormat format) {
    VkFormatProperties properties_for_format{};
    vkGetPhysicalDeviceFormatProperties(adapter->state_->physical_device_, format, &properties_for_format);
    const VkFormatFeatureFlags features_for_format = properties_for_format.optimalTilingFeatures;
    detail::DeviceState::FormatSupport support{};
    support.sampled_image = (features_for_format & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    support.storage_image = (features_for_format & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    support.color_attachment = (features_for_format & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    support.depth_stencil_attachment =
        (features_for_format & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    support.transfer_dst = (features_for_format & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0;
    support.transfer_src = (features_for_format & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
    return support;
  };
  adapter->state_->rgba8_support_ = probe_format(VK_FORMAT_R8G8B8A8_UNORM);
  adapter->state_->r32f_support_ = probe_format(VK_FORMAT_R32_SFLOAT);
  adapter->state_->d32_support_ = probe_format(VK_FORMAT_D32_SFLOAT);
  adapter->state_->framebuffer_color_sample_counts_ = properties.limits.framebufferColorSampleCounts;

  // 05 §10: a backend pipeline binary cache is explicitly non-portable, so
  // every PipelineKey this adapter builds carries this device's identity and no
  // key can look reusable across drivers.
  adapter->state_->target_identity_ = std::string("vulkan|") + properties.deviceName + "|api" +
                              std::to_string(properties.apiVersion) + "|driver" +
                              std::to_string(properties.driverVersion);

  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  features.pNext = &features12;
  features12.pNext = &features13;
  vkGetPhysicalDeviceFeatures2(adapter->state_->physical_device_, &features);
  // The feature bit, rather than extension presence alone, authorizes BDA.
  // An extension can be advertised while the feature remains disabled.
  const bool bda = features12.bufferDeviceAddress == VK_TRUE;
  const bool timeline = features12.timelineSemaphore == VK_TRUE;
  const bool atomic_int64 = features12.shaderBufferInt64Atomics == VK_TRUE && features.features.shaderInt64 == VK_TRUE;
  // sync2 requires the device to genuinely report core 1.3: chaining
  // VkPhysicalDeviceVulkan13Features into device creation is only spec-legal
  // when the physical device's own apiVersion is >= 1.3. This backend's
  // dispatch path uses classic vkQueueSubmit + VkFence (not vkQueueSubmit2),
  // so on 1.2-only hardware the EffectDag/sync2 capability bit is simply not
  // claimed rather than chasing the VK_KHR_synchronization2 extension struct.
  const bool sync2 = device_supports_1_3 && features13.synchronization2 == VK_TRUE;
  // Dynamic rendering is how 07 §9 says an AttachmentFacet pass is lowered
  // here: no VkRenderPass, no VkFramebuffer, attachment format and sample count
  // compiled into the pipeline instead. Like sync2 it is only claimable when
  // the device itself reports core 1.3, since that is what makes chaining
  // VkPhysicalDeviceVulkan13Features into vkCreateDevice legal.
  adapter->state_->supports_dynamic_rendering_ = device_supports_1_3 && features13.dynamicRendering == VK_TRUE;
#if defined(VG_GLSLC_PATH)
  constexpr bool spirv_compiler_available = true;
#else
  // Without glslc there is no SPIR-V for the facet/raster kernels at all, so
  // the three Phase C bits below are left clear rather than advertised and then
  // failed at first use. The pre-existing compute bits keep their older
  // behaviour (they report a compile failure through compile()'s Unsupported
  // event instead) so this change cannot regress an existing caller.
  constexpr bool spirv_compiler_available = false;
#endif
  if (bda) adapter->state_->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::LinearAddress);
  if (bda) adapter->state_->capabilities_.address_width = 64;
  if (timeline) adapter->state_->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::Timeline);
  if (bda && sync2 && spirv_compiler_available) {
    // Task publication uses a BDA-addressed shader and a sync2 shader-write to
    // host-read dependency. Canonical programs execute separately per Task;
    // this capability does not imply indirect execution.
    adapter->state_->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::TaskPublication);
    // Multi-Task plans consume the assembler-sealed component/wave schedule and emit sync2
    // compute memory dependencies at every conflicting effect boundary.
    adapter->state_->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::EffectDag);
  }

  // --- Phase C capability bits ---------------------------------------------
  // Each one is an obligation (device_hal.h spells them out), so each is
  // claimed only when every part of this backend's lowering for it exists on
  // this device. A missing part leaves the bit clear, and compile() then
  // rejects a plan that needs it with a VG-concept diagnostic rather than
  // succeeding as though the work had been done (START.md §4, invariant 10).
  //
  // RepresentationTransform: an optimal-tiled VkImage, a transfer copy out of
  // the allocation's linear buffer, and the two layout barriers around it.
  // Gated on sync2 because those barriers are VkImageMemoryBarrier2, on the
  // queue actually supporting transfer, and on RGBA8Unorm genuinely being
  // copyable both ways under optimal tiling. Per-request format checks still
  // run in compile(); this bit is the device-wide precondition.
  const bool representation_transform = sync2 && transfer_capable && spirv_compiler_available &&
                                        adapter->state_->rgba8_support_.transfer_dst &&
                                        adapter->state_->rgba8_support_.transfer_src;
  if (representation_transform)
    adapter->state_->capabilities_.capability_bits |=
        static_cast<uint64_t>(vg::hal::Capability::RepresentationTransform);
  const bool raster =
      graphics_capable && adapter->state_->supports_dynamic_rendering_ && sync2 &&
      spirv_compiler_available && transfer_capable &&
      adapter->state_->rgba8_support_.sampled_image &&
      adapter->state_->rgba8_support_.color_attachment &&
      adapter->state_->rgba8_support_.transfer_dst &&
      adapter->state_->rgba8_support_.transfer_src &&
      adapter->state_->d32_support_.depth_stencil_attachment &&
      adapter->state_->d32_support_.transfer_dst &&
      adapter->state_->d32_support_.transfer_src;
  if (raster) {
    adapter->state_->capabilities_.capability_bits |=
        static_cast<uint64_t>(vg::hal::Capability::Raster);
    adapter->state_->capabilities_.capability_bits |=
        static_cast<uint64_t>(vg::hal::Capability::IndexedBinding);
    // Formal user GLSL packages compile through the same glslc-backed Raster
    // pipeline, so advertising the import contract shares Raster's complete
    // device gate instead of promising a standalone parser path.
    adapter->state_->capabilities_.capability_bits |=
        static_cast<uint64_t>(vg::hal::Capability::UserShaderImport);
    adapter->state_->capabilities_.capability_bits |=
        static_cast<uint64_t>(vg::hal::Capability::IndirectTier2Select);
  }
  // CheckedFacetGeneration: the in-shader guard of 06 §6.4, which exists here
  // only because sample_facet_vulkan_source() is specialized with constant_id 0
  // = true and the token/generation-table/slot-count/violation bindings are
  // written. Both halves need a SPIR-V compiler and a genuinely sampleable
  // format, so both are required before the promise is made.
  if (spirv_compiler_available && adapter->state_->rgba8_support_.sampled_image)
    adapter->state_->capabilities_.capability_bits |=
        static_cast<uint64_t>(vg::hal::Capability::CheckedFacetGeneration);
  adapter->state_->capabilities_.validation_available = true;
  adapter->state_->capabilities_.timestamps_available = queues[adapter->state_->compute_queue_family_].timestampValidBits != 0;

  if (!bda) {
    set_error(error, "Vulkan device does not support bufferDeviceAddress, required by this backend");
    return nullptr;
  }

  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = adapter->state_->compute_queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkPhysicalDeviceFeatures enabled_features{};
  enabled_features.shaderInt64 = atomic_int64 ? VK_TRUE : VK_FALSE;

  VkPhysicalDeviceVulkan13Features enabled13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  enabled13.synchronization2 = sync2 ? VK_TRUE : VK_FALSE;
  // Enabled, not merely probed: vkCmdBeginRendering is only legal on a device
  // created with this feature on, and the Raster bit above already promised it.
  enabled13.dynamicRendering = adapter->state_->supports_dynamic_rendering_ ? VK_TRUE : VK_FALSE;

  VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  enabled12.bufferDeviceAddress = VK_TRUE;
  enabled12.timelineSemaphore = timeline ? VK_TRUE : VK_FALSE;
  enabled12.shaderBufferInt64Atomics = atomic_int64 ? VK_TRUE : VK_FALSE;
  if (device_supports_1_3) enabled12.pNext = &enabled13;

  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.pQueueCreateInfos = &queue_info;
  device_info.queueCreateInfoCount = 1;
  device_info.pNext = &enabled12;
  device_info.pEnabledFeatures = &enabled_features;
  // Everything this backend needs (BDA, timeline semaphores, 64-bit shader
  // buffer atomics, and sync2 when core-1.3) is promoted-to-core and enabled
  // purely via the feature structs chained above -- no device extension
  // needs to be requested, so this is deliberately left empty rather than
  // populated with names that would be no-ops.
  device_info.enabledExtensionCount = 0;
  device_info.ppEnabledExtensionNames = nullptr;
  if (vkCreateDevice(adapter->state_->physical_device_, &device_info, nullptr, &adapter->state_->device_) != VK_SUCCESS) {
    set_error(error, "failed to create Vulkan device");
    return nullptr;
  }
  vkGetDeviceQueue(adapter->state_->device_, adapter->state_->compute_queue_family_, 0, &adapter->state_->compute_queue_);
  // Created once here, alongside device creation, rather than truly lazily
  // on first timeline_wait/timeline_signal use: submit()'s timeline
  // pre-check queries the semaphore's counter value unconditionally
  // whenever a wait/signal is requested, so it must already exist by the
  // time any submission runs. A creation failure here is a hard adapter
  // failure, not a soft "Timeline unsupported" -- the feature bit above
  // already promised timeline support based on the physical device's own
  // reported capability.
  if (timeline) {
    std::string timeline_error;
    if (!adapter->state_->ensure_timeline_semaphore(&timeline_error)) {
      set_error(error, ("failed to create Vulkan timeline semaphore: " + timeline_error).c_str());
      return nullptr;
    }
  }
  return adapter;
#endif
}

std::unique_ptr<DeviceHal> make_device_hal(std::string* error) {
  return DeviceHal::create_impl(nullptr, error);
}

std::unique_ptr<DeviceHal> make_device_hal(const uint8_t uuid[16], std::string* error) {
  return DeviceHal::create_impl(uuid, error);
}

}  // namespace vg::vulkan
