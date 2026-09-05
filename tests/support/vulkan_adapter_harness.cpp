#include "vulkan_adapter_harness.h"
#include "backends/vulkan/vulkan_device_internal.h"
#include "ir/sha256.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace vg::vulkan {
using namespace detail;
#if defined(VG_HAS_VULKAN)
using AllocationRecord = detail::DeviceState::AllocationRecord;
using VulkanFacetRecord = detail::DeviceState::VulkanFacetRecord;

// The two harness-only graphics probes deliberately do not consult the
// production Raster capability: this narrow dynamic-rendering experiment is
// evidence about physical VkPipeline/VkRendering objects, never an E1 feature
// advertisement. It still requires the exact queue and feature it records.
bool harness_dynamic_rendering_available(const detail::DeviceState& state) {
  if (!state.supports_dynamic_rendering_ || state.compute_queue_family_ == UINT32_MAX) return false;
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(state.physical_device_, &count, nullptr);
  std::vector<VkQueueFamilyProperties> properties(count);
  vkGetPhysicalDeviceQueueFamilyProperties(state.physical_device_, &count, properties.data());
  return state.compute_queue_family_ < properties.size() &&
         (properties[state.compute_queue_family_].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
}
#endif

bool AdapterHarness::run_sample_facet(const vg::core::Arena& arena, vg::core::FacetPool& pool,
                                 vg::core::FacetRef ref, vg::core::FilterMode filter, vg::core::WrapMode wrap,
                                 const std::vector<std::array<float, 2>>& uv_coords, float lod,
                                 const std::vector<uint32_t>& array_slices,
                                 vg::core::ValidationProfile profile, SampleFacetResult* result,
                                 std::string* error) {
  auto& state = *device_.state_;
#if !defined(VG_HAS_VULKAN)
  (void)arena;
  (void)pool;
  (void)ref;
  (void)filter;
  (void)wrap;
  (void)uv_coords;
  (void)lod;
  (void)array_slices;
  (void)profile;
  (void)result;
  set_error(error, "Vulkan adapter is unavailable in this build, so no SampleFacet can be dispatched");
  return false;
#else
  if (result == nullptr) {
    set_error(error, "sample facet result output is required");
    return false;
  }
  *result = SampleFacetResult{};
  result->report = make_facet_report();
  const auto reject = [&](const std::string& message) {
    result->report.supported = false;
    result->report.diagnostic = message;
    result->report.add("sample_facet", vg::hal::LoweringClass::Unsupported, 1, 0, message);
    set_error(error, message.c_str());
    return false;
  };
  if (uv_coords.empty()) return reject("a SampleFacet dispatch needs at least one coordinate");
  // ReferenceStrict and Capture are the reference judge's and the capture
  // harness's profiles; neither has a Vulkan lowering, and 03 §12 makes a
  // profile a specialization of one program rather than a licence to run a
  // different one. So they are refused instead of silently downgraded to
  // FastNative.
  if (profile != vg::core::ValidationProfile::CheckedNative &&
      profile != vg::core::ValidationProfile::FastNative)
    return reject("Unsupported validation profile for a Vulkan SampleFacet: only CheckedNative and "
                  "FastNative have a lowering here, and substituting one profile for another would "
                  "change what was verified");
  const bool checked = profile == vg::core::ValidationProfile::CheckedNative;
  if (checked && !state.capabilities_.supports(vg::hal::Capability::CheckedFacetGeneration))
    return reject("Unsupported: this device did not claim Capability::CheckedFacetGeneration, so the "
                  "in-shader generation guard of 06 §6.4 cannot be honored and a checked dispatch must "
                  "not be answered with an unchecked one");

  const vg::core::FacetSlot* slot = state.resolve_facet(arena, pool, ref, vg::core::FacetKind::Sample, error);
  if (slot == nullptr) {
    result->report.supported = false;
    result->report.diagnostic = error != nullptr ? *error : "sample facet reference did not resolve";
    result->report.add("sample_facet", vg::hal::LoweringClass::Unsupported, 1, 0,
                       result->report.diagnostic);
    return false;
  }
  const vg::core::CanonicalView view = slot->view;
  const bool array_kernel = view.dimension == vg::core::ViewDimension::Texture2DArray;
  if (array_kernel && array_slices.size() != uv_coords.size())
    return reject("a Texture2DArray SampleFacet needs one array slice per coordinate: the array kernel "
                  "indexes both buffers by the same invocation id");
  if (!array_kernel && !array_slices.empty())
    return reject("array slices were supplied for a Texture2D SampleFacet, which has no array "
                  "dimension to select");
  for (const uint32_t slice : array_slices) {
    if (slice >= view.array_layers)
      return reject("array slice " + std::to_string(slice) + " is outside the view's " +
                    std::to_string(view.array_layers) + " layers");
  }
  if (lod < 0.0f || lod > static_cast<float>(view.mip_levels - 1))
    return reject("LOD " + std::to_string(lod) + " is outside the view's mip chain");

  // Bracketed for the whole call: the command buffer below dereferences this
  // slot, so its index must stay out of the free list until the fence is
  // signalled (07 §6's step 6).
  FacetUseGuard use(pool, ref);
  if (!use.begin(arena, error)) return false;

  bool cache_hit = false;
  uint64_t staging_bytes = 0;
  VulkanFacetRecord* image = nullptr;
  if (!state.ensure_facet_image(arena, pool, ref, vg::core::FacetKind::Sample, VK_NULL_HANDLE,
                          /*upload_source_offset=*/0, &image, &cache_hit, &staging_bytes, error))
    return false;
  result->facet_cache_hit = cache_hit;

  VkSampler sampler = VK_NULL_HANDLE;
  if (!state.ensure_sampler(filter, wrap, &sampler, error)) return false;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  if (!state.ensure_sample_facet_pipeline(array_kernel, checked, &pipeline, &pipeline_layout, &set_layout, error))
    return false;
  if (!state.ensure_descriptor_pool(error)) return false;

  const uint32_t invocations = static_cast<uint32_t>(uv_coords.size());
  std::vector<uint32_t> generations;
  pool.snapshot_generations(&generations);
  if (generations.empty()) generations.push_back(0);

  // Every buffer the kernel reads or writes, host-visible so the result can be
  // read back without a second copy. All are transient: they belong to this one
  // dispatch, not to any core::Allocation, so they are destroyed before
  // returning rather than cached (the same reasoning as TaskRingBuffers).
  RawBuffer uv_buffer{};
  RawBuffer output_buffer{};
  RawBuffer lod_buffer{};
  RawBuffer slice_buffer{};
  RawBuffer token_buffer{};
  RawBuffer table_buffer{};
  RawBuffer slot_count_buffer{};
  RawBuffer violation_buffer{};
  const auto destroy_all = [&]() {
    destroy_raw_buffer(state.device_, &uv_buffer);
    destroy_raw_buffer(state.device_, &output_buffer);
    destroy_raw_buffer(state.device_, &lod_buffer);
    destroy_raw_buffer(state.device_, &slice_buffer);
    destroy_raw_buffer(state.device_, &token_buffer);
    destroy_raw_buffer(state.device_, &table_buffer);
    destroy_raw_buffer(state.device_, &slot_count_buffer);
    destroy_raw_buffer(state.device_, &violation_buffer);
  };
  const auto create = [&](VkDeviceSize size, VkBufferUsageFlags usage, RawBuffer* out) {
    if (create_raw_buffer(state.device_, state.physical_device_, size, usage, /*want_address=*/false, /*want_map=*/true,
                          out, error))
      return true;
    destroy_all();
    return false;
  };
  const VkBufferUsageFlags storage_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  const VkBufferUsageFlags uniform_usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (!create(invocations * 2 * sizeof(float), storage_usage, &uv_buffer)) return false;
  if (!create(invocations * 4 * sizeof(float), storage_usage, &output_buffer)) return false;
  if (!create(sizeof(float), uniform_usage, &lod_buffer)) return false;
  if (!create(std::max<VkDeviceSize>(invocations * sizeof(uint32_t), sizeof(uint32_t)), storage_usage,
              &slice_buffer))
    return false;
  if (!create(2 * sizeof(uint32_t), uniform_usage, &token_buffer)) return false;
  if (!create(generations.size() * sizeof(uint32_t), storage_usage, &table_buffer)) return false;
  if (!create(sizeof(uint32_t), uniform_usage, &slot_count_buffer)) return false;
  if (!create(sizeof(uint32_t), storage_usage, &violation_buffer)) return false;

  std::memcpy(uv_buffer.mapped, uv_coords.data(), invocations * 2 * sizeof(float));
  std::memset(output_buffer.mapped, 0, invocations * 4 * sizeof(float));
  std::memcpy(lod_buffer.mapped, &lod, sizeof(lod));
  if (!array_slices.empty())
    std::memcpy(slice_buffer.mapped, array_slices.data(), invocations * sizeof(uint32_t));
  else
    std::memset(slice_buffer.mapped, 0, sizeof(uint32_t));
  const uint32_t token[2] = {ref.index, ref.generation};
  std::memcpy(token_buffer.mapped, token, sizeof(token));
  std::memcpy(table_buffer.mapped, generations.data(), generations.size() * sizeof(uint32_t));
  const uint32_t slot_count = static_cast<uint32_t>(generations.size());
  std::memcpy(slot_count_buffer.mapped, &slot_count, sizeof(slot_count));
  std::memset(violation_buffer.mapped, 0, sizeof(uint32_t));

  // Reset rather than grown: every path here waits on its own fence before
  // returning, so no set allocated by a previous call can still be in use.
  const auto descriptor_start = std::chrono::steady_clock::now();
  vkResetDescriptorPool(state.device_, state.descriptor_pool_, 0);
  VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_alloc.descriptorPool = state.descriptor_pool_;
  set_alloc.descriptorSetCount = 1;
  set_alloc.pSetLayouts = &set_layout;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(state.device_, &set_alloc, &descriptor_set) != VK_SUCCESS) {
    destroy_all();
    return reject("vkAllocateDescriptorSets failed for the sample facet kernel");
  }

  VkDescriptorImageInfo image_info{};
  image_info.sampler = sampler;
  image_info.imageView = image->view;
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  std::vector<VkDescriptorBufferInfo> buffer_infos;
  std::vector<VkWriteDescriptorSet> writes;
  buffer_infos.reserve(8);
  writes.reserve(9);
  uint64_t descriptor_bytes = sizeof(VkDescriptorImageInfo);
  VkWriteDescriptorSet sampler_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  sampler_write.dstSet = descriptor_set;
  sampler_write.dstBinding = 0;
  sampler_write.descriptorCount = 1;
  sampler_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  sampler_write.pImageInfo = &image_info;
  writes.push_back(sampler_write);
  // The buffer infos are collected first and pointed at only afterwards: a
  // vector that reallocates while VkWriteDescriptorSet::pBufferInfo already
  // pointed into it would be a dangling pointer at vkUpdateDescriptorSets time.
  struct PendingBuffer {
    uint32_t binding;
    VkDescriptorType type;
    VkBuffer buffer;
    VkDeviceSize size;
  };
  std::vector<PendingBuffer> pending;
  pending.push_back({1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uv_buffer.buffer, VK_WHOLE_SIZE});
  pending.push_back({2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, output_buffer.buffer, VK_WHOLE_SIZE});
  pending.push_back({3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, lod_buffer.buffer, VK_WHOLE_SIZE});
  if (array_kernel)
    pending.push_back({4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slice_buffer.buffer, VK_WHOLE_SIZE});
  // The guard bindings are written under both profiles even though a FastNative
  // pipeline specializes the guard away. The module declares them
  // unconditionally (compiler::vulkan_facet_guard_declarations), so leaving them
  // unwritten would leave descriptors in the set that the specialized-away
  // branch still names -- and the honest difference between the profiles is
  // what the shader does, reported through checked_generation, not a shorter
  // descriptor list here.
  pending.push_back({5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, token_buffer.buffer, VK_WHOLE_SIZE});
  pending.push_back({6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, table_buffer.buffer, VK_WHOLE_SIZE});
  pending.push_back({7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, slot_count_buffer.buffer, VK_WHOLE_SIZE});
  pending.push_back({8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, violation_buffer.buffer, VK_WHOLE_SIZE});
  buffer_infos.resize(pending.size());
  for (size_t index = 0; index < pending.size(); ++index) {
    buffer_infos[index].buffer = pending[index].buffer;
    buffer_infos[index].offset = 0;
    buffer_infos[index].range = pending[index].size;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_set;
    write.dstBinding = pending[index].binding;
    write.descriptorCount = 1;
    write.descriptorType = pending[index].type;
    write.pBufferInfo = &buffer_infos[index];
    writes.push_back(write);
    descriptor_bytes += sizeof(VkDescriptorBufferInfo);
  }
  vkUpdateDescriptorSets(state.device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  result->descriptors.set_allocation_count = 1;
  result->descriptors.descriptor_write_count = static_cast<uint32_t>(writes.size());
  result->descriptors.descriptor_write_bytes = descriptor_bytes;
  result->descriptors.cpu_descriptor_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                           descriptor_start)
          .count());
  result->descriptors.used_descriptor_buffer = false;

  if (!ensure_command_pool(state.device_, state.compute_queue_family_, &state.command_pool_, error)) {
    destroy_all();
    return false;
  }
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  if (!allocate_command_buffer(state.device_, state.command_pool_, &command_buffer, error)) {
    destroy_all();
    return false;
  }
  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command_buffer, &begin_info);
  // A cached image may be sitting in whatever layout its last use left it in
  // (a storage write leaves GENERAL, a raster pass leaves COLOR_ATTACHMENT_
  // OPTIMAL), so the transition is recorded when it is really needed and
  // counted when it is really recorded -- 07 §7's separate event, never folded
  // into the sample itself.
  const bool transitioned =
      state.record_layout_transition(command_buffer, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                          &descriptor_set, 0, nullptr);
  vkCmdDispatch(command_buffer, invocations, 1, 1);
  VkMemoryBarrier2 host_visibility{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  host_visibility.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  host_visibility.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
  host_visibility.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  host_visibility.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo host_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  host_dependency.memoryBarrierCount = 1;
  host_dependency.pMemoryBarriers = &host_visibility;
  vkCmdPipelineBarrier2(command_buffer, &host_dependency);
  vkEndCommandBuffer(command_buffer);
  if (!submit_and_wait_simple(state.device_, state.compute_queue_, state.command_pool_, command_buffer, error)) {
    destroy_all();
    return false;
  }

  result->sampled_rgba.resize(invocations);
  std::memcpy(result->sampled_rgba.data(), output_buffer.mapped, invocations * 4 * sizeof(float));
  uint32_t violations = 0;
  std::memcpy(&violations, violation_buffer.mapped, sizeof(violations));
  destroy_all();

  result->checked_generation = checked;
  result->violation_count = violations;
  result->report.command_buffer_count = 1;
  result->report.encoder_count = 1;
  result->report.barrier_count = (transitioned ? 1 : 0) + 1;
  result->report.queue_wait_count = 1;
  result->report.heap_fragmentation_bytes += image->allocation_padding_bytes;
  result->report.add("sample_facet", vg::hal::LoweringClass::DevicePass, invocations,
                     invocations * 4 * sizeof(float),
                     array_kernel ? "compiler::sample_facet_array_vulkan_source dispatched with a "
                                    "descriptor-bound sampler2DArray"
                                  : "compiler::sample_facet_vulkan_source dispatched with a "
                                    "descriptor-bound sampler2D");
  result->report.add("facet_image", cache_hit ? vg::hal::LoweringClass::CachedObject
                                              : vg::hal::LoweringClass::DevicePass,
                     1, image->backing_bytes,
                     cache_hit ? "the facet's VkImage/VkImageView were already built for this exact "
                                 "FacetRef generation, epoch, extent, format and swizzle"
                               : "an optimal-tiled VkImage was created and every subresource uploaded "
                                 "from the allocation's linear bytes through a staging buffer");
  if (staging_bytes != 0) {
    result->report.add("facet_upload_staging", vg::hal::LoweringClass::Serialized, 1, staging_bytes,
                       "a transient host-visible staging buffer was needed because this entry point "
                       "has no linear device buffer to copy from, unlike Stage 5");
  }
  if (transitioned) {
    result->report.add("image_layout_transition", vg::hal::LoweringClass::Direct, 1, 0,
                       "one vkCmdPipelineBarrier2 into SHADER_READ_ONLY_OPTIMAL, reported apart from "
                       "the sample it enables (07 §7)");
  }
  result->report.add("sample_readback_visibility", vg::hal::LoweringClass::Direct, 1, 0,
                     "vkCmdPipelineBarrier2 makes compute-written output and violation buffers visible to host readback");
  result->report.add("descriptor_update", vg::hal::LoweringClass::Direct,
                     result->descriptors.descriptor_write_count,
                     result->descriptors.descriptor_write_bytes,
                     "traditional vkUpdateDescriptorSets writes into one allocated set; this backend "
                     "has no descriptor-buffer path, so the cost is reported rather than avoided (07 §6)");
  result->report.add("facet_generation_guard",
                     checked ? vg::hal::LoweringClass::Direct : vg::hal::LoweringClass::Unsupported, 1, 0,
                     checked ? "pipeline specialized with constant_id 0 = true; the token, generation "
                               "table, slot count and violation counter were bound, so a stale FacetRef "
                               "is caught in the shader (06 §6.4)"
                             : "FastNative: constant_id 0 left false, so the guard is specialized away "
                               "and a stale FacetRef would not be caught in the shader");
  return true;
#endif
}

bool AdapterHarness::run_storage_facet(const vg::core::Arena& arena, vg::core::FacetPool& pool,
                                  vg::core::FacetRef ref, StorageFacetTarget target,
                                  const std::array<float, 4>& write_rgba, StorageFacetResult* result,
                                  std::string* error) {
  auto& state = *device_.state_;
#if !defined(VG_HAS_VULKAN)
  (void)arena;
  (void)pool;
  (void)ref;
  (void)target;
  (void)write_rgba;
  (void)result;
  set_error(error, "Vulkan adapter is unavailable in this build, so no StorageFacet can be written");
  return false;
#else
  if (result == nullptr) {
    set_error(error, "storage facet result output is required");
    return false;
  }
  *result = StorageFacetResult{};
  result->report = make_facet_report();
  result->target = target;
  const auto reject = [&](const std::string& message) {
    result->report.supported = false;
    result->report.diagnostic = message;
    result->report.add("storage_facet", vg::hal::LoweringClass::Unsupported, 1, 0, message);
    set_error(error, message.c_str());
    return false;
  };

  const vg::core::FacetSlot* slot = state.resolve_facet(arena, pool, ref, vg::core::FacetKind::Storage, error);
  if (slot == nullptr) {
    result->report.supported = false;
    result->report.diagnostic = error != nullptr ? *error : "storage facet reference did not resolve";
    result->report.add("storage_facet", vg::hal::LoweringClass::Unsupported, 1, 0,
                       result->report.diagnostic);
    return false;
  }
  const vg::core::CanonicalView view = slot->view;
  const VkFormat format = to_vk_format(view.format);

  FacetUseGuard use(pool, ref);
  if (!use.begin(arena, error)) return false;

  // The linear form writes the allocation's own bytes through the BDA buffer
  // this backend already mints for it -- the deliberate alternative 06 §6.2
  // offers ("可读写 texture 或线性 buffer"), chosen by the caller and never
  // substituted for the image form because an image was unavailable.
  if (target == StorageFacetTarget::LinearBuffer) {
    // Same rule the image path enforces through ensure_facet_image, and for the
    // same reason: a raw byte store into the linear backing has nowhere to apply
    // a channel mapping, so a facet whose contract carries one is refused
    // rather than written as though the swizzle were identity (06 §6.1).
    if (!view.swizzle.identity())
      return reject("Unsupported: a non-identity swizzle applies to a SampleFacet only; a linear-buffer "
                    "store writes the allocation's bytes directly and would ignore the channel mapping "
                    "this facet's contract asked for");
    const vg::core::Allocation* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
    if (allocation == nullptr)
      return reject("the storage facet's backing allocation is not active in this Arena");
    const uint64_t texel_bytes = vg::core::bytes_per_texel(view.format);
    if (allocation->bytes.size() < texel_bytes)
      return reject("the allocation's linear backing is too small to hold even one texel of the view");
    AllocationRecord* record = nullptr;
    if (!state.ensure_buffer(*allocation, &record, error)) return false;
    std::array<uint8_t, 16> encoded{};
    const size_t encoded_bytes = encode_first_texel(write_rgba, format, encoded.data());
    std::memcpy(record->mapped, encoded.data(), encoded_bytes);
    result->written_rgba = decode_first_texel(record->mapped, format);
    result->facet_cache_hit = false;
    // No command buffer, no encoder, no barrier: the memory is host-visible
    // coherent, so the write is a host store. Reporting a queue wait here would
    // be inventing GPU work that did not happen.
    result->report.add("storage_facet", vg::hal::LoweringClass::Direct, 1, texel_bytes,
                       "written through the allocation's existing host-visible BDA buffer, the linear "
                       "alternative to a storage image (06 §6.2)");
    return true;
  }

  if (!state.format_support(view.format).storage_image)
    return reject("Unsupported: the view's format does not advertise "
                  "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT under optimal tiling on this device, and the "
                  "view's format is not rewritten to make the write legal (06 §6.2)");

  bool cache_hit = false;
  uint64_t staging_bytes = 0;
  VulkanFacetRecord* image = nullptr;
  if (!state.ensure_facet_image(arena, pool, ref, vg::core::FacetKind::Storage, VK_NULL_HANDLE,
                          /*upload_source_offset=*/0, &image, &cache_hit, &staging_bytes, error))
    return false;
  result->facet_cache_hit = cache_hit;

  VkPipeline pipeline = VK_NULL_HANDLE;
  if (!state.ensure_storage_facet_pipeline(format, &pipeline, error)) return false;
  if (!state.ensure_descriptor_pool(error)) return false;

  RawBuffer value_buffer{};
  RawBuffer readback{};
  const auto destroy_all = [&]() {
    destroy_raw_buffer(state.device_, &value_buffer);
    destroy_raw_buffer(state.device_, &readback);
  };
  if (!create_raw_buffer(state.device_, state.physical_device_, 4 * sizeof(float), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         /*want_address=*/false, /*want_map=*/true, &value_buffer, error))
    return false;
  // The write is read back out of the image itself rather than echoed from the
  // value that was sent down: a storage write whose result is reported from the
  // input would report a success the device never produced.
  if (!create_raw_buffer(state.device_, state.physical_device_, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         /*want_address=*/false, /*want_map=*/true, &readback, error)) {
    destroy_all();
    return false;
  }
  std::memcpy(value_buffer.mapped, write_rgba.data(), 4 * sizeof(float));

  const auto descriptor_start = std::chrono::steady_clock::now();
  vkResetDescriptorPool(state.device_, state.descriptor_pool_, 0);
  VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_alloc.descriptorPool = state.descriptor_pool_;
  set_alloc.descriptorSetCount = 1;
  set_alloc.pSetLayouts = &state.storage_set_layout_;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(state.device_, &set_alloc, &descriptor_set) != VK_SUCCESS) {
    destroy_all();
    return reject("vkAllocateDescriptorSets failed for the storage facet kernel");
  }
  VkDescriptorImageInfo image_info{};
  image_info.imageView = image->view;
  image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorBufferInfo value_info{};
  value_info.buffer = value_buffer.buffer;
  value_info.offset = 0;
  value_info.range = VK_WHOLE_SIZE;
  VkWriteDescriptorSet writes[2]{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = descriptor_set;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[0].pImageInfo = &image_info;
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = descriptor_set;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[1].pBufferInfo = &value_info;
  vkUpdateDescriptorSets(state.device_, 2, writes, 0, nullptr);
  result->descriptors.set_allocation_count = 1;
  result->descriptors.descriptor_write_count = 2;
  result->descriptors.descriptor_write_bytes = sizeof(VkDescriptorImageInfo) + sizeof(VkDescriptorBufferInfo);
  result->descriptors.cpu_descriptor_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                           descriptor_start)
          .count());

  if (!ensure_command_pool(state.device_, state.compute_queue_family_, &state.command_pool_, error)) {
    destroy_all();
    return false;
  }
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  if (!allocate_command_buffer(state.device_, state.command_pool_, &command_buffer, error)) {
    destroy_all();
    return false;
  }
  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command_buffer, &begin_info);
  uint64_t barriers = 0;
  if (state.record_layout_transition(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL)) ++barriers;
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.storage_pipeline_layout_, 0, 1,
                          &descriptor_set, 0, nullptr);
  vkCmdDispatch(command_buffer, 1, 1, 1);
  if (state.record_layout_transition(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) ++barriers;
  VkBufferImageCopy copy{};
  copy.bufferOffset = 0;
  copy.bufferRowLength = 1;
  copy.bufferImageHeight = 1;
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.mipLevel = 0;
  copy.imageSubresource.baseArrayLayer = 0;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {1, 1, 1};
  vkCmdCopyImageToBuffer(command_buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
                         1, &copy);
  VkMemoryBarrier2 host_visibility{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  host_visibility.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  host_visibility.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  host_visibility.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  host_visibility.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo host_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  host_dependency.memoryBarrierCount = 1;
  host_dependency.pMemoryBarriers = &host_visibility;
  vkCmdPipelineBarrier2(command_buffer, &host_dependency);
  ++barriers;
  vkEndCommandBuffer(command_buffer);
  if (!submit_and_wait_simple(state.device_, state.compute_queue_, state.command_pool_, command_buffer, error)) {
    destroy_all();
    return false;
  }
  result->written_rgba = decode_first_texel(readback.mapped, format);
  destroy_all();

  result->report.command_buffer_count = 1;
  result->report.encoder_count = 1;
  result->report.barrier_count = barriers;
  result->report.queue_wait_count = 1;
  result->report.heap_fragmentation_bytes += image->allocation_padding_bytes;
  result->report.add("storage_facet", vg::hal::LoweringClass::DevicePass, 1,
                     vg::core::bytes_per_texel(view.format),
                     "imageStore through a writable VkImageView whose format matches the view's own, "
                     "then copied back out of the image so the reported texel is the device's");
  result->report.add("storage_readback_visibility", vg::hal::LoweringClass::Direct, 1, 0,
                     "vkCmdPipelineBarrier2 makes transfer-written readback bytes visible to the host");
  result->report.add("facet_image",
                     cache_hit ? vg::hal::LoweringClass::CachedObject : vg::hal::LoweringClass::DevicePass,
                     1, image->backing_bytes,
                     cache_hit ? "storage image reused for this exact facet contract"
                               : "an optimal-tiled storage-capable VkImage was created and uploaded");
  if (barriers != 0) {
    result->report.add("image_layout_transition", vg::hal::LoweringClass::Direct, barriers, 0,
                       "vkCmdPipelineBarrier2 image barriers into GENERAL for the write and into "
                       "TRANSFER_SRC_OPTIMAL for the readback, reported apart from the write (07 §7)");
  }
  result->report.add("descriptor_update", vg::hal::LoweringClass::Direct, 2,
                     result->descriptors.descriptor_write_bytes,
                     "one storage image plus one uniform buffer written into a freshly allocated "
                     "descriptor set (07 §6)");
  return true;
#endif
}

bool AdapterHarness::run_pipeline_classification(const std::vector<RasterPipelineVariant>& variants,
                                            PipelineClassificationResult* result, std::string* error) {
  auto& state = *device_.state_;
#if !defined(VG_HAS_VULKAN)
  (void)variants;
  (void)result;
  set_error(error, "Vulkan adapter is unavailable in this build, so no VkPipeline can be compiled");
  return false;
#else
  if (result == nullptr) {
    set_error(error, "pipeline classification result output is required");
    return false;
  }
  *result = PipelineClassificationResult{};
  result->report = make_facet_report();
  const auto reject = [&](const std::string& message) {
    result->report.supported = false;
    result->report.diagnostic = message;
    result->report.add("pipeline_classification", vg::hal::LoweringClass::Unsupported, 1, 0, message);
    set_error(error, message.c_str());
    return false;
  };
  if (variants.empty()) return reject("pipeline classification needs at least one variant");
  if (!harness_dynamic_rendering_available(state))
    return reject("Unsupported: the narrow pipeline-classification harness requires enabled dynamicRendering "
                  "and a graphics-capable selected queue; it does not advertise production Raster capability");

  // Both arms start empty so the counts describe this call and nothing else.
  // The VkPipelines themselves are left in raster_pipelines_/naive_raster_
  // pipelines_ for the destructor; ensure_raster_pipeline destroys any it would
  // otherwise shadow, so clearing the measurement caches cannot orphan one.
  state.pipeline_cache_.clear();
  state.naive_pipeline_cache_.clear();

  const std::string code_object_hash = vg::ir::sha256_hex(vg::compiler::raster_facet_vulkan_source());
  for (size_t index = 0; index < variants.size(); ++index) {
    const RasterPipelineVariant& variant = variants[index];
    const VkFormat format = to_vk_format(variant.attachment_format);
    if (!state.format_support(variant.attachment_format).color_attachment)
      return reject("variant " + std::to_string(index) +
                    " is Unsupported: its attachment format is not a color-attachment format on this "
                    "device");
    if (variant.sample_count != 1 && variant.sample_count != 2 && variant.sample_count != 4 &&
        variant.sample_count != 8)
      return reject("variant " + std::to_string(index) + " is Unsupported: sample count " +
                    std::to_string(variant.sample_count) + " has no lowering here");

    vg::compiler::PipelineKey base;
    base.code_object_hash = code_object_hash;
    base.entry = "vg_raster";
    base.attachment_formats.push_back(static_cast<uint32_t>(format));
    base.sample_count = variant.sample_count;
    base.target_identity = state.target_identity_;

    // The classified arm: classify_pipeline_state decides each block's fate,
    // and an UnsupportedNeedsConversion block makes the whole variant a
    // rejection rather than something folded into a key that would compile a
    // pipeline meaning almost-but-not-quite what was asked for (START.md §4
    // invariant 10, 06 §6.2).
    const vg::compiler::PipelineClassification classification =
        vg::compiler::classify_pipeline_state(base, variant.state);
    if (!classification.ok)
      return reject("variant " + std::to_string(index) + " is Unsupported: " + classification.message);

    // The naive arm: every block folded into the key regardless of its
    // classification, which is precisely the habit E013 exists to measure
    // against. Nothing about the pipeline it compiles differs -- only the key
    // does -- so a difference in the two counts is a difference in pipelines
    // compiled for no semantic reason.
    vg::compiler::PipelineKey naive_key = base;
    for (const auto& block : variant.state) naive_key.raster_state.emplace_back(block.name, block.value);

    VkPipeline pipeline = VK_NULL_HANDLE;
    bool cache_hit = false;
    uint64_t binary_size = 0;
    const auto naive_start = std::chrono::steady_clock::now();
    if (!state.ensure_raster_pipeline(state.naive_pipeline_cache_, state.naive_raster_pipelines_, naive_key,
                               "E013 naive full permutation", format, variant.sample_count,
                               classification.key.raster_state, &pipeline, &cache_hit, &binary_size, error))
      return false;
    result->naive_compile_ns += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - naive_start)
            .count());

    const auto classified_start = std::chrono::steady_clock::now();
    if (!state.ensure_raster_pipeline(state.pipeline_cache_, state.raster_pipelines_, classification.key,
                               "E013 VG-classified key", format, variant.sample_count,
                               classification.key.raster_state, &pipeline, &cache_hit, &binary_size, error))
      return false;
    result->classified_compile_ns += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             classified_start)
            .count());

    // What was kept out of the key, per variant, so "this state did not enter
    // the key" is a checkable claim rather than an assertion.
    if (!classification.dynamic_state.empty()) {
      result->report.add("pipeline_key_excluded_dynamic", vg::hal::LoweringClass::Direct,
                         classification.dynamic_state.size(), 0,
                         "backend dynamic state (viewport/scissor here) set at encode time, so it "
                         "cannot multiply pipelines (07 §9)");
    }
    if (!classification.shader_visible_data.empty()) {
      result->report.add("pipeline_key_excluded_shader_data", vg::hal::LoweringClass::Direct,
                         classification.shader_visible_data.size(), 0,
                         "shader-readable data (the tint UBO here) which a pipeline key must not "
                         "specialize on (06 §7)");
    }
  }

  result->naive_pipeline_count = state.naive_pipeline_cache_.pipeline_count();
  result->classified_pipeline_count = state.pipeline_cache_.pipeline_count();
  result->naive_cache_hits = state.naive_pipeline_cache_.cache_hits();
  result->classified_cache_hits = state.pipeline_cache_.cache_hits();
  result->classified_specializations = state.pipeline_cache_.reports();
  result->report.add("pipeline_classification", vg::hal::LoweringClass::DevicePass,
                     result->naive_pipeline_count + result->classified_pipeline_count, 0,
                     "real VkPipeline objects created: " + std::to_string(result->naive_pipeline_count) +
                         " folding every StateBlock into the key against " +
                         std::to_string(result->classified_pipeline_count) +
                         " honoring 07 §9's four-way split, over " + std::to_string(variants.size()) +
                         " variants");
  return true;
#endif
}

bool AdapterHarness::run_raster_facet(const vg::core::Arena& arena, vg::core::FacetPool& pool,
    vg::core::FacetRef attachment_ref, vg::core::FacetRef source_ref,
    const RasterPassDesc& desc, RasterPassResult* result, std::string* error) {
  return device_.state_->run_raster_pass(arena, pool, attachment_ref, source_ref, desc, result, error);
}

bool AdapterHarness::observe_representation_backing(const vg::core::Arena& arena, vg::core::FacetPool& pool,
                                                    vg::core::FacetRef retained_ref,
                                                    RepresentationPhysicalObservation* result,
                                                    std::string* error) {
#if !defined(VG_HAS_VULKAN)
  (void)arena;
  (void)pool;
  (void)retained_ref;
  (void)result;
  set_error(error, "Vulkan adapter is unavailable, so physical backing cannot be observed");
  return false;
#else
  if (result == nullptr) {
    set_error(error, "representation physical observation output is required");
    return false;
  }
  *result = RepresentationPhysicalObservation{};
  vg::core::FacetStatus status = vg::core::FacetStatus::Ok;
  const vg::core::FacetSlot* slot = pool.lookup(arena, retained_ref, &status);
  if (slot == nullptr || slot->kind != vg::core::FacetKind::Sample) {
    set_error(error, "retained SampleFacet did not resolve while observing representation backing");
    return false;
  }
  auto& state = *device_.state_;
  const auto linear = state.allocation_map_.find(slot->view.allocation);
  if (linear != state.allocation_map_.end()) result->cached_linear_backing_bytes = linear->second.byte_size;
  for (const auto& [key, image] : state.facet_images_) {
    if (key.facet_index != retained_ref.index || key.facet_generation != retained_ref.generation) continue;
    ++result->cached_facet_image_count;
    result->retained_facet_backing_bytes += image.backing_bytes;
  }
  if (result->cached_facet_image_count == 0 || result->retained_facet_backing_bytes == 0) {
    set_error(error, "retained SampleFacet has no Vulkan image backing to observe");
    return false;
  }
  return true;
#endif
}

}  // namespace vg::vulkan
