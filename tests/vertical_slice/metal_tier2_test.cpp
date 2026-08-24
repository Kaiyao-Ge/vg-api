// TASK-D4 (E010): Metal Tier2 heterogeneous-Node select vs the CPU oracle.
// New executable -- do not add a mode to metal_task_timeline_test.cpp.
#include "backends/device_hal.h"
#include "backends/metal/metal_device_hal.h"
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

vg::ir::Module make_probe_module(vg::core::Arena& arena) {
  const auto& allocation = arena.allocate(64);
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/v1";
  vg::ir::Instruction load;
  load.op = "load";
  load.allocation = allocation.id;
  load.generation = allocation.generation;
  load.representation_epoch = allocation.representation_epoch;
  load.offset = 0;
  load.size = 4;
  module.instructions.push_back(load);
  module.declared_effects.push_back(
      {allocation.id, 0, 64, vg::ir::Access::Read, allocation.representation_epoch});
  return module;
}

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
  for (const auto& event : report.events) {
    if (event.operation == operation && event.classification == classification &&
        event.count >= min_count)
      return true;
  }
  return false;
}

bool report_claims_device_pass_for_tier2(const vg::hal::LoweringReport& report) {
  for (const auto& event : report.events) {
    if (!event.operation.starts_with("tier2_")) continue;
    if (event.classification == vg::hal::LoweringClass::DevicePass) return true;
  }
  return false;
}

bool run_select_case(vg::metal::DeviceHal& metal, const std::vector<uint32_t>& nodes,
                     const char* label) {
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

  vg::core::Arena arena;
  const auto module = make_probe_module(arena);

  vg::hal::ExecutionPlan metal_plan;
  metal_plan.capabilities = metal.capabilities();
  metal_plan.module = module;
  metal_plan.published = true;
  metal_plan.task_graph = graph;
  metal_plan.graph_epoch = arena.topology_epoch();
  metal_plan.request_tier2_select = true;
  metal_plan.authorized_node_classes = authorized;

  vg::hal::CompiledPlan compiled;
  std::string error;
  if (!metal.compile(metal_plan, &compiled, &error)) {
    std::cerr << label << ": Metal compile failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission submission;
  if (!metal.submit(compiled, arena, &submission, &error)) {
    std::cerr << label << ": Metal submit failed: " << error << "\n";
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

bool run_unauthorized(vg::metal::DeviceHal& metal) {
  const auto graph = make_graph({0, 1, 2, 2, 1, 0, 1, 0});
  vg::core::Arena arena;
  const auto module = make_probe_module(arena);
  vg::hal::ExecutionPlan plan;
  plan.capabilities = metal.capabilities();
  plan.module = module;
  plan.published = true;
  plan.task_graph = graph;
  plan.graph_epoch = arena.topology_epoch();
  plan.request_tier2_select = true;
  plan.authorized_node_classes = {0, 1};

  vg::hal::CompiledPlan compiled;
  std::string error;
  if (!metal.compile(plan, &compiled, &error)) {
    std::cerr << "unauthorized: Metal compile failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission submission;
  if (!metal.submit(compiled, arena, &submission, &error)) {
    std::cerr << "unauthorized: Metal submit call failed: " << error << "\n";
    return false;
  }
  if (submission.result.ok) {
    std::cerr << "unauthorized: Metal accepted an unauthorized node class\n";
    return false;
  }
  if (submission.result.message.find("unauthorized") == std::string::npos) {
    std::cerr << "unauthorized: unexpected refuse message: " << submission.result.message << "\n";
    return false;
  }
  std::cout << "unauthorized: ok\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "tier2-nodes: no Metal device available on this host\n";
    return 1;
  }

  if (!run_select_case(*metal_device, {0, 1, 0, 1, 0, 1, 0, 1}, "uniform-8")) return 1;
  if (!run_select_case(*metal_device, {0, 0, 0, 0, 0, 0, 0, 1}, "skewed-8")) return 1;
  if (!run_unauthorized(*metal_device)) return 1;
  return 0;
}
