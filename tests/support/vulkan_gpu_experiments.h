#ifndef VG_TEST_SUPPORT_VULKAN_GPU_EXPERIMENTS_H_
#define VG_TEST_SUPPORT_VULKAN_GPU_EXPERIMENTS_H_

// Test-only result types for the narrow Vulkan F compute experiments.  They do
// not describe a public backend capability or an ExecutionPlan feature.
#include "backends/device_hal.h"

#include <array>
#include <cstdint>
#include <vector>

namespace vg::vulkan {

struct GpuIndirectExperimentResult {
  std::vector<std::array<uint32_t, 3>> gpu_written_dims;
  uint32_t gpu_invocation_count{};
  uint32_t indirect_dispatch_count{};
  hal::LoweringReport report;
};

struct GpuCullCompactExperimentResult {
  uint32_t visible_count{};
  std::vector<uint32_t> compact_ids;
  uint32_t gpu_dispatch_count{};
  hal::LoweringReport report;
};

struct GpuIndexedAddressExperimentResult {
  uint32_t referenced_allocation_count{};
  uint32_t gpu_dispatch_count{};
  hal::LoweringReport report;
};

// Tier2 here is deliberately the preauthorized bucketed compute experiment
// described by ADR-038. `host_preprocessed_task_count` documents the bounded
// host upload only; it never stands in for device selection or execution.
struct GpuTier2ExperimentResult {
  std::vector<uint32_t> selected_classes;
  std::vector<uint32_t> bucket_counts;
  uint32_t host_preprocessed_task_count{};
  uint32_t gpu_dispatch_count{};
  uint32_t indirect_dispatch_count{};
  hal::LoweringReport report;
};

} // namespace vg::vulkan

#endif
