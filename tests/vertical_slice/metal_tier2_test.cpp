// TASK-D4 (E010): Metal Tier2 heterogeneous-Node select vs the CPU oracle.
// New executable -- do not add a mode to metal_task_timeline_test.cpp.
#include "backends/device_hal.h"
#include "backends/metal/metal_tier2.h"
#include "backends/reference/tier2_oracle.h"
#include "ir/ir.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

using vg::core::TaskGraph;
using vg::core::TaskGraphBuilder;
using vg::core::TaskRecord;

TaskGraph make_graph(const std::vector<uint32_t>& node_classes) {
  TaskGraphBuilder builder;
  for (uint32_t node : node_classes) {
    TaskRecord task{};
    task.node_index = node;
    task.root_allocation = 1;
    if (!builder.append(task)) return {};
  }
  TaskGraph graph;
  if (!builder.seal(&graph) || !graph.publish()) return {};
  return graph;
}

std::vector<uint32_t> sorted(std::vector<uint32_t> values) {
  std::ranges::sort(values);
  return values;
}

bool has_event(const vg::hal::LoweringReport& report, const char* operation,
               vg::hal::LoweringClass classification, uint64_t min_count) {
  return std::ranges::any_of(report.events, [&](const vg::hal::LoweringEvent& event) {
    return event.operation == operation && event.classification == classification &&
           event.count >= min_count;
  });
}

bool report_claims_device_pass_for_tier2(const vg::hal::LoweringReport& report) {
  return std::ranges::any_of(report.events, [](const vg::hal::LoweringEvent& event) {
    return event.operation.starts_with("tier2_") &&
           event.classification == vg::hal::LoweringClass::DevicePass;
  });
}

bool run_select_case(const std::vector<uint32_t>& nodes, const char* label) {
  const std::vector<uint32_t> authorized{0, 1};
  const auto graph = make_graph(nodes);
  if (graph.tasks().size() != nodes.size()) {
    std::cerr << label << ": failed to build task graph\n";
    return false;
  }

  const auto oracle = vg::reference::select_tier2_nodes(graph, authorized);
  if (!oracle.ok) {
    std::cerr << label << ": oracle refused a legal authorized set: " << oracle.message << "\n";
    return false;
  }

  // Tier2 has no canonical node-class contract yet, so assembler correctly
  // rejects it. Exercise the actual physical select adapter through its
  // narrow test harness instead of constructing a fake sealed plan.
  std::string error;
  vg::hal::Submission submission;
  if (!vg::metal::tier2::run_select_test_harness(nodes, authorized, &submission, &error)) {
    std::cerr << label << ": Tier2 physical harness failed: " << error << "\n";
    return false;
  }
  if (!submission.result.ok) {
    std::cerr << label << ": Metal execution reported failure: " << submission.result.message << "\n";
    return false;
  }

  auto selected = vg::metal::tier2::last_selected_node_classes();
  if (sorted(selected) != sorted(oracle.selected_classes)) {
    std::cerr << label << ": Metal selected-class multiset != reference oracle\n";
    return false;
  }
  const bool icb = has_event(submission.report, "tier2_node_select", vg::hal::LoweringClass::DevicePass, 2) &&
                   has_event(submission.report, "tier2_icb_execute", vg::hal::LoweringClass::DevicePass, 1);
  const bool bucket =
      has_event(submission.report, "tier2_node_select", vg::hal::LoweringClass::EmulatedDevicePass, 2) &&
      has_event(submission.report, "tier2_bucket_count", vg::hal::LoweringClass::EmulatedDevicePass, 2);
  if (icb == bucket) {
    std::cerr << label << ": expected exactly one of ICB DevicePass or bucket EmulatedDevicePass\n";
    return false;
  }
  if (bucket && report_claims_device_pass_for_tier2(submission.report)) {
    std::cerr << label << ": bucket fallback claimed DevicePass\n";
    return false;
  }

  std::cout << label << ": ok (" << (icb ? "icb DevicePass" : "bucket fallback") << ")\n";
  return true;
}

bool run_unauthorized() {
  const auto graph = make_graph({0, 1, 2, 2, 1, 0, 1, 0});
  std::string error;
  vg::hal::Submission submission;
  if (vg::metal::tier2::run_select_test_harness({0, 1, 2, 2, 1, 0, 1, 0}, {0, 1},
                                                &submission, &error)) {
    std::cerr << "unauthorized: Metal accepted an unauthorized node class\n";
    return false;
  }
  if (error.find("unauthorized") == std::string::npos) {
    std::cerr << "unauthorized: unexpected refuse message: " << error << "\n";
    return false;
  }
  std::cout << "unauthorized: ok\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  if (!run_select_case({0, 1, 0, 1, 0, 1, 0, 1}, "uniform-8")) return 1;
  if (!run_select_case({0, 0, 0, 0, 0, 0, 0, 1}, "skewed-8")) return 1;
  if (!run_unauthorized()) return 1;
  return 0;
}
