#ifndef VG_BACKENDS_METAL_TIER2_H_
#define VG_BACKENDS_METAL_TIER2_H_

#include "backends/device_hal.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vg::metal::tier2 {

// TASK-D4 (E010): Metal Tier2 heterogeneous-Node select, defaulted to
// bucket compute + per-Node `dispatchThreadgroupsWithIndirectBuffer:`.
// ICB is an optional capability upgrade and is not required; this path
// never claims DevicePass / native GPU-driven ICB select.
//
// Metal objects are passed as opaque pointers so this header stays C++
// (no public texture or pipeline object). Callers in .mm files pass
// `id<MTLDevice>` / `id<MTLCommandQueue>` / `id<MTLBuffer>`.
//
// `encoder_count` / `command_buffer_count` / `queue_wait_count` are
// incremented when non-null so submit() can fold them into DispatchStats
// before it overwrites Submission's shared counters.
bool apply_select(void* mtl_device, void* mtl_command_queue, void* mtl_fields_buffer,
                  uint32_t task_count, const hal::ExecutionPlan& plan,
                  hal::Submission* submission, uint64_t* encoder_count,
                  uint64_t* command_buffer_count, uint64_t* queue_wait_count,
                  std::string* error);

// Debug/test-only readback of the GPU-authored selected class per task
// (task-vector index order). Compare as a sorted multiset against
// `reference::select_tier2_nodes`.
const std::vector<uint32_t>& last_selected_node_classes();

}  // namespace vg::metal::tier2

#endif
