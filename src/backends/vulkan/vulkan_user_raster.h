#ifndef VG_BACKENDS_VULKAN_USER_RASTER_H_
#define VG_BACKENDS_VULKAN_USER_RASTER_H_

#include "ir/ir.h"

#include <string>
#include <unordered_map>
#include <vector>

#if defined(VG_HAS_VULKAN)
#include <vulkan/vulkan.h>
#else
struct VkDevice_T;
struct VkShaderModule_T;
using VkDevice = VkDevice_T *;
using VkShaderModule = VkShaderModule_T *;
#endif

namespace vg::vulkan {

// Immutable compiler output. The key is derived only from the accepted public
// contract, so a pipeline cache never treats an MSL envelope or changed ABI as
// the same Vulkan program.
struct UserRasterSpirv {
  std::vector<uint32_t> vertex;
  std::vector<uint32_t> fragment;
  std::string cache_key;
};

// Device-independent SPIR-V cache. Shader modules remain device-owned and are
// deliberately not stored here: Vulkan modules cannot cross VkDevice lifetime
// or target-identity boundaries.
struct UserRasterSpirvCache {
  std::unordered_map<std::string, UserRasterSpirv> programs;
};

// Device-owned pair for a formal plan-raster pipeline. Call the destroy helper
// before destroying its VkDevice, including partial pipeline construction.
struct UserRasterShaderModules {
  VkShaderModule vertex{};
  VkShaderModule fragment{};
  std::string cache_key;
};

bool compile_user_raster_glsl(const ir::UserRasterShaderContract &,
                              UserRasterSpirv *, std::string * = nullptr);

bool get_or_compile_user_raster_glsl(UserRasterSpirvCache *,
                                     const ir::UserRasterShaderContract &,
                                     const UserRasterSpirv **,
                                     std::string * = nullptr);

bool create_user_raster_shader_modules(VkDevice, const UserRasterSpirv &,
                                       UserRasterShaderModules *,
                                       std::string * = nullptr);
void destroy_user_raster_shader_modules(VkDevice, UserRasterShaderModules *);

} // namespace vg::vulkan
#endif
