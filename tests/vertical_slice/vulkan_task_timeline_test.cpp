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

bool make_raster_task(vg::hal::DeviceHal& device, vg::core::Arena& arena,
                      TaskRecord* task, std::string* error) {
  task->kind = vg::core::TaskKind::Raster;
  auto acquire = [&](vg::core::FacetKind kind, uint32_t width, uint32_t height,
                     vg::core::FacetRef* ref) {
    auto& allocation = arena.allocate(width * height * 4);
    vg::core::CanonicalView view;
    view.allocation = allocation.id;
    view.allocation_generation = allocation.generation;
    view.format = vg::core::PixelFormat::RGBA8Unorm;
    view.dimension = vg::core::ViewDimension::Texture2D;
    view.width = width;
    view.height = height;
    return device.facet_pool().acquire(arena, view, kind, ref, error);
  };
  return acquire(vg::core::FacetKind::Sample, 2, 2, &task->raster_facets.source) &&
         acquire(vg::core::FacetKind::Attachment, 2, 2, &task->raster_facets.target) &&
         acquire(vg::core::FacetKind::Address, 15, 1, &task->vertex_buffer_ref);
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
  auto missing_transition = same_compiled;
  missing_transition.transition_operations.clear();
  if (!expect_tamper_rejected(std::move(missing_transition), "missing transition")) return false;
  auto fake_physical = same_compiled;
  for (auto& transition : fake_physical.transition_operations) {
    transition.barrier_count = 0;
    transition.serialized_fallback = false;
  }
  fake_physical.report.transition_barrier_count = 0;
  fake_physical.report.transition_serialized_fallback_count = 0;
  if (!expect_tamper_rejected(std::move(fake_physical), "omitted physical barrier")) return false;
  auto wave_tamper = same_compiled;
  wave_tamper.plan.execution_schedule.components[0].waves[0].tasks.clear();
  if (!expect_tamper_rejected(std::move(wave_tamper), "schedule wave")) return false;

  // Ready frontier {0,1} -> {2} -> {3}, plus independent component {4}.
  // Five Nodes, three waves in one component, two transitions regardless of
  // how many Task edges the frontiers contain. The adapter consumes waves,
  // not a producer-by-producer reconstruction of the EffectGraph.
  vg::core::Arena wave_arena;
  std::vector<vg::ir::Module> wave_modules;
  std::vector<TaskRecord> wave_tasks;
  std::vector<uint64_t> wave_allocations;
  for (uint32_t i = 0; i < 5; ++i) {
    auto& allocation = wave_arena.allocate(64);
    wave_allocations.push_back(allocation.id);
    wave_modules.push_back(make_store_module(allocation, i + 2));
    wave_tasks.push_back(vg::test_support::compute_task(allocation.id, allocation.generation));
  }
  vg::test_support::MultiNodePlanFixture wave_fixture;
  vg::core::ExecutionPlan wave_plan;
  vg::hal::CompiledPlan wave_compiled;
  vg::hal::Submission wave_submission;
  if (!vg::test_support::assemble_multi_node_plan(wave_arena, wave_modules, wave_tasks,
          {{0, 2}, {1, 2}, {2, 3}}, &wave_fixture, &wave_plan, &error) ||
      wave_plan.execution_schedule.components.size() != 2 ||
      wave_plan.execution_schedule.components[0].waves.size() != 3 ||
      !vulkan_device->compile(wave_plan, &wave_compiled, &error) ||
      wave_compiled.report.transition_barrier_count != 2 ||
      wave_compiled.report.transition_serialized_fallback_count != 2 ||
      !vulkan_device->submit(wave_compiled, wave_arena, &wave_submission, &error) ||
      !wave_submission.result.ok || wave_submission.report.transition_barrier_count != 2 ||
      wave_submission.report.transition_serialized_fallback_count != 2 ||
      wave_submission.report.barrier_count != 3 ||
      event_count(wave_submission.report, "vulkan_task_dispatch") != 5 ||
      wave_submission.published_tasks.size() != 5) {
    std::cerr << "task-tier0: sealed component/wave execution failed: " << error << "\n";
    return false;
  }
  for (uint32_t i = 0; i < 5; ++i) {
    const auto* output = wave_arena.lookup(vg::core::PointerRef{wave_allocations[i], 1});
    if (output == nullptr || output->bytes[0] != i + 2 ||
        !same_task(wave_submission.published_tasks[i],
                   wave_plan.task_graph.tasks()[wave_plan.execution_schedule.task_order[i]])) return false;
  }

  // Envelope filters observation/publication only, using the shared sealed
  // canonical suffix machinery, not ring slot order or a backend quota copy.
  vg::core::Arena quota_arena;
  auto& quota_output = quota_arena.allocate(64);
  const auto quota_module = make_store_module(quota_output, 7);
  const auto quota_task = probe_task(quota_module);
  std::vector<TaskRecord> quota_tasks(3, quota_task);
  for (uint32_t i = 0; i < quota_tasks.size(); ++i) quota_tasks[i].flags = i;
  vg::test_support::AssemblyOptions quota_options;
  quota_options.task_quota = 1;
  quota_options.timeline_signal = 1;
  vg::core::ExecutionPlan quota_plan;
  vg::hal::CompiledPlan quota_compiled;
  vg::hal::Submission quota_submission;
  if (!assemble_compute_plan(quota_arena, quota_module, quota_tasks,
          &quota_plan, &error, quota_options) ||
      !vulkan_device->compile(quota_plan, &quota_compiled, &error) ||
      !vulkan_device->submit(quota_compiled, quota_arena, &quota_submission, &error) ||
      !quota_submission.result.ok || quota_submission.published_tasks.size() != 1 ||
      !quota_submission.envelope_overflow.has_value() ||
      quota_submission.envelope_overflow->overflow_task_count != 2 ||
      quota_submission.timeline_value != 1 || load_u64(quota_output) != 0x07070707ULL ||
      event_count(quota_submission.report, "vulkan_task_dispatch") != 3 ||
      !same_task(quota_submission.published_tasks[0], quota_plan.task_graph.tasks()[0])) {
    std::cerr << "task-tier0: Envelope canonical publication split failed: " << error << "\n";
    return false;
  }
  const auto valid_overflow = *quota_submission.envelope_overflow;

  // Obtain a real device continuation from an alternate, reverse-order graph.
  // Its legitimate suffix [1,0] is not the canonical suffix [1,2] of this graph.
  // Neither plan is manually modified or stamped after assembly.
  const std::vector<std::pair<uint32_t, uint32_t>> reverse_edges{{2, 1}, {1, 0}};
  auto alternate_options = quota_options;
  alternate_options.timeline_signal = 0;
  alternate_options.dependencies = &reverse_edges;
  vg::core::ExecutionPlan alternate_plan;
  vg::hal::CompiledPlan alternate_compiled;
  vg::hal::Submission alternate_submission;
  if (!assemble_compute_plan(quota_arena, quota_module, quota_tasks, &alternate_plan,
                             &error, alternate_options) ||
      !vulkan_device->compile(alternate_plan, &alternate_compiled, &error) ||
      !vulkan_device->submit(alternate_compiled, quota_arena, &alternate_submission, &error) ||
      !alternate_submission.result.ok || !alternate_submission.envelope_overflow.has_value()) {
    std::cerr << "task-tier0: alternate continuation setup failed: " << error << "\n";
    return false;
  }

  // Every refused continuation carries both an observable store and a valid
  // representation transform on a separate allocation. Refusal must precede
  // bytes, epoch/facet lifecycle, holds, and Timeline effects, not merely precede
  // the physical publication ring. The accepted resume below signals the same
  // point to prove the refused submits never advanced the device Timeline.
  auto& representation_source = quota_arena.allocate(64);
  vg::core::RepresentationRequest representation;
  representation.view.allocation = representation_source.id;
  representation.view.allocation_generation = representation_source.generation;
  representation.view.format = vg::core::PixelFormat::RGBA8Unorm;
  representation.view.dimension = vg::core::ViewDimension::Texture2D;
  representation.view.width = representation.view.height = 2;
  const std::vector<vg::core::RepresentationRequest> representations{representation};
  const auto reject_before_effects = [&](const vg::core::EnvelopeOverflow& pending,
                                        const char* expected, uint64_t signal) {
    auto options = vg::test_support::AssemblyOptions{};
    options.pending_overflow = &pending;
    options.timeline_signal = signal;
    options.representation_requests = &representations;
    options.facet_pool = &vulkan_device->facet_pool();
    vg::core::ExecutionPlan rejected_plan;
    vg::hal::CompiledPlan rejected_compiled;
    vg::hal::Submission rejected_submission;
    std::fill(quota_output.bytes.begin(), quota_output.bytes.end(), 0x55);
    const auto before_output = quota_output.bytes;
    const auto before_representation = representation_source.bytes;
    const auto before_epoch = representation_source.representation_epoch;
    std::vector<uint32_t> before_generations;
    vulkan_device->facet_pool().snapshot_generations(&before_generations);
    std::string rejection_error;
    if (!assemble_compute_plan(quota_arena, quota_module, quota_tasks, &rejected_plan,
                               &rejection_error, options) ||
        !vulkan_device->compile(rejected_plan, &rejected_compiled, &rejection_error)) {
      std::cerr << "task-tier0: refusal fixture must reach submit: " << rejection_error << "\n";
      return false;
    }
    const bool accepted = vulkan_device->submit(rejected_compiled, quota_arena,
                                               &rejected_submission, &rejection_error);
    std::vector<uint32_t> after_generations;
    vulkan_device->facet_pool().snapshot_generations(&after_generations);
    if (accepted || rejection_error != expected || quota_output.bytes != before_output ||
        representation_source.bytes != before_representation ||
        representation_source.representation_epoch != before_epoch ||
        before_generations != after_generations ||
        quota_output.in_flight != 0 || representation_source.in_flight != 0 ||
        !rejected_submission.published_tasks.empty() ||
        !rejected_submission.representation_facets.empty() ||
        rejected_submission.result.poison == vg::core::PoisonState::PartiallyProduced ||
        rejected_submission.report.barrier_count != 0 ||
        rejected_submission.report.command_buffer_count != 0 ||
        rejected_submission.report.encoder_count != 0 ||
        rejected_submission.report.queue_wait_count != 0 ||
        rejected_submission.report.transition_barrier_count != 0 ||
        rejected_submission.report.transition_serialized_fallback_count != 0 ||
        event_count(rejected_submission.report, "vulkan_task_dispatch") != 0 ||
        event_count(rejected_submission.report, "task_publication_dispatch") != 0 ||
        event_count(rejected_submission.report, "timeline") != 0) {
      std::cerr << "task-tier0: continuation refusal had side effects or wrong result: "
                << rejection_error << "\n";
      return false;
    }
    return true;
  };
  auto rejected = valid_overflow;
  rejected.disposition = vg::core::EnvelopeOverflowDisposition::Rejected;
  rejected.continuation_token = 0;
  auto unknown = valid_overflow;
  unknown.continuation_token += 100;
  if (!reject_before_effects(rejected, "envelope leftover was rejected", 2) ||
      !reject_before_effects(unknown, "envelope continuation token does not match", 2) ||
      !reject_before_effects(*alternate_submission.envelope_overflow,
          "envelope continuation leftover is not the canonical schedule suffix", 2) ||
      !vulkan_device->envelope_continuations().contains(valid_overflow.continuation_token) ||
      !vulkan_device->envelope_continuations().contains(
          alternate_submission.envelope_overflow->continuation_token)) return false;

  // Refusal fixtures leave the sentinel intact; the store updates only bytes
  // [0,4). Compare the whole allocation so untouched bytes must stay 0x55.
  std::vector<uint8_t> expected_recovered_output(quota_output.bytes.size(), 0x55);
  std::fill_n(expected_recovered_output.begin(), 4, 0x07);
  vg::test_support::AssemblyOptions resume_options;
  resume_options.pending_overflow = &valid_overflow;
  resume_options.timeline_signal = 2;
  vg::core::ExecutionPlan resume_plan;
  vg::hal::CompiledPlan resume_compiled;
  vg::hal::Submission resume_submission;
  if (!assemble_compute_plan(quota_arena, quota_module, quota_tasks,
          &resume_plan, &error, resume_options) ||
      !vulkan_device->compile(resume_plan, &resume_compiled, &error) ||
      !vulkan_device->submit(resume_compiled, quota_arena, &resume_submission, &error) ||
      !resume_submission.result.ok || resume_submission.published_tasks.size() != 2 ||
      resume_submission.envelope_overflow.has_value() || resume_submission.timeline_value != 2 ||
      quota_output.bytes != expected_recovered_output ||
      event_count(resume_submission.report, "vulkan_task_dispatch") != 3 ||
      !same_task(resume_submission.published_tasks[0], resume_plan.task_graph.tasks()[1]) ||
      !same_task(resume_submission.published_tasks[1], resume_plan.task_graph.tasks()[2]) ||
      vulkan_device->envelope_continuations().contains(valid_overflow.continuation_token)) {
    std::cerr << "task-tier0: Envelope canonical suffix resume failed: " << error << "\n";
    return false;
  }
  if (!reject_before_effects(valid_overflow, "envelope continuation token does not match", 3))
    return false;
  vg::test_support::AssemblyOptions after_rejection_options;
  after_rejection_options.timeline_signal = 3;
  vg::core::ExecutionPlan after_rejection_plan;
  vg::hal::CompiledPlan after_rejection_compiled;
  vg::hal::Submission after_rejection_submission;
  if (!assemble_compute_plan(quota_arena, quota_module, quota_tasks, &after_rejection_plan,
                             &error, after_rejection_options) ||
      !vulkan_device->compile(after_rejection_plan, &after_rejection_compiled, &error) ||
      !vulkan_device->submit(after_rejection_compiled, quota_arena,
                             &after_rejection_submission, &error) ||
      !after_rejection_submission.result.ok || after_rejection_submission.timeline_value != 3 ||
      after_rejection_submission.published_tasks.size() != 3 ||
      quota_output.bytes != expected_recovered_output || quota_output.in_flight != 0) {
    std::cerr << "task-tier0: consumed-token refusal changed Timeline or lifetime: " << error << "\n";
    return false;
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
  if (stuck_submission.result.ok || stuck_submission.result.fault.code != "TIMELINE_WAIT_UNSATISFIED" ||
      stuck_submission.report.transition_barrier_count != 0 ||
      stuck_submission.report.command_buffer_count != 0 ||
      event_count(stuck_submission.report, "vulkan_task_dispatch") != 0 ||
      event_count(stuck_submission.report, "timeline") != 0 || !stuck_submission.published_tasks.empty()) {
    std::cerr << "timeline: unsatisfied wait did not fault as expected\n";
    return false;
  }
  std::cout << "timeline: ok\n";
  return true;
}

// Stage 7 executes a mixed compute/raster plan in the Core-sealed order and
// publishes both the compute side effect and the rendered attachment bytes.
bool run_raster_basic(const std::string& root) {
  (void)root;
  std::string device_error;
  auto vulkan_device = vg::vulkan::make_device_hal(&device_error);
  if (vulkan_device == nullptr) {
    std::cerr << "raster-basic: no Vulkan device available on this host: " << device_error << "\n";
    return false;
  }
  if (!vulkan_device->capabilities().supports(vg::hal::Capability::Raster)) {
    std::cerr << "raster-basic: Vulkan device does not advertise Raster\n";
    return false;
  }

  vg::core::Arena arena;
  const auto raster_module = make_probe_module(arena);
  TaskRecord raster_task{};
  std::string error;
  if (!make_raster_task(*vulkan_device, arena, &raster_task, &error)) {
    std::cerr << "raster-basic: facet setup failed: " << error << "\n";
    return false;
  }
  const auto* source_slot = vulkan_device->facet_pool().lookup(arena, raster_task.raster_facets.source);
  const auto* target_slot = vulkan_device->facet_pool().lookup(arena, raster_task.raster_facets.target);
  const auto* vertex_slot = vulkan_device->facet_pool().lookup(arena, raster_task.vertex_buffer_ref);
  if (source_slot == nullptr || target_slot == nullptr || vertex_slot == nullptr) {
    std::cerr << "raster-basic: live facet lookup failed\n";
    return false;
  }
  auto* source = arena.lookup(vg::core::PointerRef{source_slot->view.allocation, source_slot->view.allocation_generation});
  auto* target = arena.lookup(vg::core::PointerRef{target_slot->view.allocation, target_slot->view.allocation_generation});
  auto* vertex = arena.lookup(vg::core::PointerRef{vertex_slot->view.allocation, vertex_slot->view.allocation_generation});
  if (source == nullptr || target == nullptr || vertex == nullptr) {
    std::cerr << "raster-basic: backing allocation lookup failed\n";
    return false;
  }
  for (size_t i = 0; i < source->bytes.size(); i += 4) {
    source->bytes[i] = 255;
    source->bytes[i + 1] = 32;
    source->bytes[i + 2] = 16;
    source->bytes[i + 3] = 255;
  }
  const vg::reference::RasterVertex triangle[3] = {
      {-1.0f, -1.0f, 0.5f, 0.0f, 0.0f},
      {3.0f, -1.0f, 0.5f, 1.0f, 0.0f},
      {-1.0f, 3.0f, 0.5f, 0.0f, 1.0f},
  };
  std::memcpy(vertex->bytes.data(), triangle, sizeof(triangle));
  arena.mark_content_modified(*source);
  arena.mark_content_modified(*vertex);

  auto& compute_output = arena.allocate(64);
  TaskRecord compute_task{};
  compute_task.root_allocation = compute_output.id;
  compute_task.root_generation = compute_output.generation;
  vg::test_support::MultiNodePlanFixture fixture;
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions options;
  options.facet_pool = &vulkan_device->facet_pool();
  if (!vg::test_support::assemble_multi_node_plan(
          arena, {make_store_module(compute_output, 9), raster_module},
          {compute_task, raster_task}, {{0, 1}}, &fixture, &plan, &error, options)) {
    std::cerr << "raster-basic: plan assembly failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan compiled;
  if (!vulkan_device->compile(plan, &compiled, &error)) {
    std::cerr << "raster-basic: compile failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission submission;
  if (!vulkan_device->submit(compiled, arena, &submission, &error)) {
    std::cerr << "raster-basic: submit failed: " << error << "\n";
    return false;
  }
  uint32_t stored = 0;
  std::memcpy(&stored, compute_output.bytes.data(), sizeof(stored));
  const bool rendered = std::any_of(target->bytes.begin(), target->bytes.end(),
                                    [](uint8_t value) { return value != 0; });
  if (!submission.result.ok || stored != 0x09090909u || !rendered ||
      submission.raster_results.size() != 1 ||
      submission.raster_results[0].resolved_rgba.size() != 4 ||
      submission.published_tasks.size() != 2 ||
      event_count(submission.report, "vulkan_raster_draw") == 0) {
    std::cerr << "raster-basic: Stage7 did not publish compute+raster results: ok="
              << submission.result.ok << " stored=" << stored
              << " rendered=" << rendered
              << " raster_results=" << submission.raster_results.size()
              << " published=" << submission.published_tasks.size()
              << " draw_events=" << event_count(submission.report, "vulkan_raster_draw")
              << " fault=" << submission.result.fault.code
              << " message=" << submission.result.message << "\n";
    return false;
  }
  std::cout << "raster-basic: ok\n";
  return true;
}

// F3 (ADR-043 Decision #4): the restricted-import raster contract is placed
// in a real CodeObject/Node by assemble_single_user_raster_plan, producing
// an assembled plan whose resolved node carries the shader contract. Vulkan
// rejects the actual Raster Node before task-ring packing, naming its complete
// NodeRef and domain rather than reinterpreting it as compute.
bool run_raster_msl_rejected(const std::string& root) {
  (void)root;
  std::string device_error;
  auto vulkan_device = vg::vulkan::make_device_hal(&device_error);
  if (vulkan_device == nullptr) {
    std::cerr << "raster-msl-rejected: no Vulkan device available on this host: " << device_error << "\n";
    return false;
  }

  // Raster facets must be valid before testing a backend capability rejection.
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
  if (!make_raster_task(*vulkan_device, arena, &raster_task, &error)) return false;
  vg::test_support::AssemblyOptions options;
  options.facet_pool = &vulkan_device->facet_pool();
  if (!vg::test_support::assemble_single_user_raster_plan(
          arena, shader, {raster_task}, &fixture, &plan, &error, options)) {
    std::cerr << "raster-msl-rejected: plan assembly failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan compiled;
  if (vulkan_device->compile(plan, &compiled, &error)) {
    std::cerr << "raster-msl-rejected: compile() unexpectedly accepted a user_raster_shader Raster-kind task\n";
    return false;
  }
  if (error.find("Vulkan user Raster requires code object format vg.glsl.raster/v1; vg.msl.raster/v1 is Unsupported") == std::string::npos) {
    std::cerr << "raster-msl-rejected: unexpected error message: " << error << "\n";
    return false;
  }
  if (compiled.report.supported) {
    std::cerr << "raster-msl-rejected: report.supported should be false\n";
    return false;
  }
  bool found_unsupported_event = false;
  for (const auto& event : compiled.report.events) {
    if (event.operation == "node_raster_package" && event.classification == vg::hal::LoweringClass::Unsupported &&
        event.reason.find("vg.msl.raster/v1 is Unsupported") != std::string::npos) {
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
                 "<task-tier0|timeline|raster-basic|raster-msl-rejected> <repo_root>\n";
    return 2;
  }
  const std::string mode = argv[1];
  const std::string root = argv[2];
  bool ok = false;
  if (mode == "task-tier0") {
    ok = run_task_tier0(root);
  } else if (mode == "timeline") {
    ok = run_timeline(root);
  } else if (mode == "raster-basic") {
    ok = run_raster_basic(root);
  } else if (mode == "raster-msl-rejected") {
    ok = run_raster_msl_rejected(root);
  } else {
    std::cerr << "unknown mode: " << mode << "\n";
    return 2;
  }
  return ok ? 0 : 1;
}
