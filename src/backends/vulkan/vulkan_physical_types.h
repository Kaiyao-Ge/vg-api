#ifndef VG_BACKENDS_VULKAN_PHYSICAL_TYPES_H_
#define VG_BACKENDS_VULKAN_PHYSICAL_TYPES_H_

#include "backends/device_hal.h"
#include <array>
#include <vector>

namespace vg::vulkan {

// Existing backend-private physical descriptions shared with the narrow harness.
struct FacetDescriptorCost {
  uint32_t set_allocation_count{};
  uint32_t descriptor_write_count{};
  uint64_t descriptor_write_bytes{};
  uint64_t cpu_descriptor_ns{};
  bool used_descriptor_buffer{};
};

enum class AttachmentLoadAction : uint32_t { Clear, Load, DontCare };

enum class AttachmentStoreAction : uint32_t { Store, DontCare, MultisampleResolve };

struct RasterPassDesc {
  AttachmentLoadAction load{AttachmentLoadAction::Clear};
  AttachmentStoreAction store{AttachmentStoreAction::Store};
  std::array<float, 4> clear_rgba{};
  // Shader-visible per-draw data, so a UBO rather than a specialization
  // constant: it must not enter the pipeline key (07 §9, 06 §7).
  std::array<float, 4> tint{{1.0f, 1.0f, 1.0f, 1.0f}};
  std::vector<std::array<float, 4>> vertices;
  // >1 renders into a transient multisample VkImage that resolves into the
  // facet's own image through VkRenderingAttachmentInfo::resolveImageView.
  // Pipeline-key state (07 §9), not dynamic state.
  uint32_t sample_count{1};
  // Vulkan dynamic state (VK_DYNAMIC_STATE_VIEWPORT/SCISSOR), so deliberately
  // absent from the pipeline key. 0 means "the attachment's full extent".
  uint32_t viewport_width{};
  uint32_t viewport_height{};
};

struct RasterPassResult {
  std::array<float, 4> resolved_rgba{};
  bool facet_cache_hit{};
  uint32_t sample_count{1};
  uint32_t draw_count{};
  uint64_t pipeline_key_hash{};
  bool pipeline_cache_hit{};
  FacetDescriptorCost descriptors;
  vg::hal::LoweringReport report;
};
}  // namespace vg::vulkan

#endif
