#ifndef VG_BACKENDS_METAL_TIER2_H_
#define VG_BACKENDS_METAL_TIER2_H_

#include "backends/device_hal.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vg::metal::tier2 {

// E010: Metal Tier2 heterogeneous-Node select. Preferred path is a GPU-
// encoded ICB that writes one compute command per published task, each
// bound to a distinct pre-authorized compute PSO
// (supportIndirectCommandBuffers=YES, inheritPipelineState=NO). That
// path reports DevicePass. If ICB allocate/encode/execute is unavailable,
// the previous bucket + per-class indirect dispatch remains as fallback
// and stays EmulatedDevicePass. DevicePass is never used for a host walk
// or a single-PSO ICB wrapper.
//
// Metal objects are passed as opaque pointers so this header stays C++
// (no public texture or pipeline object). Callers in .mm files pass
// `id<MTLDevice>` / `id<MTLCommandQueue>` / `id<MTLBuffer>`.
struct DispatchCounters {
  uint64_t* encoder_count{};
  uint64_t* command_buffer_count{};
  uint64_t* queue_wait_count{};
};

struct MetalSelectContext {
  void* device{};
  void* command_queue{};
  void* fields_buffer{};
};

bool apply_select(const MetalSelectContext& metal, uint32_t task_count, const hal::ExecutionPlan& plan,
                  hal::Submission* submission, DispatchCounters counters, std::string* error);

// Narrow test-only physical-adapter harness.  It owns only the Metal device,
// queue, and task-ring fields buffer required by Tier2 selection; it does not
// invoke DeviceHal::compile or pretend that its deliberately minimal request
// is a core-assembled ExecutionPlan.
bool run_select_test_harness(const std::vector<uint32_t>& task_node_classes,
                             const std::vector<uint32_t>& authorized_node_classes,
                             hal::Submission* submission, std::string* error);

// Debug/test-only readback of the GPU-authored selected class per task
// (task-vector index order). Compare as a sorted multiset against
// `reference::select_tier2_nodes`.
const std::vector<uint32_t>& last_selected_node_classes();

}  // namespace vg::metal::tier2

#endif
