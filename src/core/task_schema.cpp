#include "core/task_schema.h"

namespace vg::core {

TaskRecord task_from_schema(const VgSchema_TaskRecord& task) {
  TaskRecord result;
  result.node_index = task.node.index;
  result.node_generation = task.node.generation;
  result.root_allocation = task.root;
  result.x = task.shape.x;
  result.y = task.shape.y;
  result.z = task.shape.z;
  result.flags = task.shape.flags;
  result.contract_index = task.contract_index;
  result.payload_size = task.payload_size;
  result.payload_or_offset = task.payload_or_offset;
  return result;
}

}  // namespace vg::core
