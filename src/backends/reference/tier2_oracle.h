#ifndef VG_BACKENDS_REFERENCE_TIER2_ORACLE_H_
#define VG_BACKENDS_REFERENCE_TIER2_ORACLE_H_

#include "core/core.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vg::reference {

// TASK-D4 (E010): CPU judge for heterogeneous Node selection.
//
// For each task in graph order (task-vector index, matching the Metal
// bucket kernel's gid), the selected class is that task's own
// `node_index` if and only if it appears in `authorized_node_classes`.
// Any unauthorized class refuses the whole selection -- the envelope
// cannot grow a new Node (Tier3 stays Unsupported).
//
// `selected_classes` is one entry per task, so a caller compares it
// against a GPU readback as a sorted multiset, never as a GPU-arrival
// order. `bucket_count` is the number of authorized classes (one bucket
// each). `command_count` is the matching per-Node indirect command
// count the Metal emulated path would issue (one per bucket).
struct Tier2SelectResult {
  bool ok{};
  bool unauthorized{};
  std::string message;
  std::vector<uint32_t> selected_classes;
  uint32_t bucket_count{};
  uint32_t command_count{};
};

Tier2SelectResult select_tier2_nodes(const core::TaskGraph& task_graph,
                                     const std::vector<uint32_t>& authorized_node_classes);

}  // namespace vg::reference

#endif
