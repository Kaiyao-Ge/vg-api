#include "backends/vulkan/vulkan_user_raster.h"

#include "backends/vulkan/vulkan_device_internal.h"
#include "ir/sha256.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace vg::vulkan {
namespace {

bool validate_user_raster_contract(const ir::UserRasterShaderContract &contract,
                                   std::string *cache_key, std::string *error) {
  if (contract.vertex_abi != ir::kRasterVertexAbiXyzuvPackedV1 ||
      contract.vertex_entry.empty() || contract.fragment_entry.empty() ||
      contract.source.size() > (1u << 20) ||
      contract.source.find("#version") == std::string::npos ||
      contract.source.find("metal_stdlib") != std::string::npos ||
      contract.source.find("[[") != std::string::npos) {
    if (error)
      *error = "Unsupported Vulkan user raster contract: requires bounded GLSL "
               "#version source, xyzuv ABI, entries, and no MSL tokens";
    return false;
  }
  if (contract.source.find("void main") != std::string::npos ||
      contract.source.find("VG_VERTEX_STAGE") == std::string::npos ||
      contract.source.find("VG_FRAGMENT_STAGE") == std::string::npos) {
    if (error)
      *error = "Unsupported Vulkan user raster contract: source must be "
               "stage-gated and declare no main";
    return false;
  }
  if (contract.source.find(contract.vertex_entry) == std::string::npos ||
      contract.source.find(contract.fragment_entry) == std::string::npos) {
    if (error)
      *error = "Unsupported Vulkan user raster contract: declared entry is "
               "absent from GLSL source";
    return false;
  }
  if (cache_key)
    *cache_key =
        ir::sha256_hex(contract.source + "\n" + contract.vertex_entry + "\n" +
                       contract.fragment_entry + "\n" + contract.vertex_abi);
  return true;
}

bool validate_spirv_descriptor_stage(const std::vector<uint32_t> &words,
                                     bool vertex_stage, std::string *error) {
  constexpr uint32_t kSpirvMagic = 0x07230203;
  constexpr uint16_t kOpTypePointer = 32;
  constexpr uint16_t kOpTypeSampledImage = 27;
  constexpr uint16_t kOpVariable = 59;
  constexpr uint16_t kOpDecorate = 71;
  constexpr uint32_t kDecorationBinding = 33;
  constexpr uint32_t kDecorationDescriptorSet = 34;
  constexpr uint32_t kStorageUniformConstant = 0;
  constexpr uint32_t kStorageUniform = 2;
  constexpr uint32_t kStorageStorageBuffer = 12;
  struct Decorations {
    std::optional<uint32_t> binding;
    std::optional<uint32_t> descriptor_set;
  };
  struct PointerType {
    uint32_t storage_class{};
    uint32_t pointee{};
  };
  struct Variable {
    uint32_t pointer_type{};
    uint32_t storage_class{};
  };
  if (words.size() < 5 || words.front() != kSpirvMagic) {
    if (error)
      *error = "Vulkan user raster compiler returned malformed SPIR-V";
    return false;
  }
  std::unordered_map<uint32_t, Decorations> decorations;
  std::unordered_map<uint32_t, PointerType> pointers;
  std::unordered_map<uint32_t, Variable> variables;
  std::unordered_set<uint32_t> sampled_image_types;
  for (size_t offset = 5; offset < words.size();) {
    const uint32_t instruction = words[offset];
    const uint16_t word_count = static_cast<uint16_t>(instruction >> 16);
    const uint16_t opcode = static_cast<uint16_t>(instruction & 0xffffu);
    if (word_count == 0 || offset + word_count > words.size()) {
      if (error)
        *error = "Vulkan user raster compiler returned truncated SPIR-V";
      return false;
    }
    const uint32_t *operands = words.data() + offset + 1;
    if (opcode == kOpDecorate && word_count >= 4) {
      auto &value = decorations[operands[0]];
      if (operands[1] == kDecorationBinding)
        value.binding = operands[2];
      else if (operands[1] == kDecorationDescriptorSet)
        value.descriptor_set = operands[2];
    } else if (opcode == kOpTypePointer && word_count == 4) {
      pointers[operands[0]] = {operands[1], operands[2]};
    } else if (opcode == kOpTypeSampledImage && word_count == 3) {
      sampled_image_types.insert(operands[0]);
    } else if (opcode == kOpVariable && word_count >= 4) {
      variables[operands[1]] = {operands[0], operands[2]};
    }
    offset += word_count;
  }

  std::unordered_set<uint32_t> seen_bindings;
  for (const auto &[id, variable] : variables) {
    const auto decorated = decorations.find(id);
    if (decorated == decorations.end() ||
        !decorated->second.binding.has_value())
      continue;
    const auto pointer = pointers.find(variable.pointer_type);
    const uint32_t binding = *decorated->second.binding;
    if (!decorated->second.descriptor_set.has_value() ||
        *decorated->second.descriptor_set != 0 || pointer == pointers.end() ||
        pointer->second.storage_class != variable.storage_class ||
        !seen_bindings.insert(binding).second) {
      if (error)
        *error = "Unsupported Vulkan user raster descriptor declaration";
      return false;
    }
    bool compatible = false;
    if (vertex_stage && binding == 0)
      compatible = variable.storage_class == kStorageStorageBuffer;
    else if (vertex_stage && binding == 3)
      compatible = variable.storage_class == kStorageUniform;
    else if (!vertex_stage && binding == 1)
      compatible = variable.storage_class == kStorageUniformConstant &&
                   sampled_image_types.contains(pointer->second.pointee);
    else if (!vertex_stage && binding == 2)
      compatible = variable.storage_class == kStorageUniform;
    if (!compatible) {
      if (error)
        *error =
            "Unsupported Vulkan user raster descriptor set/binding ABI at " +
            std::string(vertex_stage ? "vertex" : "fragment") + " binding " +
            std::to_string(binding);
      return false;
    }
  }
  return true;
}

#if defined(VG_HAS_VULKAN)
bool create_shader_module(VkDevice device, const std::vector<uint32_t> &spirv,
                          VkShaderModule *out, std::string *error) {
  if (spirv.empty()) {
    if (error)
      *error = "Vulkan user raster shader stage has no SPIR-V";
    return false;
  }
  const VkShaderModuleCreateInfo info{
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
      spirv.size() * sizeof(uint32_t), spirv.data()};
  const VkResult result = vkCreateShaderModule(device, &info, nullptr, out);
  if (result != VK_SUCCESS) {
    if (error)
      *error = "vkCreateShaderModule failed for Vulkan user raster stage: " +
               std::to_string(static_cast<int>(result));
    return false;
  }
  return true;
}
#endif

} // namespace

bool compile_user_raster_glsl(const ir::UserRasterShaderContract &contract,
                              UserRasterSpirv *out, std::string *error) {
  if (!out) {
    if (error)
      *error = "user raster SPIR-V output is required";
    return false;
  }
  std::string cache_key;
  if (!validate_user_raster_contract(contract, &cache_key, error))
    return false;

  *out = {};
  out->cache_key = std::move(cache_key);
#if !defined(VG_HAS_VULKAN)
  if (error)
    *error = "Vulkan user raster compiler is unavailable in this build after "
             "GLSL contract validation";
  return false;
#else
  const std::string vertex_source = contract.source +
                                    "\n#ifdef VG_VERTEX_STAGE\nvoid main(){ " +
                                    contract.vertex_entry + "(); }\n#endif\n";
  const std::string fragment_source =
      contract.source + "\n#ifdef VG_FRAGMENT_STAGE\nvoid main(){ " +
      contract.fragment_entry + "(); }\n#endif\n";
  if (!detail::compile_glsl_stage(vertex_source, "vertex", {"VG_VERTEX_STAGE"},
                                  &out->vertex, error) ||
      !detail::compile_glsl_stage(fragment_source, "fragment",
                                  {"VG_FRAGMENT_STAGE"}, &out->fragment,
                                  error) ||
      !validate_spirv_descriptor_stage(out->vertex, true, error) ||
      !validate_spirv_descriptor_stage(out->fragment, false, error)) {
    *out = {};
    return false;
  }
  return true;
#endif
}

bool get_or_compile_user_raster_glsl(
    UserRasterSpirvCache *cache, const ir::UserRasterShaderContract &contract,
    const UserRasterSpirv **out, std::string *error) {
  if (!cache || !out) {
    if (error)
      *error = "user raster SPIR-V cache and output are required";
    return false;
  }
  std::string cache_key;
  if (!validate_user_raster_contract(contract, &cache_key, error))
    return false;
  const auto existing = cache->programs.find(cache_key);
  if (existing != cache->programs.end()) {
    *out = &existing->second;
    return true;
  }
  UserRasterSpirv program;
  if (!compile_user_raster_glsl(contract, &program, error))
    return false;
  const auto inserted =
      cache->programs.emplace(program.cache_key, std::move(program));
  *out = &inserted.first->second;
  return true;
}

bool create_user_raster_shader_modules(VkDevice device,
                                       const UserRasterSpirv &spirv,
                                       UserRasterShaderModules *out,
                                       std::string *error) {
  if (!out) {
    if (error)
      *error = "Vulkan user raster shader-module output is required";
    return false;
  }
#if !defined(VG_HAS_VULKAN)
  (void)device;
  (void)spirv;
  if (error)
    *error = "Vulkan user raster shader modules are unavailable in this build";
  return false;
#else
  if (device == VK_NULL_HANDLE) {
    if (error)
      *error = "Vulkan user raster shader modules require a VkDevice";
    return false;
  }
  UserRasterShaderModules created;
  if (!create_shader_module(device, spirv.vertex, &created.vertex, error) ||
      !create_shader_module(device, spirv.fragment, &created.fragment, error)) {
    destroy_user_raster_shader_modules(device, &created);
    return false;
  }
  created.cache_key = spirv.cache_key;
  destroy_user_raster_shader_modules(device, out);
  *out = std::move(created);
  return true;
#endif
}

void destroy_user_raster_shader_modules(VkDevice device,
                                        UserRasterShaderModules *modules) {
  if (!modules)
    return;
#if defined(VG_HAS_VULKAN)
  if (device != VK_NULL_HANDLE && modules->vertex != VK_NULL_HANDLE)
    vkDestroyShaderModule(device, modules->vertex, nullptr);
  if (device != VK_NULL_HANDLE && modules->fragment != VK_NULL_HANDLE)
    vkDestroyShaderModule(device, modules->fragment, nullptr);
#else
  (void)device;
#endif
  *modules = {};
}

} // namespace vg::vulkan
