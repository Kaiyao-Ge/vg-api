#include "backends/vulkan/vulkan_device_internal.h"
#include <algorithm>

namespace vg::vulkan::detail {

#if defined(VG_HAS_VULKAN)
bool ensure_command_pool(VkDevice device, uint32_t queue_family, VkCommandPool* pool, std::string* error) {
  if (*pool != VK_NULL_HANDLE) return true;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = queue_family;
  if (vkCreateCommandPool(device, &pool_info, nullptr, pool) != VK_SUCCESS) {
    if (error) *error = "vkCreateCommandPool failed";
    return false;
  }
  return true;
}

bool allocate_command_buffer(VkDevice device, VkCommandPool pool, VkCommandBuffer* out, std::string* error) {
  VkCommandBufferAllocateInfo alloc_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc_info.commandPool = pool;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(device, &alloc_info, out) != VK_SUCCESS) {
    if (error) *error = "vkAllocateCommandBuffers failed";
    return false;
  }
  return true;
}

bool submit_and_wait(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer command_buffer,
                     const void* submit_pnext, uint32_t wait_count, const VkSemaphore* wait_semaphores,
                     const VkPipelineStageFlags* wait_stage_mask, uint32_t signal_count,
                     const VkSemaphore* signal_semaphores, std::string* error,
                     uint64_t* actual_host_waits) {
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence{VK_NULL_HANDLE};
  if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
    vkFreeCommandBuffers(device, pool, 1, &command_buffer);
    if (error) *error = "vkCreateFence failed";
    return false;
  }
  VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.pNext = submit_pnext;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer;
  submit_info.waitSemaphoreCount = wait_count;
  submit_info.pWaitSemaphores = wait_semaphores;
  submit_info.pWaitDstStageMask = wait_stage_mask;
  submit_info.signalSemaphoreCount = signal_count;
  submit_info.pSignalSemaphores = signal_semaphores;
  if (vkQueueSubmit(queue, 1, &submit_info, fence) != VK_SUCCESS) {
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, pool, 1, &command_buffer);
    if (error) *error = "vkQueueSubmit failed";
    return false;
  }
  if (actual_host_waits != nullptr) ++*actual_host_waits;
  const VkResult wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(device, fence, nullptr);
  vkFreeCommandBuffers(device, pool, 1, &command_buffer);
  if (wait_result != VK_SUCCESS) {
    if (error) *error = "vkWaitForFences failed";
    return false;
  }
  return true;
}

bool submit_and_wait_simple(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer command_buffer,
                            std::string* error) {
  return submit_and_wait(device, queue, pool, command_buffer, nullptr, 0, nullptr, nullptr, 0, nullptr, error);
}

void layout_sync_scope(VkImageLayout layout, VkPipelineStageFlags2* stage, VkAccessFlags2* access) {
  switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
      *stage = VK_PIPELINE_STAGE_2_NONE;
      *access = VK_ACCESS_2_NONE;
      return;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      *stage = VK_PIPELINE_STAGE_2_COPY_BIT;
      *access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      return;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      *stage = VK_PIPELINE_STAGE_2_COPY_BIT;
      *access = VK_ACCESS_2_TRANSFER_READ_BIT;
      return;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      *stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      *access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      return;
    case VK_IMAGE_LAYOUT_GENERAL:
      *stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      *access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      return;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      *access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      return;
    default:
      *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      *access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      return;
  }
}

void record_image_barrier(VkCommandBuffer command_buffer, VkImage image, VkImageLayout old_layout,
                         VkImageLayout new_layout, uint32_t mip_levels, uint32_t array_layers) {
  VkPipelineStageFlags2 src_stage = VK_PIPELINE_STAGE_2_NONE;
  VkAccessFlags2 src_access = VK_ACCESS_2_NONE;
  VkPipelineStageFlags2 dst_stage = VK_PIPELINE_STAGE_2_NONE;
  VkAccessFlags2 dst_access = VK_ACCESS_2_NONE;
  layout_sync_scope(old_layout, &src_stage, &src_access);
  layout_sync_scope(new_layout, &dst_stage, &dst_access);
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = src_stage;
  barrier.srcAccessMask = src_access;
  barrier.dstStageMask = dst_stage;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  // No queue ownership transfer: this backend runs every facet path on the one
  // queue family it selected at device creation, and 07 §4 explicitly allows an
  // adapter to serialize on a single queue. A cross-queue facet path would owe
  // a reported ownership transfer, which is why none is silently implied here.
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = mip_levels;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = array_layers;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(command_buffer, &dependency);
}

bool DeviceState::dispatch_task_graph(const std::vector<CanonicalTaskDispatch>& dispatches,
                                    uint64_t wait_value, uint64_t signal_value,
                                    TaskDispatchCounts* counts, std::string* error) {
  if (counts != nullptr) *counts = {};
  if (dispatches.empty()) {
    set_error(error, "Vulkan canonical task dispatch list is empty");
    return false;
  }
  if (!ensure_command_pool(device_, compute_queue_family_, &command_pool_, error)) return false;
  VkCommandBuffer command_buffer{VK_NULL_HANDLE};
  if (!allocate_command_buffer(device_, command_pool_, &command_buffer, error)) return false;

  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
    set_error(error, "vkBeginCommandBuffer failed for canonical task graph");
    return false;
  }
  if (counts != nullptr) ++counts->command_buffer_count;
  for (const auto& dispatch : dispatches) {
    if (dispatch.pipeline == nullptr || dispatch.pipeline->pipeline == VK_NULL_HANDLE ||
        dispatch.pipeline->pipeline_layout == VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
      set_error(error, "Vulkan task references an unavailable Node pipeline");
      return false;
    }
    for (uint32_t transition_index : dispatch.transitions_before) {
      VkMemoryBarrier2 task_memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      task_memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      task_memory.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
      task_memory.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      task_memory.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
      VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dependency.memoryBarrierCount = 1;
      dependency.pMemoryBarriers = &task_memory;
      vkCmdPipelineBarrier2(command_buffer, &dependency);
      if (counts != nullptr) {
        ++counts->barrier_count;
        counts->encoded_transitions.push_back(transition_index);
      }
    }
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      dispatch.pipeline->pipeline);
    if (!dispatch.addresses.empty()) {
      vkCmdPushConstants(command_buffer, dispatch.pipeline->pipeline_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         static_cast<uint32_t>(dispatch.addresses.size() * sizeof(VkDeviceAddress)),
                         dispatch.addresses.data());
    }
    vkCmdDispatch(command_buffer, dispatch.x, dispatch.y, dispatch.z);
    if (counts != nullptr) ++counts->dispatch_count;
  }
  if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
    set_error(error, "vkEndCommandBuffer failed for canonical task graph");
    return false;
  }

  // wait_value/signal_value of 0 mean "no timeline involvement for this
  // side" -- omitted entirely from VkTimelineSemaphoreSubmitInfo/VkSubmitInfo
  // rather than submitted as a literal 0, matching core's guarantee that a
  // required_value of 0 never reaches the backend.
  VkTimelineSemaphoreSubmitInfo timeline_info{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  const VkPipelineStageFlags wait_stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  uint32_t wait_count = 0;
  uint32_t signal_count = 0;
  if (wait_value != 0) {
    timeline_info.waitSemaphoreValueCount = 1;
    timeline_info.pWaitSemaphoreValues = &wait_value;
    wait_count = 1;
  }
  if (signal_value != 0) {
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = &signal_value;
    signal_count = 1;
  }
  const void* pnext = (wait_value != 0 || signal_value != 0) ? &timeline_info : nullptr;
  return submit_and_wait(device_, compute_queue_, command_pool_, command_buffer, pnext, wait_count,
                         wait_count != 0 ? &timeline_semaphore_ : nullptr,
                         wait_count != 0 ? &wait_stage_mask : nullptr, signal_count,
                         signal_count != 0 ? &timeline_semaphore_ : nullptr, error,
                         counts != nullptr ? &counts->queue_wait_count : nullptr);
}

bool DeviceState::ensure_timeline_semaphore(std::string* error) {
  if (timeline_semaphore_ != VK_NULL_HANDLE) return true;
  VkSemaphoreTypeCreateInfo type_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  type_info.initialValue = 0;
  VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  semaphore_info.pNext = &type_info;
  if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &timeline_semaphore_) != VK_SUCCESS) {
    if (error) *error = "vkCreateSemaphore (timeline) failed";
    return false;
  }
  return true;
}

bool DeviceState::dispatch_task_ring_publication(const TaskRingBuffers& buffers,
                                               TaskDispatchCounts* counts, std::string* error) {
  if (counts != nullptr) *counts = {};
  if (!ensure_command_pool(device_, compute_queue_family_, &command_pool_, error)) return false;
  VkCommandBuffer cb{VK_NULL_HANDLE};
  if (!allocate_command_buffer(device_, command_pool_, &cb, error)) return false;

  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cb, &begin_info) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, command_pool_, 1, &cb);
    set_error(error, "vkBeginCommandBuffer failed for task publication");
    return false;
  }
  if (counts != nullptr) ++counts->command_buffer_count;

  // Tier0: one dispatch, one invocation per task (gl_GlobalInvocationID.x ==
  // its own ring slot), matching task_ring_vulkan_source()'s local_size_x=1.
  struct {
    VkDeviceAddress state;
    VkDeviceAddress fields;
    VkDeviceAddress inputs;
  } task_pc{buffers.state_address, buffers.fields_address, buffers.inputs_address};
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, task_ring_pipeline_);
  vkCmdPushConstants(cb, task_ring_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(task_pc), &task_pc);
  vkCmdDispatch(cb, std::max<uint32_t>(buffers.task_count, 1), 1, 1);
  if (counts != nullptr) ++counts->dispatch_count;

  // Publication is a distinct physical operation. Canonical Node programs
  // have already executed through dispatch_task_graph; this barrier only
  // makes the ring shader's writes visible to the host readback after the
  // completion fence, and cannot dispatch a user Node.
  VkMemoryBarrier2 publish_to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  publish_to_host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  publish_to_host.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
  publish_to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  publish_to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo publication_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  publication_dependency.memoryBarrierCount = 1;
  publication_dependency.pMemoryBarriers = &publish_to_host;
  vkCmdPipelineBarrier2(cb, &publication_dependency);
  if (counts != nullptr) ++counts->barrier_count;

  if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, command_pool_, 1, &cb);
    set_error(error, "vkEndCommandBuffer failed for task publication");
    return false;
  }
  return submit_and_wait(device_, compute_queue_, command_pool_, cb, nullptr, 0, nullptr, nullptr, 0, nullptr, error,
                         counts != nullptr ? &counts->queue_wait_count : nullptr);
}

bool DeviceState::record_layout_transition(VkCommandBuffer command_buffer, VulkanFacetRecord* record,
                                         VkImageLayout new_layout) {
  if (record->layout == new_layout) return false;
  record_image_barrier(command_buffer, record->image, record->layout, new_layout, record->mip_levels,
                       record->array_layers);
  record->layout = new_layout;
  return true;
}
#endif

}  // namespace vg::vulkan::detail
