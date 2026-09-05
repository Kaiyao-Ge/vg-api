#ifndef VG_BACKENDS_VULKAN_PLAN_INDIRECT_H_
#define VG_BACKENDS_VULKAN_PLAN_INDIRECT_H_

#include "backends/device_hal.h"
#include "backends/vulkan/vulkan_tier2.h"
#include <functional>
#if defined(VG_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace vg::vulkan::detail {
struct DeviceState;
struct PlanIndirectCache;

struct PlanIndirectStats {
  uint64_t command_buffer_count{};
  uint64_t encoder_count{};
  uint64_t barrier_count{};
  uint64_t queue_wait_count{};
  uint64_t indirect_draw_count{};
  uint64_t temporary_bytes{};
};

struct GpuDrawCommandView {
#if defined(VG_HAS_VULKAN)
  VkBuffer buffer{VK_NULL_HANDLE};
  VkDeviceMemory memory{VK_NULL_HANDLE};
#endif
  uint32_t command_count{};
  uint32_t command_stride{};
  uint64_t byte_size{};
};

// GPU-only producer: bucket and fill compute passes write compact Vulkan draw
// indirect records. No count/command bytes are mapped or read by the host.
bool generate_plan_tier2_draw_commands(
    DeviceState &state, const std::vector<tier2::SelectionRecord> &records,
    const std::vector<tier2::AuthorizedBucket> &authorized, bool indexed_draw,
    GpuDrawCommandView *view, PlanIndirectStats *stats,
    hal::LoweringReport *report, std::string *error = nullptr);
void destroy_plan_tier2_draw_commands(DeviceState &state,
                                      GpuDrawCommandView *view);
#if defined(VG_HAS_VULKAN)
using PlanIndirectBucketBinder =
    std::function<bool(VkCommandBuffer, uint32_t, std::string *)>;
// `command_buffer` must already be inside E1's dynamic-rendering scope.
bool record_plan_tier2_draw_consumer(
    VkCommandBuffer command_buffer, const GpuDrawCommandView &view,
    bool indexed_draw, const PlanIndirectBucketBinder &bind_bucket,
    uint32_t first_command, uint32_t command_count, PlanIndirectStats *stats,
    std::string *error = nullptr);
#endif
PlanIndirectCache *ensure_plan_indirect_cache(DeviceState &state,
                                              std::string *error = nullptr);
void destroy_plan_indirect_cache(DeviceState &state);

// Stage-7 entry point. The coordinator calls it only for the explicit sealed
// ExecutionPlan Tier2 fact and supplies the matching complete packages. It
// never infers selection from a multi-Node graph or envelope authority.
bool submit_plan_tier2_indirect(
    DeviceState &state, const core::ExecutionPlan &plan,
    const std::vector<hal::CompiledPlan::PerNodePackage> &packages,
    const std::vector<tier2::SelectionRecord> &gpu_selection_records,
    bool indexed_draw, GpuDrawCommandView *view, PlanIndirectStats *stats,
    hal::LoweringReport *report, std::string *error = nullptr);
} // namespace vg::vulkan::detail

#endif
