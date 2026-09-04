#include "backends/vulkan/vulkan_device_internal.h"
#include "compiler/compute_task_ring.h"
#include <algorithm>
#include <cstring>

namespace vg::vulkan::detail {

#if defined(VG_HAS_VULKAN)
FacetUseGuard::FacetUseGuard(vg::core::FacetPool& pool, vg::core::FacetRef ref) : pool_(pool), ref_(ref) {}
FacetUseGuard::~FacetUseGuard() { if (held_) pool_.end_gpu_use(ref_); }
bool FacetUseGuard::begin(const vg::core::Arena& arena, std::string* error) {
  held_ = pool_.begin_gpu_use(arena, ref_, error);
  return held_;
}

bool find_memory_type(VkPhysicalDevice physical_device, uint32_t type_bits, VkMemoryPropertyFlags required,
                      uint32_t* out) {
  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
  for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) == 0) continue;
    if ((properties.memoryTypes[i].propertyFlags & required) == required) {
      *out = i;
      return true;
    }
  }
  return false;
}

void destroy_raw_buffer(VkDevice device, RawBuffer* buffer) {
  if (buffer->mapped != nullptr) vkUnmapMemory(device, buffer->memory);
  if (buffer->buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer->buffer, nullptr);
  if (buffer->memory != VK_NULL_HANDLE) vkFreeMemory(device, buffer->memory, nullptr);
  *buffer = RawBuffer{};
}

bool create_raw_buffer(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size,
                       VkBufferUsageFlags usage, bool want_address, bool want_map, RawBuffer* out,
                       std::string* error) {
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device, &buffer_info, nullptr, &out->buffer) != VK_SUCCESS) {
    if (error) *error = "vkCreateBuffer failed";
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, out->buffer, &requirements);
  uint32_t memory_type = 0;
  if (!find_memory_type(physical_device, requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &memory_type)) {
    destroy_raw_buffer(device, out);
    if (error) *error = "no host-visible-coherent memory type available";
    return false;
  }
  VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags_info.flags = want_address ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;
  VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  if (want_address) alloc_info.pNext = &flags_info;
  alloc_info.allocationSize = requirements.size;
  alloc_info.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(device, &alloc_info, nullptr, &out->memory) != VK_SUCCESS) {
    destroy_raw_buffer(device, out);
    if (error) *error = "vkAllocateMemory failed";
    return false;
  }
  if (vkBindBufferMemory(device, out->buffer, out->memory, 0) != VK_SUCCESS) {
    destroy_raw_buffer(device, out);
    if (error) *error = "vkBindBufferMemory failed";
    return false;
  }
  // TASK-D3 / ADR-037 (compile-review-only): the bind above is a committed,
  // fully-backed VkDeviceMemory attachment. Vulkan sparse residency
  // (VkSparseMemoryBind / sparse-queue bind) is a different, explicit
  // map/unmap contract -- it is not an automatic page fault on ordinary
  // device-address load/store, and this file does not implement it. Do not
  // read this ordinary bind as sparse, and do not invent a runtime fault
  // path to stand in for one (09 E011; 07 §13).
  if (want_map && vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->mapped) != VK_SUCCESS) {
    destroy_raw_buffer(device, out);
    if (error) *error = "vkMapMemory failed";
    return false;
  }
  if (want_address) {
    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = out->buffer;
    out->address = vkGetBufferDeviceAddress(device, &address_info);
  }
  return true;
}

bool DeviceState::ensure_buffer(const vg::core::Allocation& allocation, AllocationRecord** out, std::string* error) {
  const size_t needed = std::max<size_t>(allocation.bytes.size(), 1);
  auto it = allocation_map_.find(allocation.id);
  if (it != allocation_map_.end() &&
      (it->second.generation != allocation.generation || it->second.byte_size < needed)) {
    vkUnmapMemory(device_, it->second.memory);
    vkDestroyBuffer(device_, it->second.buffer, nullptr);
    vkFreeMemory(device_, it->second.memory, nullptr);
    allocation_map_.erase(it);
    it = allocation_map_.end();
  }
  if (it == allocation_map_.end()) {
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = static_cast<VkDeviceSize>(needed);
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer{VK_NULL_HANDLE};
    if (vkCreateBuffer(device_, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
      if (error) *error = "vkCreateBuffer failed";
      return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer, &requirements);
    uint32_t memory_type = 0;
    if (!find_memory_type(physical_device_, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &memory_type)) {
      vkDestroyBuffer(device_, buffer, nullptr);
      if (error) *error = "no host-visible-coherent memory type available";
      return false;
    }
    VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.pNext = &flags_info;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    VkDeviceMemory memory{VK_NULL_HANDLE};
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &memory) != VK_SUCCESS) {
      vkDestroyBuffer(device_, buffer, nullptr);
      if (error) *error = "vkAllocateMemory failed";
      return false;
    }
    if (vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
      vkDestroyBuffer(device_, buffer, nullptr);
      vkFreeMemory(device_, memory, nullptr);
      if (error) *error = "vkBindBufferMemory failed";
      return false;
    }
    void* mapped = nullptr;
    if (vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      vkDestroyBuffer(device_, buffer, nullptr);
      vkFreeMemory(device_, memory, nullptr);
      if (error) *error = "vkMapMemory failed";
      return false;
    }
    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = buffer;
    const VkDeviceAddress address = vkGetBufferDeviceAddress(device_, &address_info);

    AllocationRecord record{buffer, memory, address, mapped, allocation.generation, needed};
    it = allocation_map_.emplace(allocation.id, record).first;
  }
  if (!allocation.bytes.empty()) std::memcpy(it->second.mapped, allocation.bytes.data(), allocation.bytes.size());
  *out = &it->second;
  return true;
}

VkFormat to_vk_format(vg::core::PixelFormat format) {
  return format == vg::core::PixelFormat::RGBA8Unorm ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32_SFLOAT;
}

VkComponentSwizzle to_vk_swizzle(vg::core::Swizzle swizzle) {
  switch (swizzle) {
    case vg::core::Swizzle::Red: return VK_COMPONENT_SWIZZLE_R;
    case vg::core::Swizzle::Green: return VK_COMPONENT_SWIZZLE_G;
    case vg::core::Swizzle::Blue: return VK_COMPONENT_SWIZZLE_B;
    case vg::core::Swizzle::Alpha: return VK_COMPONENT_SWIZZLE_A;
    case vg::core::Swizzle::Zero: return VK_COMPONENT_SWIZZLE_ZERO;
    case vg::core::Swizzle::One: return VK_COMPONENT_SWIZZLE_ONE;
  }
  return VK_COMPONENT_SWIZZLE_IDENTITY;
}

VkComponentMapping to_vk_component_mapping(const vg::core::SwizzleChannels& swizzle) {
  VkComponentMapping mapping{};
  mapping.r = to_vk_swizzle(swizzle.red);
  mapping.g = to_vk_swizzle(swizzle.green);
  mapping.b = to_vk_swizzle(swizzle.blue);
  mapping.a = to_vk_swizzle(swizzle.alpha);
  return mapping;
}

uint32_t packed_swizzle(const vg::core::SwizzleChannels& swizzle) {
  return (static_cast<uint32_t>(swizzle.red) << 24) | (static_cast<uint32_t>(swizzle.green) << 16) |
         (static_cast<uint32_t>(swizzle.blue) << 8) | static_cast<uint32_t>(swizzle.alpha);
}

VkImageViewType to_vk_view_type(vg::core::ViewDimension dimension) {
  return dimension == vg::core::ViewDimension::Texture2DArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                             : VK_IMAGE_VIEW_TYPE_2D;
}

VkImageLayout facet_read_layout(vg::core::FacetKind kind) {
  switch (kind) {
    case vg::core::FacetKind::Sample: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case vg::core::FacetKind::Storage: return VK_IMAGE_LAYOUT_GENERAL;
    case vg::core::FacetKind::Attachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    default: return VK_IMAGE_LAYOUT_UNDEFINED;
  }
}

VkImageUsageFlags facet_image_usage(vg::core::FacetKind kind) {
  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT;
  if (kind == vg::core::FacetKind::Storage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (kind == vg::core::FacetKind::Attachment) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  return usage;
}

std::array<float, 4> decode_first_texel(const void* bytes, VkFormat format) {
  if (format == VK_FORMAT_R8G8B8A8_UNORM) {
    const uint8_t* rgba = static_cast<const uint8_t*>(bytes);
    return {rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f};
  }
  float value{};
  std::memcpy(&value, bytes, sizeof(value));
  return {value, 0.0f, 0.0f, 1.0f};
}

bool DeviceState::create_task_ring_buffers(uint32_t task_count, TaskRingBuffers* out, std::string* error) {
  static_assert(vg::compiler::kTaskRingDispatchXWord + 2 <
                vg::compiler::kTaskRingWordsPerRecord);
  const VkDeviceSize state_size = std::max<VkDeviceSize>(task_count * sizeof(uint32_t), sizeof(uint32_t));
  const VkDeviceSize record_size = std::max<VkDeviceSize>(
      static_cast<VkDeviceSize>(task_count) * vg::compiler::kTaskRingWordsPerRecord * sizeof(uint32_t),
      vg::compiler::kTaskRingWordsPerRecord * sizeof(uint32_t));
  const VkBufferUsageFlags ring_usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

  RawBuffer state;
  if (!create_raw_buffer(device_, physical_device_, state_size, ring_usage, /*want_address=*/true,
                         /*want_map=*/true, &state, error))
    return false;
  RawBuffer fields;
  if (!create_raw_buffer(device_, physical_device_, record_size, ring_usage, true, true, &fields, error)) {
    destroy_raw_buffer(device_, &state);
    return false;
  }
  RawBuffer inputs;
  if (!create_raw_buffer(device_, physical_device_, record_size, ring_usage, true, true, &inputs, error)) {
    destroy_raw_buffer(device_, &state);
    destroy_raw_buffer(device_, &fields);
    return false;
  }
  std::memset(state.mapped, 0, static_cast<size_t>(state_size));

  out->state_buffer = state.buffer;
  out->state_memory = state.memory;
  out->state_address = state.address;
  out->state_mapped = state.mapped;
  out->fields_buffer = fields.buffer;
  out->fields_memory = fields.memory;
  out->fields_address = fields.address;
  out->fields_mapped = fields.mapped;
  out->inputs_buffer = inputs.buffer;
  out->inputs_memory = inputs.memory;
  out->inputs_address = inputs.address;
  out->inputs_mapped = inputs.mapped;
  out->task_count = task_count;
  return true;
}

void DeviceState::destroy_task_ring_buffers(TaskRingBuffers* buffers) {
  RawBuffer state{buffers->state_buffer, buffers->state_memory, buffers->state_address, buffers->state_mapped};
  RawBuffer fields{buffers->fields_buffer, buffers->fields_memory, buffers->fields_address, buffers->fields_mapped};
  RawBuffer inputs{buffers->inputs_buffer, buffers->inputs_memory, buffers->inputs_address, buffers->inputs_mapped};
  destroy_raw_buffer(device_, &state);
  destroy_raw_buffer(device_, &fields);
  destroy_raw_buffer(device_, &inputs);
  *buffers = TaskRingBuffers{};
}

size_t encode_first_texel(const std::array<float, 4>& rgba, VkFormat format, uint8_t* out) {
  if (format == VK_FORMAT_R8G8B8A8_UNORM) {
    for (size_t channel = 0; channel < 4; ++channel) {
      const float clamped = std::min(1.0f, std::max(0.0f, rgba[channel]));
      out[channel] = static_cast<uint8_t>(clamped * 255.0f + 0.5f);
    }
    return 4;
  }
  std::memcpy(out, &rgba[0], sizeof(float));
  return sizeof(float);
}

const vg::core::FacetSlot* DeviceState::resolve_facet(const vg::core::Arena& arena, const vg::core::FacetPool& pool,
                                                    vg::core::FacetRef ref, vg::core::FacetKind expected_kind,
                                                    std::string* error) {
  vg::core::FacetStatus status = vg::core::FacetStatus::Ok;
  const vg::core::FacetSlot* slot = pool.lookup(arena, ref, &status);
  if (slot == nullptr) {
    if (error) *error = std::string("facet reference did not resolve: ") + vg::core::to_string(status);
    return nullptr;
  }
  if (slot->kind != expected_kind) {
    if (error) *error = "facet kind mismatch: the pool slot names a different facet of this CanonicalView";
    return nullptr;
  }
  return slot;
}

bool DeviceState::ensure_facet_image(const vg::core::Arena& arena, const vg::core::FacetPool& pool,
                                   vg::core::FacetRef ref, vg::core::FacetKind expected_kind,
                                   VkBuffer upload_source, VkDeviceSize upload_source_offset,
                                   VulkanFacetRecord** out, bool* cache_hit, uint64_t* temporary_bytes,
                                   std::string* error) {
  if (cache_hit != nullptr) *cache_hit = false;
  if (temporary_bytes != nullptr) *temporary_bytes = 0;
  const vg::core::FacetSlot* slot = resolve_facet(arena, pool, ref, expected_kind, error);
  if (slot == nullptr) return false;
  const vg::core::CanonicalView view = slot->view;

  if (expected_kind != vg::core::FacetKind::Sample && expected_kind != vg::core::FacetKind::Storage &&
      expected_kind != vg::core::FacetKind::Attachment) {
    if (error)
      *error = "Unsupported: AddressFacet/TransferFacet name how an existing linear representation is "
               "reached (BDA), not an image representation, so neither has a VkImage here (02 §3.3)";
    return false;
  }
  // A swizzle reinterprets a shader *read*. Vulkan applies a view's
  // VkComponentMapping to sampled reads, not to image stores and not to a color
  // attachment write, so rather than quietly dropping the channel mapping the
  // contract asked for, those kinds are refused (06 §6.1, START.md §4
  // invariant 10).
  if (!view.swizzle.identity() && expected_kind != vg::core::FacetKind::Sample) {
    if (error)
      *error = "Unsupported: a non-identity swizzle applies to a SampleFacet only; a Storage or "
               "Attachment facet would silently ignore it";
    return false;
  }

  const VkFormat format = to_vk_format(view.format);
  const FormatSupport& support = format_support(view.format);
  if (!support.transfer_dst || !support.transfer_src) {
    if (error)
      *error = "Unsupported: this device's optimal tiling does not advertise transfer for the view's "
               "format, so the linear backing cannot be copied into an image representation";
    return false;
  }
  if (expected_kind == vg::core::FacetKind::Sample && !support.sampled_image) {
    if (error) *error = "Unsupported: the view's format is not a sampled-image format on this device";
    return false;
  }
  if (expected_kind == vg::core::FacetKind::Storage && !support.storage_image) {
    if (error)
      *error = "Unsupported: the view's format is not a storage-image format on this device; the "
               "format is not rewritten to make the write legal (06 §6.2)";
    return false;
  }
  if (expected_kind == vg::core::FacetKind::Attachment && !support.color_attachment) {
    if (error) *error = "Unsupported: the view's format is not a color-attachment format on this device";
    return false;
  }

  FacetImageKey key{};
  key.facet_index = ref.index;
  key.facet_generation = ref.generation;
  key.representation_epoch = slot->representation_epoch;
  key.kind = static_cast<uint32_t>(slot->kind);
  key.format = static_cast<uint32_t>(format);
  key.view_type = static_cast<uint32_t>(to_vk_view_type(view.dimension));
  key.width = view.width;
  key.height = view.height;
  key.array_layers = view.array_layers;
  key.mip_levels = view.mip_levels;
  key.swizzle = packed_swizzle(view.swizzle);

  // Checked before the allocation's bytes are looked at, deliberately: after a
  // ConsumeInput the linear backing this image superseded is gone (02 §4.2,
  // core::Arena::consume_representation releases it), and the facet the
  // transform published must keep resolving anyway.
  const auto cached = facet_images_.find(key);
  if (cached != facet_images_.end()) {
    if (cache_hit != nullptr) *cache_hit = true;
    *out = &cached->second;
    return true;
  }

  const uint64_t view_bytes = view.byte_size();
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {view.width, view.height, 1};
  image_info.mipLevels = view.mip_levels;
  image_info.arrayLayers = view.array_layers;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  // 07 §13's linear<->optimal transform is the whole point of Stage 5 here:
  // the allocation's own backing is already the linear representation, so an
  // image that was also linear-tiled would make the transform a no-op that
  // still cost a copy.
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = facet_image_usage(expected_kind);
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VulkanFacetRecord record{};
  if (vkCreateImage(device_, &image_info, nullptr, &record.image) != VK_SUCCESS) {
    if (error) *error = "vkCreateImage failed for the facet's image representation";
    return false;
  }
  const auto destroy_partial = [&]() {
    if (record.view != VK_NULL_HANDLE) vkDestroyImageView(device_, record.view, nullptr);
    if (record.image != VK_NULL_HANDLE) vkDestroyImage(device_, record.image, nullptr);
    if (record.memory != VK_NULL_HANDLE) vkFreeMemory(device_, record.memory, nullptr);
  };

  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device_, record.image, &requirements);
  uint32_t memory_type = 0;
  if (!find_memory_type(physical_device_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        &memory_type)) {
    destroy_partial();
    if (error) *error = "no device-local memory type available for an optimal-tiled facet image";
    return false;
  }
  VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc_info.allocationSize = requirements.size;
  alloc_info.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(device_, &alloc_info, nullptr, &record.memory) != VK_SUCCESS) {
    destroy_partial();
    if (error) *error = "vkAllocateMemory failed for a facet image";
    return false;
  }
  if (vkBindImageMemory(device_, record.image, record.memory, 0) != VK_SUCCESS) {
    destroy_partial();
    if (error) *error = "vkBindImageMemory failed";
    return false;
  }

  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = record.image;
  view_info.viewType = to_vk_view_type(view.dimension);
  view_info.format = format;
  view_info.components = to_vk_component_mapping(view.swizzle);
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.baseMipLevel = 0;
  view_info.subresourceRange.levelCount = view.mip_levels;
  view_info.subresourceRange.baseArrayLayer = 0;
  view_info.subresourceRange.layerCount = view.array_layers;
  if (vkCreateImageView(device_, &view_info, nullptr, &record.view) != VK_SUCCESS) {
    destroy_partial();
    if (error) *error = "vkCreateImageView failed for a facet image";
    return false;
  }

  record.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  record.format = format;
  record.view_type = view_info.viewType;
  record.extent = image_info.extent;
  record.array_layers = view.array_layers;
  record.mip_levels = view.mip_levels;
  record.facet_index = ref.index;
  record.facet_generation = ref.generation;
  record.representation_epoch = slot->representation_epoch;
  record.kind = slot->kind;
  record.backing_bytes = view_bytes;
  record.allocation_padding_bytes =
      requirements.size > view_bytes ? static_cast<uint64_t>(requirements.size) - view_bytes : 0;

  // Upload source: the caller's buffer when it has one (Stage 5 hands us the
  // allocation's own linear buffer, so the transform needs no staging copy at
  // all and honestly reports 0 temporary bytes), otherwise a transient
  // host-visible staging buffer whose size is reported as temporary.
  RawBuffer staging{};
  VkBuffer source = upload_source;
  VkDeviceSize source_offset = upload_source_offset;
  if (source == VK_NULL_HANDLE) {
    const vg::core::Allocation* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
    if (allocation == nullptr) {
      destroy_partial();
      if (error) *error = "facet backing allocation is not active in this Arena";
      return false;
    }
    if (allocation->bytes.size() < view_bytes) {
      destroy_partial();
      if (error)
        *error = "the linear backing is smaller than the CanonicalView it names, so the view's "
                 "subresource layout cannot be read out of it";
      return false;
    }
    if (!create_raw_buffer(device_, physical_device_, view_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           /*want_address=*/false, /*want_map=*/true, &staging, error)) {
      destroy_partial();
      return false;
    }
    std::memcpy(staging.mapped, allocation->bytes.data(), static_cast<size_t>(view_bytes));
    source = staging.buffer;
    source_offset = 0;
    if (temporary_bytes != nullptr) *temporary_bytes = view_bytes;
  }

  // Every subresource, addressed exactly as CanonicalView says the linear
  // bytes are laid out (slice-major, then ascending mip, each level tightly
  // packed). bufferRowLength/bufferImageHeight are spelled out in texels
  // rather than left 0 ("derive from imageExtent") so the copy states the
  // layout contract it is honoring instead of relying on it coinciding.
  // Every offset is a multiple of 4 because both formats this project models
  // are 4 bytes wide and each level is tightly packed, which is what makes
  // them legal VkBufferImageCopy::bufferOffset values.
  std::vector<VkBufferImageCopy> regions;
  regions.reserve(view.subresource_count());
  for (uint32_t layer = 0; layer < view.array_layers; ++layer) {
    for (uint32_t level = 0; level < view.mip_levels; ++level) {
      VkBufferImageCopy region{};
      region.bufferOffset = source_offset + view.subresource_byte_offset({layer, level});
      region.bufferRowLength = view.mip_width(level);
      region.bufferImageHeight = view.mip_height(level);
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.mipLevel = level;
      region.imageSubresource.baseArrayLayer = layer;
      region.imageSubresource.layerCount = 1;
      region.imageOffset = {0, 0, 0};
      region.imageExtent = {view.mip_width(level), view.mip_height(level), 1};
      regions.push_back(region);
    }
  }

  if (!ensure_command_pool(device_, compute_queue_family_, &command_pool_, error)) {
    destroy_raw_buffer(device_, &staging);
    destroy_partial();
    return false;
  }
  VkCommandBuffer command_buffer{VK_NULL_HANDLE};
  if (!allocate_command_buffer(device_, command_pool_, &command_buffer, error)) {
    destroy_raw_buffer(device_, &staging);
    destroy_partial();
    return false;
  }
  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command_buffer, &begin_info);
  record_layout_transition(command_buffer, &record, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyBufferToImage(command_buffer, source, record.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         static_cast<uint32_t>(regions.size()), regions.data());
  record_layout_transition(command_buffer, &record, facet_read_layout(expected_kind));
  vkEndCommandBuffer(command_buffer);
  if (!submit_and_wait_simple(device_, compute_queue_, command_pool_, command_buffer, error)) {
    destroy_raw_buffer(device_, &staging);
    destroy_partial();
    return false;
  }
  destroy_raw_buffer(device_, &staging);

  *out = &facet_images_.emplace(key, record).first->second;
  return true;
}

uint32_t DeviceState::retire_stale_facet_images(const vg::core::Arena& arena, const vg::core::FacetPool& pool) {
  uint32_t retired = 0;
  for (auto it = facet_images_.begin(); it != facet_images_.end();) {
    const vg::core::FacetRef ref{it->second.facet_index, it->second.facet_generation};
    if (pool.lookup(arena, ref) != nullptr) {
      ++it;
      continue;
    }
    // Only slots FacetPool itself already stopped resolving are reclaimed, and
    // only after this backend's own fence waits (every path here is
    // submit-and-wait), so no VkImage is destroyed under work still in flight
    // (06 §11's "默认保留旧 facet/backing 至相关 command buffer 完成").
    if (it->second.view != VK_NULL_HANDLE) vkDestroyImageView(device_, it->second.view, nullptr);
    if (it->second.image != VK_NULL_HANDLE) vkDestroyImage(device_, it->second.image, nullptr);
    if (it->second.memory != VK_NULL_HANDLE) vkFreeMemory(device_, it->second.memory, nullptr);
    it = facet_images_.erase(it);
    ++retired;
  }
  return retired;
}

bool DeviceState::ensure_sampler(vg::core::FilterMode filter, vg::core::WrapMode wrap, VkSampler* out,
                               std::string* error) {
  const uint32_t key = (static_cast<uint32_t>(filter) << 1) | static_cast<uint32_t>(wrap);
  const auto cached = sampler_cache_.find(key);
  if (cached != sampler_cache_.end()) {
    *out = cached->second;
    return true;
  }
  const VkFilter vk_filter = filter == vg::core::FilterMode::Bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  const VkSamplerAddressMode address_mode = wrap == vg::core::WrapMode::Repeat
                                                ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                                : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = vk_filter;
  sampler_info.minFilter = vk_filter;
  // The mip filter follows the same FilterMode rather than being fixed: the
  // sample kernels take an explicit LOD (a compute dispatch has no implicit
  // derivatives), and a fractional LOD between two levels has to interpolate
  // the same way the in-level filter does or the two backends' mip results
  // would differ for a reason that has nothing to do with VG.
  sampler_info.mipmapMode = filter == vg::core::FilterMode::Bilinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                     : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = address_mode;
  sampler_info.addressModeV = address_mode;
  sampler_info.addressModeW = address_mode;
  sampler_info.minLod = 0.0f;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSampler sampler{VK_NULL_HANDLE};
  if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler) != VK_SUCCESS) {
    if (error) *error = "vkCreateSampler failed";
    return false;
  }
  sampler_cache_.emplace(key, sampler);
  *out = sampler;
  return true;
}

bool DeviceState::ensure_descriptor_pool(std::string* error) {
  if (descriptor_pool_ != VK_NULL_HANDLE) return true;
  // Sized for the largest single set any path here allocates (the array sample
  // kernel: 1 combined image sampler + 5 storage buffers + 3 uniform buffers)
  // with headroom, since the pool is reset before each use rather than grown.
  const VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16},
  };
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 4;
  pool_info.poolSizeCount = static_cast<uint32_t>(sizeof(sizes) / sizeof(sizes[0]));
  pool_info.pPoolSizes = sizes;
  if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
    if (error) *error = "vkCreateDescriptorPool failed";
    return false;
  }
  return true;
}

bool DeviceState::transform_representation(const vg::core::Arena& arena,
                                         const vg::core::RepresentationSemanticPlanItem& request,
                                         vg::core::FacetRef facet,
                                         vg::hal::RepresentationTransformCost* cost,
                                         RepresentationStageCounts* counts, std::string* error) {
  // The copy source is the linear representation reached through a
  // TransferFacet over the same CanonicalView, so even the transform's own read
  // is a pool-resolved capability rather than a raw VkBuffer handle -- and
  // reusing the allocation's existing BDA buffer means this transform needs no
  // staging buffer at all, which is why temporary_bytes below is an honest 0
  // rather than a rounded-down guess.
  vg::core::FacetRef transfer_ref{};
  if (!facet_pool().acquire(arena, request.view, vg::core::FacetKind::Transfer, &transfer_ref, error))
    return false;

  bool ok = false;
  {
    FacetUseGuard use(facet_pool(), transfer_ref);
    if (use.begin(arena, error)) {
      const vg::core::FacetSlot* transfer_slot =
          resolve_facet(arena, facet_pool(), transfer_ref, vg::core::FacetKind::Transfer, error);
      const vg::core::Allocation* allocation =
          transfer_slot == nullptr
              ? nullptr
              : arena.lookup(core::PointerRef{transfer_slot->view.allocation, transfer_slot->view.allocation_generation});
      if (transfer_slot != nullptr && allocation == nullptr && error != nullptr)
        *error = "the transform's source allocation is not active in this Arena";
      if (allocation != nullptr) {
        AllocationRecord* source = nullptr;
        uint64_t temporary_bytes = 0;
        bool cache_hit = false;
        VulkanFacetRecord* image = nullptr;
        if (ensure_buffer(*allocation, &source, error) &&
            ensure_facet_image(arena, facet_pool(), facet, request.target_kind, source->buffer,
                               /*upload_source_offset=*/0, &image, &cache_hit, &temporary_bytes, error)) {
          cost->new_backing_bytes = image->backing_bytes;
          cost->temporary_bytes = temporary_bytes;
          cost->heap_fragmentation_bytes = image->allocation_padding_bytes;
          // A real optimal-tiled device image built by a real transfer pass
          // (07 §13's linear->optimal), not a relabelled linear buffer.
          cost->used_device_optimal = true;
          // The image is storage distinct from the linear bytes it supersedes,
          // which is what makes a ConsumeInput able to release anything at all.
          cost->distinct_backing = true;
          // A cache hit recorded nothing and submitted nothing, so it must not
          // inflate the structural counts. A miss recorded exactly two image
          // barriers -- UNDEFINED -> TRANSFER_DST and TRANSFER_DST -> the facet
          // kind's read layout -- inside one command buffer waited on once,
          // and those barriers are reported separately from the transform
          // itself (07 §7).
          if (counts != nullptr && !cache_hit) {
            counts->barrier_count += 2;
            counts->command_buffer_count += 1;
            counts->queue_wait_count += 1;
          }
          ok = true;
        }
      }
    }
  }
  // The transform's source capability must not outlive the transform. Retiring
  // it here (rather than leaving it to retire_stale) keeps a TransferFacet from
  // being a second live handle onto a backing a ConsumeInput may be about to
  // release.
  std::string retire_error;
  facet_pool().retire(transfer_ref, &retire_error);
  return ok;
}
#endif

DeviceState::~DeviceState() {
#if defined(VG_HAS_VULKAN)
  if (device_ != VK_NULL_HANDLE) {
    for (auto& [id, record] : allocation_map_) {
      if (record.mapped != nullptr) vkUnmapMemory(device_, record.memory);
      if (record.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, record.buffer, nullptr);
      if (record.memory != VK_NULL_HANDLE) vkFreeMemory(device_, record.memory, nullptr);
    }
    if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
    for (auto& [key, record] : compute_pipeline_cache_) {
      if (record.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, record.pipeline, nullptr);
      if (record.pipeline_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, record.pipeline_layout, nullptr);
      if (record.shader_module != VK_NULL_HANDLE)
        vkDestroyShaderModule(device_, record.shader_module, nullptr);
    }
    if (task_ring_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, task_ring_pipeline_, nullptr);
    if (task_ring_pipeline_layout_ != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device_, task_ring_pipeline_layout_, nullptr);
    if (task_ring_shader_module_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, task_ring_shader_module_, nullptr);
    if (timeline_semaphore_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
    // Phase C facet/raster objects. Destroyed after the command pool above, so
    // nothing here can be referenced by a command buffer still alive: every
    // facet entry point in this file waits on its own VkFence before returning,
    // and submit()'s Stage 5 does the same, so at this point no recorded work
    // is outstanding.
    for (auto& [key, record] : facet_images_) {
      if (record.view != VK_NULL_HANDLE) vkDestroyImageView(device_, record.view, nullptr);
      if (record.image != VK_NULL_HANDLE) vkDestroyImage(device_, record.image, nullptr);
      if (record.memory != VK_NULL_HANDLE) vkFreeMemory(device_, record.memory, nullptr);
    }
    for (auto& [key, sampler] : sampler_cache_)
      if (sampler != VK_NULL_HANDLE) vkDestroySampler(device_, sampler, nullptr);
    for (auto& [key, pipeline] : sample_pipelines_)
      if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline, nullptr);
    for (auto& [key, pipeline] : storage_pipelines_)
      if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline, nullptr);
    for (auto& [key, module] : storage_shader_modules_)
      if (module != VK_NULL_HANDLE) vkDestroyShaderModule(device_, module, nullptr);
    for (auto& [key, pipeline] : raster_pipelines_)
      if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline, nullptr);
    for (auto& [key, pipeline] : naive_raster_pipelines_)
      if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline, nullptr);
    if (raster_fragment_module_ != VK_NULL_HANDLE)
      vkDestroyShaderModule(device_, raster_fragment_module_, nullptr);
    if (raster_vertex_module_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, raster_vertex_module_, nullptr);
    if (raster_pipeline_layout_ != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device_, raster_pipeline_layout_, nullptr);
    if (raster_set_layout_ != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(device_, raster_set_layout_, nullptr);
    if (storage_pipeline_layout_ != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device_, storage_pipeline_layout_, nullptr);
    if (storage_set_layout_ != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(device_, storage_set_layout_, nullptr);
    if (sample_array_shader_module_ != VK_NULL_HANDLE)
      vkDestroyShaderModule(device_, sample_array_shader_module_, nullptr);
    if (sample_shader_module_ != VK_NULL_HANDLE)
      vkDestroyShaderModule(device_, sample_shader_module_, nullptr);
    if (sample_array_pipeline_layout_ != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device_, sample_array_pipeline_layout_, nullptr);
    if (sample_pipeline_layout_ != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device_, sample_pipeline_layout_, nullptr);
    if (sample_array_set_layout_ != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(device_, sample_array_set_layout_, nullptr);
    if (sample_set_layout_ != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(device_, sample_set_layout_, nullptr);
    if (descriptor_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
  }
  if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
#endif
}

}  // namespace vg::vulkan::detail
