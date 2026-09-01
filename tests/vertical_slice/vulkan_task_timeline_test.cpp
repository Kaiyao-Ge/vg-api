#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "backends/vulkan/vulkan_device_hal.h"
#include "ir/ir.h"
#include "../support/assembled_plan_fixture.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using vg::core::TaskGraph;
using vg::core::TaskGraphBuilder;
using vg::core::TaskRecord;

bool assemble_compute_plan(vg::core::Arena& arena, vg::ir::Module module,
                           std::vector<TaskRecord> tasks, vg::core::ExecutionPlan* out,
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

vg::ir::Module make_store_module(const vg::core::Allocation& allocation, int64_t value) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test.node-aware-store/v1";
  vg::ir::Instruction instruction;
  instruction.op = "store";
  instruction.allocation = allocation.id;
  instruction.offset = 0;
  instruction.size = 4;
  instruction.value = value;
  instruction.generation = allocation.generation;
  instruction.representation_epoch = allocation.representation_epoch;
  module.instructions.push_back(instruction);
  module.declared_effects.push_back(
      {allocation.id, 0, 4, vg::ir::Access::Write, allocation.representation_epoch});
  return module;
}

vg::ir::Module make_atomic_module(const vg::core::Allocation& allocation, int64_t value) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test.node-aware-atomic/v1";
  vg::ir::Instruction instruction;
  instruction.op = "atomic_add";
  instruction.allocation = allocation.id;
  instruction.offset = 0;
  instruction.size = 8;
  instruction.value = value;
  instruction.generation = allocation.generation;
  instruction.representation_epoch = allocation.representation_epoch;
  module.instructions.push_back(instruction);
  module.declared_effects.push_back(
      {allocation.id, 0, 8, vg::ir::Access::Atomic, allocation.representation_epoch});
  return module;
}

uint64_t event_count(const vg::hal::LoweringReport& report, const std::string& operation) {
  uint64_t count = 0;
  for (const auto& event : report.events)
    if (event.operation == operation) count += event.count;
  return count;
}

uint64_t event_count(const vg::hal::LoweringReport& report, const std::string& operation,
                     vg::hal::LoweringClass classification) {
  uint64_t count = 0;
  for (const auto& event : report.events)
    if (event.operation == operation && event.classification == classification)
      count += event.count;
  return count;
}

bool has_event_reason(const vg::hal::LoweringReport& report, const std::string& operation,
                      const std::string& needle) {
  return std::ranges::any_of(report.events, [&](const auto& event) {
    return event.operation == operation && event.reason.find(needle) != std::string::npos;
  });
}

uint64_t load_u64(const vg::core::Allocation& allocation) {
  uint64_t value = 0;
  std::memcpy(&value, allocation.bytes.data(), sizeof(value));
  return value;
}

bool same_task(const TaskRecord& a, const TaskRecord& b) {
  return a.node_index == b.node_index && a.node_generation == b.node_generation &&
         a.root_allocation == b.root_allocation && a.root_generation == b.root_generation && a.x == b.x &&
         a.y == b.y && a.z == b.z && a.flags == b.flags && a.contract_index == b.contract_index &&
         a.payload_size == b.payload_size && a.payload_or_offset == b.payload_or_offset;
}

// Per-Node Stage 6 and per-Task Stage 7 conformance. This stays in the existing
// task-tier0 executable so Linux hardware runs it automatically without a new
// CMake test lane.
bool run_task_tier0(const std::string& root) {
  (void)root;
  std::string device_error;
  auto vulkan_device = vg::vulkan::make_device_hal(&device_error);
  if (vulkan_device == nullptr) {
    std::cerr << "task-tier0: no Vulkan device available on this host: " << device_error << "\n";
    return false;
  }
  if (!vulkan_device->capabilities().supports(vg::hal::Capability::EffectDag)) {
    std::cerr << "task-tier0: Vulkan device cannot lower multi-Task effect dependencies\n";
    return false;
  }

  // Storage order is [atomic, store], while the dependency seals execution
  // order [store, atomic]. The Tasks use distinct Node packages/pipelines.
  vg::core::Arena arena;
  auto& shared = arena.allocate(64);
  TaskRecord atomic_task{};
  atomic_task.root_allocation = shared.id;
  atomic_task.root_generation = shared.generation;
  atomic_task.x = 2;
  atomic_task.y = 1;
  atomic_task.z = 3;
  TaskRecord store_task = atomic_task;
  store_task.x = 3;
  store_task.y = 2;
  store_task.z = 1;
  store_task.flags = 7;
  store_task.contract_index = 3;
  store_task.payload_or_offset = 0x1'0000'0001ULL;
  vg::test_support::MultiNodePlanFixture multi_fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  if (!vg::test_support::assemble_multi_node_plan(
          arena, {make_atomic_module(shared, 1), make_store_module(shared, 1)},
          {atomic_task, store_task}, {{1, 0}}, &multi_fixture, &plan, &error)) {
    std::cerr << "task-tier0: plan assembly failed: " << error << "\n";
    return false;
  }
  if (plan.task_order != std::vector<uint32_t>({1, 0})) {
    std::cerr << "task-tier0: assembler did not seal reverse storage order\n";
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
  if (compiled.per_node_packages.size() != 2 ||
      event_count(compiled.report, "node_compute_package") != 2 ||
      event_count(compiled.report, "vulkan_pipeline") != 2 ||
      event_count(compiled.report, "task_effect_barrier") != 1) {
    std::cerr << "task-tier0: Stage 6 per-Node report is not exact\n";
    return false;
  }

  vg::hal::CompiledPlan cached_compiled;
  if (!vulkan_device->compile(plan, &cached_compiled, &error) ||
      event_count(cached_compiled.report, "vulkan_pipeline",
                  vg::hal::LoweringClass::CachedObject) != 2) {
    std::cerr << "task-tier0: repeated compile did not reuse both Node pipelines\n";
    return false;
  }

  vg::hal::Submission submission;
  if (!vulkan_device->submit(cached_compiled, arena, &submission, &error)) {
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
  if (load_u64(shared) != 0x01010102ULL) {
    std::cerr << "task-tier0: WAW/atomic result does not match sealed task_order\n";
    return false;
  }
  if (event_count(submission.report, "vulkan_task_dispatch") != 2 ||
      !has_event_reason(submission.report, "vulkan_task_dispatch", "task=1") ||
      !has_event_reason(submission.report, "vulkan_task_dispatch", "groups=3x2x1") ||
      !has_event_reason(submission.report, "vulkan_task_dispatch", "task=0") ||
      !has_event_reason(submission.report, "vulkan_task_dispatch", "groups=2x1x3") ||
      submission.report.barrier_count != 2 ||
      submission.report.command_buffer_count != 2 ||
      submission.report.encoder_count != 2 ||
      submission.report.queue_wait_count != 2) {
    std::cerr << "task-tier0: Stage 7 command instrumentation is not exact\n";
    return false;
  }

  // A sealed Explicit edge is an execution dependency even when the Tasks'
  // memory ranges do not conflict. Vulkan must consume that structural edge
  // directly and emit the same one barrier Stage 6 reported.
  vg::core::Arena explicit_arena;
  auto& explicit_left = explicit_arena.allocate(64);
  auto& explicit_right = explicit_arena.allocate(64);
  TaskRecord explicit_first{};
  explicit_first.root_allocation = explicit_left.id;
  explicit_first.root_generation = explicit_left.generation;
  explicit_first.x = 2;
  explicit_first.y = 2;
  explicit_first.z = 1;
  TaskRecord explicit_second{};
  explicit_second.root_allocation = explicit_right.id;
  explicit_second.root_generation = explicit_right.generation;
  explicit_second.x = 1;
  explicit_second.y = 3;
  explicit_second.z = 2;
  vg::test_support::MultiNodePlanFixture explicit_fixture;
  vg::core::ExecutionPlan explicit_plan;
  if (!vg::test_support::assemble_multi_node_plan(
          explicit_arena,
          {make_store_module(explicit_left, 2), make_store_module(explicit_right, 3)},
          {explicit_first, explicit_second}, {{0, 1}}, &explicit_fixture,
          &explicit_plan, &error) ||
      explicit_plan.validated_effect_graph.edges().size() != 1 ||
      explicit_plan.validated_effect_graph.edges().front().kind !=
          vg::core::EffectEdgeKind::Explicit) {
    std::cerr << "task-tier0: disjoint explicit-dependency plan assembly failed: "
              << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan explicit_compiled;
  vg::hal::Submission explicit_submission;
  if (!vulkan_device->compile(explicit_plan, &explicit_compiled, &error) ||
      event_count(explicit_compiled.report, "task_effect_barrier") != 1 ||
      !vulkan_device->submit(explicit_compiled, explicit_arena,
                             &explicit_submission, &error) ||
      !explicit_submission.result.ok ||
      event_count(explicit_submission.report, "task_effect_barrier") != 1 ||
      explicit_submission.report.barrier_count != 2 ||
      load_u64(explicit_left) != 0x02020202ULL ||
      load_u64(explicit_right) != 0x03030303ULL) {
    std::cerr << "task-tier0: disjoint Explicit edge did not produce one "
                 "canonical barrier: "
              << error << "\n";
    return false;
  }

  // One Node used by two Tasks must still execute twice.
  vg::core::Arena same_node_arena;
  auto& counter = same_node_arena.allocate(64);
  const auto atomic_module = make_atomic_module(counter, 1);
  TaskRecord first{};
  first.root_allocation = counter.id;
  first.root_generation = counter.generation;
  first.x = 4;
  first.y = 1;
  first.z = 2;
  TaskRecord second = first;
  second.x = 2;
  second.y = 3;
  second.z = 1;
  const std::vector<std::pair<uint32_t, uint32_t>> same_dependencies{{0, 1}};
  vg::test_support::AssemblyOptions same_options;
  same_options.dependencies = &same_dependencies;
  vg::core::ExecutionPlan same_plan;
  if (!assemble_compute_plan(same_node_arena, atomic_module, {first, second},
                             &same_plan, &error, same_options)) {
    std::cerr << "task-tier0: same-Node plan assembly failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan same_compiled;
  vg::hal::Submission same_submission;
  if (!vulkan_device->compile(same_plan, &same_compiled, &error) ||
      same_compiled.per_node_packages.size() != 1 ||
      !vulkan_device->submit(same_compiled, same_node_arena, &same_submission, &error) ||
      !same_submission.result.ok || load_u64(counter) != 2 ||
      event_count(same_submission.report, "vulkan_task_dispatch") != 2 ||
      !has_event_reason(same_submission.report, "vulkan_task_dispatch", "groups=4x1x2") ||
      !has_event_reason(same_submission.report, "vulkan_task_dispatch", "groups=2x3x1")) {
    std::cerr << "task-tier0: same Node did not dispatch once per Task: " << error << "\n";
    return false;
  }

  // All package identity failures must happen before execution.
  const uint64_t counter_before_tamper = load_u64(counter);
  const auto expect_tamper_rejected = [&](vg::hal::CompiledPlan tampered,
                                          const char* label) {
    vg::hal::Submission rejected;
    std::string rejection;
    if (vulkan_device->submit(tampered, same_node_arena, &rejected, &rejection) ||
        load_u64(counter) != counter_before_tamper) {
      std::cerr << "task-tier0: tampered " << label
                << " package was not rejected before execution\n";
      return false;
    }
    return true;
  };
  auto missing = same_compiled;
  missing.per_node_packages.clear();
  if (!expect_tamper_rejected(std::move(missing), "missing")) return false;
  auto duplicate = same_compiled;
  duplicate.per_node_packages.push_back(duplicate.per_node_packages.front());
  if (!expect_tamper_rejected(std::move(duplicate), "duplicate")) return false;
  auto hash = same_compiled;
  hash.per_node_packages.front().package->canonical_ir_hash += "-tampered";
  if (!expect_tamper_rejected(std::move(hash), "hash")) return false;
  auto generation = same_compiled;
  ++generation.per_node_packages.front().ref.generation;
  if (!expect_tamper_rejected(std::move(generation), "generation")) return false;
  auto kind = same_compiled;
  kind.per_node_packages.front().kind = vg::hal::CompiledPlan::NodePackageKind::Raster;
  if (!expect_tamper_rejected(std::move(kind), "kind")) return false;
  auto bindings = same_compiled;
  ++bindings.per_node_packages.front().package->bindings.front().binding;
  if (!expect_tamper_rejected(std::move(bindings), "bindings")) return false;

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
  vg::core::ExecutionPlan signal_plan;
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
  vg::core::ExecutionPlan wait_plan;
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
  vg::core::ExecutionPlan stuck_plan;
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
// silently republished as a default x=y=z=1 compute dispatch. The shared ring
// codec is now compute-only and would reject it too, but this earlier Stage-6
// check owns Vulkan's stable capability/Unsupported diagnostic. Same START.md
// §4 invariant 10 contract
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
  vg::core::ExecutionPlan plan;
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
  vg::core::ExecutionPlan plan;
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
