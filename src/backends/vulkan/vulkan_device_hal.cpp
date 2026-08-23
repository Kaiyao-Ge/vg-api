#include "backends/vulkan/vulkan_device_hal.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#if defined(VG_HAS_VULKAN)
#include <cstdint>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace vg::vulkan {

namespace {
void set_error(std::string* error, const char* message) {
  if (error) *error = message;
}
}  // namespace

#if defined(VG_HAS_VULKAN)
extern "C" char** environ;

namespace {

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

// Invokes glslc as a subprocess: GLSL source in over stdin, SPIR-V binary
// back over stdout, stderr captured for diagnostics. Runtime subprocess
// (rather than a build-time CMake custom command) because the GLSL text is
// generated per-ir::Module, not a static build asset.
bool compile_glsl_to_spirv(const std::string& glsl_source, std::vector<uint32_t>* spirv, std::string* error) {
#if !defined(VG_GLSLC_PATH)
  if (error) *error = "glslc was not located at CMake configure time (VG_GLSLC_PATH unset)";
  return false;
#else
  int stdin_pipe[2];
  int stdout_pipe[2];
  int stderr_pipe[2];
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
    if (error) *error = "failed to create pipes for glslc";
    return false;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, stdin_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
  posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
  posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, stderr_pipe[1]);

  char program[] = VG_GLSLC_PATH;
  char arg1[] = "-fshader-stage=compute";
  char arg2[] = "--target-env=vulkan1.2";
  char arg3[] = "-o";
  char arg4[] = "-";
  char arg5[] = "-";
  char* argv[] = {program, arg1, arg2, arg3, arg4, arg5, nullptr};

  pid_t pid = 0;
  const int spawn_result = posix_spawn(&pid, program, &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);
  if (spawn_result != 0) {
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    if (error) *error = std::string("failed to spawn glslc at ") + VG_GLSLC_PATH;
    return false;
  }

  // GLSL inputs and SPIR-V outputs for this linear-subset codegen are a few
  // hundred bytes to a few KB -- well under a pipe's buffer capacity, so a
  // simple write-then-read (rather than select()-driven interleaving) never
  // deadlocks in practice for this backend's fixtures.
  size_t written = 0;
  while (written < glsl_source.size()) {
    const ssize_t n = write(stdin_pipe[1], glsl_source.data() + written, glsl_source.size() - written);
    if (n <= 0) break;
    written += static_cast<size_t>(n);
  }
  close(stdin_pipe[1]);

  std::vector<uint8_t> stdout_bytes;
  {
    uint8_t buffer[4096];
    ssize_t n;
    while ((n = read(stdout_pipe[0], buffer, sizeof(buffer))) > 0)
      stdout_bytes.insert(stdout_bytes.end(), buffer, buffer + n);
  }
  close(stdout_pipe[0]);

  std::string stderr_text;
  {
    char buffer[4096];
    ssize_t n;
    while ((n = read(stderr_pipe[0], buffer, sizeof(buffer))) > 0)
      stderr_text.append(buffer, static_cast<size_t>(n));
  }
  close(stderr_pipe[0]);

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (error)
      *error = "glslc failed: " + (stderr_text.empty() ? ("unknown error, exit status " + std::to_string(status))
                                                        : stderr_text);
    return false;
  }
  if (stdout_bytes.empty() || (stdout_bytes.size() % 4) != 0) {
    if (error) *error = "glslc produced no valid SPIR-V output";
    return false;
  }
  spirv->resize(stdout_bytes.size() / 4);
  std::memcpy(spirv->data(), stdout_bytes.data(), stdout_bytes.size());
  return true;
#endif
}

// Packs/unpacks a core::TaskRecord as 14 little-endian uint32 words, matching
// compiler::task_ring_vulkan_source()'s expected buffer layout -- word-for-
// word identical to Metal's pack_task_record/unpack_task_record
// (metal_device_hal.mm) so both backends' published_tasks compare byte-exact
// against the same reference oracle.
void pack_task_record(const vg::core::TaskRecord& task, uint32_t* out) {
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

vg::core::TaskRecord unpack_task_record(const uint32_t* in) {
  vg::core::TaskRecord task;
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

// Shared allocate-bind-map(-address) helper factored out of ensure_buffer so
// TaskRingBuffers' four buffers (state/fields/inputs/indirect) don't each
// duplicate the same VkBuffer/VkMemory creation sequence.
struct RawBuffer {
  VkBuffer buffer{VK_NULL_HANDLE};
  VkDeviceMemory memory{VK_NULL_HANDLE};
  VkDeviceAddress address{};
  void* mapped{nullptr};
};

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

}  // namespace

bool DeviceHal::ensure_pipeline(const std::string& ir_hash, const std::string& glsl_source, uint32_t binding_count,
                                std::string* error) {
  if (compute_pipeline_ != VK_NULL_HANDLE && cached_ir_hash_ == ir_hash) return true;
  if (compute_pipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, compute_pipeline_, nullptr);
    compute_pipeline_ = VK_NULL_HANDLE;
  }
  if (pipeline_layout_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    pipeline_layout_ = VK_NULL_HANDLE;
  }
  if (shader_module_ != VK_NULL_HANDLE) {
    vkDestroyShaderModule(device_, shader_module_, nullptr);
    shader_module_ = VK_NULL_HANDLE;
  }
  cached_ir_hash_.clear();

  std::vector<uint32_t> spirv;
  if (!compile_glsl_to_spirv(glsl_source, &spirv, error)) return false;

  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = spirv.size() * sizeof(uint32_t);
  module_info.pCode = spirv.data();
  if (vkCreateShaderModule(device_, &module_info, nullptr, &shader_module_) != VK_SUCCESS) {
    if (error) *error = "vkCreateShaderModule failed";
    return false;
  }

  VkPushConstantRange push_range{};
  push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_range.offset = 0;
  push_range.size = std::max<uint32_t>(binding_count, 1) * static_cast<uint32_t>(sizeof(VkDeviceAddress));

  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
    if (error) *error = "vkCreatePipelineLayout failed";
    return false;
  }

  VkPipelineShaderStageCreateInfo stage_info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage_info.module = shader_module_;
  stage_info.pName = "main";

  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = stage_info;
  pipeline_info.layout = pipeline_layout_;
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &compute_pipeline_) !=
      VK_SUCCESS) {
    if (error) *error = "vkCreateComputePipelines failed";
    return false;
  }
  cached_ir_hash_ = ir_hash;
  return true;
}

bool DeviceHal::ensure_buffer(const vg::core::Allocation& allocation, AllocationRecord** out, std::string* error) {
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

namespace {
// command_pool_ is created lazily by whichever of dispatch_and_wait/
// dispatch_task_ring_and_tier1 runs first; both reuse the same pool.
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

// Submits `command_buffer` and blocks on a transient VkFence until it
// completes. Returns false (with *error set) on any Vulkan-level failure;
// the fence and command buffer are always cleaned up before returning.
bool submit_and_wait(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer command_buffer,
                     const void* submit_pnext, uint32_t wait_count, const VkSemaphore* wait_semaphores,
                     const VkPipelineStageFlags* wait_stage_mask, uint32_t signal_count,
                     const VkSemaphore* signal_semaphores, std::string* error) {
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
  const VkResult wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(device, fence, nullptr);
  vkFreeCommandBuffers(device, pool, 1, &command_buffer);
  if (wait_result != VK_SUCCESS) {
    if (error) *error = "vkWaitForFences failed";
    return false;
  }
  return true;
}
}  // namespace

bool DeviceHal::dispatch_and_wait(const std::vector<VkDeviceAddress>& addresses, uint64_t wait_value,
                                  uint64_t signal_value, std::string* error) {
  if (!ensure_command_pool(device_, compute_queue_family_, &command_pool_, error)) return false;
  VkCommandBuffer command_buffer{VK_NULL_HANDLE};
  if (!allocate_command_buffer(device_, command_pool_, &command_buffer, error)) return false;

  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command_buffer, &begin_info);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline_);
  vkCmdPushConstants(command_buffer, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     static_cast<uint32_t>(addresses.size() * sizeof(VkDeviceAddress)),
                     addresses.empty() ? nullptr : addresses.data());
  vkCmdDispatch(command_buffer, 1, 1, 1);
  vkEndCommandBuffer(command_buffer);

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
                         signal_count != 0 ? &timeline_semaphore_ : nullptr, error);
}

bool DeviceHal::ensure_timeline_semaphore(std::string* error) {
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

bool DeviceHal::ensure_task_ring_pipeline(std::string* error) {
  if (task_ring_pipeline_ != VK_NULL_HANDLE) return true;
  std::vector<uint32_t> spirv;
  if (!compile_glsl_to_spirv(vg::compiler::task_ring_vulkan_source(), &spirv, error)) return false;

  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = spirv.size() * sizeof(uint32_t);
  module_info.pCode = spirv.data();
  if (vkCreateShaderModule(device_, &module_info, nullptr, &task_ring_shader_module_) != VK_SUCCESS) {
    if (error) *error = "vkCreateShaderModule (task ring) failed";
    return false;
  }

  // Three consecutive VkDeviceAddress push constants (task_state, task_fields,
  // task_inputs), matching task_ring_vulkan_source()'s VgTaskPushConstants
  // block field-for-field with no padding (all three fields are the same
  // 8-byte-aligned buffer_reference handle type).
  VkPushConstantRange push_range{};
  push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_range.offset = 0;
  push_range.size = 3 * static_cast<uint32_t>(sizeof(VkDeviceAddress));

  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &task_ring_pipeline_layout_) != VK_SUCCESS) {
    if (error) *error = "vkCreatePipelineLayout (task ring) failed";
    return false;
  }

  VkPipelineShaderStageCreateInfo stage_info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage_info.module = task_ring_shader_module_;
  stage_info.pName = "main";

  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = stage_info;
  pipeline_info.layout = task_ring_pipeline_layout_;
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &task_ring_pipeline_) !=
      VK_SUCCESS) {
    if (error) *error = "vkCreateComputePipelines (task ring) failed";
    return false;
  }
  return true;
}

bool DeviceHal::create_task_ring_buffers(uint32_t task_count, TaskRingBuffers* out, std::string* error) {
  const VkDeviceSize state_size = std::max<VkDeviceSize>(task_count * sizeof(uint32_t), sizeof(uint32_t));
  const VkDeviceSize record_size = std::max<VkDeviceSize>(
      static_cast<VkDeviceSize>(task_count) * vg::compiler::kTaskRingWordsPerRecord * sizeof(uint32_t),
      vg::compiler::kTaskRingWordsPerRecord * sizeof(uint32_t));
  const VkDeviceSize indirect_size = std::max<VkDeviceSize>(
      static_cast<VkDeviceSize>(task_count) * sizeof(VkDispatchIndirectCommand), sizeof(VkDispatchIndirectCommand));

  const VkBufferUsageFlags ring_usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

  RawBuffer state;
  if (!create_raw_buffer(device_, physical_device_, state_size, ring_usage, /*want_address=*/true,
                         /*want_map=*/true, &state, error))
    return false;
  RawBuffer fields;
  // fields_buffer also needs TRANSFER_SRC: dispatch_task_ring_and_tier1 copies
  // its x/y/z window directly into the Tier1 indirect buffer.
  if (!create_raw_buffer(device_, physical_device_, record_size, ring_usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                         true, &fields, error)) {
    destroy_raw_buffer(device_, &state);
    return false;
  }
  RawBuffer inputs;
  if (!create_raw_buffer(device_, physical_device_, record_size, ring_usage, true, true, &inputs, error)) {
    destroy_raw_buffer(device_, &state);
    destroy_raw_buffer(device_, &fields);
    return false;
  }
  RawBuffer indirect;
  if (!create_raw_buffer(device_, physical_device_, indirect_size,
                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         /*want_address=*/false, /*want_map=*/false, &indirect, error)) {
    destroy_raw_buffer(device_, &state);
    destroy_raw_buffer(device_, &fields);
    destroy_raw_buffer(device_, &inputs);
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
  out->indirect_buffer = indirect.buffer;
  out->indirect_memory = indirect.memory;
  out->task_count = task_count;
  return true;
}

void DeviceHal::destroy_task_ring_buffers(TaskRingBuffers* buffers) {
  RawBuffer state{buffers->state_buffer, buffers->state_memory, buffers->state_address, buffers->state_mapped};
  RawBuffer fields{buffers->fields_buffer, buffers->fields_memory, buffers->fields_address, buffers->fields_mapped};
  RawBuffer inputs{buffers->inputs_buffer, buffers->inputs_memory, buffers->inputs_address, buffers->inputs_mapped};
  RawBuffer indirect{buffers->indirect_buffer, buffers->indirect_memory, {}, nullptr};
  destroy_raw_buffer(device_, &state);
  destroy_raw_buffer(device_, &fields);
  destroy_raw_buffer(device_, &inputs);
  destroy_raw_buffer(device_, &indirect);
  *buffers = TaskRingBuffers{};
}

bool DeviceHal::dispatch_task_ring_and_tier1(const TaskRingBuffers& buffers, const std::vector<uint32_t>& order,
                                             const std::vector<VkDeviceAddress>& addresses, std::string* error) {
  if (!ensure_command_pool(device_, compute_queue_family_, &command_pool_, error)) return false;
  VkCommandBuffer cb{VK_NULL_HANDLE};
  if (!allocate_command_buffer(device_, command_pool_, &cb, error)) return false;

  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &begin_info);

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

  // sync2 first real use (ADR-017's tracked gap): make the ring's write
  // visible to the transfer stage before copying x/y/z out of it.
  VkBufferMemoryBarrier2 write_to_copy{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  write_to_copy.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  write_to_copy.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
  write_to_copy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  write_to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  write_to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  write_to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  write_to_copy.buffer = buffers.fields_buffer;
  write_to_copy.offset = 0;
  write_to_copy.size = VK_WHOLE_SIZE;
  VkDependencyInfo dep1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep1.bufferMemoryBarrierCount = 1;
  dep1.pBufferMemoryBarriers = &write_to_copy;
  vkCmdPipelineBarrier2(cb, &dep1);

  // Tier1 conformance floor: each task's x/y/z window (words 5..7 of its
  // 14-word record) is byte-identical to VkDispatchIndirectCommand{x,y,z} --
  // both are three consecutive 4-byte-aligned uint32s -- so this is a plain
  // device-side copy, no host-side repacking. `order` is the caller's
  // dependency-respecting sequence, so indirect dispatch below preserves it.
  for (size_t slot = 0; slot < order.size(); ++slot) {
    VkBufferCopy region{};
    region.srcOffset =
        static_cast<VkDeviceSize>(order[slot]) * vg::compiler::kTaskRingWordsPerRecord * sizeof(uint32_t) +
        5 * sizeof(uint32_t);
    region.dstOffset = static_cast<VkDeviceSize>(slot) * sizeof(VkDispatchIndirectCommand);
    region.size = sizeof(VkDispatchIndirectCommand);
    vkCmdCopyBuffer(cb, buffers.fields_buffer, buffers.indirect_buffer, 1, &region);
  }

  VkBufferMemoryBarrier2 copy_to_indirect{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  copy_to_indirect.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  copy_to_indirect.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  copy_to_indirect.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  copy_to_indirect.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
  copy_to_indirect.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  copy_to_indirect.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  copy_to_indirect.buffer = buffers.indirect_buffer;
  copy_to_indirect.offset = 0;
  copy_to_indirect.size = VK_WHOLE_SIZE;
  VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep2.bufferMemoryBarrierCount = 1;
  dep2.pBufferMemoryBarriers = &copy_to_indirect;
  vkCmdPipelineBarrier2(cb, &dep2);

  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline_);
  vkCmdPushConstants(cb, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     static_cast<uint32_t>(addresses.size() * sizeof(VkDeviceAddress)),
                     addresses.empty() ? nullptr : addresses.data());
  for (size_t slot = 0; slot < order.size(); ++slot)
    vkCmdDispatchIndirect(cb, buffers.indirect_buffer, static_cast<VkDeviceSize>(slot) * sizeof(VkDispatchIndirectCommand));

  vkEndCommandBuffer(cb);
  return submit_and_wait(device_, compute_queue_, command_pool_, cb, nullptr, 0, nullptr, nullptr, 0, nullptr, error);
}
#endif  // defined(VG_HAS_VULKAN)

DeviceHal::~DeviceHal() {
#if defined(VG_HAS_VULKAN)
  if (device_ != VK_NULL_HANDLE) {
    for (auto& [id, record] : allocation_map_) {
      if (record.mapped != nullptr) vkUnmapMemory(device_, record.memory);
      if (record.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, record.buffer, nullptr);
      if (record.memory != VK_NULL_HANDLE) vkFreeMemory(device_, record.memory, nullptr);
    }
    if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
    if (compute_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, compute_pipeline_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (shader_module_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, shader_module_, nullptr);
    if (task_ring_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, task_ring_pipeline_, nullptr);
    if (task_ring_pipeline_layout_ != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device_, task_ring_pipeline_layout_, nullptr);
    if (task_ring_shader_module_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, task_ring_shader_module_, nullptr);
    if (timeline_semaphore_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
    vkDestroyDevice(device_, nullptr);
  }
  if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
#endif
}

const vg::hal::CapabilitySnapshot& DeviceHal::capabilities() const {
  return capabilities_;
}

// TASK-B14 (E012), compile-review-only (ADR-024/ADR-027): this backend does
// not act on ExecutionPlan::effect_dag_passes -- compile() below falls
// through to the single-`plan.module` path unconditionally, exactly as it
// did before E012, and a caller that sets effect_dag_passes on a Vulkan
// plan gets ordinary single-module compilation with the rest silently
// unused. No Vulkan hardware is reachable from this machine (permanent
// constraint, ADR-024), so this file only documents, rather than
// implements or executes, how the same 3 in-scope Effect DAG shapes Metal
// lowers (core::EffectGraphShape, core.h) would map onto this backend's
// already-implemented primitives:
//
// - LinearChain: a single ordered sequence of vkCmdDispatch calls recorded
//   into one VkCommandBuffer, each pass's dispatch preceded by a
//   vkCmdPipelineBarrier2 (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT src/dst,
//   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT -> READ_BIT|WRITE_BIT as needed per
//   pass's declared_effects) between each consecutive pair -- the direct
//   analogue of Metal's single-encoder, in-order dispatch_node() loop.
// - IndependentBranches: vkCmdDispatch per pass with no barrier between
//   them at all, relying on Vulkan's explicit synchronization model having
//   nothing to order (no dependency edges, no conflicting effects) --
//   mirrors Metal's per-node-encoder-with-no-fence branch, except Vulkan
//   has no implicit hazard tracking to lean on the way Metal's default
//   automatic tracking does, so the *absence* of a barrier here is a
//   deliberate assertion (backed by classify_effect_graph_shape's own
//   zero-structural-edges precondition for this shape) that no ordering is
//   required, not an oversight.
// - ForkJoin: per-node vkCmdPipelineBarrier2 calls with a
//   VkDependencyInfo whose buffer memory barriers name exactly the
//   predecessor passes' output buffers, built the same way
//   dispatch_effect_dag's Metal ForkJoin branch builds `predecessors` from
//   EffectGraph::edges() (Explicit + InferredConflict only) -- every node
//   barriers against every node with a direct structural edge into it, not
//   just an assumed source/join role split (see that function's doc
//   comment in metal_device_hal.mm for why the classifier's ForkJoin
//   detection always forces at least one direct edge between two non-
//   source/join nodes once node_count > 3, which this mapping must
//   preserve rather than silently ignore).
// - Unsupported ("cross queue", representation-transition, external-present
//   shaped graphs): reported Unsupported, exactly as Metal's compile()
//   does -- never lowered with a guessed barrier placement.
//
// Wiring this in for real (a second VkCommandBuffer-recording path
// alongside the existing single-module one, keyed on
// plan.effect_dag_passes.empty()) is out of scope for this milestone: E012
// only requires Vulkan evidence to be compile-review-only (ADR-024), and
// this backend already has no independently-runnable multi-pass dispatch
// loop to extend by analogy the way ADR-026 extended Metal's Tier0 dispatch
// infrastructure for Tier1.

// TASK-B15 (E002), compile-review-only (ADR-024/ADR-028): compile() below
// still calls compiler::build_linear_compute_package() unconditionally, so a
// pointer-graph module (any load_ref/load_via/store_via instruction) would
// fail here exactly as it would have on Metal before that backend's E002
// wiring -- this file is not extended to branch on
// compiler::build_pointer_graph_compute_package() the way metal_device_hal.mm
// now does, because no Vulkan hardware is reachable from this machine
// (permanent constraint) and no ctest exercises this backend's compile()/
// submit() at all, so a code path that can never actually run buys nothing
// over documenting the mapping.
//
// This also documents a deliberate divergence from this plan's own original
// framing of the Vulkan side of E002 ("加载出的 PointerRef 直接转换成
// buffer_reference 句柄，实现零描述符的真实指针追逐设计" -- a real BDA-based
// device-pointer dereference). The Metal implementation that this file
// mirrors did NOT build that: ADR-028 scopes E002 to a CachedObject-only
// lowering on both backends, for the same reason on each --
// declared_pointer_edges already statically resolves a load_via/store_via's
// target allocation host-side, in ir::verify(), before compile() ever runs,
// so no *dynamic* GPU-side pointer chase is needed to find it. The mapping
// this backend's CachedObject lowering would take, by direct analogy with
// build_pointer_graph_compute_package()'s Metal output:
//
// - load_ref is elided from the generated shader entirely, exactly as on
//   Metal -- its 12-byte {allocation, generation} value is read only by the
//   reference executor's dynamic dangling-ref check, never by any GPU code
//   in this lowering.
// - load_via/store_via targets are bound the same way build_linear_compute_
//   package's ordinary loads/stores already are on this backend: a
//   buffer_reference (BDA) handle passed through the existing push-constant
//   VgAllocationRef array (see build_linear_compute_package's GLSL output),
//   selected by the statically-resolved binding index -- not by
//   reinterpreting load_ref's loaded value as a raw VkDeviceAddress.
// - A real device-pointer variant (reinterpreting load_ref's value as a
//   VkDeviceAddress and dereferencing it directly via a buffer_reference cast
//   in the shader) would need a host-side allocation-id -> VkDeviceAddress
//   table this backend does not maintain today -- disproportionate new
//   infrastructure for a compile-review-only requirement, exactly the
//   argument ADR-028 makes for Metal.

// TASK-B16 (E007), compile-review-only (ADR-024/ADR-029): compile() below
// never branches on plan.request_indexed_binding and never calls
// compiler::build_indexed_compute_package() -- this backend's compile()
// still unconditionally lowers plan.module through build_linear_compute_
// package(), exactly as it did before this milestone. No ctest exercises
// this backend at all (no Vulkan hardware is reachable from this machine,
// a permanent constraint), so wiring a second, never-runnable dispatch path
// buys nothing over documenting the mapping, mirroring the E002 comment
// block above.
//
// The mapping this backend's indexed-binding lowering would take, by direct
// analogy with build_indexed_compute_package()'s Metal output
// (compute_package.cpp): every distinct allocation referenced by
// plan.module collapses into ONE table -- an array of VgAllocationRef
// buffer_reference handles (see build_linear_compute_package's own
// push-constant VgAllocationRef convention), passed as a single
// push-constant array (`VgAllocationRef table[count]`) rather than as
// individually-typed push-constant fields, one per referenced allocation.
// Each instruction dereferences its target through its compile-time-known
// slot in that array (`table[K].words[word]`) rather than through its own
// dedicated push-constant field -- one binding regardless of how many
// distinct allocations plan.module references, the same N-vs-1 contrast
// ADR-029 documents for Metal.
//
// This is the same "VG root pointer" design build_indexed_compute_package()
// already emits as its vulkan_glsl_source output (compute_package.cpp), so
// this backend would only need to dispatch that already-generated shader,
// not design a new one -- the gap here is purely the dispatch wiring
// (push-constant population, pipeline creation), not the codegen.
//
// Traditional bindless via real descriptor indexing
// (VK_EXT_descriptor_indexing / nonuniformEXT, sampled through a
// descriptor-set array rather than BDA) is explicitly out of scope for this
// milestone -- this backend has no descriptor-set/pool infrastructure at
// all today, and building one solely to give a compile-review-only
// requirement a second comparison baseline is disproportionate. ADR-029
// records this as the largest deferral in the whole Phase B gate plan.

bool DeviceHal::compile(const vg::hal::ExecutionPlan& plan,
                        vg::hal::CompiledPlan* compiled,
                        std::string* error) {
  if (!compiled) { set_error(error, "compiled plan output is null"); return false; }
  if (!plan.validate(error)) return false;
  if (plan.capabilities.backend != vg::hal::BackendKind::Vulkan) {
    set_error(error, "execution plan backend does not match Vulkan adapter");
    return false;
  }
  const auto package = vg::compiler::build_linear_compute_package(plan.module);
  if (!package.ok) { if (error) *error = package.message; return false; }
  compiled->abi_version = vg::hal::kDeviceHalAbiVersion;
  compiled->plan = plan;
  compiled->compute_package = package.package;
  compiled->report = {};
  compiled->report.backend = vg::hal::BackendKind::Vulkan;
  compiled->report.add("compute_package", vg::hal::LoweringClass::Direct, 1, package.package.bindings.size(),
                       "GLSL source generated by B4");

#if !defined(VG_HAS_VULKAN)
  compiled->report.supported = false;
  compiled->report.diagnostic = "Vulkan adapter is unavailable in this build";
  compiled->report.add("vulkan_pipeline", vg::hal::LoweringClass::Unsupported, 1, 0, compiled->report.diagnostic);
  set_error(error, compiled->report.diagnostic.c_str());
  return false;
#else
  if ((plan.timeline_wait != 0 || plan.timeline_signal != 0) &&
      !capabilities_.supports(vg::hal::Capability::Timeline)) {
    compiled->report.supported = false;
    compiled->report.diagnostic = "timeline wait/signal requested but device does not support timeline semaphores";
    compiled->report.add("timeline", vg::hal::LoweringClass::Unsupported, 1, 0, compiled->report.diagnostic);
    set_error(error, compiled->report.diagnostic.c_str());
    return false;
  }
  std::string pipeline_error;
  const bool ok = ensure_pipeline(package.package.canonical_ir_hash, package.package.vulkan_glsl_source,
                                  static_cast<uint32_t>(package.package.bindings.size()), &pipeline_error);
  if (ok) {
    compiled->report.supported = true;
    compiled->report.add("vulkan_pipeline", vg::hal::LoweringClass::Direct, 1, 0,
                         "SPIR-V pipeline compiled via glslc, bound via push-constant BDA addresses");
    if (!plan.task_graph.tasks().empty()) {
      compiled->report.add("task_publication", vg::hal::LoweringClass::Direct, 1, 0,
                           "Vulkan task ring GPU publication kernel + vkCmdDispatchIndirect Tier1");
    }
    if (plan.timeline_signal != 0) {
      compiled->report.add("timeline", vg::hal::LoweringClass::Direct, 1, 0,
                           "VkSemaphore(TIMELINE) wait/signal chained via VkTimelineSemaphoreSubmitInfo");
    }
    return true;
  }
  // Unlike Metal, no HostAssisted fallback: the target NVIDIA/Linux hardware
  // is expected to natively support buffer device address + 64-bit shader
  // atomics (both promoted to core Vulkan 1.2), so a genuine compile failure
  // is reported honestly rather than silently degraded.
  compiled->report.supported = false;
  compiled->report.diagnostic = "Vulkan pipeline compilation failed: " + pipeline_error;
  compiled->report.add("vulkan_pipeline", vg::hal::LoweringClass::Unsupported, 1, 0, pipeline_error);
  set_error(error, compiled->report.diagnostic.c_str());
  return false;
#endif
}

bool DeviceHal::submit(const vg::hal::CompiledPlan& compiled, vg::core::Arena& arena,
                       vg::hal::Submission* submission, std::string* error) {
  if (!submission) { set_error(error, "submission output is null"); return false; }
  if (!compiled.report.supported) { set_error(error, "compiled plan is unsupported"); return false; }
  if (!compiled.compute_package.has_value()) { set_error(error, "compiled plan has no compute package"); return false; }
  if (!compiled.plan.graph_epoch_matches(arena, error)) return false;

  submission->abi_version = vg::hal::kDeviceHalAbiVersion;
  submission->report = compiled.report;

#if !defined(VG_HAS_VULKAN)
  set_error(error, "Vulkan adapter is unavailable in this build");
  return false;
#else
  const uint64_t wait_value = compiled.plan.timeline_wait;
  const uint64_t signal_value = compiled.plan.timeline_signal;
  // Pre-checked host-side, mirroring reference::execute()'s Timeline::
  // validate_wait/signal and Metal's identical pre-check: fail fast on an
  // unsatisfied wait or a non-monotonic signal rather than letting
  // vkQueueSubmit block forever on a wait value nothing will ever reach.
  if (wait_value != 0 || signal_value != 0) {
    std::string timeline_error;
    if (!ensure_timeline_semaphore(&timeline_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::Poisoned;
      submission->result.message = timeline_error;
      submission->result.fault.code = "TIMELINE_UNAVAILABLE";
      submission->result.fault.message = timeline_error;
      return true;
    }
    uint64_t current = 0;
    if (vkGetSemaphoreCounterValue(device_, timeline_semaphore_, &current) != VK_SUCCESS) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::Poisoned;
      submission->result.message = "vkGetSemaphoreCounterValue failed";
      submission->result.fault.code = "TIMELINE_UNAVAILABLE";
      submission->result.fault.message = submission->result.message;
      return true;
    }
    if (wait_value != 0 && current < wait_value) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::Poisoned;
      submission->result.message = "timeline wait point is unsatisfied";
      submission->result.fault.code = "TIMELINE_WAIT_UNSATISFIED";
      submission->result.fault.message = submission->result.message;
      return true;
    }
    if (signal_value != 0 && signal_value <= current) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::Poisoned;
      submission->result.message = "timeline signal must be strictly monotonic";
      submission->result.fault.code = "TIMELINE_SIGNAL_NOT_MONOTONIC";
      submission->result.fault.message = submission->result.message;
      return true;
    }
  }

  if (!compiled.plan.certificate.ranges.empty()) {
    const auto verification = vg::ir::verify(compiled.plan.module);
    for (const auto& effect : verification.inferred_effects) {
      if (!compiled.plan.certificate.covers(effect)) {
        submission->result.ok = false;
        submission->result.poison = vg::core::PoisonState::Poisoned;
        submission->result.message = "certificate does not cover inferred effect";
        submission->result.missing_effects.push_back(effect);
        return true;
      }
    }
  }

  const auto& package = *compiled.compute_package;
  std::map<uint64_t, std::pair<uint32_t, uint32_t>> generation_by_allocation;
  for (const auto& instruction : compiled.plan.module.instructions)
    generation_by_allocation.emplace(instruction.allocation,
                                     std::make_pair(instruction.generation, instruction.representation_epoch));

  std::vector<VkDeviceAddress> addresses;
  std::vector<vg::core::Allocation*> touched;
  for (const auto& binding : package.bindings) {
    auto it = generation_by_allocation.find(binding.allocation);
    vg::core::Allocation* allocation = it == generation_by_allocation.end()
        ? nullptr
        : arena.lookup(binding.allocation, it->second.first, it->second.second);
    if (allocation == nullptr) {
      submission->result.ok = false;
      submission->result.poison = vg::core::PoisonState::Poisoned;
      submission->result.message = "stale generation, representation epoch, or out-of-bounds allocation reference";
      return true;
    }
    AllocationRecord* record = nullptr;
    std::string buffer_error;
    if (!ensure_buffer(*allocation, &record, &buffer_error)) {
      submission->result.ok = false;
      submission->result.message = "Vulkan buffer allocation failed: " + buffer_error;
      return true;
    }
    addresses.push_back(record->device_address);
    touched.push_back(allocation);
  }

  std::string dispatch_error;
  const auto dispatch_start = std::chrono::steady_clock::now();
  if (!dispatch_and_wait(addresses, wait_value, signal_value, &dispatch_error)) {
    submission->result.ok = false;
    submission->result.message = "Vulkan dispatch failed: " + dispatch_error;
    return true;
  }
  // TASK-B12, compile-review-only: structural counts match exactly what
  // dispatch_and_wait() above issues (see its body) -- 1 command buffer, one
  // bind+push+dispatch scope, no explicit barrier, one host-blocking
  // vkWaitForFences inside submit_and_wait(). cpu_submit_ns covers the whole
  // record-through-fence-wait call because this path does not separately
  // instrument vkEndCommandBuffer as a distinct "encode" boundary, so
  // cpu_encode_ns stays 0 rather than a guessed split. None of this has ever
  // executed on real hardware in this environment; it is reviewed against
  // the Vulkan calls actually issued, not measured.
  submission->cpu_submit_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dispatch_start).count();
  submission->report.command_buffer_count = 1;
  submission->report.encoder_count = 1;
  submission->report.barrier_count = 0;
  submission->report.queue_wait_count = 1;
  // No separate host-side mirror: timeline_value always reflects what the
  // GPU actually reached, read back fresh rather than passed through from
  // the plan (which is merely the requested value).
  if (timeline_semaphore_ != VK_NULL_HANDLE) {
    uint64_t current = 0;
    vkGetSemaphoreCounterValue(device_, timeline_semaphore_, &current);
    submission->timeline_value = current;
  }

  for (auto* allocation : touched) {
    auto it = allocation_map_.find(allocation->id);
    if (it != allocation_map_.end() && !allocation->bytes.empty())
      std::memcpy(allocation->bytes.data(), it->second.mapped, allocation->bytes.size());
  }

  for (size_t index = 0; index < compiled.plan.module.instructions.size(); ++index) {
    const auto& instruction = compiled.plan.module.instructions[index];
    const vg::ir::Access access = instruction.op == "load"        ? vg::ir::Access::Read
                                  : instruction.op == "store"      ? vg::ir::Access::Write
                                  : instruction.op == "atomic_add" ? vg::ir::Access::Atomic
                                                                    : vg::ir::Access::Publish;
    const vg::ir::Effect effect{instruction.allocation, instruction.offset, instruction.size, access,
                                instruction.representation_epoch};
    submission->result.trace.push_back(effect);
    submission->result.witness.record(effect, static_cast<uint32_t>(index));
  }
  submission->result.ok = true;
  submission->result.poison = vg::core::PoisonState::Valid;

  if (!compiled.plan.task_graph.tasks().empty()) {
    // Pack -> dispatch the GPU publish kernel + Tier1 indirect conversion ->
    // read back -> verify every slot reached Published -> unpack, walking
    // slots in the task graph's deterministic dependency order so
    // submission->published_tasks is sequence-identical to
    // reference::execute_task_graph()'s oracle output (same ordering
    // convention as Metal's submit()).
    std::vector<uint32_t> order;
    std::string order_error;
    if (!compiled.plan.task_graph.deterministic_order(&order, &order_error)) {
      submission->result.ok = false;
      submission->result.message = order_error;
      return true;
    }
    const auto& tasks = compiled.plan.task_graph.tasks();
    const uint32_t count = static_cast<uint32_t>(tasks.size());

    std::string task_pipeline_error;
    if (!ensure_task_ring_pipeline(&task_pipeline_error)) {
      submission->result.ok = false;
      submission->result.message = "Vulkan task ring pipeline compile failed: " + task_pipeline_error;
      return true;
    }

    TaskRingBuffers ring_buffers{};
    std::string ring_error;
    if (!create_task_ring_buffers(count, &ring_buffers, &ring_error)) {
      submission->result.ok = false;
      submission->result.message = "Vulkan task ring buffer allocation failed: " + ring_error;
      return true;
    }
    uint32_t* inputs = static_cast<uint32_t*>(ring_buffers.inputs_mapped);
    for (uint32_t i = 0; i < count; ++i)
      pack_task_record(tasks[i], inputs + i * vg::compiler::kTaskRingWordsPerRecord);

    std::string tier1_error;
    const auto tier1_start = std::chrono::steady_clock::now();
    if (!dispatch_task_ring_and_tier1(ring_buffers, order, addresses, &tier1_error)) {
      submission->result.ok = false;
      submission->result.message = "Vulkan task ring dispatch failed: " + tier1_error;
      destroy_task_ring_buffers(&ring_buffers);
      return true;
    }
    // TASK-B12, compile-review-only: this branch replaces the counts set
    // above with dispatch_task_ring_and_tier1()'s own structural shape (see
    // its body) -- still 1 command buffer, but 2 bind+dispatch scopes (task
    // ring publish, then the Tier1 indirect compute dispatch) and the 2
    // explicit vkCmdPipelineBarrier2 calls that make the ring write visible
    // to the copy stage and the copy visible to indirect-draw reads.
    // cpu_submit_ns again covers the whole record-through-fence-wait span.
    submission->cpu_submit_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tier1_start).count();
    submission->report.command_buffer_count = 1;
    submission->report.encoder_count = 2;
    submission->report.barrier_count = 2;
    submission->report.queue_wait_count = 1;

    const uint32_t* states = static_cast<const uint32_t*>(ring_buffers.state_mapped);
    const uint32_t* fields = static_cast<const uint32_t*>(ring_buffers.fields_mapped);
    submission->published_tasks.reserve(count);
    for (uint32_t index : order) {
      if (states[index] != static_cast<uint32_t>(vg::core::PublicationState::Published)) {
        submission->result.ok = false;
        submission->result.message = "task ring slot did not reach Published state";
        destroy_task_ring_buffers(&ring_buffers);
        return true;
      }
      submission->published_tasks.push_back(unpack_task_record(fields + index * vg::compiler::kTaskRingWordsPerRecord));
    }
    destroy_task_ring_buffers(&ring_buffers);
  }
  return true;
#endif
}

std::unique_ptr<DeviceHal> make_device_hal(std::string* error) {
  auto adapter = std::unique_ptr<DeviceHal>(new DeviceHal());
#if !defined(VG_HAS_VULKAN)
  adapter->capabilities_.backend = vg::hal::BackendKind::Vulkan;
  adapter->capabilities_.adapter_name = "Vulkan unavailable (build without VG_HAS_VULKAN)";
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
  if (vkCreateInstance(&instance_info, nullptr, &adapter->instance_) != VK_SUCCESS) {
    set_error(error, "failed to create Vulkan instance");
    return nullptr;
  }

  uint32_t device_count = 0;
  if (vkEnumeratePhysicalDevices(adapter->instance_, &device_count, nullptr) != VK_SUCCESS ||
      device_count == 0) {
    set_error(error, "no Vulkan physical device available");
    return nullptr;
  }
  std::vector<VkPhysicalDevice> devices(device_count);
  vkEnumeratePhysicalDevices(adapter->instance_, &device_count, devices.data());
  adapter->physical_device_ = devices.front();
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(adapter->physical_device_, &properties);

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

  adapter->capabilities_.backend = vg::hal::BackendKind::Vulkan;
  adapter->capabilities_.adapter_name = properties.deviceName;
  adapter->capabilities_.driver = "Vulkan " + std::to_string(VK_API_VERSION_MAJOR(properties.apiVersion)) +
      "." + std::to_string(VK_API_VERSION_MINOR(properties.apiVersion));
  adapter->capabilities_.max_buffer_size = properties.limits.maxStorageBufferRange;
  adapter->capabilities_.address_width = 0;
  adapter->capabilities_.min_buffer_alignment = static_cast<uint32_t>(
      std::min<VkDeviceSize>(properties.limits.minStorageBufferOffsetAlignment,
                             std::numeric_limits<uint32_t>::max()));

  uint32_t queue_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(adapter->physical_device_, &queue_count, nullptr);
  std::vector<VkQueueFamilyProperties> queues(queue_count);
  vkGetPhysicalDeviceQueueFamilyProperties(adapter->physical_device_, &queue_count, queues.data());
  for (uint32_t i = 0; i < queue_count; ++i) {
    if ((queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
      adapter->compute_queue_family_ = i;
      break;
    }
  }
  if (adapter->compute_queue_family_ == UINT32_MAX) {
    set_error(error, "Vulkan device has no compute queue family");
    return nullptr;
  }

  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  features.pNext = &features12;
  features12.pNext = &features13;
  vkGetPhysicalDeviceFeatures2(adapter->physical_device_, &features);
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
  if (bda) adapter->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::LinearAddress);
  if (bda) adapter->capabilities_.address_width = 64;
  if (timeline) adapter->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::Timeline);
  if (sync2) adapter->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::EffectDag);
  if (sync2) {
    // Task ring's Tier0 publish -> Tier1 indirect-dispatch conversion relies
    // on vkCmdPipelineBarrier2 (see dispatch_task_ring_and_tier1), so both
    // bits are gated on sync2 rather than claimed unconditionally now that
    // B7/B8 actually wire task-graph submission through this backend.
    adapter->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::TaskPublication);
    adapter->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::IndirectTier1);
  }
  adapter->capabilities_.validation_available = true;
  adapter->capabilities_.timestamps_available = queues[adapter->compute_queue_family_].timestampValidBits != 0;

  if (!bda) {
    set_error(error, "Vulkan device does not support bufferDeviceAddress, required by this backend");
    return nullptr;
  }

  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = adapter->compute_queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkPhysicalDeviceFeatures enabled_features{};
  enabled_features.shaderInt64 = atomic_int64 ? VK_TRUE : VK_FALSE;

  VkPhysicalDeviceVulkan13Features enabled13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  enabled13.synchronization2 = sync2 ? VK_TRUE : VK_FALSE;

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
  if (vkCreateDevice(adapter->physical_device_, &device_info, nullptr, &adapter->device_) != VK_SUCCESS) {
    set_error(error, "failed to create Vulkan device");
    return nullptr;
  }
  vkGetDeviceQueue(adapter->device_, adapter->compute_queue_family_, 0, &adapter->compute_queue_);
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
    if (!adapter->ensure_timeline_semaphore(&timeline_error)) {
      set_error(error, ("failed to create Vulkan timeline semaphore: " + timeline_error).c_str());
      return nullptr;
    }
  }
  return adapter;
#endif
}

}  // namespace vg::vulkan
