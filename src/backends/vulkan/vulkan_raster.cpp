#include "backends/vulkan/vulkan_device_internal.h"
#include "ir/sha256.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace vg::vulkan::detail {

#if defined(VG_HAS_VULKAN)
VkSampleCountFlagBits to_vk_sample_count(uint32_t sample_count) {
  switch (sample_count) {
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
  }
}

VkAttachmentLoadOp to_vk_load_op(vg::vulkan::AttachmentLoadAction load) {
  switch (load) {
    case vg::vulkan::AttachmentLoadAction::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case vg::vulkan::AttachmentLoadAction::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case vg::vulkan::AttachmentLoadAction::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  }
  return VK_ATTACHMENT_LOAD_OP_CLEAR;
}

VkAttachmentStoreOp to_vk_store_op(vg::vulkan::AttachmentStoreAction store) {
  switch (store) {
    case vg::vulkan::AttachmentStoreAction::Store: return VK_ATTACHMENT_STORE_OP_STORE;
    case vg::vulkan::AttachmentStoreAction::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    case vg::vulkan::AttachmentStoreAction::MultisampleResolve: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
  }
  return VK_ATTACHMENT_STORE_OP_STORE;
}
#endif

bool DeviceState::run_raster_pass(const vg::core::Arena& arena, vg::core::FacetPool& pool,
                                 vg::core::FacetRef attachment_ref, vg::core::FacetRef source_ref,
                                 const RasterPassDesc& desc, RasterPassResult* result, std::string* error) {
#if !defined(VG_HAS_VULKAN)
  (void)arena;
  (void)pool;
  (void)attachment_ref;
  (void)source_ref;
  (void)desc;
  (void)result;
  set_error(error, "Vulkan adapter is unavailable in this build, so no dynamic-rendering pass can run");
  return false;
#else
  if (result == nullptr) {
    set_error(error, "raster pass result output is required");
    return false;
  }
  *result = RasterPassResult{};
  result->report = make_facet_report();
  result->sample_count = desc.sample_count;
  const auto reject = [&](const std::string& message) {
    result->report.supported = false;
    result->report.diagnostic = message;
    result->report.add("raster_facet", vg::hal::LoweringClass::Unsupported, 1, 0, message);
    set_error(error, message.c_str());
    return false;
  };
  const bool draws = !desc.vertices.empty();
  if (draws && !capabilities_.supports(vg::hal::Capability::Raster))
    return reject("Unsupported: this device did not claim Capability::Raster, so a draw cannot be "
                  "lowered through dynamic rendering here and must not be approximated with a compute "
                  "blit (device_hal.h, Capability::Raster)");
  if (desc.vertices.size() % 3 != 0)
    return reject("a triangle-list draw needs a vertex count that is a multiple of three");
  if (desc.sample_count != 1 && desc.sample_count != 2 && desc.sample_count != 4 && desc.sample_count != 8)
    return reject("Unsupported sample count " + std::to_string(desc.sample_count) +
                  ": this backend lowers 1, 2, 4 and 8 only");

  const vg::core::FacetSlot* attachment_slot =
      resolve_facet(arena, pool, attachment_ref, vg::core::FacetKind::Attachment, error);
  if (attachment_slot == nullptr) {
    result->report.supported = false;
    result->report.diagnostic = error != nullptr ? *error : "attachment facet reference did not resolve";
    result->report.add("raster_facet", vg::hal::LoweringClass::Unsupported, 1, 0,
                       result->report.diagnostic);
    return false;
  }
  const vg::core::CanonicalView attachment_view = attachment_slot->view;
  const VkFormat attachment_format = to_vk_format(attachment_view.format);
  if (!format_support(attachment_view.format).color_attachment)
    return reject("Unsupported: the attachment view's format does not advertise "
                  "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT under optimal tiling on this device");
  if (desc.sample_count > 1 &&
      (framebuffer_color_sample_counts_ & to_vk_sample_count(desc.sample_count)) == 0)
    return reject("Unsupported sample count " + std::to_string(desc.sample_count) +
                  ": this device's framebufferColorSampleCounts does not include it");

  FacetUseGuard attachment_use(pool, attachment_ref);
  if (!attachment_use.begin(arena, error)) return false;

  bool cache_hit = false;
  uint64_t staging_bytes = 0;
  VulkanFacetRecord* attachment_image = nullptr;
  if (!ensure_facet_image(arena, pool, attachment_ref, vg::core::FacetKind::Attachment, VK_NULL_HANDLE,
                          /*upload_source_offset=*/0, &attachment_image, &cache_hit, &staging_bytes, error))
    return false;
  result->facet_cache_hit = cache_hit;

  // The source is a second facet, resolved and bracketed exactly like the
  // attachment: a draw that read a raw VkImageView here would be the one place
  // the public API leaked a backend handle.
  VulkanFacetRecord* source_image = nullptr;
  FacetUseGuard source_use(pool, source_ref);
  if (draws) {
    if (!source_use.begin(arena, error)) return false;
    bool source_cache_hit = false;
    uint64_t source_staging = 0;
    if (!ensure_facet_image(arena, pool, source_ref, vg::core::FacetKind::Sample, VK_NULL_HANDLE,
                            /*upload_source_offset=*/0, &source_image, &source_cache_hit, &source_staging,
                            error))
      return false;
    staging_bytes += source_staging;
  }

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  RawBuffer vertex_buffer{};
  RawBuffer tint_buffer{};
  RawBuffer readback{};
  const auto destroy_all = [&]() {
    destroy_raw_buffer(device_, &vertex_buffer);
    destroy_raw_buffer(device_, &tint_buffer);
    destroy_raw_buffer(device_, &readback);
  };

  if (draws) {
    // 07 §9's four-way split, spelled out. Only the two structural inputs
    // (attachment format, sample count) plus the raster state this backend
    // genuinely compiles in enter the key; the viewport is
    // VK_DYNAMIC_STATE_VIEWPORT and the tint is a UBO the fragment stage reads,
    // so neither appears here at all -- which is exactly the "小的动态状态不应
    // 无故扩大 key" constraint of 06 §7.
    vg::compiler::PipelineKey key;
    key.code_object_hash = vg::ir::sha256_hex(vg::compiler::raster_facet_vulkan_source());
    key.entry = "vg_raster";
    key.attachment_formats.push_back(static_cast<uint32_t>(attachment_format));
    key.sample_count = desc.sample_count;
    key.target_identity = target_identity_;
    const std::vector<std::pair<std::string, uint64_t>> raster_state{};
    uint64_t binary_size = 0;
    bool pipeline_cache_hit = false;
    if (!ensure_raster_pipeline(pipeline_cache_, raster_pipelines_, key,
                                "AttachmentFacet dynamic-rendering pass", attachment_format,
                                desc.sample_count, raster_state, &pipeline, &pipeline_cache_hit,
                                &binary_size, error))
      return false;
    result->pipeline_key_hash = key.hash();
    result->pipeline_cache_hit = pipeline_cache_hit;
    result->draw_count = 1;

    if (!ensure_sampler(vg::core::FilterMode::Bilinear, vg::core::WrapMode::Clamp, &sampler, error))
      return false;
    if (!ensure_descriptor_pool(error)) return false;
    if (!create_raw_buffer(device_, physical_device_, desc.vertices.size() * 4 * sizeof(float),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, /*want_address=*/false, /*want_map=*/true,
                           &vertex_buffer, error))
      return false;
    if (!create_raw_buffer(device_, physical_device_, 4 * sizeof(float), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           /*want_address=*/false, /*want_map=*/true, &tint_buffer, error)) {
      destroy_all();
      return false;
    }
    std::memcpy(vertex_buffer.mapped, desc.vertices.data(), desc.vertices.size() * 4 * sizeof(float));
    std::memcpy(tint_buffer.mapped, desc.tint.data(), 4 * sizeof(float));

    const auto descriptor_start = std::chrono::steady_clock::now();
    vkResetDescriptorPool(device_, descriptor_pool_, 0);
    VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_alloc.descriptorPool = descriptor_pool_;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &raster_set_layout_;
    if (vkAllocateDescriptorSets(device_, &set_alloc, &descriptor_set) != VK_SUCCESS) {
      destroy_all();
      return reject("vkAllocateDescriptorSets failed for the raster facet pipeline");
    }
    VkDescriptorBufferInfo vertex_info{};
    vertex_info.buffer = vertex_buffer.buffer;
    vertex_info.range = VK_WHOLE_SIZE;
    VkDescriptorImageInfo source_info{};
    source_info.sampler = sampler;
    source_info.imageView = source_image->view;
    source_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo tint_info{};
    tint_info.buffer = tint_buffer.buffer;
    tint_info.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptor_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &vertex_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptor_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &source_info;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = descriptor_set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &tint_info;
    vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
    result->descriptors.set_allocation_count = 1;
    result->descriptors.descriptor_write_count = 3;
    result->descriptors.descriptor_write_bytes =
        2 * sizeof(VkDescriptorBufferInfo) + sizeof(VkDescriptorImageInfo);
    result->descriptors.cpu_descriptor_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             descriptor_start)
            .count());
  }

  if (!create_raw_buffer(device_, physical_device_, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         /*want_address=*/false, /*want_map=*/true, &readback, error)) {
    destroy_all();
    return false;
  }

  // The multisample target, when asked for, is backend-private scratch rather
  // than a facet: nothing outside this call can name it, and it exists only so
  // VkRenderingAttachmentInfo::resolveImageView can hand the resolved samples
  // to the facet's own single-sample image.
  VkImage multisample_image = VK_NULL_HANDLE;
  VkDeviceMemory multisample_memory = VK_NULL_HANDLE;
  VkImageView multisample_view = VK_NULL_HANDLE;
  const auto destroy_multisample = [&]() {
    if (multisample_view != VK_NULL_HANDLE) vkDestroyImageView(device_, multisample_view, nullptr);
    if (multisample_image != VK_NULL_HANDLE) vkDestroyImage(device_, multisample_image, nullptr);
    if (multisample_memory != VK_NULL_HANDLE) vkFreeMemory(device_, multisample_memory, nullptr);
  };
  if (desc.sample_count > 1) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = attachment_format;
    image_info.extent = {attachment_view.width, attachment_view.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = to_vk_sample_count(desc.sample_count);
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &image_info, nullptr, &multisample_image) != VK_SUCCESS) {
      destroy_all();
      return reject("vkCreateImage failed for the transient multisample attachment");
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, multisample_image, &requirements);
    uint32_t memory_type = 0;
    if (!find_memory_type(physical_device_, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type)) {
      destroy_multisample();
      destroy_all();
      return reject("no device-local memory type available for the transient multisample attachment");
    }
    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &multisample_memory) != VK_SUCCESS ||
        vkBindImageMemory(device_, multisample_image, multisample_memory, 0) != VK_SUCCESS) {
      destroy_multisample();
      destroy_all();
      return reject("allocating or binding the transient multisample attachment failed");
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = multisample_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = attachment_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_info, nullptr, &multisample_view) != VK_SUCCESS) {
      destroy_multisample();
      destroy_all();
      return reject("vkCreateImageView failed for the transient multisample attachment");
    }
  }

  if (!ensure_command_pool(device_, compute_queue_family_, &command_pool_, error)) {
    destroy_multisample();
    destroy_all();
    return false;
  }
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  if (!allocate_command_buffer(device_, command_pool_, &command_buffer, error)) {
    destroy_multisample();
    destroy_all();
    return false;
  }
  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command_buffer, &begin_info);
  uint64_t barriers = 0;
  if (record_layout_transition(command_buffer, attachment_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL))
    ++barriers;
  if (draws &&
      record_layout_transition(command_buffer, source_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
    ++barriers;
  if (multisample_image != VK_NULL_HANDLE) {
    record_image_barrier(command_buffer, multisample_image, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 1);
    ++barriers;
  }

  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = multisample_view != VK_NULL_HANDLE ? multisample_view : attachment_image->view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = to_vk_load_op(desc.load);
  color.storeOp = to_vk_store_op(desc.store);
  for (size_t channel = 0; channel < 4; ++channel)
    color.clearValue.color.float32[channel] = desc.clear_rgba[channel];
  if (multisample_view != VK_NULL_HANDLE) {
    // 06 §6.3's MultisampleResolve, expressed the Vulkan way: the resolve is a
    // property of the attachment, and the facet's own image is what receives
    // it. A caller that asked for Store with sample_count > 1 still gets the
    // resolve (there is nowhere else for the samples to go) and the store op
    // above records that the multisample samples themselves are discarded.
    color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color.resolveImageView = attachment_image->view;
    color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.offset = {0, 0};
  rendering.renderArea.extent = {attachment_view.width, attachment_view.height};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  vkCmdBeginRendering(command_buffer, &rendering);
  if (draws) {
    const uint32_t viewport_width =
        desc.viewport_width != 0 ? desc.viewport_width : attachment_view.width;
    const uint32_t viewport_height =
        desc.viewport_height != 0 ? desc.viewport_height : attachment_view.height;
    // Y-down, deliberately not flipped. compiler.h already records that Vulkan
    // NDC is Y-down where Metal's is Y-up; flipping the viewport here would
    // hide a real difference between the two backends behind an unreported
    // fix-up, so the discrepancy is reported below as a cost the host decides
    // what to do about (10 §12).
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(viewport_width);
    viewport.height = static_cast<float>(viewport_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {viewport_width, viewport_height};
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, raster_pipeline_layout_, 0, 1,
                            &descriptor_set, 0, nullptr);
    vkCmdDraw(command_buffer, static_cast<uint32_t>(desc.vertices.size()), 1, 0, 0);
  }
  vkCmdEndRendering(command_buffer);

  if (record_layout_transition(command_buffer, attachment_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))
    ++barriers;
  VkBufferImageCopy copy{};
  copy.bufferRowLength = 1;
  copy.bufferImageHeight = 1;
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {1, 1, 1};
  vkCmdCopyImageToBuffer(command_buffer, attachment_image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         readback.buffer, 1, &copy);
  vkEndCommandBuffer(command_buffer);
  if (!submit_and_wait_simple(device_, compute_queue_, command_pool_, command_buffer, error)) {
    destroy_multisample();
    destroy_all();
    return false;
  }
  result->resolved_rgba = decode_first_texel(readback.mapped, attachment_format);
  destroy_multisample();
  destroy_all();

  result->report.command_buffer_count = 1;
  result->report.encoder_count = 1;
  result->report.barrier_count = barriers;
  result->report.queue_wait_count = 1;
  result->report.heap_fragmentation_bytes += attachment_image->allocation_padding_bytes;
  result->report.add("attachment_facet", vg::hal::LoweringClass::DevicePass, 1,
                     attachment_image->backing_bytes,
                     "one vkCmdBeginRendering/vkCmdEndRendering pass with the caller's load/store "
                     "lowered to VkAttachmentLoadOp/VkAttachmentStoreOp (07 §9)");
  if (draws) {
    result->report.add("raster_facet", vg::hal::LoweringClass::DevicePass, 1,
                       desc.vertices.size() * 4 * sizeof(float),
                       "compiler::raster_facet_vulkan_source compiled once per stage and drawn as one "
                       "triangle list, the fragment stage sampling the SampleFacet through the same "
                       "descriptor set");
    // The discrepancy, reported rather than fixed. Not classified Unsupported:
    // the draw really happened and the image really is what Vulkan's NDC
    // produces; what the host may need is a flipped viewport if it wants the
    // Metal-identical image.
    result->report.add("ndc_orientation", vg::hal::LoweringClass::Direct, 1, 0,
                       "Vulkan NDC is Y-down where Metal's is Y-up: the vertices were passed through "
                       "unmodified, so a host wanting a byte-identical image across the two backends "
                       "must flip its own viewport (or its vertices) rather than rely on this backend "
                       "having quietly done it");
    result->report.add("descriptor_update", vg::hal::LoweringClass::Direct, 3,
                       result->descriptors.descriptor_write_bytes,
                       "vertex storage buffer, combined image sampler and tint uniform written into "
                       "one allocated set; the tint is a UBO precisely so it stays out of the "
                       "pipeline key (07 §9, 06 §7)");
    result->report.add("raster_pipeline",
                       result->pipeline_cache_hit ? vg::hal::LoweringClass::CachedObject
                                                  : vg::hal::LoweringClass::DevicePass,
                       1, 0,
                       result->pipeline_cache_hit
                           ? "graphics pipeline reused for this attachment format and sample count"
                           : "graphics pipeline created for this attachment format and sample count, "
                             "the two structural key inputs dynamic rendering compiles in");
  }
  if (desc.sample_count > 1) {
    result->report.add("multisample_resolve", vg::hal::LoweringClass::DevicePass, 1,
                       attachment_image->backing_bytes,
                       "a transient multisample VkImage was rendered into and resolved into the "
                       "facet's own image through VkRenderingAttachmentInfo::resolveImageView");
  }
  if (barriers != 0) {
    result->report.add("image_layout_transition", vg::hal::LoweringClass::Direct, barriers, 0,
                       "vkCmdPipelineBarrier2 image barriers around the pass, reported apart from the "
                       "pass itself (07 §7)");
  }
  if (staging_bytes != 0) {
    result->report.add("facet_upload_staging", vg::hal::LoweringClass::Serialized, 1, staging_bytes,
                       "transient host-visible staging buffers were needed to fill the facet images "
                       "this pass reads, since this entry point has no linear device buffer to copy from");
  }
  return true;
#endif
}

}  // namespace vg::vulkan::detail
