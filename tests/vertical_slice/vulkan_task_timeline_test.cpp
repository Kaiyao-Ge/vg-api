#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "backends/vulkan/vulkan_device_hal.h"
#include "ir/ir.h"
#include "../support/assembled_plan_fixture.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using vg::core::TaskGraph;
using vg::core::TaskGraphBuilder;
using vg::core::TaskRecord;

bool assemble_compute_plan(vg::core::Arena& arena, vg::ir::Module module,
                           std::vector<TaskRecord> tasks, vg::hal::ExecutionPlan* out,
                           std::string* error,
                           const vg::test_support::AssemblyOptions& options = {}) {
  vg::test_support::AssembledPlanFixture fixture;
  return vg::test_support::assemble_single_node_plan(
      arena, std::move(module), tasks, &fixture, out, error, options);
}

TaskRecord probe_task(const vg::ir::Module& module) {
  TaskRecord task{};
  task.root_allocation = module.instructions.front().allocation;
  task.root_generation = module.instructions.front().generation;
  task.x = task.y = task.z = 1;
  return task;
}

// A minimal single-load module. Its only purpose is to give compile()/
// submit() a valid linear compute package to run so the timeline/task-ring
// paths (which don't otherwise touch module semantics) can be exercised
// end to end; the loaded value itself is never inspected. Mirrors
// tests/vertical_slice/metal_task_timeline_test.cpp's make_probe_module.
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
  module.declared_effects.push_back({allocation.id, 0, 64, vg::ir::Access::Read, allocation.representation_epoch});
  return module;
}

bool same_task(const TaskRecord& a, const TaskRecord& b) {
  return a.node_index == b.node_index && a.node_generation == b.node_generation &&
         a.root_allocation == b.root_allocation && a.root_generation == b.root_generation && a.x == b.x &&
         a.y == b.y && a.z == b.z && a.flags == b.flags && a.contract_index == b.contract_index &&
         a.payload_size == b.payload_size && a.payload_or_offset == b.payload_or_offset;
}

// Two tasks with an explicit dependency (1 depends on 0), non-trivial x/y/z
// so the GPU dispatch-sizing path (TaskRecord.x/y/z, read back from the Task
// ring via vkCmdCopyBuffer into the Tier1 indirect buffer, not a hardcoded
// (1,1,1)) is actually exercised. Vulkan's GPU task ring publication must
// report published_tasks byte-identical, in the same order, to the reference
// oracle (reference::execute_task_graph()) -- same assertion shape as
// metal_task_timeline_test.cpp's run_task_tier0, compile-review-only here
// since no Linux/NVIDIA hardware is available to actually run this binary.
bool run_task_tier0(const std::string& root) {
  (void)root;
  vg::core::Arena arena;
  const auto module = make_probe_module(arena);
  TaskRecord task0 = probe_task(module);
  task0.x = 3;
  task0.y = 2;
  task0.z = 1;
  task0.payload_size = 8;
  TaskRecord task1 = probe_task(module);
  task1.x = 1;
  task1.y = 1;
  task1.z = 1;
  task1.flags = 7;
  task1.contract_index = 3;
  task1.payload_or_offset = 0x1'0000'0001ULL;
  std::string device_error;
  auto vulkan_device = vg::vulkan::make_device_hal(&device_error);
  if (vulkan_device == nullptr) {
    std::cerr << "task-tier0: no Vulkan device available on this host: " << device_error << "\n";
    return false;
  }

  vg::hal::ExecutionPlan plan;
  std::string error;
  const std::vector<std::pair<uint32_t, uint32_t>> dependencies{{0, 1}};
  vg::test_support::AssemblyOptions options;
  options.dependencies = &dependencies;
  if (!assemble_compute_plan(arena, module, {task0, task1}, &plan, &error, options)) {
    std::cerr << "task-tier0: plan assembly failed: " << error << "\n";
    return false;
  }
  auto oracle = vg::reference::execute_task_graph(plan.task_graph);
  if (!oracle.ok || oracle.published_tasks.size() != 2) {
    std::cerr << "task-tier0: reference oracle failed: " << oracle.message << "\n";
    return false;
  }
  vg::hal::CompiledPlan compiled;
  if (!vulkan_device->compile(plan, &compiled, &error)) {
    std::cerr << "task-tier0: Vulkan compile failed: " << error << "\n";
    return false;
  }

  vg::hal::Submission submission;
  if (!vulkan_device->submit(compiled, arena, &submission, &error)) {
    std::cerr << "task-tier0: Vulkan submit failed: " << error << "\n";
    return false;
  }
  if (!submission.result.ok) {
    std::cerr << "task-tier0: Vulkan execution reported failure: " << submission.result.message << "\n";
    return false;
  }
  if (submission.published_tasks.size() != oracle.published_tasks.size()) {
    std::cerr << "task-tier0: published_tasks count mismatch\n";
    return false;
  }
  for (size_t i = 0; i < oracle.published_tasks.size(); ++i) {
    if (!same_task(submission.published_tasks[i], oracle.published_tasks[i])) {
      std::cerr << "task-tier0: published_tasks[" << i << "] mismatches reference oracle\n";
      return false;
    }
  }
  std::cout << "task-tier0: ok\n";
  return true;
}

// timeline_signal advances the device's VkSemaphore(TIMELINE); a subsequent
// submission's timeline_wait for that exact value succeeds; a wait for a
// value nothing has signaled yet faults honestly (submit() still returns
// true, matching the reference/Metal/Vulkan convention that submit()
// reports host-side acceptance while submission.result.ok reports the
// execution outcome). Mirrors metal_task_timeline_test.cpp's run_timeline.
bool run_timeline(const std::string& root) {
  (void)root;
  std::string device_error;
  auto vulkan_device = vg::vulkan::make_device_hal(&device_error);
  if (vulkan_device == nullptr) {
    std::cerr << "timeline: no Vulkan device available on this host: " << device_error << "\n";
    return false;
  }
  if (!vulkan_device->capabilities().supports(vg::hal::Capability::Timeline)) {
    std::cerr << "timeline: device does not advertise Timeline support, skipping\n";
    return true;
  }

  vg::core::Arena arena;
  const auto module = make_probe_module(arena);
  std::string error;

  vg::test_support::AssemblyOptions signal_options;
  signal_options.timeline_signal = 5;
  vg::hal::ExecutionPlan signal_plan;
  if (!assemble_compute_plan(arena, module, {probe_task(module)}, &signal_plan, &error, signal_options)) {
    std::cerr << "timeline: assembly (signal) failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan signal_compiled;
  if (!vulkan_device->compile(signal_plan, &signal_compiled, &error)) {
    std::cerr << "timeline: compile (signal) failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission signal_submission;
  if (!vulkan_device->submit(signal_compiled, arena, &signal_submission, &error)) {
    std::cerr << "timeline: submit (signal) failed: " << error << "\n";
    return false;
  }
  if (!signal_submission.result.ok || signal_submission.timeline_value != 5) {
    std::cerr << "timeline: signal submission did not reach value 5\n";
    return false;
  }

  vg::test_support::AssemblyOptions wait_options;
  wait_options.timeline_wait = 5;
  wait_options.timeline_signal = 10;
  vg::hal::ExecutionPlan wait_plan;
  if (!assemble_compute_plan(arena, module, {probe_task(module)}, &wait_plan, &error, wait_options)) {
    std::cerr << "timeline: assembly (wait) failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan wait_compiled;
  if (!vulkan_device->compile(wait_plan, &wait_compiled, &error)) {
    std::cerr << "timeline: compile (wait) failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission wait_submission;
  if (!vulkan_device->submit(wait_compiled, arena, &wait_submission, &error)) {
    std::cerr << "timeline: submit (wait) failed: " << error << "\n";
    return false;
  }
  if (!wait_submission.result.ok || wait_submission.timeline_value != 10) {
    std::cerr << "timeline: satisfied wait did not advance to value 10\n";
    return false;
  }

  vg::test_support::AssemblyOptions stuck_options;
  stuck_options.timeline_wait = 999;
  stuck_options.timeline_signal = 1000;
  vg::hal::ExecutionPlan stuck_plan;
  if (!assemble_compute_plan(arena, module, {probe_task(module)}, &stuck_plan, &error, stuck_options)) {
    std::cerr << "timeline: assembly (stuck) failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan stuck_compiled;
  if (!vulkan_device->compile(stuck_plan, &stuck_compiled, &error)) {
    std::cerr << "timeline: compile (stuck) failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission stuck_submission;
  if (!vulkan_device->submit(stuck_compiled, arena, &stuck_submission, &error)) {
    std::cerr << "timeline: submit (stuck) call itself failed: " << error << "\n";
    return false;
  }
  if (stuck_submission.result.ok || stuck_submission.result.fault.code != "TIMELINE_WAIT_UNSATISFIED") {
    std::cerr << "timeline: unsatisfied wait did not fault as expected\n";
    return false;
  }
  std::cout << "timeline: ok\n";
  return true;
}

// F2 (ADR-046) wired TaskGraph-driven rasterization through compile()/
// submit() for the reference and Metal backends only -- this backend's own
// raster machinery (ensure_raster_pipeline/run_raster_facet in
// vulkan_device_hal.cpp) is separate, pre-existing, and permanently
// compile-review-only (ADR-043 §7). A Raster-kind TaskRecord reaching this
// backend's TaskGraph must be rejected at compile() time (Unsupported), not
// silently republished as a default x=y=z=1 compute dispatch --
// pack_task_record/unpack_task_record (vulkan_device_hal.cpp) never read
// task.kind, so without this check the task would fall straight through the
// GPU task-ring publication path. Same START.md §4 invariant 10 contract
// reference/Metal already enforce for index_count > 0 (see
// reference_raster_test.cpp / metal_task_timeline_test.cpp's indexed-draw
// sub-case). Compile-review-only here since no Linux/NVIDIA hardware is
// available to actually run this binary.
bool run_raster_rejected(const std::string& root) {
  (void)root;
  std::string device_error;
  auto vulkan_device = vg::vulkan::make_device_hal(&device_error);
  if (vulkan_device == nullptr) {
    std::cerr << "raster-rejected: no Vulkan device available on this host: " << device_error << "\n";
    return false;
  }

  vg::core::Arena arena;
  const auto module = make_probe_module(arena);

  // An otherwise-default TaskRecord is enough to reach the kind==Raster
  // rejection: TaskGraph::validate_execution() (run inside plan.validate(),
  // ahead of this check) only requires the graph to be sealed/published with
  // non-zero node/root generation, both of which default to 1, and never
  // inspects FacetRef contents.
  TaskRecord raster_task{};
  raster_task.kind = vg::core::TaskKind::Raster;
  vg::hal::ExecutionPlan plan;
  std::string error;
  if (!assemble_compute_plan(arena, module, {raster_task}, &plan, &error)) {
    std::cerr << "raster-rejected: plan assembly failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan compiled;
  if (vulkan_device->compile(plan, &compiled, &error)) {
    std::cerr << "raster-rejected: compile() unexpectedly accepted a Raster-kind task\n";
    return false;
  }
  if (error != "raster tasks not supported on Vulkan backend") {
    std::cerr << "raster-rejected: unexpected error message: " << error << "\n";
    return false;
  }
  if (compiled.report.supported) {
    std::cerr << "raster-rejected: report.supported should be false\n";
    return false;
  }
  bool found_unsupported_event = false;
  for (const auto& event : compiled.report.events) {
    if (event.operation == "raster_task" && event.classification == vg::hal::LoweringClass::Unsupported) {
      found_unsupported_event = true;
      break;
    }
  }
  if (!found_unsupported_event) {
    std::cerr << "raster-rejected: missing Unsupported raster_task LoweringEvent\n";
    return false;
  }
  std::cout << "raster-rejected: ok\n";
  return true;
}

// F3 (ADR-043 Decision #4): the restricted-import raster contract is placed
// in a real CodeObject/Node by assemble_single_user_raster_plan, producing
// an assembled plan whose resolved node carries the shader contract. Vulkan
// intentionally projects that transitional raster shape to its explicit
// Unsupported path before task-ring packing; it must retain the same named
// raster_task diagnostic rather than reinterpret it as compute.
bool run_raster_msl_rejected(const std::string& root) {
  (void)root;
  std::string device_error;
  auto vulkan_device = vg::vulkan::make_device_hal(&device_error);
  if (vulkan_device == nullptr) {
    std::cerr << "raster-msl-rejected: no Vulkan device available on this host: " << device_error << "\n";
    return false;
  }

  // An otherwise-default TaskRecord is enough to reach the kind==Raster
  // rejection, same as raster-rejected above.
  TaskRecord raster_task{};
  raster_task.kind = vg::core::TaskKind::Raster;
  vg::core::Arena arena;
  // Do not hand-build a plan with an empty module here: the fixture below
  // materializes the imported contract into the resolved Node snapshot.
  const vg::ir::UserRasterShaderContract shader{
      "vg.test.raster/v1", "vg_test_vertex", "vg_test_fragment",
      vg::ir::kRasterVertexAbiXyzuvPackedV1,
      "#version 450\nvoid main() {}\n"};
  vg::test_support::AssembledPlanFixture fixture;
  vg::hal::ExecutionPlan plan;
  std::string error;
  if (!vg::test_support::assemble_single_user_raster_plan(
          arena, shader, {raster_task}, &fixture, &plan, &error)) {
    std::cerr << "raster-msl-rejected: plan assembly failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan compiled;
  if (vulkan_device->compile(plan, &compiled, &error)) {
    std::cerr << "raster-msl-rejected: compile() unexpectedly accepted a user_raster_shader Raster-kind task\n";
    return false;
  }
  if (error != "raster tasks not supported on Vulkan backend") {
    std::cerr << "raster-msl-rejected: unexpected error message: " << error << "\n";
    return false;
  }
  if (compiled.report.supported) {
    std::cerr << "raster-msl-rejected: report.supported should be false\n";
    return false;
  }
  bool found_unsupported_event = false;
  for (const auto& event : compiled.report.events) {
    if (event.operation == "raster_task" && event.classification == vg::hal::LoweringClass::Unsupported) {
      found_unsupported_event = true;
      break;
    }
  }
  if (!found_unsupported_event) {
    std::cerr << "raster-msl-rejected: missing Unsupported raster_task LoweringEvent\n";
    return false;
  }
  std::cout << "raster-msl-rejected: ok\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: vg_vulkan_task_timeline_test "
                 "<task-tier0|timeline|raster-rejected|raster-msl-rejected> <repo_root>\n";
    return 2;
  }
  const std::string mode = argv[1];
  const std::string root = argv[2];
  bool ok = false;
  if (mode == "task-tier0") {
    ok = run_task_tier0(root);
  } else if (mode == "timeline") {
    ok = run_timeline(root);
  } else if (mode == "raster-rejected") {
    ok = run_raster_rejected(root);
  } else if (mode == "raster-msl-rejected") {
    ok = run_raster_msl_rejected(root);
  } else {
    std::cerr << "unknown mode: " << mode << "\n";
    return 2;
  }
  return ok ? 0 : 1;
}
