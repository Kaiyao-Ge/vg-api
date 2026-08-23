// TASK-D4 (E010): CPU oracle for heterogeneous Node selection.
// Assert-based like tests/unit/core_test.cpp -- no test framework.
#include "backends/reference/tier2_oracle.h"
#include "core/core.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using vg::core::TaskGraph;
using vg::core::TaskGraphBuilder;
using vg::core::TaskRecord;
using vg::reference::select_tier2_nodes;

TaskGraph make_graph(const std::vector<uint32_t>& node_classes) {
  TaskGraphBuilder builder;
  for (uint32_t node : node_classes) {
    TaskRecord task{};
    task.node_index = node;
    task.root_allocation = 1;
    assert(builder.append(task));
  }
  TaskGraph graph;
  assert(builder.seal(&graph));
  assert(graph.publish());
  return graph;
}

std::vector<uint32_t> sorted(std::vector<uint32_t> values) {
  std::sort(values.begin(), values.end());
  return values;
}

}  // namespace

int main() {
  const std::vector<uint32_t> authorized{0, 1};

  // Uniform: 8 tasks, 4+4.
  {
    const std::vector<uint32_t> nodes{0, 1, 0, 1, 0, 1, 0, 1};
    const auto graph = make_graph(nodes);
    const auto result = select_tier2_nodes(graph, authorized);
    assert(result.ok);
    assert(!result.unauthorized);
    assert(result.bucket_count == 2);
    assert(result.command_count == 2);
    assert(sorted(result.selected_classes) == sorted(nodes));
  }

  // Skewed: 8 tasks, 7+1.
  {
    const std::vector<uint32_t> nodes{0, 0, 0, 0, 0, 0, 0, 1};
    const auto graph = make_graph(nodes);
    const auto result = select_tier2_nodes(graph, authorized);
    assert(result.ok);
    assert(!result.unauthorized);
    assert(result.bucket_count == 2);
    assert(result.command_count == 2);
    assert(sorted(result.selected_classes) == sorted(nodes));
  }

  // Unauthorized node class is refused; no new Node is invented (Tier3).
  {
    const auto graph = make_graph({0, 1, 2});
    const auto result = select_tier2_nodes(graph, authorized);
    assert(!result.ok);
    assert(result.unauthorized);
    assert(result.message.find("unauthorized") != std::string::npos);
  }

  // Structural refuse: fewer than two authorized classes.
  {
    const auto graph = make_graph({0, 0});
    const auto result = select_tier2_nodes(graph, {0});
    assert(!result.ok);
    assert(!result.unauthorized);
  }

  return 0;
}
