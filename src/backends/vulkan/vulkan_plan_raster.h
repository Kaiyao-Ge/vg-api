#ifndef VG_BACKENDS_VULKAN_PLAN_RASTER_H_
#define VG_BACKENDS_VULKAN_PLAN_RASTER_H_

#include "backends/device_hal.h"
#if defined(VG_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace vg::vulkan::detail {
struct DeviceState;
struct PlanRasterCache;
struct GpuDrawCommandView;

// Stage-6/Stage-7 helpers for exactly one already-sealed Raster NodeRef or
// TaskRecord. The caller owns schedule traversal, transition lowering, fault
// reachability, and publication; this module must never reconstruct them from
// TaskGraph.
bool compile_plan_raster_package(DeviceState &state,
                                 const core::ExecutionPlan::ResolvedNode &node,
                                 hal::CompiledPlan::PerNodePackage *out,
                                 hal::LoweringReport *report,
                                 std::string *error = nullptr);

struct PlanRasterStepStats {
  uint64_t draw_count{};
  uint64_t command_buffer_count{};
  uint64_t encoder_count{};
  uint64_t barrier_count{};
  uint64_t queue_wait_count{};
};

// Validates and encodes one Raster task selected by the coordinator from the
// immutable ExecutionSchedule. `transitions_before` is diagnostic context from
// that schedule, never an invitation to discover dependencies locally.
bool submit_plan_raster_step(DeviceState &state,
                             const hal::CompiledPlan &compiled,
                             uint32_t task_index,
                             const std::vector<uint32_t> &transitions_before,
                             core::Arena &arena, hal::Submission *submission,
                             PlanRasterStepStats *stats,
                             std::string *error = nullptr,
                             const GpuDrawCommandView *tier2_view = nullptr,
                             uint32_t tier2_command = 0);

// Coordinator-owned DeviceState stores one of these for the adapter lifetime;
// its destructor destroys VkPipeline/VkShaderModule/VkDescriptorSetLayout and
// VkPipelineLayout in reverse creation order after the device is idle.
PlanRasterCache *ensure_plan_raster_cache(DeviceState &state,
                                          std::string *error = nullptr);
void destroy_plan_raster_cache(DeviceState &state);

#if defined(VG_HAS_VULKAN)
bool ensure_plan_raster_pipeline(DeviceState &state, VkFormat color_format,
                                 VkFormat depth_format, bool has_depth,
                                 uint32_t sample_count, bool depth_test_enable,
                                 bool depth_write_enable,
                                 core::DepthCompareOp depth_compare_op,
                                 VkPipeline *out, bool *cache_hit,
                                 std::string *error = nullptr);
#endif
} // namespace vg::vulkan::detail

#endif
