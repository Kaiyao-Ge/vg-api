#include "backends/vulkan/vulkan_device_hal.h"
#include "compiler/compute_task_ring.h"

// The raster pipeline key hashes the GLSL source itself rather than an IR hash:
// compiler::raster_facet_vulkan_source() is a hand-written kernel with no
// ir::Module behind it, and 06 §7 requires the code object's identity in the
// key regardless of where that code came from.
#include "ir/sha256.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <string_view>
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

#if defined(VG_HAS_VULKAN)
void append_cache_key_component(std::string* key, std::string_view value) {
  key->append(std::to_string(value.size()));
  key->push_back(':');
  key->append(value);
  key->push_back('|');
}

std::string compute_pipeline_cache_key(const vg::core::ExecutionPlan::ResolvedNode& node,
                                       const vg::compiler::ComputePackage& package) {
  std::string key;
  append_cache_key_component(&key, package.canonical_ir_hash);
  append_cache_key_component(&key, node.entry_name);
  append_cache_key_component(&key, package.root_schema);
  append_cache_key_component(&key, package.vulkan_glsl_source);
  key.append(std::to_string(package.bindings.size()));
  key.push_back('|');
  for (const auto& binding : package.bindings) {
    key.append(std::to_string(binding.allocation));
    key.push_back(':');
    key.append(std::to_string(binding.binding));
    key.push_back('|');
  }
  return key;
}
#endif

bool same_compute_bindings(const std::vector<vg::compiler::ComputeBinding>& left,
                           const std::vector<vg::compiler::ComputeBinding>& right) {
  return left.size() == right.size() &&
         std::ranges::equal(left, right, [](const auto& lhs, const auto& rhs) {
           return lhs.allocation == rhs.allocation && lhs.binding == rhs.binding;
         });
}

bool same_vulkan_compute_package(const vg::compiler::ComputePackage& actual,
                                 const vg::compiler::ComputePackage& expected) {
  return actual.canonical_ir_hash == expected.canonical_ir_hash &&
         actual.root_schema == expected.root_schema &&
         actual.vulkan_glsl_source == expected.vulkan_glsl_source &&
         same_compute_bindings(actual.bindings, expected.bindings);
}

bool node_ref_equal(vg::core::NodeTable::Ref left, vg::core::NodeTable::Ref right) {
  return left.index == right.index && left.generation == right.generation;
}

// Stage 6 chooses one conservative compute-wide sync2 memory barrier for
// each dependent wave. Prelude representation work has already completed in
// the shared once-per-submission representation stage, so it needs no extra
// compute barrier. No raw graph/effect interpretation belongs in this adapter.
#if defined(VG_HAS_VULKAN)
void lower_wave_transitions(vg::hal::CompiledPlan* compiled) {
  for (auto& transition : compiled->transition_operations) {
    transition.state = vg::hal::CompiledPlan::TransitionLoweringState::Lowered;
    if (!transition.covers_execution_completion) continue;
    transition.barrier_count = 1;
    transition.serialized_fallback = true;
    ++compiled->report.transition_barrier_count;
    ++compiled->report.transition_serialized_fallback_count;
    compiled->report.add("task_effect_barrier", vg::hal::LoweringClass::Serialized, 1, 0,
                         "sealed wave transition=" + std::to_string(transition.semantic_transition) +
                         "; conservative global compute memory visibility via vkCmdPipelineBarrier2");
  }
}
#endif
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
//
// `shader_stage` is glslc's -fshader-stage value and `defines` its -D list.
// Both exist for raster_facet_vulkan_source(), which is one string holding two
// entry points behind VG_RASTER_VERTEX_STAGE / VG_RASTER_FRAGMENT_STAGE
// (compiler.h explains why: a GLSL translation unit has exactly one entry
// point), so the host has to compile the same text twice with different
// stage/define pairs.
bool compile_glsl_stage(const std::string& glsl_source, const char* shader_stage,
                        const std::vector<std::string>& defines, std::vector<uint32_t>* spirv,
                        std::string* error) {
#if !defined(VG_GLSLC_PATH)
  (void)shader_stage;
  (void)defines;
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

  // Built as owning strings first: the argument list is no longer fixed
  // (stage and -D flags vary per shader), and posix_spawn wants a NULL-
  // terminated char* array whose entries stay alive across the call.
  std::vector<std::string> arguments;
  arguments.emplace_back(VG_GLSLC_PATH);
  arguments.emplace_back(std::string("-fshader-stage=") + shader_stage);
  arguments.emplace_back("--target-env=vulkan1.2");
  for (const auto& define : defines) arguments.emplace_back("-D" + define);
  arguments.emplace_back("-o");
  arguments.emplace_back("-");
  arguments.emplace_back("-");
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);

  pid_t pid = 0;
  const int spawn_result = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
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

bool compile_glsl_to_spirv(const std::string& glsl_source, std::vector<uint32_t>* spirv, std::string* error) {
  return compile_glsl_stage(glsl_source, "compute", {}, spirv, error);
}

// Shared allocate-bind-map(-address) helper factored out of ensure_buffer so
// TaskRingBuffers' three publication buffers (state/fields/inputs) don't each
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

}  // namespace

bool DeviceHal::ensure_pipeline(const std::string& cache_key, const std::string& glsl_source,
                                uint32_t binding_count, const ComputePipelineRecord** out,
                                bool* cache_hit, std::string* error) {
  const auto existing = compute_pipeline_cache_.find(cache_key);
  if (existing != compute_pipeline_cache_.end()) {
    if (out != nullptr) *out = &existing->second;
    if (cache_hit != nullptr) *cache_hit = true;
    return true;
  }

  ComputePipelineRecord created;
  const auto destroy_created = [&] {
    if (created.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, created.pipeline, nullptr);
    if (created.pipeline_layout != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device_, created.pipeline_layout, nullptr);
    if (created.shader_module != VK_NULL_HANDLE)
      vkDestroyShaderModule(device_, created.shader_module, nullptr);
    created = {};
  };

  std::vector<uint32_t> spirv;
  if (!compile_glsl_to_spirv(glsl_source, &spirv, error)) return false;

  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = spirv.size() * sizeof(uint32_t);
  module_info.pCode = spirv.data();
  if (vkCreateShaderModule(device_, &module_info, nullptr, &created.shader_module) != VK_SUCCESS) {
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
  if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &created.pipeline_layout) != VK_SUCCESS) {
    if (error) *error = "vkCreatePipelineLayout failed";
    destroy_created();
    return false;
  }

  VkPipelineShaderStageCreateInfo stage_info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage_info.module = created.shader_module;
  stage_info.pName = "main";

  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = stage_info;
  pipeline_info.layout = created.pipeline_layout;
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &created.pipeline) !=
      VK_SUCCESS) {
    if (error) *error = "vkCreateComputePipelines failed";
    destroy_created();
    return false;
  }
  created.binding_count = binding_count;
  auto [inserted, was_inserted] = compute_pipeline_cache_.emplace(cache_key, created);
  if (!was_inserted) {
    destroy_created();
    if (out != nullptr) *out = &inserted->second;
    if (cache_hit != nullptr) *cache_hit = true;
    return true;
  }
  if (out != nullptr) *out = &inserted->second;
  if (cache_hit != nullptr) *cache_hit = false;
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
// command_pool_ is created lazily by whichever canonical task-graph,
// publication, or facet path records first; all paths reuse the same pool.
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
                     const VkSemaphore* signal_semaphores, std::string* error,
                     uint64_t* actual_host_waits = nullptr) {
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
}  // namespace

namespace {

// --- Phase C facet lowering helpers ---------------------------------------
// Everything here is compile-review-only (ADR-024): no Vulkan hardware is
// reachable from this machine, so these mappings have been written and read
// against the Vulkan contract and the 07 rules they cite, never executed.

// The two formats core::PixelFormat models. Both are 4 bytes wide, which is
// what makes every CanonicalView byte offset below a multiple of 4 and so a
// legal VkBufferImageCopy::bufferOffset without extra alignment padding.
VkFormat to_vk_format(vg::core::PixelFormat format) {
  return format == vg::core::PixelFormat::RGBA8Unorm ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32_SFLOAT;
}

const char* storage_image_format_qualifier(VkFormat format) {
  return format == VK_FORMAT_R8G8B8A8_UNORM ? "rgba8" : "r32f";
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

// VkComponentMapping on the *view*, not a copy that rewrites the bytes: a
// swizzle changes what a shader read yields, and Vulkan expresses exactly that
// on the image view (06 §6.1 lists swizzle among a facet's compiled inputs).
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

// The layout each facet kind is read in. This is the adapter's own
// representation state (07 §7: "VG 不向用户暴露 old/new layout"); a caller
// never names a VkImageLayout and cannot observe one.
VkImageLayout facet_read_layout(vg::core::FacetKind kind) {
  switch (kind) {
    case vg::core::FacetKind::Sample: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case vg::core::FacetKind::Storage: return VK_IMAGE_LAYOUT_GENERAL;
    case vg::core::FacetKind::Attachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    default: return VK_IMAGE_LAYOUT_UNDEFINED;
  }
}

// TRANSFER_DST is unconditional because every facet image here is filled from
// the allocation's linear bytes, and TRANSFER_SRC because texel readback
// copies back out of it. SAMPLED is added even to Storage/Attachment images
// because the fragment stage of raster_facet_vulkan_source() samples what a
// previous pass wrote -- a usage flag omitted here would make that a
// validation error rather than a reported Unsupported.
VkImageUsageFlags facet_image_usage(vg::core::FacetKind kind) {
  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT;
  if (kind == vg::core::FacetKind::Storage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (kind == vg::core::FacetKind::Attachment) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  return usage;
}

// The sync2 scope each layout is entered/left in. Every layout this backend
// ever puts an image in is enumerated, so the conservative default below is
// unreachable -- it exists because 07 §7 forbids a "conservatively unknown"
// scope from silently becoming a device-wide barrier: if a future layout ever
// reaches it, the barrier is honestly ALL_COMMANDS and the caller's barrier
// count still reports it as one recorded barrier rather than hiding it.
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

// Records one image layout transition over `range`. Split out from
// DeviceHal::record_layout_transition so the transient multisample attachment
// (which has no facet record because it is backend-private scratch, not a
// facet) can be transitioned by the same code.
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

// Holds a FacetPool GPU-use bracket for as long as a command buffer may still
// reference the slot (07 §6's step 6, 06 §6.4/§11). Scoped rather than
// hand-paired because these entry points have many early returns and a leaked
// use would pin the slot's index out of the free list forever. Copied in shape
// from Metal's FacetUseGuard so both backends' facet lifetime discipline is
// recognizably the same mechanism.
class FacetUseGuard {
 public:
  FacetUseGuard(vg::core::FacetPool& pool, vg::core::FacetRef ref) : pool_(pool), ref_(ref) {}
  FacetUseGuard(const FacetUseGuard&) = delete;
  FacetUseGuard& operator=(const FacetUseGuard&) = delete;
  FacetUseGuard(FacetUseGuard&&) = delete;
  FacetUseGuard& operator=(FacetUseGuard&&) = delete;
  ~FacetUseGuard() {
    if (held_) pool_.end_gpu_use(ref_);
  }

  bool begin(const vg::core::Arena& arena, std::string* error) {
    held_ = pool_.begin_gpu_use(arena, ref_, error);
    return held_;
  }

 private:
  vg::core::FacetPool& pool_;
  vg::core::FacetRef ref_;
  bool held_{};
};

vg::hal::LoweringReport make_facet_report() {
  vg::hal::LoweringReport report;
  report.backend = vg::hal::BackendKind::Vulkan;
  report.supported = true;
  return report;
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

// Backend-private StorageFacet write kernel, the same "independent
// hand-written kernel + dedicated pipeline" precedent compiler.h sets for
// task_ring/cull_compact/sample_facet: a storage-image write is a different
// resource class than the buffer-only IR taxonomy build_linear_compute_package
// compiles, and it has exactly one consumer. It lives here rather than in
// vg_compiler because it is not part of any cross-backend contract -- Metal's
// storage kernel is likewise private to metal_device_hal.mm.
//
// The format qualifier must match the image's own format (GLSL requires it),
// which is why this is a generator over the two formats core::PixelFormat
// models rather than one fixed string. Writing through a mismatched qualifier
// would be exactly the silent format substitution 06 §6.2 forbids.
std::string storage_facet_glsl_source(const char* format_qualifier) {
  std::string source = "#version 450\n";
  source += "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n";
  source += std::string("layout(set = 0, binding = 0, ") + format_qualifier +
            ") uniform writeonly image2D vg_image;\n";
  source += "layout(set = 0, binding = 1) uniform VgStorageWrite { vec4 value; } vg_write;\n";
  source += "void main() {\n";
  source += "  imageStore(vg_image, ivec2(gl_GlobalInvocationID.xy), vg_write.value);\n";
  source += "}\n";
  return source;
}

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

// MultisampleResolve is a store action in 06 §6.3's taxonomy, but in Vulkan the
// resolve is a property of the attachment (resolveMode/resolveImageView) and
// the store op still says what happens to the multisample image itself. This
// backend stores DONT_CARE for the multisample samples in that case, which is
// what makes the resolve the only thing that reaches memory.
VkAttachmentStoreOp to_vk_store_op(vg::vulkan::AttachmentStoreAction store) {
  switch (store) {
    case vg::vulkan::AttachmentStoreAction::Store: return VK_ATTACHMENT_STORE_OP_STORE;
    case vg::vulkan::AttachmentStoreAction::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    case vg::vulkan::AttachmentStoreAction::MultisampleResolve: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
  }
  return VK_ATTACHMENT_STORE_OP_STORE;
}

}  // namespace

bool DeviceHal::dispatch_task_graph(const std::vector<CanonicalTaskDispatch>& dispatches,
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
  if (task_ring_pipeline_layout_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device_, task_ring_pipeline_layout_, nullptr);
    task_ring_pipeline_layout_ = VK_NULL_HANDLE;
  }
  if (task_ring_shader_module_ != VK_NULL_HANDLE) {
    vkDestroyShaderModule(device_, task_ring_shader_module_, nullptr);
    task_ring_shader_module_ = VK_NULL_HANDLE;
  }
  std::vector<uint32_t> spirv;
  if (!compile_glsl_to_spirv(vg::compiler::task_ring_vulkan_source(), &spirv, error)) return false;

  VkShaderModule shader_module{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
  VkPipeline pipeline{VK_NULL_HANDLE};
  const auto cleanup = [&] {
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline, nullptr);
    if (pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout, nullptr);
    if (shader_module != VK_NULL_HANDLE) vkDestroyShaderModule(device_, shader_module, nullptr);
  };

  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = spirv.size() * sizeof(uint32_t);
  module_info.pCode = spirv.data();
  if (vkCreateShaderModule(device_, &module_info, nullptr, &shader_module) != VK_SUCCESS) {
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
  if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
    if (error) *error = "vkCreatePipelineLayout (task ring) failed";
    cleanup();
    return false;
  }

  VkPipelineShaderStageCreateInfo stage_info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage_info.module = shader_module;
  stage_info.pName = "main";

  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = stage_info;
  pipeline_info.layout = pipeline_layout;
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) !=
      VK_SUCCESS) {
    if (error) *error = "vkCreateComputePipelines (task ring) failed";
    cleanup();
    return false;
  }
  task_ring_shader_module_ = shader_module;
  task_ring_pipeline_layout_ = pipeline_layout;
  task_ring_pipeline_ = pipeline;
  return true;
}

bool DeviceHal::create_task_ring_buffers(uint32_t task_count, TaskRingBuffers* out, std::string* error) {
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

void DeviceHal::destroy_task_ring_buffers(TaskRingBuffers* buffers) {
  RawBuffer state{buffers->state_buffer, buffers->state_memory, buffers->state_address, buffers->state_mapped};
  RawBuffer fields{buffers->fields_buffer, buffers->fields_memory, buffers->fields_address, buffers->fields_mapped};
  RawBuffer inputs{buffers->inputs_buffer, buffers->inputs_memory, buffers->inputs_address, buffers->inputs_mapped};
  destroy_raw_buffer(device_, &state);
  destroy_raw_buffer(device_, &fields);
  destroy_raw_buffer(device_, &inputs);
  *buffers = TaskRingBuffers{};
}

bool DeviceHal::dispatch_task_ring_publication(const TaskRingBuffers& buffers,
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

namespace {
// Inverse of decode_first_texel: what one texel of `format` looks like in the
// linear byte layout CanonicalView describes. Used by the StorageFacet write
// path, which has to produce the same bytes a shader store would.
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
}  // namespace

const vg::core::FacetSlot* DeviceHal::resolve_facet(const vg::core::Arena& arena, const vg::core::FacetPool& pool,
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

bool DeviceHal::record_layout_transition(VkCommandBuffer command_buffer, VulkanFacetRecord* record,
                                         VkImageLayout new_layout) {
  if (record->layout == new_layout) return false;
  record_image_barrier(command_buffer, record->image, record->layout, new_layout, record->mip_levels,
                       record->array_layers);
  record->layout = new_layout;
  return true;
}

bool DeviceHal::ensure_facet_image(const vg::core::Arena& arena, const vg::core::FacetPool& pool,
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

uint32_t DeviceHal::retire_stale_facet_images(const vg::core::Arena& arena, const vg::core::FacetPool& pool) {
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

bool DeviceHal::ensure_sampler(vg::core::FilterMode filter, vg::core::WrapMode wrap, VkSampler* out,
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

bool DeviceHal::ensure_descriptor_pool(std::string* error) {
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

bool DeviceHal::ensure_sample_facet_pipeline(bool array_kernel, bool checked_profile, VkPipeline* pipeline,
                                             VkPipelineLayout* layout, VkDescriptorSetLayout* set_layout,
                                             std::string* error) {
  VkDescriptorSetLayout& cached_set_layout = array_kernel ? sample_array_set_layout_ : sample_set_layout_;
  VkPipelineLayout& cached_layout = array_kernel ? sample_array_pipeline_layout_ : sample_pipeline_layout_;
  VkShaderModule& cached_module = array_kernel ? sample_array_shader_module_ : sample_shader_module_;

  if (cached_set_layout == VK_NULL_HANDLE) {
    // Binding numbers come from the emitted GLSL, not from a second table
    // maintained here: sample_facet_vulkan_source() declares 0 = combined image
    // sampler, 1 = uv, 2 = output, 3 = lod, 5 = facet token, 6 = generation
    // table, 7 = slot count, 8 = violation counter, and the array kernel adds
    // 4 = per-coordinate array slices.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    const auto add_binding = [&bindings](uint32_t binding, VkDescriptorType type) {
      VkDescriptorSetLayoutBinding entry{};
      entry.binding = binding;
      entry.descriptorType = type;
      entry.descriptorCount = 1;
      entry.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      bindings.push_back(entry);
    };
    add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    add_binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    if (array_kernel) add_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    add_binding(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    add_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    add_binding(7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    add_binding(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    VkDescriptorSetLayoutCreateInfo set_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
    set_layout_info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr, &cached_set_layout) != VK_SUCCESS) {
      if (error) *error = "vkCreateDescriptorSetLayout failed for the sample facet kernel";
      return false;
    }
  }
  if (cached_layout == VK_NULL_HANDLE) {
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &cached_set_layout;
    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &cached_layout) != VK_SUCCESS) {
      if (error) *error = "vkCreatePipelineLayout failed for the sample facet kernel";
      return false;
    }
  }
  if (cached_module == VK_NULL_HANDLE) {
    const std::string source = array_kernel ? vg::compiler::sample_facet_array_vulkan_source()
                                            : vg::compiler::sample_facet_vulkan_source();
    std::vector<uint32_t> spirv;
    if (!compile_glsl_to_spirv(source, &spirv, error)) return false;
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = spirv.size() * sizeof(uint32_t);
    module_info.pCode = spirv.data();
    if (vkCreateShaderModule(device_, &module_info, nullptr, &cached_module) != VK_SUCCESS) {
      if (error) *error = "vkCreateShaderModule failed for the sample facet kernel";
      return false;
    }
  }

  const uint32_t pipeline_key = (array_kernel ? 2u : 0u) | (checked_profile ? 1u : 0u);
  const auto cached_pipeline = sample_pipelines_.find(pipeline_key);
  if (cached_pipeline == sample_pipelines_.end()) {
    // 03 §12: the profile is a specialization of one module, never a second
    // shader. constant_id 0 defaults to false in the GLSL, so a FastNative
    // pipeline compiles the guard, its four extra bindings' accesses and its
    // atomic away entirely instead of paying for a check it did not ask for.
    const VkBool32 checked_value = checked_profile ? VK_TRUE : VK_FALSE;
    VkSpecializationMapEntry entry{};
    entry.constantID = vg::compiler::kFacetCheckedProfileFunctionConstant;
    entry.offset = 0;
    entry.size = sizeof(VkBool32);
    VkSpecializationInfo specialization{};
    specialization.mapEntryCount = 1;
    specialization.pMapEntries = &entry;
    specialization.dataSize = sizeof(checked_value);
    specialization.pData = &checked_value;

    VkPipelineShaderStageCreateInfo stage_info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage_info.module = cached_module;
    stage_info.pName = "main";
    stage_info.pSpecializationInfo = &specialization;
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage = stage_info;
    pipeline_info.layout = cached_layout;
    VkPipeline created{VK_NULL_HANDLE};
    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &created) != VK_SUCCESS) {
      if (error) *error = "vkCreateComputePipelines failed for the sample facet kernel";
      return false;
    }
    sample_pipelines_.emplace(pipeline_key, created);
    *pipeline = created;
  } else {
    *pipeline = cached_pipeline->second;
  }
  *layout = cached_layout;
  *set_layout = cached_set_layout;
  return true;
}

bool DeviceHal::ensure_storage_facet_pipeline(VkFormat format, VkPipeline* pipeline, std::string* error) {
  if (storage_set_layout_ == VK_NULL_HANDLE) {
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout_info.bindingCount = 2;
    set_layout_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr, &storage_set_layout_) != VK_SUCCESS) {
      if (error) *error = "vkCreateDescriptorSetLayout failed for the storage facet kernel";
      return false;
    }
  }
  if (storage_pipeline_layout_ == VK_NULL_HANDLE) {
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &storage_set_layout_;
    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &storage_pipeline_layout_) != VK_SUCCESS) {
      if (error) *error = "vkCreatePipelineLayout failed for the storage facet kernel";
      return false;
    }
  }
  const uint32_t format_key = static_cast<uint32_t>(format);
  const auto cached_pipeline = storage_pipelines_.find(format_key);
  if (cached_pipeline != storage_pipelines_.end()) {
    *pipeline = cached_pipeline->second;
    return true;
  }
  if (storage_shader_modules_.find(format_key) == storage_shader_modules_.end()) {
    std::vector<uint32_t> spirv;
    if (!compile_glsl_to_spirv(storage_facet_glsl_source(storage_image_format_qualifier(format)), &spirv, error))
      return false;
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = spirv.size() * sizeof(uint32_t);
    module_info.pCode = spirv.data();
    VkShaderModule module{VK_NULL_HANDLE};
    if (vkCreateShaderModule(device_, &module_info, nullptr, &module) != VK_SUCCESS) {
      if (error) *error = "vkCreateShaderModule failed for the storage facet kernel";
      return false;
    }
    storage_shader_modules_.emplace(format_key, module);
  }
  VkPipelineShaderStageCreateInfo stage_info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage_info.module = storage_shader_modules_[format_key];
  stage_info.pName = "main";
  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = stage_info;
  pipeline_info.layout = storage_pipeline_layout_;
  VkPipeline created{VK_NULL_HANDLE};
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &created) != VK_SUCCESS) {
    if (error) *error = "vkCreateComputePipelines failed for the storage facet kernel";
    return false;
  }
  storage_pipelines_.emplace(format_key, created);
  *pipeline = created;
  return true;
}

bool DeviceHal::ensure_raster_shader_modules(std::string* error) {
  if (raster_set_layout_ == VK_NULL_HANDLE) {
    // raster_facet_vulkan_source() declares set 0 binding 0 = vertex array
    // (vertex stage), binding 1 = combined image sampler and binding 2 = tint
    // uniform block (both fragment stage). One set layout covers both stages,
    // unlike Metal's per-stage binding tables.
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout_info.bindingCount = 3;
    set_layout_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr, &raster_set_layout_) != VK_SUCCESS) {
      if (error) *error = "vkCreateDescriptorSetLayout failed for the raster facet pipeline";
      return false;
    }
  }
  if (raster_pipeline_layout_ == VK_NULL_HANDLE) {
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &raster_set_layout_;
    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &raster_pipeline_layout_) != VK_SUCCESS) {
      if (error) *error = "vkCreatePipelineLayout failed for the raster facet pipeline";
      return false;
    }
  }
  if (raster_vertex_module_ != VK_NULL_HANDLE && raster_fragment_module_ != VK_NULL_HANDLE) return true;

  const std::string source = vg::compiler::raster_facet_vulkan_source();
  const auto compile_stage = [&](const char* stage, const char* define, VkShaderModule* module) {
    if (*module != VK_NULL_HANDLE) return true;
    std::vector<uint32_t> spirv;
    if (!compile_glsl_stage(source, stage, {define}, &spirv, error)) return false;
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = spirv.size() * sizeof(uint32_t);
    module_info.pCode = spirv.data();
    if (vkCreateShaderModule(device_, &module_info, nullptr, module) != VK_SUCCESS) {
      if (error) *error = std::string("vkCreateShaderModule failed for the raster ") + stage + " stage";
      return false;
    }
    return true;
  };
  if (!compile_stage("vertex", "VG_RASTER_VERTEX_STAGE", &raster_vertex_module_)) return false;
  if (!compile_stage("fragment", "VG_RASTER_FRAGMENT_STAGE", &raster_fragment_module_)) return false;
  return true;
}

bool DeviceHal::ensure_raster_pipeline(vg::compiler::PipelineClassificationCache& cache,
                                       std::map<uint64_t, VkPipeline>& pipelines,
                                       const vg::compiler::PipelineKey& key, const std::string& trigger_reason,
                                       VkFormat attachment_format, uint32_t sample_count,
                                       const std::vector<std::pair<std::string, uint64_t>>& raster_state,
                                       VkPipeline* pipeline, bool* cache_hit, uint64_t* binary_size,
                                       std::string* error) {
  if (!supports_dynamic_rendering_) {
    if (error)
      *error = "Unsupported: this device does not support dynamic rendering, which is how this backend "
               "lowers an AttachmentFacet pass (07 §9)";
    return false;
  }
  if (!ensure_raster_shader_modules(error)) return false;

  // 07 §9's pipeline-key state, and only the parts this backend really can
  // compile in. A name with no lowering here is rejected rather than dropped:
  // it was classified as PipelineKey by a layer that believed this backend
  // would honor it.
  VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
  VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
  VkBool32 blend_enable = VK_FALSE;
  for (const auto& [name, value] : raster_state) {
    if (name == "cull_mode") {
      if (value == 1) cull_mode = VK_CULL_MODE_FRONT_BIT;
      else if (value == 2) cull_mode = VK_CULL_MODE_BACK_BIT;
      else cull_mode = VK_CULL_MODE_NONE;
    } else if (name == "front_face") {
      front_face = value == 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    } else if (name == "polygon_mode") {
      polygon_mode = value == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    } else if (name == "blend_enable") {
      blend_enable = value != 0 ? VK_TRUE : VK_FALSE;
    } else {
      if (error)
        *error = "Unsupported pipeline-key raster state '" + name +
                 "': this backend has no lowering for it, and folding it into the key would compile a "
                 "pipeline meaning something other than what was asked for";
      return false;
    }
  }

  const uint64_t hash = key.hash();
  vg::compiler::SpecializationReport report;
  const auto create = [&](uint64_t* created_binary_size, std::string* create_error) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = raster_vertex_module_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = raster_fragment_module_;
    stages[1].pName = "main";

    // No VkVertexInputBindingDescription at all: the vertex stage indexes a
    // storage buffer by gl_VertexIndex, which is this project's addressing
    // philosophy (04 §8, 06 §5) and keeps vertex layout out of the pipeline
    // key (06 §7) exactly as the Metal side keeps MTLVertexDescriptor out of
    // its own.
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport/scissor counts are fixed at 1 but their values are dynamic, so
    // neither enters the key (07 §9's "Vulkan dynamic state" bucket).
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = polygon_mode;
    rasterization.cullMode = cull_mode;
    rasterization.frontFace = front_face;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = to_vk_sample_count(sample_count);

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = blend_enable;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    // Dynamic rendering (07 §9): no VkRenderPass and no VkFramebuffer object,
    // but the attachment format and sample count are still compiled in, which
    // is exactly why both are pipeline-key fields rather than dynamic state.
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &attachment_format;

    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.pNext = &rendering;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = raster_pipeline_layout_;
    pipeline_info.renderPass = VK_NULL_HANDLE;
    VkPipeline created{VK_NULL_HANDLE};
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &created) != VK_SUCCESS) {
      if (create_error) *create_error = "vkCreateGraphicsPipelines failed for the raster facet pipeline";
      return false;
    }
    // A previously created pipeline under the same key can only exist if the
    // owning cache was cleared (run_pipeline_classification clears both arms so
    // its numbers describe that call alone); destroying it keeps that from
    // leaking while still counting this compile as the real compile it was.
    const auto existing = pipelines.find(hash);
    if (existing != pipelines.end()) {
      vkDestroyPipeline(device_, existing->second, nullptr);
      pipelines.erase(existing);
    }
    pipelines.emplace(hash, created);
    // 10 §12: a cost this backend cannot observe is not written as a number.
    // VK_KHR_pipeline_executable_properties is not enabled here, so the
    // pipeline's real binary size stays 0 rather than becoming an estimate.
    *created_binary_size = 0;
    return true;
  };

  if (!cache.acquire(key, trigger_reason, create, &report, error)) return false;
  const auto found = pipelines.find(hash);
  if (found == pipelines.end()) {
    if (error)
      *error = "pipeline cache reported a hit for a key whose VkPipeline this backend does not hold";
    return false;
  }
  *pipeline = found->second;
  if (cache_hit != nullptr) *cache_hit = report.cache_hit;
  if (binary_size != nullptr) *binary_size = report.binary_size;
  return true;
}

bool DeviceHal::transform_representation(const vg::core::Arena& arena,
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

const vg::hal::CapabilitySnapshot& DeviceHal::capabilities() const {
  return capabilities_;
}

// Multi-Node canonical compute is lowered below through one immutable package
// and backend-owned pipeline per full NodeRef, followed by one dispatch per
// Task in the assembler-sealed order. Pointer-graph Nodes remain deliberately
// Unsupported because the current BDA package only implements the linear IR
// subset; compile() owns that refusal and emits a Vulkan LoweringReport.
// Stage 5 (03 §7), compile-review-only (ADR-024): this backend now has a real
// facet/RepresentationEpoch lowering -- optimal-tiled VkImage creation,
// vkCmdCopyBufferToImage from the allocation's own BDA buffer, layout
// transitions reported separately from the transform (07 §7), and an
// adapter-side facet image cache keyed off the device-owned core::FacetPool
// (06 §2/§6.4 place that pool inside the adapter). None of it has ever run:
// no Vulkan hardware is reachable from this machine (permanent constraint,
// ADR-024), so this is code reviewed against the Vulkan contract, never
// execution evidence.
//
// What that changes for compile(): a plan carrying representation_requests is
// no longer rejected outright. It is accepted when this device can genuinely
// carry every request out -- Capability::RepresentationTransform set, the
// view's format usable for the requested facet kind under optimal tiling, and
// nothing in the plan that the transform would silently break -- and rejected
// with an Unsupported LoweringEvent naming the first request that fails
// otherwise. What it never does is drop a request and report a successful
// compile as if none had been asked for (START.md §4, invariant 10).
bool DeviceHal::can_lower_representation_requests(const vg::core::ExecutionPlan& plan,
                                                  std::string* reason) const {
  if (plan.representation_plan.empty()) return true;
#if !defined(VG_HAS_VULKAN)
  if (reason != nullptr)
    *reason = "Stage 5 representation transform is Unsupported: the Vulkan adapter is unavailable in "
              "this build, so no facet image can be created";
  return false;
#else
  if (!capabilities_.supports(vg::hal::Capability::RepresentationTransform)) {
    if (reason != nullptr)
      *reason = "Stage 5 representation transform is Unsupported on this device: the adapter did not "
                "claim Capability::RepresentationTransform, which requires synchronization2 for the "
                "layout barriers and an optimal-tiled image path for the target facet";
    return false;
  }
  for (size_t index = 0; index < plan.representation_plan.size(); ++index) {
    const auto& request = plan.representation_plan[index];
    const std::string label = "representation request " + std::to_string(index);
    const FormatSupport& support = format_support(request.view.format);
    if (!support.transfer_dst || !support.transfer_src) {
      if (reason != nullptr)
        *reason = label + " is Unsupported: this device's optimal tiling does not advertise transfer "
                          "for the view's format, so its linear backing cannot be copied into an image";
      return false;
    }
    // The target facet decides which format feature has to be present. A
    // missing one is reported rather than worked around by substituting a
    // format the caller did not ask for (06 §6.2).
    if (request.target_kind == vg::core::FacetKind::Sample && !support.sampled_image) {
      if (reason != nullptr)
        *reason = label + " is Unsupported: the view's format is not a sampled-image format on this device";
      return false;
    }
    if (request.target_kind == vg::core::FacetKind::Storage && !support.storage_image) {
      if (reason != nullptr)
        *reason = label + " is Unsupported: the view's format is not a storage-image format on this device";
      return false;
    }
    if (request.target_kind == vg::core::FacetKind::Attachment && !support.color_attachment) {
      if (reason != nullptr)
        *reason =
            label + " is Unsupported: the view's format is not a color-attachment format on this device";
      return false;
    }
    if (!request.view.swizzle.identity() && request.target_kind != vg::core::FacetKind::Sample) {
      if (reason != nullptr)
        *reason = label + " is Unsupported: a non-identity swizzle applies to a SampleFacet only, and a "
                          "Storage or Attachment target would silently ignore the channel mapping asked for";
      return false;
    }
    if (!request.consume_input) continue;
    // A ConsumeInput releases the allocation's linear backing at once
    // (core::Arena::consume_representation), and Stage 5 runs before this
    // submission's compute dispatch (03 §7). A plan that both consumes an
    // allocation's linear representation and then computes over that same
    // allocation is therefore asking for two incompatible things, and the
    // honest answer is to say so here rather than to dispatch over a buffer
    // whose bytes were just handed back.
    for (const auto& effects : plan.task_effects) for (const auto& effect : effects) {
      if (effect.allocation != request.view.allocation) continue;
      if (reason != nullptr)
        *reason = label + " is Unsupported: it asks for ConsumeInput on allocation " +
                  std::to_string(request.view.allocation) +
                  ", whose linear representation this plan's compute module also reads or writes; the "
                  "consume releases that backing before the dispatch could run";
      return false;
    }
  }
  return true;
#endif
}

bool DeviceHal::compile(const vg::core::ExecutionPlan& plan,
                        vg::hal::CompiledPlan* compiled,
                        std::string* error) {
  if (!compiled) { set_error(error, "compiled plan output is null"); return false; }
  *compiled = {};
  if (!plan.validate(error)) return false;
  // Keep Vulkan's advertised capabilities tied to command sequences this
  // adapter actually records. These checks deliberately precede the shared
  // Stage-6 preflight so discovery/working-set policy and multi-Node lowering
  // cannot inherit a promise merely because synchronization2 is
  // available.  In every case return a complete, Vulkan-owned Unsupported
  // report rather than accepting the plan and silently using the linear path.
  const auto reject_plan_requirement = [&](const char* operation, const std::string& message) {
    compiled->abi_version = vg::hal::kDeviceHalAbiVersion;
    compiled->plan = plan;
    compiled->report = {};
    compiled->report.backend = vg::hal::BackendKind::Vulkan;
    compiled->report.supported = false;
    compiled->report.diagnostic = message;
    compiled->report.add(operation, vg::hal::LoweringClass::Unsupported, 1, 0, message);
    set_error(error, message.c_str());
    return false;
  };
  for (const auto& task : plan.task_graph.tasks()) {
    if (task.kind == vg::core::TaskKind::Raster)
      return reject_plan_requirement(
          "raster_task", "Vulkan Unsupported: NodeRef{" + std::to_string(task.node_index) + "," +
              std::to_string(task.node_generation) + "} domain=Raster; complete plan rejected before Commit");
  }
  if (!plan.discovery_seeds.empty())
    return reject_plan_requirement("discovery",
                                   "discovery is Unsupported on Vulkan: no host-assisted reachable-set walk is "
                                   "implemented by this adapter");
  if (plan.working_set_budget.has_value() || plan.working_set_lease.has_value())
    return reject_plan_requirement("working_set_sparse",
                                   "working-set residency is Unsupported on Vulkan: sparse binding is not an "
                                   "automatic fault-managed working-set implementation");
  if (!vg::hal::preflight_stage6(plan, capabilities(), vg::hal::BackendKind::Vulkan, compiled, error)) return false;
  // F2 (ADR-046) wired TaskGraph-driven rasterization through compile()/
  // submit() for the reference and Metal backends only; this backend's own
  // raster machinery (ensure_raster_pipeline/run_raster_facet below) is
  // separate, pre-existing, and permanently compile-review-only (ADR-043
  // §7). Without this check a Raster-kind task would fall through this
  // file's compute-only task-graph publication path. The shared checked codec
  // would also refuse it, but Stage 6 retains this capability-specific
  // Unsupported diagnostic for every raster shape, including
  // F5 indexed draws (START.md §4, invariant 10: "任何无法在当前
  // 硬件表达的语义必须返回 Unsupported...不允许静默伪装").
  std::string representation_reason;
  if (!can_lower_representation_requests(plan, &representation_reason)) {
    compiled->abi_version = vg::hal::kDeviceHalAbiVersion;
    compiled->plan = plan;
    compiled->report = {};
    compiled->report.backend = vg::hal::BackendKind::Vulkan;
    compiled->report.supported = false;
    compiled->report.diagnostic = representation_reason;
    compiled->report.add("representation_transform", vg::hal::LoweringClass::Unsupported,
                         plan.representation_plan.size(), 0, representation_reason);
    set_error(error, compiled->report.diagnostic.c_str());
    return false;
  }
  compiled->abi_version = vg::hal::kDeviceHalAbiVersion;
  compiled->plan = plan;
  compiled->report = {};
  compiled->report.backend = vg::hal::BackendKind::Vulkan;
  compiled->per_node_packages.reserve(plan.resolved_nodes.size());
  for (const auto& node : plan.resolved_nodes) {
    if (!node.module.has_value())
      return reject_plan_requirement(
          "per_node_lowering",
          "Vulkan compute-only lowering requires a canonical module for NodeRef{" +
              std::to_string(node.ref.index) + "," + std::to_string(node.ref.generation) + "}");
    if (std::ranges::any_of(node.module->instructions, [](const auto& instruction) {
          return instruction.op == "load_ref" || instruction.op == "load_via" ||
                 instruction.op == "store_via";
        }))
      return reject_plan_requirement(
          "node_compute_package",
          "Vulkan pointer-graph Node lowering is Unsupported by the current linear BDA package for NodeRef{" +
              std::to_string(node.ref.index) + "," + std::to_string(node.ref.generation) + "}");
    const auto package = vg::compiler::build_linear_compute_package(*node.module);
    if (!package.ok)
      return reject_plan_requirement(
          "node_compute_package",
          "Vulkan per-Node package compilation failed for NodeRef{" +
              std::to_string(node.ref.index) + "," + std::to_string(node.ref.generation) + "}: " +
              package.message);
    compiled->per_node_packages.push_back(
        {node.ref, vg::hal::CompiledPlan::NodePackageKind::CanonicalCompute,
         package.package, false});
    compiled->report.add(
        "node_compute_package", vg::hal::LoweringClass::Direct, 1,
        package.package.bindings.size(),
        "full NodeRef{" + std::to_string(node.ref.index) + "," +
            std::to_string(node.ref.generation) + "}-keyed GLSL package generated by B4");
  }

  // Accepted Stage 5 work, described before it runs so a caller can see what
  // submit() has committed to (03 §7's stage 6 output is the LoweringReport,
  // not a promise made after the fact). Three separate events per request
  // because 07 §7 requires a layout transition and a representation transform
  // to be reported apart, and 02 §4.2/06 §11 make a ConsumeInput a distinct
  // decision from the transform that made it possible.
  if (!plan.representation_plan.empty()) {
    for (const auto& request : plan.representation_plan) {
      compiled->representation_operations.push_back({vg::hal::CompiledPlan::RepresentationOperation::CopyToImage,
                                                     request.transform_order, "Vulkan buffer-to-image copy"});
      compiled->report.add("representation_transform", vg::hal::LoweringClass::DevicePass, 1,
                           request.view.byte_size(),
                           "linear->optimal transfer pass: an optimal-tiled VkImage plus one "
                           "vkCmdCopyBufferToImage per subresource out of the allocation's existing "
                           "BDA buffer, publishing a new RepresentationEpoch (02 §8, 07 §13)");
      compiled->report.add("image_layout_transition", vg::hal::LoweringClass::Direct, 2, 0,
                           "two vkCmdPipelineBarrier2 image barriers (UNDEFINED->TRANSFER_DST, then "
                           "TRANSFER_DST->the target facet's read layout), reported apart from the "
                           "transform itself (07 §7)");
      if (request.consume_input) {
        compiled->report.add("consume_input", vg::hal::LoweringClass::Direct, 1, 0,
                             "the superseded linear backing is released at once instead of being "
                             "retained until command-buffer completion, and this backend additionally "
                             "destroys that allocation's VkBuffer (07 §14)");
      }
    }
  }

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
  for (size_t index = 0; index < compiled->per_node_packages.size(); ++index) {
    const auto& node = plan.resolved_nodes[index];
    const auto& package = *compiled->per_node_packages[index].package;
    const std::string cache_key = compute_pipeline_cache_key(node, package);
    const ComputePipelineRecord* pipeline = nullptr;
    bool cache_hit = false;
    std::string pipeline_error;
    if (!ensure_pipeline(cache_key, package.vulkan_glsl_source,
                         static_cast<uint32_t>(package.bindings.size()),
                         &pipeline, &cache_hit, &pipeline_error)) {
      compiled->report.supported = false;
      compiled->report.diagnostic =
          "Vulkan pipeline compilation failed for NodeRef{" +
          std::to_string(node.ref.index) + "," + std::to_string(node.ref.generation) + "}: " +
          pipeline_error;
      compiled->report.add("vulkan_pipeline", vg::hal::LoweringClass::Unsupported, 1, 0,
                           compiled->report.diagnostic);
      set_error(error, compiled->report.diagnostic.c_str());
      return false;
    }
    (void)pipeline;
    compiled->report.add(
        "vulkan_pipeline",
        cache_hit ? vg::hal::LoweringClass::CachedObject : vg::hal::LoweringClass::Direct,
        1, 0,
        "NodeRef{" + std::to_string(node.ref.index) + "," +
            std::to_string(node.ref.generation) + "} SPIR-V pipeline " +
            (cache_hit ? "reused from the backend-owned package cache"
                       : "compiled via glslc and cached by immutable package/entry identity"));
  }
  compiled->report.supported = true;
  if (!plan.task_graph.tasks().empty()) {
    const bool publication_pipeline_cache_hit = task_ring_pipeline_ != VK_NULL_HANDLE;
    std::string publication_pipeline_error;
    if (!ensure_task_ring_pipeline(&publication_pipeline_error)) {
      compiled->report.supported = false;
      compiled->report.diagnostic =
          "Vulkan task publication pipeline compilation failed: " + publication_pipeline_error;
      compiled->report.add("task_publication", vg::hal::LoweringClass::Unsupported, 1, 0,
                           compiled->report.diagnostic);
      set_error(error, compiled->report.diagnostic.c_str());
      return false;
    }
    compiled->report.add(
        "task_publication",
        publication_pipeline_cache_hit ? vg::hal::LoweringClass::CachedObject
                                       : vg::hal::LoweringClass::Direct,
        1, 0,
        "Vulkan task ring GPU publication pass; canonical Nodes execute separately per Task");
  }
  lower_wave_transitions(compiled);
  if (plan.timeline_wait != 0 || plan.timeline_signal != 0) {
    compiled->report.add("timeline", vg::hal::LoweringClass::Direct, 1, 0,
                         "VkSemaphore(TIMELINE) wait/signal surrounds the canonical task-graph submission");
  }
  return true;
#endif
}

bool DeviceHal::submit(const vg::hal::CompiledPlan& compiled, vg::core::Arena& arena,
                       vg::hal::Submission* submission, std::string* error) {
  if (!vg::hal::validate_stage7_compiled_plan(compiled, vg::hal::BackendKind::Vulkan, error)) return false;
  if (!submission) { set_error(error, "submission output is null"); return false; }
  if (!compiled.report.supported) { set_error(error, "compiled plan is unsupported"); return false; }
  if (!compiled.plan.validate(error)) return false;
  for (const auto& transition : compiled.transition_operations) {
    const uint64_t expected_barriers = transition.covers_execution_completion ? 1 : 0;
    if (transition.state != vg::hal::CompiledPlan::TransitionLoweringState::Lowered ||
        transition.barrier_count != expected_barriers ||
        transition.serialized_fallback != transition.covers_execution_completion ||
        transition.fence_count != 0 || transition.encoder_boundary_count != 0 ||
        transition.host_wait_count != 0) {
      set_error(error, "Vulkan compiled wave transition does not match its physical lowering");
      return false;
    }
  }
  if (compiled.per_node_packages.size() != compiled.plan.resolved_nodes.size()) {
    set_error(error, "compiled plan must contain exactly one package for every immutable NodeRef");
    return false;
  }
  for (const auto& node : compiled.plan.resolved_nodes) {
    if (!node.module.has_value()) {
      set_error(error, "compiled Vulkan plan contains a non-compute Node snapshot");
      return false;
    }
    const auto package_it = std::ranges::find_if(compiled.per_node_packages, [&](const auto& candidate) {
      return node_ref_equal(candidate.ref, node.ref);
    });
    if (package_it == compiled.per_node_packages.end() ||
        std::ranges::count_if(compiled.per_node_packages, [&](const auto& candidate) {
          return node_ref_equal(candidate.ref, node.ref);
        }) != 1) {
      set_error(error, "compiled plan has a missing or duplicate immutable NodeRef package");
      return false;
    }
    if (package_it->kind != vg::hal::CompiledPlan::NodePackageKind::CanonicalCompute ||
        !package_it->package.has_value()) {
      set_error(error, "compiled Vulkan NodeRef package has the wrong domain kind");
      return false;
    }
    const auto expected = vg::compiler::build_linear_compute_package(*node.module);
    if (!expected.ok || !same_vulkan_compute_package(*package_it->package, expected.package)) {
      set_error(error, "compiled NodeRef package contents disagree with the immutable module snapshot");
      return false;
    }
  }
  if (!compiled.plan.graph_epoch_matches(arena, error)) return false;

  *submission = {};
  submission->abi_version = vg::hal::kDeviceHalAbiVersion;
  submission->report = compiled.report;
  // Compilation costs remain historical facts. Planned execution costs are
  // replaced below only after the corresponding commands were really encoded.
  std::erase_if(submission->report.events, [](const auto& event) {
    return event.operation == "task_effect_barrier" || event.operation == "representation_transform" ||
           event.operation == "image_layout_transition" || event.operation == "consume_input" ||
           event.operation == "timeline";
  });
  submission->report.transition_barrier_count = 0;
  submission->report.transition_serialized_fallback_count = 0;
  if (!vg::hal::run_discovery_stage(compiled.plan, arena, submission, error)) return false;
  if (!vg::hal::apply_working_set_budget(compiled.plan, arena, submission, error)) return false;

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

  // Validate and consume the shared continuation exactly once, before any
  // representation commit, GPU execution, output writeback, or Timeline signal.
  // The resulting order filters publication only; all scheduled Tasks still run.
  std::vector<uint32_t> publish_order;
  if (!vg::hal::apply_envelope_continuation(compiled.plan, &envelope_continuations(),
                                            submission, &publish_order, error))
    return false;

  vg::hal::SubmissionLifetimeHold lifetime_hold;
  if (!lifetime_hold.prepare(compiled.plan, arena, facet_pool(), error)) return false;

  // Stage 5 runs before Stage 6/7's dispatch (03 §7), not alongside it. That
  // ordering is load-bearing for more than tidiness: a transform publishes a
  // new RepresentationEpoch, and the binding loop below resolves through
  // core::Arena::lookup(id, generation, epoch), which demands an exact epoch
  // match. A plan that transforms an allocation and then computes over it must
  // therefore author its instructions at the post-transform epoch -- the
  // caller's contract, not something this backend may quietly patch up.
  // compile() already refused the one combination that cannot be authored at
  // all (a ConsumeInput of an allocation the same module reads or writes).
  RepresentationStageCounts representation_counts{};
  if (!compiled.plan.representation_plan.empty()) {
    std::string representation_error;
    // The shared helper owns the parts that are core's, not the backend's:
    // acquiring the facet out of the device-owned pool, calling
    // core::Arena::transform_representation, deciding whether a ConsumeInput
    // is admissible and calling consume_representation, retiring stale facets,
    // and writing the RepresentationEvent into the submission (02 §4.2, 06
    // §11). This backend supplies only the physical step below, which is why
    // ConsumeInput is never inferred here.
    const bool representation_committed = vg::hal::commit_representation_operations(
            compiled.plan, compiled.representation_operations, arena, facet_pool(),
            [&](const vg::core::RepresentationSemanticPlanItem& request, const vg::hal::CompiledPlan::PhysicalRepresentationOperation&, vg::core::FacetRef facet,
                vg::hal::RepresentationTransformCost* cost, std::string* physical_error) {
              return transform_representation(arena, request, facet, cost, &representation_counts,
                                              physical_error);
            },
            submission, &representation_error);
    submission->report.barrier_count = representation_counts.barrier_count;
    submission->report.command_buffer_count = representation_counts.command_buffer_count;
    submission->report.encoder_count = representation_counts.command_buffer_count;
    submission->report.queue_wait_count = representation_counts.queue_wait_count;
    if (!representation_committed) {
      // A physical failure mid-stage is a hard submit() failure rather than a
      // poisoned result: the transform either happened or it did not, and the
      // helper has already recorded an Unsupported event describing which
      // request stopped.
      set_error(error, representation_error.c_str());
      return false;
    }
    if (representation_counts.barrier_count != 0) {
      submission->report.add(
          "image_layout_transition", vg::hal::LoweringClass::Direct, representation_counts.barrier_count, 0,
          "vkCmdPipelineBarrier2 image barriers issued by the transfer passes above "
          "(UNDEFINED->TRANSFER_DST, then TRANSFER_DST->the target facet's read layout), reported apart "
          "from the representation transforms themselves (07 §7)");
    }
    // Only facets the pool has already retired are touched here: the images
    // are keyed by FacetRef generation and RepresentationEpoch, so a superseded
    // epoch's VkImage/VkImageView can be destroyed without disturbing the
    // allocation's current backing (07 §14). The new image is that backing now,
    // so nothing in this pass destroys it.
    const uint32_t retired_images = retire_stale_facet_images(arena, facet_pool());
    if (retired_images != 0) {
      submission->report.add("facet_image_retire", vg::hal::LoweringClass::Direct, retired_images, 0,
                             "VkImage/VkImageView/VkDeviceMemory belonging to retired facet slots or "
                             "superseded RepresentationEpochs destroyed after the stage");
    }
    // ConsumeInput follow-through. Whether a consume happened is core's
    // decision, taken inside the helper (which refuses one whose transform
    // produced no distinct backing) -- this backend does not infer it and does
    // not re-derive it from the request. What it does is observe the outcome:
    // core::Arena::consume_representation() clears the allocation's bytes, so
    // an emptied allocation whose device mirror is still sized is exactly the
    // case where this backend is holding onto a superseded linear
    // representation. Leaving that VkBuffer alive would mean the peak-memory
    // saving E005 measures never materializes on the device side at all. Safe
    // here only because the stage's command buffers were waited on above.
    if (submission->consumed_allocation_count != 0) {
      for (const auto& request : compiled.plan.representation_plan) {
        if (!request.consume_input) continue;
        const vg::core::Allocation* allocation =
            arena.lookup(core::PointerRef{request.view.allocation, request.view.allocation_generation});
        if (allocation == nullptr || !allocation->bytes.empty()) continue;
        const auto it = allocation_map_.find(request.view.allocation);
        if (it == allocation_map_.end() || it->second.byte_size == 0) continue;
        const uint64_t released = static_cast<uint64_t>(it->second.byte_size);
        if (it->second.mapped != nullptr) vkUnmapMemory(device_, it->second.memory);
        if (it->second.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, it->second.buffer, nullptr);
        if (it->second.memory != VK_NULL_HANDLE) vkFreeMemory(device_, it->second.memory, nullptr);
        allocation_map_.erase(it);
        submission->report.add("consume_input_backing_release", vg::hal::LoweringClass::Direct, 1, released,
                               "the superseded linear representation's device buffer was destroyed at once "
                               "rather than retained to command-buffer completion (07 §14, E005)");
      }
    }
  }

  if (!lifetime_hold.acquire(submission->representation_facets, error)) return false;

  const auto& tasks = compiled.plan.task_graph.tasks();
  std::vector<CanonicalTaskDispatch> task_dispatches;
  task_dispatches.reserve(tasks.size());
  const auto& schedule = compiled.plan.execution_schedule;
  for (uint32_t component_index = 0; component_index < schedule.components.size(); ++component_index) {
    const auto& component = schedule.components[component_index];
    for (uint32_t wave_index = 0; wave_index < component.waves.size(); ++wave_index) {
      const auto& wave = component.waves[wave_index];
      bool first_in_wave = true;
      for (uint32_t task_index : wave.tasks) {
        CanonicalTaskDispatch dispatch;
        dispatch.task_index = task_index;
        if (first_in_wave)
          for (const auto& transition : compiled.transition_operations)
            if (transition.component == component_index && transition.after_wave == wave_index &&
                transition.barrier_count != 0)
              dispatch.transitions_before.push_back(transition.semantic_transition);
        task_dispatches.push_back(std::move(dispatch));
        first_in_wave = false;
      }
    }
  }
  std::vector<vg::core::Allocation*> touched;
  for (auto& dispatch : task_dispatches) {
    const uint32_t task_index = dispatch.task_index;
    const auto& task = tasks[task_index];
    const vg::core::NodeTable::Ref task_ref{task.node_index, task.node_generation};
    const auto node_it = std::ranges::find_if(compiled.plan.resolved_nodes, [&](const auto& node) {
      return node_ref_equal(node.ref, task_ref);
    });
    const auto package_it = std::ranges::find_if(compiled.per_node_packages, [&](const auto& package) {
      return node_ref_equal(package.ref, task_ref);
    });
    if (node_it == compiled.plan.resolved_nodes.end() ||
        package_it == compiled.per_node_packages.end() ||
        !node_it->module.has_value() || !package_it->package.has_value()) {
      set_error(error, "sealed Task references an unavailable immutable NodeRef package");
      return false;
    }
    const auto& module = *node_it->module;
    const auto& package = *package_it->package;
    const std::string cache_key = compute_pipeline_cache_key(*node_it, package);
    const auto pipeline_it = compute_pipeline_cache_.find(cache_key);
    if (pipeline_it == compute_pipeline_cache_.end() ||
        pipeline_it->second.binding_count != package.bindings.size()) {
      set_error(error, "Stage 7 cannot find the Stage-6 Vulkan pipeline for a sealed NodeRef package");
      return false;
    }

    std::map<uint64_t, std::pair<uint32_t, uint32_t>> generation_by_allocation;
    for (const auto& instruction : module.instructions)
      generation_by_allocation.emplace(
          instruction.allocation,
          std::make_pair(instruction.generation, instruction.representation_epoch));

    dispatch.x = task.x;
    dispatch.y = task.y;
    dispatch.z = task.z;
    dispatch.pipeline = &pipeline_it->second;
    dispatch.addresses.reserve(package.bindings.size());
    for (const auto& binding : package.bindings) {
      const auto generation = generation_by_allocation.find(binding.allocation);
      vg::core::Allocation* allocation = generation == generation_by_allocation.end()
          ? nullptr
          : arena.lookup(core::RepresentationRef{binding.allocation,
                                                 generation->second.first,
                                                 generation->second.second});
      if (allocation == nullptr) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::Poisoned;
        submission->result.message =
            "stale generation, representation epoch, or out-of-bounds allocation reference";
        submission->result.fault.task_index = task_index;
        submission->result.fault.code = "STALE_ALLOCATION";
        submission->result.fault.message = submission->result.message;
        return true;
      }
      AllocationRecord* record = nullptr;
      std::string buffer_error;
      if (!ensure_buffer(*allocation, &record, &buffer_error)) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::Poisoned;
        submission->result.message = "Vulkan buffer allocation failed: " + buffer_error;
        submission->result.fault.task_index = task_index;
        submission->result.fault.code = "BACKEND_ALLOCATION_FAILED";
        submission->result.fault.message = submission->result.message;
        return true;
      }
      dispatch.addresses.push_back(record->device_address);
      if (std::ranges::find(touched, allocation) == touched.end()) touched.push_back(allocation);
    }

  }

  std::string dispatch_error;
  const auto dispatch_start = std::chrono::steady_clock::now();
  TaskDispatchCounts dispatch_counts;
  const bool dispatched = dispatch_task_graph(task_dispatches, wait_value, signal_value,
                                               &dispatch_counts, &dispatch_error);
  submission->cpu_submit_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dispatch_start).count();
  for (size_t i = 0; i < dispatch_counts.dispatch_count; ++i) {
    const auto& dispatch = task_dispatches[i];
    const auto& task = tasks[dispatch.task_index];
    submission->report.add(
        "vulkan_task_dispatch", vg::hal::LoweringClass::Direct, 1, 0,
        "task=" + std::to_string(dispatch.task_index) + " NodeRef{" +
            std::to_string(task.node_index) + "," + std::to_string(task.node_generation) +
            "} groups=" + std::to_string(dispatch.x) + "x" +
            std::to_string(dispatch.y) + "x" + std::to_string(dispatch.z));
  }
  submission->report.command_buffer_count = representation_counts.command_buffer_count + dispatch_counts.command_buffer_count;
  submission->report.encoder_count = representation_counts.command_buffer_count + dispatch_counts.command_buffer_count;
  submission->report.barrier_count =
      representation_counts.barrier_count + dispatch_counts.barrier_count;
  submission->report.queue_wait_count = representation_counts.queue_wait_count + dispatch_counts.queue_wait_count;
  for (uint32_t transition : dispatch_counts.encoded_transitions) {
    ++submission->report.transition_barrier_count;
    ++submission->report.transition_serialized_fallback_count;
    submission->report.add("task_effect_barrier", vg::hal::LoweringClass::Serialized, 1, 0,
                           "encoded sealed wave transition=" + std::to_string(transition) +
                           "; conservative global compute visibility via vkCmdPipelineBarrier2");
  }
  if ((wait_value != 0 || signal_value != 0) && dispatch_counts.queue_wait_count != 0)
    submission->report.add("timeline", vg::hal::LoweringClass::Direct, 1, 0,
                           "submitted Vulkan timeline semaphore wait/signal");
  if (!dispatched) {
    submission->result.ok = false;
    submission->result.outputs_valid = false;
    submission->result.poison = dispatch_counts.queue_wait_count != 0
        ? vg::core::PoisonState::PartiallyProduced : vg::core::PoisonState::Poisoned;
    submission->result.message = "Vulkan dispatch failed: " + dispatch_error;
    submission->result.fault.code = "BACKEND_DISPATCH_FAILED";
    submission->result.fault.message = submission->result.message;
    return true;
  }
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

  uint32_t witness_index = 0;
  for (uint32_t task_index : compiled.plan.execution_schedule.task_order)
    for (const auto& effect : compiled.plan.task_effects[task_index]) {
      submission->result.trace.push_back(effect);
      submission->result.witness.record(effect, witness_index++);
    }
  submission->result.ok = true;
  submission->result.outputs_valid = true;
  submission->result.poison = vg::core::PoisonState::Valid;

  if (!compiled.plan.task_graph.tasks().empty()) {
    // TASK-D5 / ADR-039 (compile-review-only): envelope continuation is a
    // host split of the assembler-sealed canonical schedule (HostAssisted). This
    // file does not implement overflow-buffer / next-submit, and must not
    // pretend a DelegatedEnvelope or firmware enlarge exists. Envelope
    // refusal uses "envelope task quota exceeded" / leftover deferred --
    // never the publication-ring string "publication ring quota overflow".
    // Pack -> dispatch the GPU publication kernel -> read back -> verify every
    // slot reached Published -> unpack, walking
    // slots in that sealed dependency/effect order so
    // submission->published_tasks is sequence-identical to
    // reference::execute_task_graph()'s oracle output (same ordering
    // convention as Metal's submit()).
    const auto& tasks = compiled.plan.task_graph.tasks();
    const uint32_t count = static_cast<uint32_t>(tasks.size());

    std::string task_pipeline_error;
    if (!ensure_task_ring_pipeline(&task_pipeline_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::PartiallyProduced;
      submission->result.message = "Vulkan task ring pipeline compile failed: " + task_pipeline_error;
      submission->result.fault.code = "TASK_PUBLICATION_FAILED";
      submission->result.fault.message = submission->result.message;
      return true;
    }

    TaskRingBuffers ring_buffers{};
    std::string ring_error;
    if (!create_task_ring_buffers(count, &ring_buffers, &ring_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::PartiallyProduced;
      submission->result.message = "Vulkan task ring buffer allocation failed: " + ring_error;
      submission->result.fault.code = "TASK_PUBLICATION_FAILED";
      submission->result.fault.message = submission->result.message;
      return true;
    }
    uint32_t* inputs = static_cast<uint32_t*>(ring_buffers.inputs_mapped);
    for (uint32_t i = 0; i < count; ++i) {
      vg::compiler::ComputeTaskRingRecord record;
      std::string codec_error;
      if (!vg::compiler::make_compute_task_ring_record(tasks[i], &record, &codec_error) ||
          !vg::compiler::pack_compute_task_ring_record(
              record,
              std::span<uint32_t>(inputs + i * vg::compiler::kTaskRingWordsPerRecord,
                                  vg::compiler::kTaskRingWordsPerRecord),
              &codec_error)) {
        submission->result.ok = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message = "Vulkan compute Task ring encode failed: " + codec_error;
        submission->result.outputs_valid = false;
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        destroy_task_ring_buffers(&ring_buffers);
        return true;
      }
    }

    std::string publication_error;
    const auto publication_start = std::chrono::steady_clock::now();
    TaskDispatchCounts publication_counts;
    const bool published = dispatch_task_ring_publication(ring_buffers, &publication_counts, &publication_error);
    submission->report.command_buffer_count += publication_counts.command_buffer_count;
    submission->report.encoder_count += publication_counts.command_buffer_count;
    submission->report.barrier_count += publication_counts.barrier_count;
    submission->report.queue_wait_count += publication_counts.queue_wait_count;
    if (publication_counts.dispatch_count != 0)
      submission->report.add("task_publication_dispatch", vg::hal::LoweringClass::Direct,
                             publication_counts.dispatch_count, 0,
                             "compute-only GPU ring published " + std::to_string(count) +
                             " records; Envelope-filtered canonical observation has " +
                             std::to_string(publish_order.size()) + " Tasks");
    if (publication_counts.barrier_count != 0)
      submission->report.add(
          "task_publication_barrier", vg::hal::LoweringClass::Direct, publication_counts.barrier_count, 0,
          "vkCmdPipelineBarrier2 makes the publication shader writes visible to host readback");
    if (!published) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::PartiallyProduced;
      submission->result.message = "Vulkan task ring publication failed: " + publication_error;
      submission->result.fault.code = "TASK_PUBLICATION_FAILED";
      submission->result.fault.message = submission->result.message;
      destroy_task_ring_buffers(&ring_buffers);
      return true;
    }
    submission->cpu_submit_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - publication_start).count();

    const uint32_t* states = static_cast<const uint32_t*>(ring_buffers.state_mapped);
    const uint32_t* fields = static_cast<const uint32_t*>(ring_buffers.fields_mapped);
    submission->published_tasks.reserve(count);
    for (uint32_t index : publish_order) {
      if (states[index] != static_cast<uint32_t>(vg::core::PublicationState::Published)) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message = "task ring slot did not reach Published state";
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        destroy_task_ring_buffers(&ring_buffers);
        return true;
      }
      vg::compiler::ComputeTaskRingRecord record;
      std::string codec_error;
      if (!vg::compiler::unpack_compute_task_ring_record(
              std::span<const uint32_t>(fields + index * vg::compiler::kTaskRingWordsPerRecord,
                                        vg::compiler::kTaskRingWordsPerRecord),
              &record, &codec_error)) {
        submission->result.ok = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message = "Vulkan compute Task ring decode failed: " + codec_error;
        submission->result.outputs_valid = false;
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        destroy_task_ring_buffers(&ring_buffers);
        return true;
      }
      const auto* expected_words = inputs + index * vg::compiler::kTaskRingWordsPerRecord;
      const auto* published_words = fields + index * vg::compiler::kTaskRingWordsPerRecord;
      if (!std::equal(expected_words, expected_words + vg::compiler::kTaskRingWordsPerRecord, published_words)) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message = "Vulkan publication record disagrees with sealed Task";
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        destroy_task_ring_buffers(&ring_buffers);
        return true;
      }
      // The ring verifies physical publication; only Core owns Task identity.
      submission->published_tasks.push_back(tasks[index]);
    }
    destroy_task_ring_buffers(&ring_buffers);
  }
  return true;
#endif
}

// --- Phase C facet entry points -------------------------------------------
// Compile-review-only, every one of them (ADR-024): no Vulkan hardware is
// reachable from this machine, so the vkCmd* sequences below have been written
// and read against the Vulkan contract and the 07 rules they cite, and have
// never executed. Each reports what it recorded, not what it wishes it did --
// a count here is a count of commands actually put into a command buffer.

bool DeviceHal::run_sample_facet(const vg::core::Arena& arena, vg::core::FacetPool& pool,
                                 vg::core::FacetRef ref, vg::core::FilterMode filter, vg::core::WrapMode wrap,
                                 const std::vector<std::array<float, 2>>& uv_coords, float lod,
                                 const std::vector<uint32_t>& array_slices,
                                 vg::core::ValidationProfile profile, SampleFacetResult* result,
                                 std::string* error) {
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
  if (checked && !capabilities_.supports(vg::hal::Capability::CheckedFacetGeneration))
    return reject("Unsupported: this device did not claim Capability::CheckedFacetGeneration, so the "
                  "in-shader generation guard of 06 §6.4 cannot be honored and a checked dispatch must "
                  "not be answered with an unchecked one");

  const vg::core::FacetSlot* slot = resolve_facet(arena, pool, ref, vg::core::FacetKind::Sample, error);
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
  if (!ensure_facet_image(arena, pool, ref, vg::core::FacetKind::Sample, VK_NULL_HANDLE,
                          /*upload_source_offset=*/0, &image, &cache_hit, &staging_bytes, error))
    return false;
  result->facet_cache_hit = cache_hit;

  VkSampler sampler = VK_NULL_HANDLE;
  if (!ensure_sampler(filter, wrap, &sampler, error)) return false;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  if (!ensure_sample_facet_pipeline(array_kernel, checked, &pipeline, &pipeline_layout, &set_layout, error))
    return false;
  if (!ensure_descriptor_pool(error)) return false;

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
    destroy_raw_buffer(device_, &uv_buffer);
    destroy_raw_buffer(device_, &output_buffer);
    destroy_raw_buffer(device_, &lod_buffer);
    destroy_raw_buffer(device_, &slice_buffer);
    destroy_raw_buffer(device_, &token_buffer);
    destroy_raw_buffer(device_, &table_buffer);
    destroy_raw_buffer(device_, &slot_count_buffer);
    destroy_raw_buffer(device_, &violation_buffer);
  };
  const auto create = [&](VkDeviceSize size, VkBufferUsageFlags usage, RawBuffer* out) {
    if (create_raw_buffer(device_, physical_device_, size, usage, /*want_address=*/false, /*want_map=*/true,
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
  vkResetDescriptorPool(device_, descriptor_pool_, 0);
  VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_alloc.descriptorPool = descriptor_pool_;
  set_alloc.descriptorSetCount = 1;
  set_alloc.pSetLayouts = &set_layout;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device_, &set_alloc, &descriptor_set) != VK_SUCCESS) {
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
  vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  result->descriptors.set_allocation_count = 1;
  result->descriptors.descriptor_write_count = static_cast<uint32_t>(writes.size());
  result->descriptors.descriptor_write_bytes = descriptor_bytes;
  result->descriptors.cpu_descriptor_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                           descriptor_start)
          .count());
  result->descriptors.used_descriptor_buffer = false;

  if (!ensure_command_pool(device_, compute_queue_family_, &command_pool_, error)) {
    destroy_all();
    return false;
  }
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  if (!allocate_command_buffer(device_, command_pool_, &command_buffer, error)) {
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
      record_layout_transition(command_buffer, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                          &descriptor_set, 0, nullptr);
  vkCmdDispatch(command_buffer, invocations, 1, 1);
  vkEndCommandBuffer(command_buffer);
  if (!submit_and_wait_simple(device_, compute_queue_, command_pool_, command_buffer, error)) {
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
  result->report.barrier_count = transitioned ? 1 : 0;
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

bool DeviceHal::run_storage_facet(const vg::core::Arena& arena, vg::core::FacetPool& pool,
                                  vg::core::FacetRef ref, StorageFacetTarget target,
                                  const std::array<float, 4>& write_rgba, StorageFacetResult* result,
                                  std::string* error) {
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

  const vg::core::FacetSlot* slot = resolve_facet(arena, pool, ref, vg::core::FacetKind::Storage, error);
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
    if (!ensure_buffer(*allocation, &record, error)) return false;
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

  if (!format_support(view.format).storage_image)
    return reject("Unsupported: the view's format does not advertise "
                  "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT under optimal tiling on this device, and the "
                  "view's format is not rewritten to make the write legal (06 §6.2)");

  bool cache_hit = false;
  uint64_t staging_bytes = 0;
  VulkanFacetRecord* image = nullptr;
  if (!ensure_facet_image(arena, pool, ref, vg::core::FacetKind::Storage, VK_NULL_HANDLE,
                          /*upload_source_offset=*/0, &image, &cache_hit, &staging_bytes, error))
    return false;
  result->facet_cache_hit = cache_hit;

  VkPipeline pipeline = VK_NULL_HANDLE;
  if (!ensure_storage_facet_pipeline(format, &pipeline, error)) return false;
  if (!ensure_descriptor_pool(error)) return false;

  RawBuffer value_buffer{};
  RawBuffer readback{};
  const auto destroy_all = [&]() {
    destroy_raw_buffer(device_, &value_buffer);
    destroy_raw_buffer(device_, &readback);
  };
  if (!create_raw_buffer(device_, physical_device_, 4 * sizeof(float), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         /*want_address=*/false, /*want_map=*/true, &value_buffer, error))
    return false;
  // The write is read back out of the image itself rather than echoed from the
  // value that was sent down: a storage write whose result is reported from the
  // input would report a success the device never produced.
  if (!create_raw_buffer(device_, physical_device_, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         /*want_address=*/false, /*want_map=*/true, &readback, error)) {
    destroy_all();
    return false;
  }
  std::memcpy(value_buffer.mapped, write_rgba.data(), 4 * sizeof(float));

  const auto descriptor_start = std::chrono::steady_clock::now();
  vkResetDescriptorPool(device_, descriptor_pool_, 0);
  VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_alloc.descriptorPool = descriptor_pool_;
  set_alloc.descriptorSetCount = 1;
  set_alloc.pSetLayouts = &storage_set_layout_;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device_, &set_alloc, &descriptor_set) != VK_SUCCESS) {
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
  vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
  result->descriptors.set_allocation_count = 1;
  result->descriptors.descriptor_write_count = 2;
  result->descriptors.descriptor_write_bytes = sizeof(VkDescriptorImageInfo) + sizeof(VkDescriptorBufferInfo);
  result->descriptors.cpu_descriptor_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                           descriptor_start)
          .count());

  if (!ensure_command_pool(device_, compute_queue_family_, &command_pool_, error)) {
    destroy_all();
    return false;
  }
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  if (!allocate_command_buffer(device_, command_pool_, &command_buffer, error)) {
    destroy_all();
    return false;
  }
  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command_buffer, &begin_info);
  uint64_t barriers = 0;
  if (record_layout_transition(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL)) ++barriers;
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, storage_pipeline_layout_, 0, 1,
                          &descriptor_set, 0, nullptr);
  vkCmdDispatch(command_buffer, 1, 1, 1);
  if (record_layout_transition(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) ++barriers;
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
  vkEndCommandBuffer(command_buffer);
  if (!submit_and_wait_simple(device_, compute_queue_, command_pool_, command_buffer, error)) {
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

bool DeviceHal::run_raster_facet(const vg::core::Arena& arena, vg::core::FacetPool& pool,
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

bool DeviceHal::run_pipeline_classification(const std::vector<RasterPipelineVariant>& variants,
                                            PipelineClassificationResult* result, std::string* error) {
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
  if (!capabilities_.supports(vg::hal::Capability::Raster))
    return reject("Unsupported: this device did not claim Capability::Raster, so no graphics pipeline "
                  "can be created to count");

  // Both arms start empty so the counts describe this call and nothing else.
  // The VkPipelines themselves are left in raster_pipelines_/naive_raster_
  // pipelines_ for the destructor; ensure_raster_pipeline destroys any it would
  // otherwise shadow, so clearing the measurement caches cannot orphan one.
  pipeline_cache_.clear();
  naive_pipeline_cache_.clear();

  const std::string code_object_hash = vg::ir::sha256_hex(vg::compiler::raster_facet_vulkan_source());
  for (size_t index = 0; index < variants.size(); ++index) {
    const RasterPipelineVariant& variant = variants[index];
    const VkFormat format = to_vk_format(variant.attachment_format);
    if (!format_support(variant.attachment_format).color_attachment)
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
    base.target_identity = target_identity_;

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
    if (!ensure_raster_pipeline(naive_pipeline_cache_, naive_raster_pipelines_, naive_key,
                               "E013 naive full permutation", format, variant.sample_count,
                               classification.key.raster_state, &pipeline, &cache_hit, &binary_size, error))
      return false;
    result->naive_compile_ns += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - naive_start)
            .count());

    const auto classified_start = std::chrono::steady_clock::now();
    if (!ensure_raster_pipeline(pipeline_cache_, raster_pipelines_, classification.key,
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

  result->naive_pipeline_count = naive_pipeline_cache_.pipeline_count();
  result->classified_pipeline_count = pipeline_cache_.pipeline_count();
  result->naive_cache_hits = naive_pipeline_cache_.cache_hits();
  result->classified_cache_hits = pipeline_cache_.cache_hits();
  result->classified_specializations = pipeline_cache_.reports();
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

std::unique_ptr<DeviceHal> DeviceHal::create_impl(const uint8_t* uuid, std::string* error) {
  auto adapter = std::unique_ptr<DeviceHal>(new DeviceHal());
#if !defined(VG_HAS_VULKAN)
  (void)uuid;
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
  if (uuid == nullptr) {
    adapter->physical_device_ = devices.front();
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
        adapter->physical_device_ = candidate;
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
  adapter->compute_queue_family_ = graphics_capable ? graphics_and_compute_family : compute_only_family;
  if (adapter->compute_queue_family_ == UINT32_MAX) {
    set_error(error, "Vulkan device has no compute queue family");
    return nullptr;
  }
  // A queue family that cannot do transfers cannot carry Stage 5's
  // vkCmdCopyBufferToImage. In practice VK_QUEUE_COMPUTE_BIT implies transfer
  // support, but the bit is read rather than assumed, because the whole point
  // of the capability probe is that this backend claims only what the device
  // actually reported (07 §1).
  const bool transfer_capable =
      (queues[adapter->compute_queue_family_].queueFlags &
       (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;

  // 07 §6's facet lowering rests on what the device says each format can do
  // under VK_IMAGE_TILING_OPTIMAL, so it is queried once here and never
  // guessed at a use site. A format missing a feature makes the corresponding
  // request Unsupported later; it never causes a substitute format to be
  // chosen quietly (06 §6.2).
  const auto probe_format = [&](VkFormat format) {
    VkFormatProperties properties_for_format{};
    vkGetPhysicalDeviceFormatProperties(adapter->physical_device_, format, &properties_for_format);
    const VkFormatFeatureFlags features_for_format = properties_for_format.optimalTilingFeatures;
    DeviceHal::FormatSupport support{};
    support.sampled_image = (features_for_format & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    support.storage_image = (features_for_format & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    support.color_attachment = (features_for_format & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    support.transfer_dst = (features_for_format & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0;
    support.transfer_src = (features_for_format & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
    return support;
  };
  adapter->rgba8_support_ = probe_format(VK_FORMAT_R8G8B8A8_UNORM);
  adapter->r32f_support_ = probe_format(VK_FORMAT_R32_SFLOAT);
  adapter->framebuffer_color_sample_counts_ = properties.limits.framebufferColorSampleCounts;

  // 05 §10: a backend pipeline binary cache is explicitly non-portable, so
  // every PipelineKey this adapter builds carries this device's identity and no
  // key can look reusable across drivers.
  adapter->target_identity_ = std::string("vulkan|") + properties.deviceName + "|api" +
                              std::to_string(properties.apiVersion) + "|driver" +
                              std::to_string(properties.driverVersion);

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
  // Dynamic rendering is how 07 §9 says an AttachmentFacet pass is lowered
  // here: no VkRenderPass, no VkFramebuffer, attachment format and sample count
  // compiled into the pipeline instead. Like sync2 it is only claimable when
  // the device itself reports core 1.3, since that is what makes chaining
  // VkPhysicalDeviceVulkan13Features into vkCreateDevice legal.
  adapter->supports_dynamic_rendering_ = device_supports_1_3 && features13.dynamicRendering == VK_TRUE;
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
  if (bda) adapter->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::LinearAddress);
  if (bda) adapter->capabilities_.address_width = 64;
  if (timeline) adapter->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::Timeline);
  if (bda && sync2 && spirv_compiler_available) {
    // Task publication uses a BDA-addressed shader and a sync2 shader-write to
    // host-read dependency. Canonical programs execute separately per Task;
    // this capability does not imply indirect execution.
    adapter->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::TaskPublication);
    // Multi-Task plans consume the assembler-sealed component/wave schedule and emit sync2
    // compute memory dependencies at every conflicting effect boundary.
    adapter->capabilities_.capability_bits |= static_cast<uint64_t>(vg::hal::Capability::EffectDag);
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
                                        adapter->rgba8_support_.transfer_dst &&
                                        adapter->rgba8_support_.transfer_src;
  if (representation_transform)
    adapter->capabilities_.capability_bits |=
        static_cast<uint64_t>(vg::hal::Capability::RepresentationTransform);
  // Do not advertise Raster.  The standalone facet helper has reviewed
  // vkCmdBeginRendering code, but ExecutionPlan Raster tasks are explicitly
  // rejected above and therefore the DeviceHal capability contract is not
  // fulfilled.  A standalone helper must not upgrade an unsupported plan.
  // CheckedFacetGeneration: the in-shader guard of 06 §6.4, which exists here
  // only because sample_facet_vulkan_source() is specialized with constant_id 0
  // = true and the token/generation-table/slot-count/violation bindings are
  // written. Both halves need a SPIR-V compiler and a genuinely sampleable
  // format, so both are required before the promise is made.
  if (spirv_compiler_available && adapter->rgba8_support_.sampled_image)
    adapter->capabilities_.capability_bits |=
        static_cast<uint64_t>(vg::hal::Capability::CheckedFacetGeneration);
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
  // Enabled, not merely probed: vkCmdBeginRendering is only legal on a device
  // created with this feature on, and the Raster bit above already promised it.
  enabled13.dynamicRendering = adapter->supports_dynamic_rendering_ ? VK_TRUE : VK_FALSE;

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

std::unique_ptr<DeviceHal> make_device_hal(std::string* error) {
  return DeviceHal::create_impl(nullptr, error);
}

std::unique_ptr<DeviceHal> make_device_hal(const uint8_t uuid[16], std::string* error) {
  return DeviceHal::create_impl(uuid, error);
}

}  // namespace vg::vulkan
