#ifndef VG_CORE_TASK_SCHEMA_H_
#define VG_CORE_TASK_SCHEMA_H_

#include "core/task_graph.h"
#include "vg_task_root.h"

namespace vg::core {

// Converts the generated, ABI-stable TaskRecord layout into the checked core record.
TaskRecord task_from_schema(const VgSchema_TaskRecord& task);

}  // namespace vg::core

#endif
