#include "backends/vulkan/vulkan_device_internal.h"
#include "compiler/compute_task_ring.h"
#include "ir/sha256.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>
#if defined(VG_HAS_VULKAN)
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern "C" char** environ;
#endif

namespace vg::vulkan::detail {

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

bool DeviceState::ensure_pipeline(const std::string& cache_key, const std::string& glsl_source,
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

const char* storage_image_format_qualifier(VkFormat format) {
  return format == VK_FORMAT_R8G8B8A8_UNORM ? "rgba8" : "r32f";
}

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

bool DeviceState::ensure_task_ring_pipeline(std::string* error) {
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

bool DeviceState::ensure_sample_facet_pipeline(bool array_kernel, bool checked_profile, VkPipeline* pipeline,
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

bool DeviceState::ensure_storage_facet_pipeline(VkFormat format, VkPipeline* pipeline, std::string* error) {
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

bool DeviceState::ensure_raster_shader_modules(std::string* error) {
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

bool DeviceState::ensure_raster_pipeline(vg::compiler::PipelineClassificationCache& cache,
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
#endif

}  // namespace vg::vulkan::detail
