#include "backends/reference/tier2_oracle.h"

namespace vg::reference {
namespace {

bool is_authorized(uint32_t node_class, const std::vector<uint32_t>& authorized) {
  for (uint32_t candidate : authorized) {
    if (candidate == node_class) return true;
  }
  return false;
}

}  // namespace

Tier2SelectResult select_tier2_nodes(const core::TaskGraph& task_graph,
                                     const std::vector<uint32_t>& authorized_node_classes) {
  Tier2SelectResult result;
  if (authorized_node_classes.size() < 2) {
    result.message = "tier2 select requires at least two authorized node classes";
    return result;
  }
  const auto& tasks = task_graph.tasks();
  if (tasks.empty()) {
    result.message = "tier2 select requires a published task graph";
    return result;
  }
  result.bucket_count = static_cast<uint32_t>(authorized_node_classes.size());
  result.command_count = result.bucket_count;
  result.selected_classes.reserve(tasks.size());
  for (const auto& task : tasks) {
    if (!is_authorized(task.node_index, authorized_node_classes)) {
      result.ok = false;
      result.unauthorized = true;
      result.selected_classes.push_back(task.node_index);
      result.message = "tier2 select refused unauthorized node class " +
                       std::to_string(task.node_index);
      return result;
    }
    result.selected_classes.push_back(task.node_index);
  }
  result.ok = true;
  return result;
}

}  // namespace vg::reference
