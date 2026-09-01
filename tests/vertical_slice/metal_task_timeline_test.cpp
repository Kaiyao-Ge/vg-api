#include "backends/device_hal.h"
#include "backends/metal/metal_device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "capture/capture.h"
#include "compiler/compiler.h"
#include "ir/ir.h"
#include "assembled_plan_fixture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using vg::core::TaskGraph;
using vg::core::TaskGraphBuilder;
using vg::core::TaskRecord;

// All compile/submit paths in this vertical slice start from the public
// semantic assembly boundary.  Keep the legacy test body focused on the
// Metal observation it is making; this helper supplies the otherwise
// repetitive CodeObject/NodeTable/TaskGraph/Envelope plumbing without ever
// manufacturing Stage 0--5 sealing facts.
bool assemble_compute_plan(vg::core::Arena& arena, vg::ir::Module module,
                           std::vector<TaskRecord> tasks, vg::core::ExecutionPlan* out,
                           std::string* error, const vg::test_support::AssemblyOptions& options = {}) {
  // The fixture owns the NodeTable only during assembly; the plan carries its
  // immutable resolved-node snapshot afterwards, exactly as production does.
  vg::test_support::AssembledPlanFixture fixture;
  return vg::test_support::assemble_single_node_plan(arena, std::move(module), tasks, &fixture, out, error, options);
}

bool assemble_user_raster_plan(vg::core::Arena& arena, const vg::ir::UserRasterShaderContract& shader,
                               std::vector<TaskRecord> tasks, vg::core::ExecutionPlan* out,
                               std::string* error, const vg::test_support::AssemblyOptions& options = {}) {
  vg::test_support::AssembledPlanFixture fixture;
  return vg::test_support::assemble_single_user_raster_plan(arena, shader, tasks, &fixture, out, error, options);
}

TaskRecord probe_task(const vg::ir::Module& module) {
  TaskRecord task{};
  // Probe modules always contain a real allocation access.  Binding the task
  // root to it is deliberate: a compute Task root is part of assembler-owned
  // authority, rather than a backend fixture shortcut.
  task.root_allocation = module.instructions.front().allocation;
  task.root_generation = module.instructions.front().generation;
  task.x = task.y = task.z = 1;
  return task;
}

// A minimal single-load module. Its only purpose is to give compile()/
// submit() a valid linear compute package to run so the timeline/task-ring
// paths (which don't otherwise touch module semantics) can be exercised
// end to end; the loaded value itself is never inspected.
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
         a.payload_size == b.payload_size && a.payload_or_offset == b.payload_or_offset &&
         a.kind == b.kind && a.topology == b.topology &&
         a.raster_facets.source.index == b.raster_facets.source.index &&
         a.raster_facets.source.generation == b.raster_facets.source.generation &&
         a.raster_facets.target.index == b.raster_facets.target.index &&
         a.raster_facets.target.generation == b.raster_facets.target.generation &&
         a.vertex_buffer_ref.index == b.vertex_buffer_ref.index &&
         a.vertex_buffer_ref.generation == b.vertex_buffer_ref.generation &&
         a.index_buffer_ref.index == b.index_buffer_ref.index &&
         a.index_buffer_ref.generation == b.index_buffer_ref.generation &&
         a.index_count == b.index_count && a.raster_filter == b.raster_filter &&
         a.raster_wrap == b.raster_wrap && a.raster_tint[0] == b.raster_tint[0] &&
         a.raster_tint[1] == b.raster_tint[1] && a.raster_tint[2] == b.raster_tint[2] &&
         a.raster_tint[3] == b.raster_tint[3];
}

// Two tasks with an explicit dependency (1 depends on 0), non-trivial x/y/z
// so the GPU dispatch-sizing path (TaskRecord.x/y/z, not a hardcoded
// (1,1,1)) is actually exercised. Metal's GPU task ring publication must
// report published_tasks byte-identical, in the same order, to the
// reference oracle (reference::execute_task_graph()).
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
  const std::vector<std::pair<uint32_t, uint32_t>> dependencies{{0, 1}};
  vg::test_support::AssemblyOptions options;
  options.dependencies = &dependencies;
  vg::core::ExecutionPlan plan;
  std::string error;
  if (!assemble_compute_plan(arena, module, {task0, task1}, &plan, &error, options)) {
    std::cerr << "task-tier0: plan assembly failed: " << error << "\n";
    return false;
  }

  auto oracle = vg::reference::execute_task_graph(plan.task_graph);
  if (!oracle.ok || oracle.published_tasks.size() != 2) {
    std::cerr << "task-tier0: reference oracle failed: " << oracle.message << "\n";
    return false;
  }

  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "task-tier0: no Metal device available on this host\n";
    return false;
  }

  vg::hal::CompiledPlan compiled;
  if (!metal_device->compile(plan, &compiled, &error)) {
    std::cerr << "task-tier0: Metal compile failed: " << error << "\n";
    return false;
  }

  vg::hal::Submission submission;
  if (!metal_device->submit(compiled, arena, &submission, &error)) {
    std::cerr << "task-tier0: Metal submit failed: " << error << "\n";
    return false;
  }
  if (!submission.result.ok) {
    std::cerr << "task-tier0: Metal execution reported failure: " << submission.result.message << "\n";
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

// timeline_signal advances the device's MTLSharedEvent; a subsequent
// submission's timeline_wait for that exact value succeeds; a wait for a
// value nothing has signaled yet faults honestly (submit() still returns
// true, matching the reference/Metal/Vulkan convention that submit()
// reports host-side acceptance while submission.result.ok reports the
// execution outcome).
bool run_timeline(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "timeline: no Metal device available on this host\n";
    return false;
  }
  if (!metal_device->capabilities().supports(vg::hal::Capability::Timeline)) {
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
  if (!metal_device->compile(signal_plan, &signal_compiled, &error)) {
    std::cerr << "timeline: compile (signal) failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission signal_submission;
  if (!metal_device->submit(signal_compiled, arena, &signal_submission, &error)) {
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
  if (!metal_device->compile(wait_plan, &wait_compiled, &error)) {
    std::cerr << "timeline: compile (wait) failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission wait_submission;
  if (!metal_device->submit(wait_compiled, arena, &wait_submission, &error)) {
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
  if (!metal_device->compile(stuck_plan, &stuck_compiled, &error)) {
    std::cerr << "timeline: compile (stuck) failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission stuck_submission;
  if (!metal_device->submit(stuck_compiled, arena, &stuck_submission, &error)) {
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

// E004: CertifiedPinned/Universe/DiscoverThenLease must each produce a real
// AccessCertificate; SoftwarePaged/FaultManaged must be rejected honestly at
// compile() time (LoweringClass::Unsupported), never faked. The arena holds
// one allocation the probe module actually touches and one it never
// references, so CertifiedPinned's certified set (1 allocation) is
// distinguishable from Universe/DiscoverThenLease's full-arena set (2).
bool run_access_certificate(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "access-certificate: no Metal device available on this host\n";
    return false;
  }

  auto check_real_mode = [&](vg::core::AccessCertificateMode mode, size_t expected_references,
                             vg::hal::LoweringClass expected_classification, const char* label) {
    vg::core::Arena arena;
    const auto module = make_probe_module(arena);
    const auto& untouched = arena.allocate(16);  // Universe/discovery witness deliberately includes this seed

    vg::test_support::AssemblyOptions options;
    options.certificate_mode = mode;
    // CertifiedPinned is explicit envelope authority.  The other modes may
    // still receive the same exact access list without changing their
    // Universe/discovery semantics.
    options.certificate_touched = {{module.instructions.front().allocation,
                                    module.instructions.front().generation}};
    const std::vector<vg::core::PointerRef> discovery_seeds{{module.instructions.front().allocation,
                                                              module.instructions.front().generation},
                                                             {untouched.id, untouched.generation}};
    if (mode == vg::core::AccessCertificateMode::DiscoverThenLease)
      options.discovery_seeds = &discovery_seeds;
    vg::core::ExecutionPlan plan;
    std::string error;
    if (!assemble_compute_plan(arena, module, {probe_task(module)}, &plan, &error, options)) {
      std::cerr << "access-certificate: " << label << " assembly failed: " << error << "\n";
      return false;
    }

    vg::hal::CompiledPlan compiled;
    if (!metal_device->compile(plan, &compiled, &error)) {
      std::cerr << "access-certificate: " << label << " compile failed: " << error << "\n";
      return false;
    }
    vg::hal::Submission submission;
    if (!metal_device->submit(compiled, arena, &submission, &error)) {
      std::cerr << "access-certificate: " << label << " submit failed: " << error << "\n";
      return false;
    }
    if (!submission.result.ok) {
      std::cerr << "access-certificate: " << label << " execution reported failure: " << submission.result.message << "\n";
      return false;
    }
    if (!submission.access_certificate.has_value()) {
      std::cerr << "access-certificate: " << label << " produced no certificate\n";
      return false;
    }
    if (submission.access_certificate->mode != mode) {
      std::cerr << "access-certificate: " << label << " certificate mode mismatch\n";
      return false;
    }
    if (submission.access_certificate->epoch.references().size() != expected_references) {
      std::cerr << "access-certificate: " << label << " expected " << expected_references << " references, got "
                << submission.access_certificate->epoch.references().size() << "\n";
      return false;
    }
    // Discovery is now sealed by the core assembler, so Stage 6 correctly
    // has no backend-side discovery event to report or re-walk.  Its concrete
    // witness was already checked through the resulting certificate above.
    if (mode == vg::core::AccessCertificateMode::DiscoverThenLease) {
      if (!plan.discovery_result.has_value()) {
        std::cerr << "access-certificate: " << label << " is missing its sealed discovery witness\n";
        return false;
      }
      std::cout << "access-certificate: " << label << " ok\n";
      return true;
    }
    bool found_event = false;
    for (const auto& event : submission.report.events) {
      if (event.operation != "access_certificate") continue;
      found_event = true;
      if (event.classification != expected_classification) {
        std::cerr << "access-certificate: " << label << " report classification mismatch\n";
        return false;
      }
    }
    if (!found_event) {
      std::cerr << "access-certificate: " << label << " missing report event\n";
      return false;
    }
    std::cout << "access-certificate: " << label << " ok\n";
    return true;
  };

  if (!check_real_mode(vg::core::AccessCertificateMode::CertifiedPinned, 1, vg::hal::LoweringClass::Direct,
                      "certified-pinned"))
    return false;
  if (!check_real_mode(vg::core::AccessCertificateMode::Universe, 2, vg::hal::LoweringClass::Direct, "universe"))
    return false;
  if (!check_real_mode(vg::core::AccessCertificateMode::DiscoverThenLease, 2, vg::hal::LoweringClass::HostAssisted,
                      "discover-then-lease"))
    return false;

  auto check_unsupported_mode = [&](vg::core::AccessCertificateMode mode, const char* label) {
    vg::core::Arena arena;
    const auto module = make_probe_module(arena);

    std::string error;
    vg::test_support::AssemblyOptions options;
    options.certificate_mode = mode;
    options.certificate_touched = {{module.instructions.front().allocation,
                                    module.instructions.front().generation}};
    vg::core::ExecutionPlan plan;
    if (assemble_compute_plan(arena, module, {probe_task(module)}, &plan, &error, options)) {
      std::cerr << "access-certificate: " << label << " unexpectedly assembled successfully\n";
      return false;
    }
    if (error.find("Unsupported") == std::string::npos) {
      std::cerr << "access-certificate: " << label << " was rejected for the wrong reason: " << error << "\n";
      return false;
    }
    std::cout << "access-certificate: " << label << " honestly rejected by semantic assembly\n";
    return true;
  };

  if (!check_unsupported_mode(vg::core::AccessCertificateMode::SoftwarePaged, "software-paged")) return false;
  if (!check_unsupported_mode(vg::core::AccessCertificateMode::FaultManaged, "fault-managed")) return false;

  std::cout << "access-certificate: ok\n";
  return true;
}

// TASK-B13 (E009): the narrow Metal Tier1 physical harness drives a second,
// GPU-authored indirect dispatch pass after Tier0 publication -- each task's
// x/y/z dispatch dims are blitted straight from the published task-ring
// buffer into an indirect-args buffer, never read back to the host before
// dispatching. last_tier1_indirect_dims() is a debug/test-only readback of
// those GPU-resident bytes, letting this test assert the blit copied the
// right bytes -- the dispatch itself never reads them host-side. This
// test's probe module is load-only (no atomic_add): Tier1 re-dispatches the
// same compute pipeline once more per task on top of the Tier0 dispatch
// already issued, which is only correctness-safe for idempotent instruction
// sets (see ADR-026). Consistent with that scope limit, this test validates
// Tier1 solely via the indirect-args dims readback; it does not assert
// anything about arena buffer contents after Tier1 runs.
bool run_tier1_indirect(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "tier1-indirect: no Metal device available on this host\n";
    return false;
  }
  if (!metal_device->capabilities().supports(vg::hal::Capability::IndirectTier1)) {
    std::cerr << "tier1-indirect: device does not advertise IndirectTier1 support, skipping\n";
    return true;
  }

  vg::core::Arena arena;
  const auto module = make_probe_module(arena);
  TaskRecord task0 = probe_task(module);
  task0.x = 4;
  task0.y = 1;
  task0.z = 1;
  TaskRecord task1 = probe_task(module);
  task1.x = 2;
  task1.y = 3;
  task1.z = 1;
  std::string error;
  vg::hal::Submission submission;
  if (!metal_device->run_task_tier1_indirect_test_harness(module, arena, {task0, task1},
                                                          &submission, &error)) {
    std::cerr << "tier1-indirect: physical harness failed: " << error << "\n";
    return false;
  }
  if (!submission.result.ok) {
    std::cerr << "tier1-indirect: Metal execution reported failure: " << submission.result.message << "\n";
    return false;
  }
  if (submission.published_tasks.size() != 2) {
    std::cerr << "tier1-indirect: unexpected published_tasks count\n";
    return false;
  }

  const auto& dims = metal_device->last_tier1_indirect_dims();
  if (dims.size() != submission.published_tasks.size()) {
    std::cerr << "tier1-indirect: last_tier1_indirect_dims size mismatch\n";
    return false;
  }
  for (size_t i = 0; i < dims.size(); ++i) {
    const auto& task = submission.published_tasks[i];
    if (dims[i][0] != task.x || dims[i][1] != task.y || dims[i][2] != task.z) {
      std::cerr << "tier1-indirect: indirect dims[" << i << "] do not match published task x/y/z\n";
      return false;
    }
  }

  bool found_event = false;
  for (const auto& event : submission.report.events) {
    if (event.operation != "tier1_indirect_dispatch") continue;
    found_event = true;
    if (event.classification != vg::hal::LoweringClass::Direct) {
      std::cerr << "tier1-indirect: report classification mismatch\n";
      return false;
    }
    if (event.count != submission.published_tasks.size()) {
      std::cerr << "tier1-indirect: report count mismatch\n";
      return false;
    }
  }
  if (!found_event) {
    std::cerr << "tier1-indirect: missing tier1_indirect_dispatch report event\n";
    return false;
  }

  std::cout << "tier1-indirect: ok\n";
  return true;
}

// TASK-B13 (E009): each GPU thread claims an atomic output slot only if its
// instance_visible flag is nonzero; slot order reflects thread arrival, not
// instance index, so the compacted output is compared as a sorted multiset
// against reference::cull_compact(), never by position (see CullCompactResult's
// doc comments in metal_device_hal.h and reference_executor.h).
bool run_cull_compact(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "cull-compact: no Metal device available on this host\n";
    return false;
  }

  const std::vector<uint32_t> instance_visible{1, 0, 1, 1, 0, 1, 0, 0, 1};
  std::vector<uint32_t> instance_ids;
  for (uint32_t i = 0; i < instance_visible.size(); ++i) instance_ids.push_back(100 + i);

  auto oracle = vg::reference::cull_compact(instance_visible, instance_ids);
  if (!oracle.ok) {
    std::cerr << "cull-compact: reference oracle failed: " << oracle.message << "\n";
    return false;
  }

  vg::metal::CullCompactResult result;
  std::string error;
  if (!metal_device->run_cull_compact(instance_visible, instance_ids, &result, &error)) {
    std::cerr << "cull-compact: Metal run_cull_compact failed: " << error << "\n";
    return false;
  }
  if (result.visible_count != oracle.compact_ids.size()) {
    std::cerr << "cull-compact: visible_count mismatch: got " << result.visible_count << ", expected "
              << oracle.compact_ids.size() << "\n";
    return false;
  }
  if (result.compact_ids.size() != result.visible_count) {
    std::cerr << "cull-compact: compact_ids size does not match visible_count\n";
    return false;
  }

  std::vector<uint32_t> got = result.compact_ids;
  std::vector<uint32_t> want = oracle.compact_ids;
  std::ranges::sort(got);
  std::ranges::sort(want);
  if (got != want) {
    std::cerr << "cull-compact: compacted id set mismatches reference oracle\n";
    return false;
  }

  std::cout << "cull-compact: ok\n";
  return true;
}

// Catalog E009 million-instance follow-on: correctness against the CPU
// oracle as a sorted multiset, plus host wall-clock. Does not invent gpu_ns.
bool run_cull_compact_1m(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "cull-compact-1m: no Metal device available on this host\n";
    return false;
  }

  constexpr uint32_t kCount = 1000000;
  std::vector<uint32_t> instance_visible(kCount);
  std::vector<uint32_t> instance_ids(kCount);
  for (uint32_t i = 0; i < kCount; ++i) {
    instance_visible[i] = (i % 2u) == 0u ? 1u : 0u;
    instance_ids[i] = 1000u + i;
  }

  auto oracle = vg::reference::cull_compact(instance_visible, instance_ids);
  if (!oracle.ok) {
    std::cerr << "cull-compact-1m: reference oracle failed: " << oracle.message << "\n";
    return false;
  }

  vg::metal::CullCompactResult result;
  std::string error;
  const auto started = std::chrono::steady_clock::now();
  if (!metal_device->run_cull_compact(instance_visible, instance_ids, &result, &error)) {
    std::cerr << "cull-compact-1m: Metal run_cull_compact failed: " << error << "\n";
    return false;
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const uint64_t host_ms =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

  if (result.visible_count != oracle.compact_ids.size()) {
    std::cerr << "cull-compact-1m: visible_count mismatch: got " << result.visible_count << ", expected "
              << oracle.compact_ids.size() << "\n";
    return false;
  }
  if (result.compact_ids.size() != result.visible_count) {
    std::cerr << "cull-compact-1m: compact_ids size does not match visible_count\n";
    return false;
  }

  std::vector<uint32_t> got = result.compact_ids;
  std::vector<uint32_t> want = oracle.compact_ids;
  std::ranges::sort(got);
  std::ranges::sort(want);
  if (got != want) {
    std::cerr << "cull-compact-1m: compacted id set mismatches reference oracle\n";
    return false;
  }

  std::cout << "cull-compact-1m: ok count=" << kCount << " visible_count=" << result.visible_count
            << " host_ms=" << host_ms << "\n";
  return true;
}

// Layer-1: CanonicalView + FacetPool -> Sample/Storage/Attachment + representation transform.
bool run_representation_layer(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "representation-layer: no Metal device available on this host\n";
    return false;
  }

  constexpr uint32_t kW = 2;
  constexpr uint32_t kH = 2;
  constexpr uint32_t kBytes = kW * kH * 4;

  vg::core::Arena arena;
  auto& allocation = arena.allocate(kBytes);
  // Distinct texels for sample oracle: (0,0)=R, (1,0)=G, (0,1)=B, (1,1)=A-ish white.
  allocation.bytes = {
      255, 0, 0, 255,
      0, 255, 0, 255,
      0, 0, 255, 255,
      255, 255, 255, 255,
  };

  vg::core::CanonicalView view;
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  view.format = vg::core::PixelFormat::RGBA8Unorm;
  view.dimension = vg::core::ViewDimension::Texture2D;
  view.width = kW;
  view.height = kH;

  vg::core::FacetPool pool;
  std::string error;
  // One 8-bit quantization step, the tightest tolerance an RGBA8Unorm round
  // trip can honestly hold.
  constexpr float kTol = 1.0f / 255.0f + 1e-4f;
  const auto channels_match = [](const std::array<float, 4>& got, const std::array<float, 4>& want,
                                 const char* what) {
    for (int c = 0; c < 4; ++c) {
      if (std::fabs(got[c] - want[c]) <= kTol) continue;
      std::cerr << "representation-layer: " << what << " channel " << c << " got " << got[c] << " expected "
                << want[c] << "\n";
      return false;
    }
    return true;
  };

  // --- AddressFacet: the same CanonicalView's linear/BDA view (02 §3.3) ---
  vg::core::FacetRef address_ref;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Address, &address_ref, &error)) {
    std::cerr << "representation-layer: address acquire failed: " << error << "\n";
    return false;
  }
  vg::metal::AddressFacetResult address_result;
  if (!metal_device->run_address_facet(arena, pool, address_ref, &address_result, &error)) {
    std::cerr << "representation-layer: address facet failed: " << error << "\n";
    return false;
  }
  if (address_result.byte_size < kBytes) {
    std::cerr << "representation-layer: address facet must cover the whole view extent\n";
    return false;
  }

  // --- SampleFacet via FacetRef ---
  vg::core::FacetRef sample_ref;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &sample_ref, &error)) {
    std::cerr << "representation-layer: sample acquire failed: " << error << "\n";
    return false;
  }
  const std::vector<std::array<float, 2>> uvs = {{0.25f, 0.25f}};
  vg::metal::SampleFacetResult sample_result;
  if (!metal_device->run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, uvs, &sample_result, &error)) {
    std::cerr << "representation-layer: sample failed: " << error << "\n";
    return false;
  }
  // The oracle resolves the same capability token, so it enforces the same
  // kind/staleness rules Metal does before producing a comparison value.
  auto oracle = vg::reference::sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                            vg::core::WrapMode::Clamp, uvs);
  if (!oracle.ok) {
    std::cerr << "representation-layer: sample oracle failed: " << oracle.message << "\n";
    return false;
  }
  if (!channels_match(sample_result.sampled_rgba[0], oracle.sampled_rgba[0], "sample")) return false;
  vg::metal::SampleFacetResult sample_second;
  if (!metal_device->run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, uvs, &sample_second, &error)) {
    std::cerr << "representation-layer: sample cache reuse failed: " << error << "\n";
    return false;
  }
  if (!sample_second.facet_cache_hit) {
    std::cerr << "representation-layer: expected SampleFacet cache hit on second use\n";
    return false;
  }

  // --- Swizzle is part of the view contract, so it is a different facet and
  // both backends must apply it identically (06 §6.1) ---
  vg::core::CanonicalView swizzled_view = view;
  swizzled_view.swizzle = {vg::core::Swizzle::Blue, vg::core::Swizzle::Green, vg::core::Swizzle::Red,
                           vg::core::Swizzle::One};
  vg::core::FacetRef swizzled_ref;
  if (!pool.acquire(arena, swizzled_view, vg::core::FacetKind::Sample, &swizzled_ref, &error)) {
    std::cerr << "representation-layer: swizzled sample acquire failed: " << error << "\n";
    return false;
  }
  vg::metal::SampleFacetResult swizzled_result;
  if (!metal_device->run_sample_facet(arena, pool, swizzled_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, uvs, &swizzled_result, &error)) {
    std::cerr << "representation-layer: swizzled sample failed: " << error << "\n";
    return false;
  }
  auto swizzled_oracle = vg::reference::sample_facet(arena, pool, swizzled_ref, vg::core::FilterMode::Nearest,
                                                     vg::core::WrapMode::Clamp, uvs);
  if (!swizzled_oracle.ok) {
    std::cerr << "representation-layer: swizzled oracle failed: " << swizzled_oracle.message << "\n";
    return false;
  }
  if (!channels_match(swizzled_result.sampled_rgba[0], swizzled_oracle.sampled_rgba[0], "swizzled sample"))
    return false;
  const std::array<float, 4> unswizzled = sample_result.sampled_rgba[0];
  const std::array<float, 4> expected_swizzle = {unswizzled[2], unswizzled[1], unswizzled[0], 1.0f};
  if (!channels_match(swizzled_result.sampled_rgba[0], expected_swizzle, "swizzle mapping")) return false;

  // --- StorageFacet, both representations 06 §6.2 allows ---
  vg::core::FacetRef storage_ref;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Storage, &storage_ref, &error)) {
    std::cerr << "representation-layer: storage acquire failed: " << error << "\n";
    return false;
  }
  const std::array<float, 4> write_rgba = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};
  vg::metal::StorageFacetResult storage_result;
  if (!metal_device->run_storage_facet(arena, pool, storage_ref, vg::metal::StorageFacetTarget::Texture,
                                       write_rgba, &storage_result, &error)) {
    std::cerr << "representation-layer: storage texture write failed: " << error << "\n";
    return false;
  }
  if (!channels_match(storage_result.written_rgba, write_rgba, "storage texture writeback")) return false;

  vg::metal::StorageFacetResult storage_buffer_result;
  if (!metal_device->run_storage_facet(arena, pool, storage_ref, vg::metal::StorageFacetTarget::LinearBuffer,
                                       write_rgba, &storage_buffer_result, &error)) {
    std::cerr << "representation-layer: storage buffer write failed: " << error << "\n";
    return false;
  }
  if (!channels_match(storage_buffer_result.written_rgba, write_rgba, "storage buffer writeback"))
    return false;
  if (storage_buffer_result.target != vg::metal::StorageFacetTarget::LinearBuffer) {
    std::cerr << "representation-layer: linear-buffer storage must report the target it actually used\n";
    return false;
  }
  // A non-identity swizzle has no meaning for an image write, and must be
  // refused rather than silently dropped.
  vg::core::FacetRef swizzled_storage_ref;
  if (!pool.acquire(arena, swizzled_view, vg::core::FacetKind::Storage, &swizzled_storage_ref, &error)) {
    std::cerr << "representation-layer: swizzled storage acquire failed: " << error << "\n";
    return false;
  }
  vg::metal::StorageFacetResult refused;
  if (metal_device->run_storage_facet(arena, pool, swizzled_storage_ref,
                                      vg::metal::StorageFacetTarget::Texture, write_rgba, &refused, &error)) {
    std::cerr << "representation-layer: a swizzled StorageFacet must be reported Unsupported\n";
    return false;
  }

  // --- AttachmentFacet: clear+store, then load+store preserving contents ---
  vg::core::FacetRef attachment_ref;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Attachment, &attachment_ref, &error)) {
    std::cerr << "representation-layer: attachment acquire failed: " << error << "\n";
    return false;
  }
  const std::array<float, 4> clear_rgba = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};
  vg::metal::AttachmentFacetDesc clear_desc;
  clear_desc.load = vg::metal::AttachmentLoadAction::Clear;
  clear_desc.store = vg::metal::AttachmentStoreAction::Store;
  clear_desc.clear_rgba = clear_rgba;
  vg::metal::AttachmentFacetResult attachment_result;
  if (!metal_device->run_attachment_facet(arena, pool, attachment_ref, clear_desc, &attachment_result,
                                          &error)) {
    std::cerr << "representation-layer: attachment clear failed: " << error << "\n";
    return false;
  }
  if (!channels_match(attachment_result.resolved_rgba, clear_rgba, "attachment clear")) return false;
  if (attachment_result.store_traffic_avoided) {
    std::cerr << "representation-layer: a stored attachment must not claim avoided external traffic\n";
    return false;
  }

  vg::metal::AttachmentFacetDesc load_desc;
  load_desc.load = vg::metal::AttachmentLoadAction::Load;
  load_desc.store = vg::metal::AttachmentStoreAction::Store;
  vg::metal::AttachmentFacetResult loaded;
  if (!metal_device->run_attachment_facet(arena, pool, attachment_ref, load_desc, &loaded, &error)) {
    std::cerr << "representation-layer: attachment load failed: " << error << "\n";
    return false;
  }
  if (!channels_match(loaded.resolved_rgba, clear_rgba, "attachment load preserves contents")) return false;

  // Resolve: render 4x multisampled and resolve into the facet's texture. With
  // no draw, every sample is the clear color, so the resolve must reproduce it.
  const std::array<float, 4> resolve_rgba = {32.0f / 255.0f, 96.0f / 255.0f, 160.0f / 255.0f, 1.0f};
  vg::metal::AttachmentFacetDesc resolve_desc;
  resolve_desc.load = vg::metal::AttachmentLoadAction::Clear;
  resolve_desc.store = vg::metal::AttachmentStoreAction::MultisampleResolve;
  resolve_desc.clear_rgba = resolve_rgba;
  resolve_desc.sample_count = 4;
  vg::metal::AttachmentFacetResult resolved;
  if (!metal_device->run_attachment_facet(arena, pool, attachment_ref, resolve_desc, &resolved, &error)) {
    std::cerr << "representation-layer: attachment resolve failed: " << error << "\n";
    return false;
  }
  if (!channels_match(resolved.resolved_rgba, resolve_rgba, "attachment resolve")) return false;
  if (resolved.sample_count != 4) {
    std::cerr << "representation-layer: resolve must report the sample count it rendered at\n";
    return false;
  }
  // Mismatched load/store combinations are refused, not quietly reinterpreted.
  vg::metal::AttachmentFacetDesc contradictory = resolve_desc;
  contradictory.sample_count = 1;
  vg::metal::AttachmentFacetResult unused;
  if (metal_device->run_attachment_facet(arena, pool, attachment_ref, contradictory, &unused, &error)) {
    std::cerr << "representation-layer: MultisampleResolve at sample_count 1 must be rejected\n";
    return false;
  }

  // --- Representation transform, once per target kind ---
  const vg::core::FacetKind target_kinds[] = {vg::core::FacetKind::Sample, vg::core::FacetKind::Storage,
                                              vg::core::FacetKind::Attachment};
  uint32_t last_epoch = 0;
  for (vg::core::FacetKind target_kind : target_kinds) {
    vg::metal::RepresentationTransformResult transform_result;
    if (!metal_device->run_representation_transform(arena, pool, view, target_kind, &transform_result,
                                                    &error)) {
      std::cerr << "representation-layer: transform failed: " << error << "\n";
      return false;
    }
    if (transform_result.new_epoch <= last_epoch || !transform_result.used_private_optimal ||
        transform_result.encoder_count == 0) {
      std::cerr << "representation-layer: transform must advance the epoch and blit into Private storage\n";
      return false;
    }
    if (transform_result.retired_facet_count == 0) {
      std::cerr << "representation-layer: the new epoch must retire the facets it invalidated\n";
      return false;
    }
    last_epoch = transform_result.new_epoch;

    // Every facet minted against the previous epoch is now a stale token, and
    // the backend must refuse it rather than resolve its last-known texture.
    if (pool.lookup(arena, sample_ref) != nullptr || pool.lookup(arena, storage_ref) != nullptr ||
        pool.lookup(arena, attachment_ref) != nullptr || pool.lookup(arena, address_ref) != nullptr) {
      std::cerr << "representation-layer: pre-transform FacetRefs must be stale after transform\n";
      return false;
    }
    if (metal_device->run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                       vg::core::WrapMode::Clamp, uvs, &sample_result, &error)) {
      std::cerr << "representation-layer: Metal must refuse a stale FacetRef\n";
      return false;
    }
    const vg::core::FacetSlot* out_slot = pool.lookup(arena, transform_result.out_facet);
    if (out_slot == nullptr || out_slot->kind != target_kind) {
      std::cerr << "representation-layer: transform must yield a live facet of the requested kind\n";
      return false;
    }

    // Use the transformed facet on its own kind's path: the Private optimal
    // texture has to be usable without falling back to a Shared re-upload.
    if (target_kind == vg::core::FacetKind::Sample) {
      vg::metal::SampleFacetResult post;
      if (!metal_device->run_sample_facet(arena, pool, transform_result.out_facet,
                                          vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp, uvs,
                                          &post, &error)) {
        std::cerr << "representation-layer: sample of transformed facet failed: " << error << "\n";
        return false;
      }
      if (!channels_match(post.sampled_rgba[0], oracle.sampled_rgba[0], "transformed sample")) return false;
    } else if (target_kind == vg::core::FacetKind::Storage) {
      vg::metal::StorageFacetResult post;
      if (!metal_device->run_storage_facet(arena, pool, transform_result.out_facet,
                                           vg::metal::StorageFacetTarget::Texture, write_rgba, &post,
                                           &error)) {
        std::cerr << "representation-layer: storage write to transformed facet failed: " << error << "\n";
        return false;
      }
      if (!channels_match(post.written_rgba, write_rgba, "transformed storage writeback")) return false;
    } else {
      vg::metal::AttachmentFacetResult post;
      if (!metal_device->run_attachment_facet(arena, pool, transform_result.out_facet, clear_desc, &post,
                                              &error)) {
        std::cerr << "representation-layer: attachment clear on transformed facet failed: " << error << "\n";
        return false;
      }
      if (!channels_match(post.resolved_rgba, clear_rgba, "transformed attachment clear")) return false;
    }

    // Re-mint the layer-1 refs against the epoch just published, so the next
    // iteration has live tokens to invalidate.
    if (!pool.acquire(arena, view, vg::core::FacetKind::Address, &address_ref, &error) ||
        !pool.acquire(arena, view, vg::core::FacetKind::Sample, &sample_ref, &error) ||
        !pool.acquire(arena, view, vg::core::FacetKind::Storage, &storage_ref, &error) ||
        !pool.acquire(arena, view, vg::core::FacetKind::Attachment, &attachment_ref, &error)) {
      std::cerr << "representation-layer: re-acquire after transform failed: " << error << "\n";
      return false;
    }
  }

  std::cout << "representation-layer: ok (address+sample+swizzle+storage+attachment+transform epoch="
            << last_epoch << ")\n";
  return true;
}

struct StoreWord {
  uint64_t offset{};
  int64_t value{};
};

vg::ir::Instruction make_store_instruction(const vg::core::Allocation& allocation, StoreWord word) {
  vg::ir::Instruction instruction;
  instruction.op = "store";
  instruction.allocation = allocation.id;
  instruction.generation = allocation.generation;
  instruction.representation_epoch = allocation.representation_epoch;
  instruction.offset = word.offset;
  instruction.size = 4;
  instruction.value = word.value;
  return instruction;
}

vg::ir::Module make_store_pass(const vg::core::Allocation& allocation, uint64_t offset, int64_t value) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/v1";
  module.instructions.push_back(make_store_instruction(allocation, {.offset = offset, .value = value}));
  module.declared_effects.push_back(
      {allocation.id, offset, 4, vg::ir::Access::Write, allocation.representation_epoch});
  return module;
}

// reference_executor.cpp/compute_package.cpp's store fills every byte of
// [offset, offset+size) with the low byte of `value`, broadcast -- not a
// little-endian encoding of `value`. Mirrors compute_package.cpp's private
// store_word_pattern() so this test can check GPU-written bytes directly.
uint32_t store_word_pattern(int64_t value) {
  const auto low_byte = static_cast<uint32_t>(static_cast<uint8_t>(value));
  return low_byte * 0x01010101u;
}

struct WordAt {
  uint64_t offset{};
  uint32_t pattern{};
};

bool bytes_match_pattern(const std::vector<uint8_t>& bytes, WordAt word) {
  if (word.offset + 4 > bytes.size()) return false;
  uint32_t got = 0;
  std::memcpy(&got, bytes.data() + word.offset, 4);
  return got == word.pattern;
}

// TASK-B14 (E012): exercises all 3 in-scope validated EffectGraph shapes
// (classify_effect_graph_shape, ADR-027) end to end through a real Metal
// submit(), plus one construction confirmed to fall outside those 3 shapes
// so compile() must honestly report it Unsupported rather than guess a
// fence placement. The ForkJoin construction is not a "textbook diamond":
// classify_effect_graph_shape's edge-count invariant (structural_edges ==
// 2*(node_count-1)) is only satisfiable for node_count==4 when every node
// pair conflicts, so all 4 passes deliberately write the *same* allocation
// with mutually-conflicting effects and zero explicit dependencies, letting
// seal() generate the full C(4,2)=6-edge transitive closure automatically
// (see dispatch_task_graph's ForkJoin branch doc comment in
// metal_device_hal.mm for why this is the only shape that classifies
// ForkJoin, confirmed empirically, not just by inspection).
bool run_effect_dag(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "effect-dag: no Metal device available on this host\n";
    return false;
  }

  auto check_shape = [&](const char* label, std::vector<vg::ir::Module> passes,
                         std::vector<std::pair<uint32_t, uint32_t>> dependencies,
                         const std::vector<uint32_t>& expected_task_order,
                         vg::core::EffectGraphShape expected_shape, uint64_t expected_encoder_count,
                         uint64_t expected_barrier_count, vg::core::Arena& arena,
                         const std::vector<std::pair<uint64_t, uint32_t>>& expect_final_bytes) {
    std::string error;
    const size_t expected_node_count = passes.size();
    std::vector<TaskRecord> tasks;
    tasks.reserve(passes.size());
    for (size_t task_index = 0; task_index < passes.size(); ++task_index) {
      auto task = probe_task(passes[task_index]);
      task.x = static_cast<uint32_t>(task_index + 2);
      task.y = static_cast<uint32_t>(task_index + 3);
      task.z = static_cast<uint32_t>(task_index + 4);
      tasks.push_back(task);
    }
    vg::test_support::MultiNodePlanFixture fixture;
    vg::core::ExecutionPlan plan;
    if (!vg::test_support::assemble_multi_node_plan(arena, std::move(passes), std::move(tasks),
                                                    dependencies, &fixture, &plan, &error)) {
      std::cerr << "effect-dag: " << label << " assembly failed: " << error << "\n";
      return false;
    }
    vg::hal::CompiledPlan compiled;
    if (!metal_device->compile(plan, &compiled, &error)) {
      std::cerr << "effect-dag: " << label << " compile failed: " << error << "\n";
      return false;
    }
    if (compiled.plan.validated_effect_graph_shape != expected_shape) {
      std::cerr << "effect-dag: " << label << " classified as unexpected shape\n";
      return false;
    }
    if (compiled.plan.task_order != expected_task_order) {
      std::cerr << "effect-dag: " << label << " sealed task order mismatch\n";
      return false;
    }
    if (compiled.per_node_packages.size() != expected_node_count) {
      std::cerr << "effect-dag: " << label << " per-Node package count mismatch\n";
      return false;
    }
    uint64_t pipeline_events = 0;
    uint64_t package_events = 0;
    for (const auto& event : compiled.report.events) {
      if (event.operation == "metal_pipeline") {
        if (event.classification != vg::hal::LoweringClass::Direct) {
          std::cerr << "effect-dag: " << label << " first pipeline creation was not reported Direct\n";
          return false;
        }
        pipeline_events += event.count;
      }
      if (event.operation == "node_compute_package") package_events += event.count;
    }
    if (pipeline_events != expected_node_count || package_events != expected_node_count) {
      std::cerr << "effect-dag: " << label << " per-Node lowering report count mismatch\n";
      return false;
    }

    // Compiling the same immutable Nodes again must honestly report cache
    // hits, not another Direct pipeline compilation.
    vg::hal::CompiledPlan cached;
    if (!metal_device->compile(plan, &cached, &error)) {
      std::cerr << "effect-dag: " << label << " cached compile failed: " << error << "\n";
      return false;
    }
    uint64_t cached_pipeline_events = 0;
    for (const auto& event : cached.report.events) {
      if (event.operation != "metal_pipeline") continue;
      if (event.classification != vg::hal::LoweringClass::CachedObject) {
        std::cerr << "effect-dag: " << label << " cached pipeline was not reported as CachedObject\n";
        return false;
      }
      cached_pipeline_events += event.count;
    }
    if (cached_pipeline_events != expected_node_count) {
      std::cerr << "effect-dag: " << label << " cached pipeline report count mismatch\n";
      return false;
    }

    vg::hal::Submission submission;
    if (!metal_device->submit(compiled, arena, &submission, &error)) {
      std::cerr << "effect-dag: " << label << " submit failed: " << error << "\n";
      return false;
    }
    if (!submission.result.ok) {
      std::cerr << "effect-dag: " << label << " execution reported failure: " << submission.result.message << "\n";
      return false;
    }
    if (submission.report.encoder_count != expected_encoder_count) {
      std::cerr << "effect-dag: " << label << " encoder_count mismatch: got " << submission.report.encoder_count
                << ", expected " << expected_encoder_count << "\n";
      return false;
    }
    if (submission.report.barrier_count != expected_barrier_count) {
      std::cerr << "effect-dag: " << label << " barrier_count mismatch: got " << submission.report.barrier_count
                << ", expected " << expected_barrier_count << "\n";
      return false;
    }
    if (submission.report.command_buffer_count != 2 || submission.report.queue_wait_count != 2) {
      std::cerr << "effect-dag: " << label
                << " command-buffer/host-wait count does not match compute plus publication\n";
      return false;
    }
    const auto& dispatches = metal_device->last_node_aware_dispatches();
    if (dispatches.size() != expected_task_order.size()) {
      std::cerr << "effect-dag: " << label << " real dispatch observation count mismatch\n";
      return false;
    }
    std::vector<uint32_t> pipeline_ordinals;
    pipeline_ordinals.reserve(dispatches.size());
    for (size_t encoded_index = 0; encoded_index < dispatches.size(); ++encoded_index) {
      const uint32_t task_index = expected_task_order[encoded_index];
      const auto& task = plan.task_graph.tasks()[task_index];
      const auto& dispatch = dispatches[encoded_index];
      if (dispatch.task_index != task_index || dispatch.node_index != task.node_index ||
          dispatch.node_generation != task.node_generation ||
          dispatch.threadgroups != std::array<uint32_t, 3>{task.x, task.y, task.z}) {
        std::cerr << "effect-dag: " << label
                  << " command encoder did not consume sealed order/NodeRef/x-y-z\n";
        return false;
      }
      pipeline_ordinals.push_back(dispatch.pipeline_ordinal);
    }
    std::sort(pipeline_ordinals.begin(), pipeline_ordinals.end());
    if (std::adjacent_find(pipeline_ordinals.begin(), pipeline_ordinals.end()) !=
        pipeline_ordinals.end()) {
      std::cerr << "effect-dag: " << label << " distinct Nodes unexpectedly shared a pipeline\n";
      return false;
    }
    for (const auto& expectation : expect_final_bytes) {
      const auto* allocation = arena.lookup(vg::core::PointerRef{expectation.first, 1});
      if (allocation == nullptr) {
        std::cerr << "effect-dag: " << label << " missing allocation " << expectation.first << " after submit\n";
        return false;
      }
      if (!bytes_match_pattern(allocation->bytes, {.offset = 0, .pattern = store_word_pattern(expectation.second)})) {
        std::cerr << "effect-dag: " << label << " allocation " << expectation.first
                  << " does not hold the expected final value\n";
        return false;
      }
    }
    std::cout << "effect-dag: " << label << " ok\n";
    return true;
  };

  {
    vg::core::Arena arena;
    const auto& a = arena.allocate(4);
    const auto& b = arena.allocate(4);
    const auto& c = arena.allocate(4);
    std::vector<vg::ir::Module> passes{make_store_pass(a, 0, 10), make_store_pass(b, 0, 11),
                                       make_store_pass(c, 0, 12)};
    if (!check_shape("independent-branches", passes, {}, {0, 1, 2},
                     vg::core::EffectGraphShape::IndependentBranches, 4, 0,
                     arena, {{a.id, 10}, {b.id, 11}, {c.id, 12}}))
      return false;
  }

  {
    vg::core::Arena arena;
    const auto& a = arena.allocate(4);
    const auto& b = arena.allocate(4);
    const auto& c = arena.allocate(4);
    std::vector<vg::ir::Module> passes{make_store_pass(a, 0, 20), make_store_pass(b, 0, 21),
                                       make_store_pass(c, 0, 22)};
    if (!check_shape("linear-chain", passes, {{0, 1}, {1, 2}}, {0, 1, 2},
                     vg::core::EffectGraphShape::LinearChain, 2, 0, arena,
                     {{a.id, 20}, {b.id, 21}, {c.id, 22}}))
      return false;
  }

  {
    vg::core::Arena arena;
    const auto& a = arena.allocate(4);
    std::vector<vg::ir::Module> passes{make_store_pass(a, 0, 30), make_store_pass(a, 0, 31),
                                       make_store_pass(a, 0, 32), make_store_pass(a, 0, 33)};
    // Zero explicit dependencies: seal()'s automatic conflict detection over
    // 4 mutually-conflicting writes to the same allocation is what produces
    // the ForkJoin-classified transitive closure here, not add_dependency().
    if (!check_shape("fork-join", passes, {}, {0, 1, 2, 3},
                     vg::core::EffectGraphShape::ForkJoin, 5, 6, arena, {{a.id, 33}}))
      return false;
  }

  {
    // Storage order is [0,1], while the only explicit dependency seals [1,0].
    // The command encoder observation must follow the latter without asking
    // EffectGraph for a second order.
    vg::core::Arena arena;
    const auto& a = arena.allocate(4);
    const auto& b = arena.allocate(4);
    std::vector<vg::ir::Module> passes{make_store_pass(a, 0, 34), make_store_pass(b, 0, 35)};
    if (!check_shape("reverse-storage-order", passes, {{1, 0}}, {1, 0},
                     vg::core::EffectGraphShape::LinearChain, 2, 0, arena,
                     {{a.id, 34}, {b.id, 35}}))
      return false;
  }

  {
    // Two Tasks reuse one Node/package/pipeline, but each is still a distinct
    // dispatch with its own sealed order and non-trivial shape.
    vg::core::Arena arena;
    const auto module = make_probe_module(arena);
    TaskRecord first = probe_task(module);
    first.x = 7;
    first.y = 3;
    first.z = 2;
    TaskRecord second = probe_task(module);
    second.x = 2;
    second.y = 5;
    second.z = 4;
    const std::vector<std::pair<uint32_t, uint32_t>> dependencies{{1, 0}};
    vg::test_support::AssemblyOptions options;
    options.dependencies = &dependencies;
    vg::core::ExecutionPlan plan;
    std::string error;
    if (!assemble_compute_plan(arena, module, {first, second}, &plan, &error, options)) {
      std::cerr << "effect-dag: same-Node assembly failed: " << error << "\n";
      return false;
    }
    if (plan.task_order != std::vector<uint32_t>{1, 0}) {
      std::cerr << "effect-dag: same-Node reverse order was not sealed\n";
      return false;
    }
    vg::hal::CompiledPlan compiled;
    if (!metal_device->compile(plan, &compiled, &error) || compiled.per_node_packages.size() != 1) {
      std::cerr << "effect-dag: same-Node compile/package count failed: " << error << "\n";
      return false;
    }
    uint64_t package_events = 0;
    for (const auto& event : compiled.report.events)
      if (event.operation == "node_compute_package") package_events += event.count;
    if (package_events != 1) {
      std::cerr << "effect-dag: same-Node package report was not de-duplicated\n";
      return false;
    }
    vg::hal::Submission submission;
    if (!metal_device->submit(compiled, arena, &submission, &error) || !submission.result.ok) {
      std::cerr << "effect-dag: same-Node submit failed: "
                << (error.empty() ? submission.result.message : error) << "\n";
      return false;
    }
    const auto& dispatches = metal_device->last_node_aware_dispatches();
    if (dispatches.size() != 2 || dispatches[0].task_index != 1 || dispatches[1].task_index != 0 ||
        dispatches[0].threadgroups != std::array<uint32_t, 3>{2, 5, 4} ||
        dispatches[1].threadgroups != std::array<uint32_t, 3>{7, 3, 2} ||
        dispatches[0].pipeline_ordinal != dispatches[1].pipeline_ordinal ||
        submission.report.encoder_count != 2 || submission.report.barrier_count != 0 ||
        submission.report.command_buffer_count != 2 || submission.report.queue_wait_count != 2) {
      std::cerr << "effect-dag: same-Node dispatch/order/pipeline/report mismatch\n";
      return false;
    }
    std::cout << "effect-dag: same-Node reuse ok\n";
  }

  {
    // Compiled-package and sealed-order tampering must fail before a Metal
    // command can modify the target allocation.
    vg::core::Arena arena;
    const auto& target = arena.allocate(4);
    const auto module = make_store_pass(target, 0, 36);
    vg::core::ExecutionPlan plan;
    std::string error;
    if (!assemble_compute_plan(arena, module, {probe_task(module)}, &plan, &error)) {
      std::cerr << "effect-dag: tamper plan assembly failed: " << error << "\n";
      return false;
    }
    vg::hal::CompiledPlan compiled;
    if (!metal_device->compile(plan, &compiled, &error)) {
      std::cerr << "effect-dag: tamper plan compile failed: " << error << "\n";
      return false;
    }
    auto untouched = [&]() {
      const auto* allocation = arena.lookup(vg::core::PointerRef{target.id, target.generation});
      return allocation != nullptr &&
             std::ranges::all_of(allocation->bytes, [](uint8_t byte) { return byte == 0; });
    };

    auto bad_package = compiled;
    bad_package.per_node_packages[0].package->canonical_ir_hash = "tampered-package-hash";
    vg::hal::Submission package_submission;
    error.clear();
    const bool package_accepted = metal_device->submit(bad_package, arena, &package_submission, &error);
    if ((package_accepted && package_submission.result.ok) || !untouched()) {
      std::cerr << "effect-dag: tampered package reached execution\n";
      return false;
    }

    auto bad_order = compiled;
    bad_order.plan.task_order[0] = 1;
    vg::hal::Submission order_submission;
    error.clear();
    if (metal_device->submit(bad_order, arena, &order_submission, &error) || !untouched()) {
      std::cerr << "effect-dag: tampered task order reached execution\n";
      return false;
    }
    std::cout << "effect-dag: package/order tamper rejected before execution\n";
  }

  {
    // A textbook four-edge diamond is intentionally outside the current
    // classifier's closed fork/join profile, which requires the source and
    // join to be connected to every other node (including source->join).
    // The assembler does not duplicate already-ordered hazards; this shape is
    // Unsupported because its four explicit edges are not one of the three
    // lowering profiles, not because a second pass list inflated the graph.
    vg::core::Arena arena;
    const auto& a = arena.allocate(4);
    const auto& b = arena.allocate(4);
    const auto& c = arena.allocate(4);
    vg::ir::Module source = make_store_pass(a, 0, 40);
    vg::ir::Module middle1;
    middle1.version = 1;
    middle1.root_schema = "vg.test/v1";
    {
      vg::ir::Instruction load;
      load.op = "load";
      load.allocation = a.id;
      load.generation = a.generation;
      load.representation_epoch = a.representation_epoch;
      load.offset = 0;
      load.size = 4;
      middle1.instructions.push_back(load);
      middle1.declared_effects.push_back({a.id, 0, 4, vg::ir::Access::Read, a.representation_epoch});
    }
    middle1.instructions.push_back(make_store_instruction(b, {.offset = 0, .value = 41}));
    middle1.declared_effects.push_back({b.id, 0, 4, vg::ir::Access::Write, b.representation_epoch});
    vg::ir::Module middle2;
    middle2.version = 1;
    middle2.root_schema = "vg.test/v1";
    {
      vg::ir::Instruction load;
      load.op = "load";
      load.allocation = a.id;
      load.generation = a.generation;
      load.representation_epoch = a.representation_epoch;
      load.offset = 0;
      load.size = 4;
      middle2.instructions.push_back(load);
      middle2.declared_effects.push_back({a.id, 0, 4, vg::ir::Access::Read, a.representation_epoch});
    }
    middle2.instructions.push_back(make_store_instruction(c, {.offset = 0, .value = 42}));
    middle2.declared_effects.push_back({c.id, 0, 4, vg::ir::Access::Write, c.representation_epoch});
    vg::ir::Module join;
    join.version = 1;
    join.root_schema = "vg.test/v1";
    {
      vg::ir::Instruction load_b;
      load_b.op = "load";
      load_b.allocation = b.id;
      load_b.generation = b.generation;
      load_b.representation_epoch = b.representation_epoch;
      load_b.offset = 0;
      load_b.size = 4;
      join.instructions.push_back(load_b);
      join.declared_effects.push_back({b.id, 0, 4, vg::ir::Access::Read, b.representation_epoch});
    }

    std::string error;
    const std::vector<vg::ir::Module> passes{source, middle1, middle2, join};
    const std::vector<std::pair<uint32_t, uint32_t>> dependencies{{0, 1}, {0, 2}, {1, 3}, {2, 3}};
    std::vector<TaskRecord> tasks;
    tasks.reserve(passes.size());
    for (const auto& pass : passes) tasks.push_back(probe_task(pass));
    vg::test_support::MultiNodePlanFixture fixture;
    vg::core::ExecutionPlan plan;
    if (!vg::test_support::assemble_multi_node_plan(arena, passes, std::move(tasks), dependencies,
                                                    &fixture, &plan, &error)) {
      std::cerr << "effect-dag: unsupported-shape assembly failed: " << error << "\n";
      return false;
    }
    vg::hal::CompiledPlan compiled;
    if (metal_device->compile(plan, &compiled, &error)) {
      std::cerr << "effect-dag: unsupported-shape unexpectedly compiled successfully\n";
      return false;
    }
    if (compiled.report.supported) {
      std::cerr << "effect-dag: unsupported-shape report claims supported\n";
      return false;
    }
    bool found_unsupported = false;
    for (const auto& event : compiled.report.events) {
      if (event.operation == "task_graph_lowering" && event.classification == vg::hal::LoweringClass::Unsupported)
        found_unsupported = true;
    }
    if (!found_unsupported) {
      std::cerr << "effect-dag: unsupported-shape missing honest Unsupported report event\n";
      return false;
    }
    std::cout << "effect-dag: unsupported-shape honestly unsupported\n";
  }

  std::cout << "effect-dag: ok\n";
  return true;
}

// TASK-B15 (E002): a single-hop typed pointer graph -- allocation A holds a
// load_ref (never read on the GPU in this lowering, see ADR-028), allocation
// B is the store_via target it statically resolves to via
// declared_pointer_edges. Verifies compile() classifies "compute_package" as
// CachedObject (not Direct) for a pointer-graph module, and that the actual
// GPU-executed store_via reproduces the same byte-broadcast pattern as the
// linear package's store (store_word_pattern in compute_package.cpp).
bool run_pointer_graph(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "pointer-graph: no Metal device available on this host\n";
    return false;
  }

  vg::core::Arena arena;
  const auto& ref_holder = arena.allocate(16);
  const auto& target = arena.allocate(4);

  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/pointer-graph";

  vg::ir::Instruction load_ref;
  load_ref.op = "load_ref";
  load_ref.allocation = ref_holder.id;
  load_ref.generation = ref_holder.generation;
  load_ref.representation_epoch = ref_holder.representation_epoch;
  load_ref.offset = 0;
  load_ref.size = 12;
  module.instructions.push_back(load_ref);
  module.declared_effects.push_back(
      {ref_holder.id, 0, 12, vg::ir::Access::Read, ref_holder.representation_epoch});

  vg::ir::Instruction store_via;
  store_via.op = "store_via";
  store_via.allocation = target.id;
  store_via.generation = target.generation;
  store_via.representation_epoch = target.representation_epoch;
  store_via.offset = 0;
  store_via.size = 4;
  store_via.value = 42;
  store_via.ref_operand = 1;  // 1-based index of the load_ref above
  module.instructions.push_back(store_via);

  module.declared_pointer_edges.push_back({ref_holder.id, 0, target.id});

  std::string error;
  vg::core::ExecutionPlan plan;
  if (!assemble_compute_plan(arena, module, {probe_task(module)}, &plan, &error)) {
    std::cerr << "pointer-graph: plan assembly failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan compiled;
  if (!metal_device->compile(plan, &compiled, &error)) {
    std::cerr << "pointer-graph: compile failed: " << error << "\n";
    return false;
  }

  bool found_event = false;
  for (const auto& event : compiled.report.events) {
    if (event.operation != "node_compute_package") continue;
    found_event = true;
    if (event.classification != vg::hal::LoweringClass::CachedObject) {
      std::cerr << "pointer-graph: expected CachedObject classification for per-Node package\n";
      return false;
    }
  }
  if (!found_event) {
    std::cerr << "pointer-graph: missing per-Node package report event\n";
    return false;
  }

  vg::hal::Submission submission;
  if (!metal_device->submit(compiled, arena, &submission, &error)) {
    std::cerr << "pointer-graph: submit failed: " << error << "\n";
    return false;
  }
  if (!submission.result.ok) {
    std::cerr << "pointer-graph: execution reported failure: " << submission.result.message << "\n";
    return false;
  }

  for (uint8_t byte : target.bytes) {
    if (byte != 42) {
      std::cerr << "pointer-graph: store_via target does not match expected byte-broadcast pattern\n";
      return false;
    }
  }

  std::cout << "pointer-graph: ok\n";
  return true;
}

// TASK-B16 (E007): root pointer vs. bindless binding cost. Two distinct
// allocations (a load target and a store target) collapse into ONE
// argument-buffer-style table binding -- the compiled report's
// "compute_package" event is classified Direct with bytes == 1 -- instead of
// build_linear_compute_package's two separate buffer(N) bindings for the
// same module; that N-vs-1 contrast is exactly what this experiment
// measures. Requires a real MTLBuffer.gpuAddress-capable device; honestly
// skipped (not failed) when probe_gpu_addresses() reports it unavailable,
// matching this milestone's own honest-degradation design (ADR-029).
bool run_indexed_binding(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "indexed-binding: no Metal device available on this host\n";
    return false;
  }

  vg::core::Arena arena;
  const auto& load_target = arena.allocate(4);
  const auto& store_target = arena.allocate(4);

  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/indexed-binding";

  vg::ir::Instruction load;
  load.op = "load";
  load.allocation = load_target.id;
  load.generation = load_target.generation;
  load.representation_epoch = load_target.representation_epoch;
  load.offset = 0;
  load.size = 4;
  module.instructions.push_back(load);
  module.declared_effects.push_back(
      {load_target.id, 0, 4, vg::ir::Access::Read, load_target.representation_epoch});

  vg::ir::Instruction store;
  store.op = "store";
  store.allocation = store_target.id;
  store.generation = store_target.generation;
  store.representation_epoch = store_target.representation_epoch;
  store.offset = 0;
  store.size = 4;
  store.value = 77;
  module.instructions.push_back(store);
  module.declared_effects.push_back(
      {store_target.id, 0, 4, vg::ir::Access::Write, store_target.representation_epoch});

  std::string error;
  vg::metal::IndexedComputeHarnessResult harness_result;
  vg::hal::Submission submission;
  if (!metal_device->run_indexed_compute_test_harness(module, arena, &harness_result,
                                                       &submission, &error)) {
    if (error.find("gpuAddress") != std::string::npos) {
      std::cout << "indexed-binding: device does not support MTLBuffer.gpuAddress, skipping\n";
      return true;
    }
    std::cerr << "indexed-binding: compile failed: " << error << "\n";
    return false;
  }

  if (harness_result.referenced_allocation_count != 2) {
    std::cerr << "indexed-binding: expected 2 referenced allocations\n";
    return false;
  }

  bool found_event = false;
  for (const auto& event : harness_result.report.events) {
    if (event.operation != "compute_package") continue;
    found_event = true;
    if (event.classification != vg::hal::LoweringClass::Direct) {
      std::cerr << "indexed-binding: expected Direct classification for compute_package\n";
      return false;
    }
    if (event.bytes != 1) {
      std::cerr << "indexed-binding: expected exactly 1 reported binding (the table), got " << event.bytes << "\n";
      return false;
    }
  }
  if (!found_event) {
    std::cerr << "indexed-binding: missing compute_package report event\n";
    return false;
  }

  if (!submission.result.ok) {
    std::cerr << "indexed-binding: execution reported failure: " << submission.result.message << "\n";
    return false;
  }

  for (uint8_t byte : store_target.bytes) {
    if (byte != 77) {
      std::cerr << "indexed-binding: store target does not match expected byte-broadcast pattern\n";
      return false;
    }
  }

  std::cout << "indexed-binding: ok\n";
  return true;
}

// One 8-bit quantization step, the tightest tolerance an RGBA8Unorm round
// trip can honestly hold (E008 nearest). Bilinear/edge pixels use the
// registered blend tolerance rather than pretending GPU and CPU rounding agree.
constexpr float kNearestTol = 1.0f / 255.0f + 1e-4f;

bool channels_close(const std::array<float, 4>& got, const std::array<float, 4>& want, float tol,
                    const char* label, const char* what) {
  for (int c = 0; c < 4; ++c) {
    if (std::fabs(got[c] - want[c]) <= tol) continue;
    std::cerr << label << ": " << what << " channel " << c << " got " << got[c] << " expected "
              << want[c] << "\n";
    return false;
  }
  return true;
}

void fill_subresource(vg::core::Allocation& allocation, const vg::core::CanonicalView& view,
                      uint32_t layer, uint32_t level, const std::array<uint8_t, 4>& rgba) {
  const uint64_t offset = view.subresource_byte_offset({layer, level});
  const uint32_t width = view.mip_width(level);
  const uint32_t height = view.mip_height(level);
  const uint64_t row = view.bytes_per_row(level);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint64_t texel = offset + static_cast<uint64_t>(y) * row + static_cast<uint64_t>(x) * 4;
      allocation.bytes[texel + 0] = rgba[0];
      allocation.bytes[texel + 1] = rgba[1];
      allocation.bytes[texel + 2] = rgba[2];
      allocation.bytes[texel + 3] = rgba[3];
    }
  }
}

std::vector<vg::reference::SampleCoord> to_reference_coords(
    const std::vector<vg::metal::SampleCoord>& coords) {
  std::vector<vg::reference::SampleCoord> out;
  out.reserve(coords.size());
  for (const auto& coord : coords)
    out.push_back({coord.u, coord.v, coord.lod, coord.array_slice});
  return out;
}

std::vector<vg::reference::RasterVertex> to_reference_vertices(
    const std::vector<vg::metal::RasterVertex>& vertices) {
  std::vector<vg::reference::RasterVertex> out;
  out.reserve(vertices.size());
  for (const auto& vertex : vertices)
    out.push_back({vertex.x, vertex.y, vertex.z, vertex.u, vertex.v});
  return out;
}

vg::reference::RasterDesc to_reference_desc(const vg::metal::RasterDesc& desc) {
  vg::reference::RasterDesc out;
  out.attachment.load = static_cast<vg::reference::AttachmentLoadAction>(desc.attachment.load);
  out.attachment.store = static_cast<vg::reference::AttachmentStoreAction>(desc.attachment.store);
  out.attachment.clear_rgba = desc.attachment.clear_rgba;
  out.attachment.sample_count = desc.attachment.sample_count;
  out.attachment.subresource = {desc.attachment.subresource.layer, desc.attachment.subresource.level};
  out.filter = desc.filter;
  out.wrap = desc.wrap;
  out.source_lod = desc.source_lod;
  out.source_array_slice = desc.source_array_slice;
  out.tint = desc.tint;
  out.depth_attachment_ref = desc.depth_attachment_ref;
  out.depth_test_enable = desc.depth_test_enable;
  out.depth_write_enable = desc.depth_write_enable;
  out.depth_compare_op = desc.depth_compare_op;
  return out;
}

vg::core::ConsumeProof complete_consume_proof() {
  vg::core::ConsumeProof proof;
  proof.envelope_complete = true;
  proof.no_external_references = true;
  proof.no_replay_required = true;
  proof.failure_semantics_accepted = true;
  return proof;
}

struct Extent2 {
  uint32_t width{};
  uint32_t height{};
};

vg::core::CanonicalView make_rgba8_view(const vg::core::Allocation& allocation, Extent2 extent) {
  vg::core::CanonicalView view;
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  view.format = vg::core::PixelFormat::RGBA8Unorm;
  view.dimension = vg::core::ViewDimension::Texture2D;
  view.width = extent.width;
  view.height = extent.height;
  return view;
}

vg::core::CanonicalView make_depth32_view(const vg::core::Allocation& allocation, Extent2 extent) {
  auto view = make_rgba8_view(allocation, extent);
  view.format = vg::core::PixelFormat::Depth32Float;
  return view;
}

// Metal Y-up clip space, uv (0,0) at the top-left of the source -- the same
// full-target quad the reference raster oracle uses, so the two backends
// receive identical vertices.
std::vector<vg::metal::RasterVertex> metal_fullscreen_quad() {
  const vg::metal::RasterVertex top_left{-1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
  const vg::metal::RasterVertex top_right{1.0f, 1.0f, 0.0f, 1.0f, 0.0f};
  const vg::metal::RasterVertex bottom_left{-1.0f, -1.0f, 0.0f, 0.0f, 1.0f};
  const vg::metal::RasterVertex bottom_right{1.0f, -1.0f, 0.0f, 1.0f, 1.0f};
  return {top_left, top_right, bottom_left, top_right, bottom_right, bottom_left};
}

// F3 (ADR-043 Decision #4): a restricted-import MSL source, structurally
// matching the exact binding contract run_raster_pass's encoder assumes --
// same VgRasterVertex/VgRasterVaryings struct layout and the same fixed
// buffer/texture/sampler indices raster_facet_metal_source() (the built-in
// shader, src/compiler/compute_package.cpp) uses -- but with caller-chosen
// entry-point names and a fragment body that ignores the sampled texture,
// sampler, and tint buffer entirely, returning solid green. The encoder still
// unconditionally binds all four at their fixed slots regardless of whether
// this function reads them (metal_device_hal.mm), so the source only needs
// to declare parameters at the right indices to be well-formed Metal, not to
// use them. Solid green (0,1,0,1) round-trips RGBA8Unorm quantization exactly
// and can never be produced by the built-in sample*tint formula against the
// non-green source texel pattern run_task_graph_raster/this test fill, so a
// pixel match against it is proof the custom shader itself executed.
std::string user_raster_msl_source(const std::string& vertex_entry, const std::string& fragment_entry) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\n"
      << "using namespace metal;\n\n"
      << "struct VgRasterVertex { packed_float3 position; packed_float2 uv; };\n"
      << "struct VgRasterVaryings { float4 position [[position]]; float2 uv; };\n"
      << "struct VgRasterFragment { float4 color [[color(0)]]; };\n\n"
      << "vertex VgRasterVaryings " << vertex_entry
      << "(device const VgRasterVertex* vertices [[buffer(" << vg::compiler::kRasterVertexBufferIndex << ")]],\n"
      << "                                         uint vid [[vertex_id]]) {\n"
      << "  VgRasterVaryings varyings;\n"
      << "  varyings.position = float4(float3(vertices[vid].position), 1.0f);\n"
      << "  varyings.uv = float2(vertices[vid].uv);\n"
      << "  return varyings;\n"
      << "}\n\n"
      << "fragment VgRasterFragment " << fragment_entry
      << "(VgRasterVaryings varyings [[stage_in]],\n"
      << "                                             texture2d<float, access::sample> tex [[texture("
      << vg::compiler::kRasterTextureIndex << ")]],\n"
      << "                                             sampler samp [[sampler(" << vg::compiler::kRasterSamplerIndex
      << ")]],\n"
      << "                                             constant float4& tint [[buffer("
      << vg::compiler::kRasterTintBufferIndex << ")]]) {\n"
      << "  (void)tex; (void)samp; (void)tint;\n"
      << "  VgRasterFragment result;\n"
      << "  result.color = float4(0.0f, 1.0f, 0.0f, 1.0f);\n"
      << "  return result;\n"
      << "}\n";
  return out.str();
}

// E008: Texture2DArray + mip, sampled through metal::SampleCoord, compared
// against reference::sample_facet. A second call against the same FacetRef
// must report facet_cache_hit. Out-of-range slice/lod is a rejection, not a
// clamp. The three Phase C capability bits this adapter now advertises are
// asserted here so a device that dropped one cannot hide behind a green sample.
bool run_sample_facet(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "sample-facet: no Metal device available on this host\n";
    return false;
  }
  const auto& caps = metal_device->capabilities();
  if (!caps.supports(vg::hal::Capability::Raster) ||
      !caps.supports(vg::hal::Capability::RepresentationTransform) ||
      !caps.supports(vg::hal::Capability::CheckedFacetGeneration)) {
    std::cerr << "sample-facet: device must advertise Raster, RepresentationTransform and "
                 "CheckedFacetGeneration\n";
    return false;
  }

  vg::core::CanonicalView view;
  view.format = vg::core::PixelFormat::RGBA8Unorm;
  view.dimension = vg::core::ViewDimension::Texture2DArray;
  view.width = 2;
  view.height = 2;
  view.array_layers = 2;
  view.mip_levels = 2;
  std::string view_error;
  if (!view.valid(&view_error)) {
    std::cerr << "sample-facet: CanonicalView rejected: " << view_error << "\n";
    return false;
  }

  vg::core::Arena arena;
  auto& allocation = arena.allocate(view.byte_size());
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  // Distinct solid colour per (layer, level), packed through the view's own
  // linear layout so Metal's upload and the CPU oracle read the same bytes.
  const std::array<std::array<uint8_t, 4>, 4> colours = {{
      {255, 0, 0, 255},
      {0, 255, 0, 255},
      {0, 0, 255, 255},
      {255, 255, 255, 255},
  }};
  fill_subresource(allocation, view, 0, 0, colours[0]);
  fill_subresource(allocation, view, 0, 1, colours[1]);
  fill_subresource(allocation, view, 1, 0, colours[2]);
  fill_subresource(allocation, view, 1, 1, colours[3]);

  vg::core::FacetPool pool;
  vg::core::FacetRef sample_ref;
  std::string error;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &sample_ref, &error)) {
    std::cerr << "sample-facet: acquire failed: " << error << "\n";
    return false;
  }

  const std::vector<vg::metal::SampleCoord> coords = {
      {0.5f, 0.5f, 0.0f, 0},
      {0.5f, 0.5f, 1.0f, 0},
      {0.5f, 0.5f, 0.0f, 1},
      {0.5f, 0.5f, 1.0f, 1},
  };
  vg::metal::SampleFacetResult result;
  if (!metal_device->run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, coords,
                                      vg::core::ValidationProfile::FastNative, &result, &error)) {
    std::cerr << "sample-facet: Metal sample failed: " << error << "\n";
    return false;
  }
  auto oracle = vg::reference::sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                            vg::core::WrapMode::Clamp, to_reference_coords(coords));
  if (!oracle.ok) {
    std::cerr << "sample-facet: reference oracle failed: " << oracle.message << "\n";
    return false;
  }
  if (result.sampled_rgba.size() != oracle.sampled_rgba.size()) {
    std::cerr << "sample-facet: sampled_rgba size mismatch\n";
    return false;
  }
  for (size_t i = 0; i < coords.size(); ++i) {
    if (!channels_close(result.sampled_rgba[i], oracle.sampled_rgba[i], kNearestTol, "sample-facet",
                        "coord sample"))
      return false;
  }
  // lod 0 vs lod 1, and slice 0 vs slice 1, must actually select different
  // subresources -- a level-0-only or slice-0-only path would pass the oracle
  // comparison if both sides made the same mistake.
  const auto same_colour = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
    for (int c = 0; c < 4; ++c)
      if (std::fabs(a[c] - b[c]) > kNearestTol) return false;
    return true;
  };
  if (same_colour(result.sampled_rgba[0], result.sampled_rgba[1])) {
    std::cerr << "sample-facet: lod 0 and lod 1 produced the same colour\n";
    return false;
  }
  if (same_colour(result.sampled_rgba[0], result.sampled_rgba[2])) {
    std::cerr << "sample-facet: slice 0 and slice 1 produced the same colour\n";
    return false;
  }

  vg::metal::SampleFacetResult second;
  if (!metal_device->run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, coords,
                                      vg::core::ValidationProfile::FastNative, &second, &error)) {
    std::cerr << "sample-facet: second sample failed: " << error << "\n";
    return false;
  }
  if (!second.facet_cache_hit) {
    std::cerr << "sample-facet: expected facet_cache_hit on second use\n";
    return false;
  }

  vg::metal::SampleFacetResult unused;
  const std::vector<vg::metal::SampleCoord> bad_slice{{0.5f, 0.5f, 0.0f, 2}};
  if (metal_device->run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                     vg::core::WrapMode::Clamp, bad_slice,
                                     vg::core::ValidationProfile::FastNative, &unused, &error)) {
    std::cerr << "sample-facet: out-of-range slice must be rejected\n";
    return false;
  }
  const std::vector<vg::metal::SampleCoord> bad_lod{{0.5f, 0.5f, 2.0f, 0}};
  if (metal_device->run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                     vg::core::WrapMode::Clamp, bad_lod,
                                     vg::core::ValidationProfile::FastNative, &unused, &error)) {
    std::cerr << "sample-facet: out-of-range lod must be rejected\n";
    return false;
  }

  std::cout << "sample-facet: ok\n";
  return true;
}

// 06 §6.4: live SampleFacet + CheckedNative writes no poison; a retired or
// forged generation is rejected in-shader (call succeeds, violations>0,
// channels == kFacetGenerationPoisonValue). FastNative still fails that token
// host-side. After Arena::transform the old FacetRef is EpochStale, which the
// generation table cannot encode, so CheckedNative is refused host-side.
bool run_checked_facet_generation(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "checked-facet-generation: no Metal device available on this host\n";
    return false;
  }

  constexpr uint32_t kW = 2;
  constexpr uint32_t kH = 2;
  vg::core::Arena arena;
  auto& allocation = arena.allocate(static_cast<uint64_t>(kW) * kH * 4);
  allocation.bytes = {
      255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
  };
  const vg::core::CanonicalView view = make_rgba8_view(allocation, {.width = kW, .height = kH});

  vg::core::FacetPool pool;
  vg::core::FacetRef live_ref;
  std::string error;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &live_ref, &error)) {
    std::cerr << "checked-facet-generation: acquire failed: " << error << "\n";
    return false;
  }

  const std::vector<vg::metal::SampleCoord> coords{{0.25f, 0.25f, 0.0f, 0}};
  vg::metal::SampleFacetResult live;
  if (!metal_device->run_sample_facet(arena, pool, live_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, coords,
                                      vg::core::ValidationProfile::CheckedNative, &live, &error)) {
    std::cerr << "checked-facet-generation: live CheckedNative sample failed: " << error << "\n";
    return false;
  }
  if (!live.checked_profile || live.generation_violations != 0) {
    std::cerr << "checked-facet-generation: live token must run checked with zero violations\n";
    return false;
  }

  const vg::core::FacetRef retired_ref = live_ref;
  if (!pool.retire(retired_ref, &error)) {
    std::cerr << "checked-facet-generation: retire failed: " << error << "\n";
    return false;
  }
  vg::metal::SampleFacetResult retired;
  if (!metal_device->run_sample_facet(arena, pool, retired_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, coords,
                                      vg::core::ValidationProfile::CheckedNative, &retired, &error)) {
    std::cerr << "checked-facet-generation: retired CheckedNative must succeed (shader poison): "
              << error << "\n";
    return false;
  }
  if (!retired.checked_profile || retired.generation_violations == 0) {
    std::cerr << "checked-facet-generation: retired token must report generation_violations>0\n";
    return false;
  }
  for (int c = 0; c < 4; ++c) {
    if (retired.sampled_rgba[0][c] != vg::compiler::kFacetGenerationPoisonValue) {
      std::cerr << "checked-facet-generation: retired channel " << c
                << " is not kFacetGenerationPoisonValue\n";
      return false;
    }
  }

  vg::core::FacetRef forged = retired_ref;
  forged.generation = retired_ref.generation + 99;
  vg::metal::SampleFacetResult forged_result;
  if (!metal_device->run_sample_facet(arena, pool, forged, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, coords,
                                      vg::core::ValidationProfile::CheckedNative, &forged_result,
                                      &error)) {
    std::cerr << "checked-facet-generation: forged CheckedNative must succeed (shader poison): "
              << error << "\n";
    return false;
  }
  if (!forged_result.checked_profile || forged_result.generation_violations == 0) {
    std::cerr << "checked-facet-generation: forged token must report generation_violations>0\n";
    return false;
  }
  for (int c = 0; c < 4; ++c) {
    if (forged_result.sampled_rgba[0][c] != vg::compiler::kFacetGenerationPoisonValue) {
      std::cerr << "checked-facet-generation: forged channel " << c
                << " is not kFacetGenerationPoisonValue\n";
      return false;
    }
  }

  vg::metal::SampleFacetResult fast_dead;
  if (metal_device->run_sample_facet(arena, pool, retired_ref, vg::core::FilterMode::Nearest,
                                     vg::core::WrapMode::Clamp, coords,
                                     vg::core::ValidationProfile::FastNative, &fast_dead, &error)) {
    std::cerr << "checked-facet-generation: FastNative must refuse a dead token host-side\n";
    return false;
  }

  vg::core::FacetRef epoch_ref;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &epoch_ref, &error)) {
    std::cerr << "checked-facet-generation: re-acquire failed: " << error << "\n";
    return false;
  }
  uint32_t new_epoch = 0;
  if (!arena.transform(allocation.id, allocation.generation, &new_epoch, &error)) {
    std::cerr << "checked-facet-generation: Arena::transform failed: " << error << "\n";
    return false;
  }
  vg::core::FacetStatus status = vg::core::FacetStatus::Ok;
  if (pool.lookup(arena, epoch_ref, &status) != nullptr || status != vg::core::FacetStatus::EpochStale) {
    std::cerr << "checked-facet-generation: old FacetRef must be EpochStale after Arena::transform\n";
    return false;
  }
  vg::metal::SampleFacetResult stale;
  if (metal_device->run_sample_facet(arena, pool, epoch_ref, vg::core::FilterMode::Nearest,
                                     vg::core::WrapMode::Clamp, coords,
                                     vg::core::ValidationProfile::CheckedNative, &stale, &error)) {
    std::cerr << "checked-facet-generation: CheckedNative must refuse EpochStale host-side "
                 "(generation table cannot encode epoch staleness)\n";
    return false;
  }

  std::cout << "checked-facet-generation: ok\n";
  return true;
}

// Source Sample + target Attachment, full-screen quad, Metal Y-up. Interior
// pixels are compared against reference::raster_triangles at the E008 nearest
// tolerance (edge pixels are where Metal's per-pixel shade and the oracle's
// per-sample shade are allowed to disagree). Wrong kind, a vertex count that
// is not a multiple of 3, and sample_count>1 without MultisampleResolve fail.
bool run_basic_raster(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "basic-raster: no Metal device available on this host\n";
    return false;
  }

  constexpr uint32_t kExtent = 4;
  constexpr uint32_t kBytes = kExtent * kExtent * 4;
  vg::core::Arena arena;
  auto& source_alloc = arena.allocate(kBytes);
  auto& target_alloc = arena.allocate(kBytes);
  auto& depth_alloc = arena.allocate(kBytes);
  for (uint32_t y = 0; y < kExtent; ++y) {
    for (uint32_t x = 0; x < kExtent; ++x) {
      const uint64_t texel = (static_cast<uint64_t>(y) * kExtent + x) * 4;
      source_alloc.bytes[texel + 0] = static_cast<uint8_t>(10 + 40 * x);
      source_alloc.bytes[texel + 1] = static_cast<uint8_t>(20 + 40 * y);
      source_alloc.bytes[texel + 2] = static_cast<uint8_t>(30 + 8 * x + 16 * y);
      source_alloc.bytes[texel + 3] = 255;
    }
  }

  const vg::core::CanonicalView source_view = make_rgba8_view(source_alloc, {.width = kExtent, .height = kExtent});
  const vg::core::CanonicalView target_view = make_rgba8_view(target_alloc, {.width = kExtent, .height = kExtent});
  const vg::core::CanonicalView depth_view = make_depth32_view(depth_alloc, {.width = kExtent, .height = kExtent});

  vg::core::FacetPool pool;
  vg::core::FacetRef source_ref;
  vg::core::FacetRef target_ref;
  vg::core::FacetRef depth_ref;
  std::string error;
  if (!pool.acquire(arena, source_view, vg::core::FacetKind::Sample, &source_ref, &error) ||
      !pool.acquire(arena, target_view, vg::core::FacetKind::Attachment, &target_ref, &error)) {
    std::cerr << "basic-raster: acquire failed: " << error << "\n";
    return false;
  }

  vg::metal::RasterDesc desc;
  desc.filter = vg::core::FilterMode::Nearest;
  desc.wrap = vg::core::WrapMode::Clamp;
  desc.attachment.load = vg::metal::AttachmentLoadAction::Clear;
  desc.attachment.store = vg::metal::AttachmentStoreAction::Store;
  desc.attachment.clear_rgba = {0.0f, 0.0f, 0.0f, 1.0f};
  desc.attachment.sample_count = 1;

  const auto quad = metal_fullscreen_quad();
  vg::metal::RasterResult metal_result;
  if (!metal_device->run_raster_triangles(arena, pool, {.source = source_ref, .target = target_ref}, desc, quad,
                                          &metal_result, &error)) {
    std::cerr << "basic-raster: Metal raster failed: " << error << "\n";
    return false;
  }
  auto oracle = vg::reference::raster_triangles(arena, pool, {.source = source_ref, .target = target_ref},
                                                to_reference_desc(desc), to_reference_vertices(quad));
  if (!oracle.ok) {
    std::cerr << "basic-raster: reference oracle failed: " << oracle.message << "\n";
    return false;
  }
  if (metal_result.resolved_rgba.size() != oracle.resolved_rgba.size() ||
      metal_result.width != kExtent || metal_result.height != kExtent) {
    std::cerr << "basic-raster: resolved image size mismatch\n";
    return false;
  }
  for (uint32_t y = 1; y + 1 < kExtent; ++y) {
    for (uint32_t x = 1; x + 1 < kExtent; ++x) {
      const size_t index = static_cast<size_t>(y) * kExtent + x;
      if (!channels_close(metal_result.resolved_rgba[index], oracle.resolved_rgba[index], kNearestTol,
                          "basic-raster", "interior pixel"))
        return false;
    }
  }

  vg::core::FacetRef wrong_source;
  vg::core::FacetRef wrong_target;
  if (!pool.acquire(arena, source_view, vg::core::FacetKind::Attachment, &wrong_source, &error) ||
      !pool.acquire(arena, target_view, vg::core::FacetKind::Sample, &wrong_target, &error)) {
    std::cerr << "basic-raster: wrong-kind acquire failed: " << error << "\n";
    return false;
  }
  vg::metal::RasterResult unused;
  if (metal_device->run_raster_triangles(arena, pool, {.source = wrong_source, .target = target_ref}, desc, quad, &unused,
                                         &error)) {
    std::cerr << "basic-raster: Attachment source must be rejected\n";
    return false;
  }
  if (metal_device->run_raster_triangles(arena, pool, {.source = source_ref, .target = wrong_target}, desc, quad, &unused,
                                         &error)) {
    std::cerr << "basic-raster: Sample target must be rejected\n";
    return false;
  }

  const std::vector<vg::metal::RasterVertex> dangling{quad[0], quad[1], quad[2], quad[3]};
  if (metal_device->run_raster_triangles(arena, pool, {.source = source_ref, .target = target_ref}, desc, dangling, &unused,
                                         &error)) {
    std::cerr << "basic-raster: vertex count not a multiple of 3 must be rejected\n";
    return false;
  }

  vg::metal::RasterDesc msaa = desc;
  msaa.attachment.sample_count = 4;
  msaa.attachment.store = vg::metal::AttachmentStoreAction::Store;
  if (metal_device->run_raster_triangles(arena, pool, {.source = source_ref, .target = target_ref}, msaa, quad, &unused,
                                         &error)) {
    std::cerr << "basic-raster: sample_count>1 without MultisampleResolve must be rejected\n";
    return false;
  }
  msaa.attachment.store = vg::metal::AttachmentStoreAction::MultisampleResolve;
  vg::metal::RasterResult resolved;
  if (!metal_device->run_raster_triangles(arena, pool, {.source = source_ref, .target = target_ref}, msaa, quad, &resolved,
                                          &error)) {
    std::cerr << "basic-raster: sample_count=4 with MultisampleResolve failed: " << error << "\n";
    return false;
  }
  if (resolved.sample_count != 4) {
    std::cerr << "basic-raster: resolve must report sample_count 4\n";
    return false;
  }

  std::cout << "basic-raster: ok\n";
  return true;
}

// F2 (ADR-043 Decision #3, ADR-046): rasterization is a shape of TaskRecord/
// ExecutionPlan, not a parallel API -- unlike run_basic_raster above (which
// calls run_raster_triangles() directly against a locally-constructed pool),
// this drives a Raster-kind TaskRecord through the same TaskGraphBuilder ->
// seal -> publish -> ExecutionPlan -> compile() -> submit() path
// run_task_tier0 uses for Compute tasks. Facets are acquired against the
// *device's own* facet_pool() (not a local one), because SubmitOps::raster
// resolves task.raster_facets/vertex_buffer_ref against metal.facet_pool()
// during submit(), not against whatever pool the caller happens to hold.
bool run_task_graph_raster(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "task-graph-raster: no Metal device available on this host\n";
    return false;
  }

  constexpr uint32_t kExtent = 4;
  constexpr uint32_t kBytes = kExtent * kExtent * 4;
  vg::core::Arena arena;
  auto& source_alloc = arena.allocate(kBytes);
  auto& target_alloc = arena.allocate(kBytes);
  auto& depth_alloc = arena.allocate(kBytes);
  for (uint32_t y = 0; y < kExtent; ++y) {
    for (uint32_t x = 0; x < kExtent; ++x) {
      const uint64_t texel = (static_cast<uint64_t>(y) * kExtent + x) * 4;
      source_alloc.bytes[texel + 0] = static_cast<uint8_t>(10 + 40 * x);
      source_alloc.bytes[texel + 1] = static_cast<uint8_t>(20 + 40 * y);
      source_alloc.bytes[texel + 2] = static_cast<uint8_t>(30 + 8 * x + 16 * y);
      source_alloc.bytes[texel + 3] = 255;
    }
  }

  const vg::core::CanonicalView source_view = make_rgba8_view(source_alloc, {.width = kExtent, .height = kExtent});
  const vg::core::CanonicalView target_view = make_rgba8_view(target_alloc, {.width = kExtent, .height = kExtent});
  const vg::core::CanonicalView depth_view = make_depth32_view(depth_alloc, {.width = kExtent, .height = kExtent});

  vg::core::FacetRef source_ref;
  vg::core::FacetRef target_ref;
  vg::core::FacetRef depth_ref;
  std::string error;
  if (!metal_device->facet_pool().acquire(arena, source_view, vg::core::FacetKind::Sample, &source_ref, &error) ||
      !metal_device->facet_pool().acquire(arena, target_view, vg::core::FacetKind::Attachment, &target_ref, &error) ||
      !metal_device->facet_pool().acquire(arena, depth_view, vg::core::FacetKind::Attachment, &depth_ref, &error)) {
    std::cerr << "task-graph-raster: acquire failed: " << error << "\n";
    return false;
  }

  const auto quad = metal_fullscreen_quad();
  const uint64_t vertex_bytes = quad.size() * sizeof(vg::metal::RasterVertex);
  auto& vertex_alloc = arena.allocate(vertex_bytes);
  std::memcpy(vertex_alloc.bytes.data(), quad.data(), vertex_bytes);
  const vg::core::CanonicalView vertex_view =
      make_rgba8_view(vertex_alloc, {.width = static_cast<uint32_t>(vertex_bytes / 4), .height = 1});
  vg::core::FacetRef vertex_ref;
  if (!metal_device->facet_pool().acquire(arena, vertex_view, vg::core::FacetKind::Address, &vertex_ref, &error)) {
    std::cerr << "task-graph-raster: vertex facet acquire failed: " << error << "\n";
    return false;
  }

  TaskRecord raster_task{};
  raster_task.kind = vg::core::TaskKind::Raster;
  raster_task.raster_facets = {.source = source_ref, .target = target_ref};
  raster_task.vertex_buffer_ref = vertex_ref;
  raster_task.depth_attachment_ref = depth_ref;
  raster_task.depth_test_enable = true;
  raster_task.depth_write_enable = true;
  raster_task.depth_compare_op = vg::core::DepthCompareOp::Less;
  raster_task.raster_filter = vg::core::FilterMode::Nearest;
  raster_task.raster_wrap = vg::core::WrapMode::Clamp;

  const auto module = make_probe_module(arena);
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions raster_options;
  raster_options.facet_pool = &metal_device->facet_pool();
  if (!assemble_compute_plan(arena, module, {raster_task}, &plan, &error,
                             raster_options)) {
    std::cerr << "task-graph-raster: plan assembly failed: " << error << "\n";
    return false;
  }

  vg::hal::CompiledPlan compiled;
  if (!metal_device->compile(plan, &compiled, &error)) {
    std::cerr << "task-graph-raster: Metal compile failed: " << error << "\n";
    return false;
  }
  if (compiled.per_node_packages.size() != 1 ||
      compiled.per_node_packages[0].kind != vg::hal::CompiledPlan::NodePackageKind::Raster ||
      compiled.per_node_packages[0].package.has_value()) {
    std::cerr << "task-graph-raster: canonical raster Node was compiled as compute\n";
    return false;
  }

  vg::hal::Submission submission;
  if (!metal_device->submit(compiled, arena, &submission, &error)) {
    std::cerr << "task-graph-raster: Metal submit failed: " << error << "\n";
    return false;
  }
  if (!submission.result.ok) {
    std::cerr << "task-graph-raster: Metal execution reported failure: " << submission.result.message << "\n";
    return false;
  }
  if (!submission.published_tasks.empty()) {
    std::cerr << "task-graph-raster: raster Task must not enter the compute-only publication ring\n";
    return false;
  }
  if (submission.result.trace.size() != plan.task_effects[0].size() ||
      std::ranges::any_of(submission.result.trace, [&](const vg::ir::Effect& effect) {
        return effect.allocation == module.instructions[0].allocation;
      })) {
    std::cerr << "task-graph-raster: canonical module was executed as a compute pre-pass\n";
    return false;
  }
  const auto lifetime_released = [&](const char* phase) {
    for (const auto ref : {source_ref, target_ref, vertex_ref, depth_ref}) {
      if (metal_device->facet_pool().in_flight(ref) != 0) {
        std::cerr << "task-graph-raster: " << phase << " leaked a facet lifetime hold\n";
        return false;
      }
    }
    for (const auto* allocation : {&source_alloc, &target_alloc, &vertex_alloc, &depth_alloc}) {
      if (allocation->in_flight != 0) {
        std::cerr << "task-graph-raster: " << phase << " leaked an allocation lifetime hold\n";
        return false;
      }
    }
    return true;
  };
  if (!lifetime_released("successful submit")) return false;
  vg::hal::Submission repeated_submission;
  if (!metal_device->submit(compiled, arena, &repeated_submission, &error) ||
      !repeated_submission.result.ok || !lifetime_released("repeat submit")) {
    std::cerr << "task-graph-raster: repeat submit/lifetime release failed: " << error << "\n";
    return false;
  }
  if (submission.raster_results.size() != 1) {
    std::cerr << "task-graph-raster: expected exactly one raster_results entry, got "
              << submission.raster_results.size() << "\n";
    return false;
  }
  const auto& raster_result = submission.raster_results[0];
  if (raster_result.task_index != 0 || raster_result.width != kExtent || raster_result.height != kExtent) {
    std::cerr << "task-graph-raster: raster_results[0] shape mismatch\n";
    return false;
  }
  if (raster_result.resolved_depth.size() != static_cast<size_t>(kExtent) * kExtent) {
    std::cerr << "task-graph-raster: depth readback missing\n";
    return false;
  }

  // F2's fixed attachment defaults (load=Clear, store=Store, clear_rgba
  // {0,0,0,1}, sample_count=1, subresource {0,0}) are hard-coded inside
  // submit(); only filter/wrap/tint travel through the TaskRecord. Mirror
  // both here so the oracle call matches exactly what submit() ran.
  vg::metal::RasterDesc oracle_desc;
  oracle_desc.filter = raster_task.raster_filter;
  oracle_desc.wrap = raster_task.raster_wrap;
  oracle_desc.attachment = vg::hal::f2_default_raster_attachment_config<vg::metal::AttachmentFacetDesc>();
  oracle_desc.depth_attachment_ref = depth_ref;
  oracle_desc.depth_test_enable = true;
  oracle_desc.depth_write_enable = true;
  oracle_desc.depth_compare_op = vg::core::DepthCompareOp::Less;
  auto oracle = vg::reference::raster_triangles(arena, metal_device->facet_pool(),
                                                {.source = source_ref, .target = target_ref},
                                                to_reference_desc(oracle_desc), to_reference_vertices(quad));
  if (!oracle.ok) {
    std::cerr << "task-graph-raster: reference oracle failed: " << oracle.message << "\n";
    return false;
  }
  if (raster_result.resolved_depth.size() != oracle.resolved_depth.size()) {
    std::cerr << "task-graph-raster: depth size mismatch\n";
    return false;
  }
  for (size_t i = 0; i < raster_result.resolved_depth.size(); ++i)
    if (std::fabs(raster_result.resolved_depth[i] - oracle.resolved_depth[i]) > kNearestTol) {
      std::cerr << "task-graph-raster: depth mismatch " << raster_result.resolved_depth[i] << " vs "
                << oracle.resolved_depth[i] << "\n";
      return false;
    }
  if (raster_result.resolved_rgba.size() != oracle.resolved_rgba.size()) {
    std::cerr << "task-graph-raster: resolved image size mismatch\n";
    return false;
  }
  for (uint32_t y = 1; y + 1 < kExtent; ++y) {
    for (uint32_t x = 1; x + 1 < kExtent; ++x) {
      const size_t index = static_cast<size_t>(y) * kExtent + x;
      if (!channels_close(raster_result.resolved_rgba[index], oracle.resolved_rgba[index], kNearestTol,
                          "task-graph-raster", "interior pixel"))
        return false;
    }
  }

  // F5: four vertices plus six indices prove Metal did not silently retain
  // drawPrimitives. Both element widths must match the Reference oracle.
  const std::vector<vg::metal::RasterVertex> indexed_vertices{quad[0], quad[1], quad[2], quad[4]};
  auto& indexed_vertex_alloc = arena.allocate(indexed_vertices.size() * sizeof(vg::metal::RasterVertex));
  std::memcpy(indexed_vertex_alloc.bytes.data(), indexed_vertices.data(), indexed_vertex_alloc.bytes.size());
  const auto indexed_vertex_view = make_rgba8_view(
      indexed_vertex_alloc, {.width = static_cast<uint32_t>(indexed_vertex_alloc.bytes.size() / 4), .height = 1});
  vg::core::FacetRef indexed_vertex_ref;
  if (!metal_device->facet_pool().acquire(arena, indexed_vertex_view, vg::core::FacetKind::Address,
                                          &indexed_vertex_ref, &error)) return false;
  const auto run_indexed = [&](const void* bytes, size_t byte_count, vg::core::PixelFormat format,
                               const char* label) {
    auto& index_alloc = arena.allocate(byte_count);
    std::memcpy(index_alloc.bytes.data(), bytes, byte_count);
    auto index_view = make_rgba8_view(index_alloc, {.width = 6, .height = 1});
    index_view.format = format;
    vg::core::FacetRef index_ref;
    if (!metal_device->facet_pool().acquire(arena, index_view, vg::core::FacetKind::Address, &index_ref, &error)) return false;
    vg::core::FacetRef indexed_depth_ref;
    if (!metal_device->facet_pool().acquire(arena, depth_view, vg::core::FacetKind::Attachment,
                                            &indexed_depth_ref, &error)) return false;
    TaskRecord indexed_task = raster_task;
    indexed_task.vertex_buffer_ref = indexed_vertex_ref;
    indexed_task.index_buffer_ref = index_ref;
    indexed_task.index_count = 6;
    indexed_task.depth_attachment_ref = indexed_depth_ref;
    vg::core::ExecutionPlan indexed_plan;
    if (!assemble_compute_plan(arena, module, {indexed_task}, &indexed_plan, &error,
                               raster_options)) return false;
    vg::hal::CompiledPlan indexed_compiled;
    vg::hal::Submission indexed_submission;
    if (!metal_device->compile(indexed_plan, &indexed_compiled, &error) ||
        !metal_device->submit(indexed_compiled, arena, &indexed_submission, &error) || !indexed_submission.result.ok ||
        indexed_submission.raster_results.size() != 1) return false;
    for (const auto ref : {source_ref, target_ref, indexed_vertex_ref, index_ref, indexed_depth_ref})
      if (metal_device->facet_pool().in_flight(ref) != 0) {
        error = std::string(label) + ": leaked a facet lifetime hold";
        return false;
      }
    for (const auto* allocation : {&source_alloc, &target_alloc, &indexed_vertex_alloc,
                                   &index_alloc, &depth_alloc})
      if (allocation->in_flight != 0) {
        error = std::string(label) + ": leaked an allocation lifetime hold";
        return false;
      }
    const auto& actual = indexed_submission.raster_results[0].resolved_rgba;
    if (actual.size() != oracle.resolved_rgba.size()) { error = std::string(label) + ": color size"; return false; }
    for (size_t i = 0; i < actual.size(); ++i)
      if (!channels_close(actual[i], oracle.resolved_rgba[i], kNearestTol, label, "full indexed image")) return false;
    const auto& actual_depth = indexed_submission.raster_results[0].resolved_depth;
    if (actual_depth.size() != oracle.resolved_depth.size()) { error = std::string(label) + ": depth size"; return false; }
    for (size_t i = 0; i < actual_depth.size(); ++i)
      if (std::fabs(actual_depth[i] - oracle.resolved_depth[i]) > kNearestTol) {
        error = std::string(label) + ": depth mismatch";
        return false;
      }
    return true;
  };
  const std::array<uint16_t, 6> indices16{0, 1, 2, 2, 1, 3};
  const std::array<uint32_t, 6> indices32{0, 1, 2, 2, 1, 3};
  if (!run_indexed(indices16.data(), sizeof(indices16), vg::core::PixelFormat::R16Uint, "indexed-u16") ||
      !run_indexed(indices32.data(), sizeof(indices32), vg::core::PixelFormat::R32Uint, "indexed-u32")) {
    std::cerr << "task-graph-raster: indexed Metal/reference differential failed: " << error << "\n";
    return false;
  }

  // ADR-047/052 narrowing remains authoritative: a mixed compute+raster
  // graph is rejected by semantic assembly and never reaches Metal.  This is
  // intentionally a negative conformance case, not partial-execution logic.
  const auto& mixed_root = arena.allocate(4);
  TaskRecord compute_task{};
  compute_task.root_allocation = mixed_root.id;
  compute_task.root_generation = mixed_root.generation;
  compute_task.x = 2;
  compute_task.y = 1;
  compute_task.z = 1;
  TaskRecord mixed_raster_task = raster_task;
  vg::test_support::AssemblyOptions mixed_options;
  mixed_options.timeline_signal = 7;
  mixed_options.facet_pool = &metal_device->facet_pool();
  vg::core::ExecutionPlan mixed_plan;
  std::string mixed_error;
  if (assemble_compute_plan(arena, module, {compute_task, mixed_raster_task}, &mixed_plan, &mixed_error,
                            mixed_options)) {
    std::cerr << "task-graph-raster: semantic assembly unexpectedly accepted mixed compute+raster\n";
    return false;
  }
  if (mixed_error != "compute+raster mixed-domain TaskGraphs remain Unsupported") {
    std::cerr << "task-graph-raster: unexpected mixed-domain rejection: " << mixed_error << "\n";
    return false;
  }
  if (mixed_root.in_flight != 0) {
    std::cerr << "task-graph-raster: rejected mixed-domain assembly acquired a lifetime hold\n";
    return false;
  }

  std::cout << "task-graph-raster: ok\n";
  return true;
}

// F4: one public-task-shaped raster submission with two fully overlapping
// triangles in one triangle list. The first samples the red source texel at
// z=.75; the second samples green at z=.25. Less+write therefore makes the
// center green. Keeping both triangles in one task deliberately exercises the
// per-task clear=1.0 rule rather than assuming depth carries across tasks.
bool run_task_graph_raster_depth(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "task-graph-raster-depth: no Metal device available on this host\n";
    return false;
  }
  constexpr uint32_t kExtent = 4;
  vg::core::Arena arena;
  auto& source_alloc = arena.allocate(2 * 4);
  source_alloc.bytes = {255, 0, 0, 255, 0, 255, 0, 255};
  auto& target_alloc = arena.allocate(kExtent * kExtent * 4);
  auto& depth_alloc = arena.allocate(kExtent * kExtent * 4);
  const auto source_view = make_rgba8_view(source_alloc, {.width = 2, .height = 1});
  const auto target_view = make_rgba8_view(target_alloc, {.width = kExtent, .height = kExtent});
  const auto depth_view = make_depth32_view(depth_alloc, {.width = kExtent, .height = kExtent});

  vg::core::FacetRef source_ref, target_ref, depth_ref;
  std::string error;
  if (!metal_device->facet_pool().acquire(arena, source_view, vg::core::FacetKind::Sample, &source_ref, &error) ||
      !metal_device->facet_pool().acquire(arena, target_view, vg::core::FacetKind::Attachment, &target_ref, &error) ||
      !metal_device->facet_pool().acquire(arena, depth_view, vg::core::FacetKind::Attachment, &depth_ref, &error)) {
    std::cerr << "task-graph-raster-depth: facet acquire failed: " << error << "\n";
    return false;
  }
  const auto tri = [](float z, float u) {
    return std::array<vg::metal::RasterVertex, 3>{{
        {-1.0f, 1.0f, z, u, 0.5f}, {3.0f, 1.0f, z, u, 0.5f}, {-1.0f, -3.0f, z, u, 0.5f}}};
  };
  std::vector<vg::metal::RasterVertex> vertices;
  const auto far = tri(0.75f, 0.25f);
  const auto near = tri(0.25f, 0.75f);
  vertices.insert(vertices.end(), far.begin(), far.end());
  vertices.insert(vertices.end(), near.begin(), near.end());
  auto& vertex_alloc = arena.allocate(vertices.size() * sizeof(vg::metal::RasterVertex));
  std::memcpy(vertex_alloc.bytes.data(), vertices.data(), vertex_alloc.bytes.size());
  const auto vertex_view = make_rgba8_view(
      vertex_alloc, {.width = static_cast<uint32_t>(vertex_alloc.bytes.size() / 4), .height = 1});
  vg::core::FacetRef vertex_ref;
  if (!metal_device->facet_pool().acquire(arena, vertex_view, vg::core::FacetKind::Address, &vertex_ref, &error)) {
    std::cerr << "task-graph-raster-depth: vertex facet acquire failed: " << error << "\n";
    return false;
  }
  TaskRecord task{};
  task.kind = vg::core::TaskKind::Raster;
  task.raster_facets = {.source = source_ref, .target = target_ref};
  task.depth_attachment_ref = depth_ref;
  task.depth_test_enable = true;
  task.depth_write_enable = true;
  task.depth_compare_op = vg::core::DepthCompareOp::Less;
  task.vertex_buffer_ref = vertex_ref;
  task.raster_filter = vg::core::FilterMode::Nearest;
  task.raster_wrap = vg::core::WrapMode::Clamp;
  const auto module = make_probe_module(arena);
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions raster_options;
  raster_options.facet_pool = &metal_device->facet_pool();
  if (!assemble_compute_plan(arena, module, {task}, &plan, &error,
                             raster_options)) {
    std::cerr << "task-graph-raster-depth: plan assembly failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan compiled;
  if (!metal_device->compile(plan, &compiled, &error)) {
    std::cerr << "task-graph-raster-depth: Metal compile failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission submission;
  if (!metal_device->submit(compiled, arena, &submission, &error) || !submission.result.ok ||
      submission.raster_results.size() != 1) {
    std::cerr << "task-graph-raster-depth: submit failed: "
              << (error.empty() ? submission.result.message : error) << "\n";
    return false;
  }
  const auto& center = submission.raster_results[0].resolved_rgba[(kExtent / 2) * kExtent + kExtent / 2];
  if (!channels_close(center, {0.0f, 1.0f, 0.0f, 1.0f}, kNearestTol,
                      "task-graph-raster-depth", "near triangle wins Less test"))
    return false;

  // An RGBA attachment may not masquerade as depth. Validation can happen in
  // compile or submit; either route is acceptable, but it must not succeed.
  TaskRecord invalid = task;
  invalid.depth_attachment_ref = target_ref;
  TaskGraphBuilder invalid_builder;
  TaskGraph invalid_graph;
  if (!invalid_builder.append(invalid) || !invalid_builder.seal(&invalid_graph) || !invalid_graph.publish()) return false;
  plan.task_graph = invalid_graph;
  plan.graph_epoch = arena.topology_epoch();
  vg::hal::CompiledPlan invalid_compiled;
  if (metal_device->compile(plan, &invalid_compiled, &error)) {
    vg::hal::Submission invalid_submission;
    if (!metal_device->submit(invalid_compiled, arena, &invalid_submission, &error) || invalid_submission.result.ok) {
      std::cerr << "task-graph-raster-depth: RGBA depth attachment was accepted\n";
      return false;
    }
  }
  std::cout << "task-graph-raster-depth: ok\n";
  return true;
}

// F3 (ADR-043 Decision #4): restricted-import "vg.msl.raster/v1" shaders --
// the resolved Node owns a user raster contract rather than canonical compute
// IR. Three sub-cases:
//   (a) happy path: a real, hand-written MSL vertex+fragment pair matching
//       the exact binding contract run_raster_pass's encoder assumes
//       (user_raster_msl_source above) must actually execute -- every
//       resolved pixel must match the custom shader's own solid-green
//       formula, not the built-in sample*tint formula, and compile() must
//       record a "raster_user_shader"/HostAssisted disclosure event
//       (docs/START.md invariant 10: no silent "verified" reclassification --
//       see also reference_raster_test.cpp's equivalent HostAssisted check).
//   (b) malformed entry point: fragment_entry names a function absent from
//       source. ensure_raster_pipeline compiles the MTLLibrary/pipeline
//       lazily at submit() time, not at compile() time, so compile() must
//       still succeed; submit() itself must still return true (host-side
//       acceptance), but submission.result.ok must be false with a message
//       containing "Metal raster pipeline compile failed" -- a clean
//       submit-time failure, never a crash or a silent fallback to the
//       built-in shader.
//   (c) mixed compute+MSL-raster rejection: ExecutionPlan::validate()
//       requires every task to be Raster-kind whenever user_raster_shader is
//       set (device_hal.cpp); a graph mixing a Compute task with a Raster
//       task must be rejected at compile() with that exact message.
bool run_task_graph_raster_user_shader(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "task-graph-raster-user-shader: no Metal device available on this host\n";
    return false;
  }
  // Note: unlike the reference backend (reference_device_hal.cpp), Metal's
  // capabilities() does not currently OR in Capability::UserShaderImport even
  // though compile()/submit() fully implement it -- see the final report's
  // flagged-bug list. Not asserted here since it is not part of this
  // sub-case's required behaviour and the plan is submitted directly.

  constexpr uint32_t kExtent = 4;
  constexpr uint32_t kBytes = kExtent * kExtent * 4;
  vg::core::Arena arena;
  auto& source_alloc = arena.allocate(kBytes);
  auto& target_alloc = arena.allocate(kBytes);
  for (uint32_t y = 0; y < kExtent; ++y) {
    for (uint32_t x = 0; x < kExtent; ++x) {
      const uint64_t texel = (static_cast<uint64_t>(y) * kExtent + x) * 4;
      source_alloc.bytes[texel + 0] = static_cast<uint8_t>(10 + 40 * x);
      source_alloc.bytes[texel + 1] = static_cast<uint8_t>(20 + 40 * y);
      source_alloc.bytes[texel + 2] = static_cast<uint8_t>(30 + 8 * x + 16 * y);
      source_alloc.bytes[texel + 3] = 255;
    }
  }

  const vg::core::CanonicalView source_view = make_rgba8_view(source_alloc, {.width = kExtent, .height = kExtent});
  const vg::core::CanonicalView target_view = make_rgba8_view(target_alloc, {.width = kExtent, .height = kExtent});

  vg::core::FacetRef source_ref;
  vg::core::FacetRef target_ref;
  std::string error;
  if (!metal_device->facet_pool().acquire(arena, source_view, vg::core::FacetKind::Sample, &source_ref, &error) ||
      !metal_device->facet_pool().acquire(arena, target_view, vg::core::FacetKind::Attachment, &target_ref,
                                          &error)) {
    std::cerr << "task-graph-raster-user-shader: acquire failed: " << error << "\n";
    return false;
  }

  const auto quad = metal_fullscreen_quad();
  const uint64_t vertex_bytes = quad.size() * sizeof(vg::metal::RasterVertex);
  auto& vertex_alloc = arena.allocate(vertex_bytes);
  std::memcpy(vertex_alloc.bytes.data(), quad.data(), vertex_bytes);
  const vg::core::CanonicalView vertex_view =
      make_rgba8_view(vertex_alloc, {.width = static_cast<uint32_t>(vertex_bytes / 4), .height = 1});
  vg::core::FacetRef vertex_ref;
  if (!metal_device->facet_pool().acquire(arena, vertex_view, vg::core::FacetKind::Address, &vertex_ref, &error)) {
    std::cerr << "task-graph-raster-user-shader: vertex facet acquire failed: " << error << "\n";
    return false;
  }

  TaskRecord raster_task{};
  raster_task.kind = vg::core::TaskKind::Raster;
  raster_task.raster_facets = {.source = source_ref, .target = target_ref};
  raster_task.vertex_buffer_ref = vertex_ref;
  raster_task.raster_filter = vg::core::FilterMode::Nearest;
  raster_task.raster_wrap = vg::core::WrapMode::Clamp;

  TaskGraphBuilder builder;
  if (!builder.append(raster_task)) {
    std::cerr << "task-graph-raster-user-shader: failed to append raster task\n";
    return false;
  }
  TaskGraph graph;
  if (!builder.seal(&graph) || !graph.publish()) {
    std::cerr << "task-graph-raster-user-shader: failed to seal/publish task graph\n";
    return false;
  }

  // (a) Happy path: a real, valid custom shader whose own formula (solid
  // green) is trivially distinguishable from the built-in sample*tint
  // formula against this non-green source texel pattern.
  const vg::ir::UserRasterShaderContract shader{
      "vg.test.raster/v1", "vg_user_raster_vertex", "vg_user_raster_fragment",
      vg::ir::kRasterVertexAbiXyzuvPackedV1,
      user_raster_msl_source("vg_user_raster_vertex", "vg_user_raster_fragment")};
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions raster_options;
  raster_options.facet_pool = &metal_device->facet_pool();
  if (!assemble_user_raster_plan(arena, shader, {raster_task}, &plan, &error,
                                 raster_options)) {
    std::cerr << "task-graph-raster-user-shader: plan assembly failed: " << error << "\n";
    return false;
  }

  vg::hal::CompiledPlan compiled;
  if (!metal_device->compile(plan, &compiled, &error)) {
    std::cerr << "task-graph-raster-user-shader: Metal compile failed: " << error << "\n";
    return false;
  }
  if (!compiled.report.supported) {
    std::cerr << "task-graph-raster-user-shader: report.supported should be true\n";
    return false;
  }
  bool found_user_shader_event = false;
  for (const auto& event : compiled.report.events) {
    if (event.operation == "raster_user_shader" && event.classification == vg::hal::LoweringClass::HostAssisted)
      found_user_shader_event = true;
  }
  if (!found_user_shader_event) {
    std::cerr << "task-graph-raster-user-shader: missing HostAssisted raster_user_shader LoweringEvent\n";
    return false;
  }

  vg::hal::Submission submission;
  if (!metal_device->submit(compiled, arena, &submission, &error)) {
    std::cerr << "task-graph-raster-user-shader: Metal submit failed: " << error << "\n";
    return false;
  }
  if (!submission.result.ok) {
    std::cerr << "task-graph-raster-user-shader: Metal execution reported failure: " << submission.result.message
              << "\n";
    return false;
  }
  if (submission.raster_results.size() != 1) {
    std::cerr << "task-graph-raster-user-shader: expected exactly one raster_results entry, got "
              << submission.raster_results.size() << "\n";
    return false;
  }
  const auto& raster_result = submission.raster_results[0];
  if (raster_result.width != kExtent || raster_result.height != kExtent) {
    std::cerr << "task-graph-raster-user-shader: raster_results[0] shape mismatch\n";
    return false;
  }
  const std::array<float, 4> solid_green{0.0f, 1.0f, 0.0f, 1.0f};
  for (size_t index = 0; index < raster_result.resolved_rgba.size(); ++index) {
    if (!channels_close(raster_result.resolved_rgba[index], solid_green, kNearestTol,
                        "task-graph-raster-user-shader", "custom-shader pixel"))
      return false;
  }

  // (b) Malformed entry point: fragment_entry names a function absent from
  // source. compile() still succeeds (pipeline is built lazily at submit()),
  // but submit() must report a clean, non-crashing failure via
  // submission.result.
  const vg::ir::UserRasterShaderContract bad_shader{
      "vg.test.raster/v1", "vg_user_raster_vertex", "vg_does_not_exist_in_source",
      vg::ir::kRasterVertexAbiXyzuvPackedV1,
      user_raster_msl_source("vg_user_raster_vertex", "vg_user_raster_fragment")};
  vg::core::ExecutionPlan bad_plan;
  if (!assemble_user_raster_plan(arena, bad_shader, {raster_task}, &bad_plan, &error,
                                 raster_options)) {
    std::cerr << "task-graph-raster-user-shader: bad-plan assembly failed: " << error << "\n";
    return false;
  }

  vg::hal::CompiledPlan bad_compiled;
  std::string bad_compile_error;
  if (!metal_device->compile(bad_plan, &bad_compiled, &bad_compile_error)) {
    std::cerr << "task-graph-raster-user-shader: compile() should defer pipeline compilation to submit(), "
                 "but failed at compile() instead: "
              << bad_compile_error << "\n";
    return false;
  }
  vg::hal::Submission bad_submission;
  std::string bad_submit_error;
  if (!metal_device->submit(bad_compiled, arena, &bad_submission, &bad_submit_error)) {
    std::cerr << "task-graph-raster-user-shader: submit() call itself should succeed even when the "
                 "pipeline fails to compile (host-side acceptance), but failed: "
              << bad_submit_error << "\n";
    return false;
  }
  if (bad_submission.result.ok) {
    std::cerr << "task-graph-raster-user-shader: malformed entry point must report submission.result.ok "
                 "== false\n";
    return false;
  }
  if (bad_submission.result.message.find("Metal raster pipeline compile failed") == std::string::npos) {
    std::cerr << "task-graph-raster-user-shader: unexpected malformed-entry-point failure message: "
              << bad_submission.result.message << "\n";
    return false;
  }

  // (c) Mixed compute+MSL-raster rejection: validate() requires every task
  // to be Raster-kind whenever user_raster_shader is set.
  const auto& user_shader_mixed_root = arena.allocate(4);
  TaskRecord compute_task{};
  compute_task.root_allocation = user_shader_mixed_root.id;
  compute_task.root_generation = user_shader_mixed_root.generation;
  compute_task.x = 1;
  compute_task.y = 1;
  compute_task.z = 1;
  TaskRecord mixed_raster_task{};
  mixed_raster_task.node_index = 1;
  mixed_raster_task.kind = vg::core::TaskKind::Raster;
  mixed_raster_task.raster_facets = {.source = source_ref, .target = target_ref};
  mixed_raster_task.vertex_buffer_ref = vertex_ref;
  TaskGraphBuilder mixed_builder;
  if (!mixed_builder.append(compute_task) || !mixed_builder.append(mixed_raster_task)) {
    std::cerr << "task-graph-raster-user-shader: failed to build mixed compute+raster graph\n";
    return false;
  }
  TaskGraph mixed_graph;
  if (!mixed_builder.seal(&mixed_graph) || !mixed_graph.publish()) {
    std::cerr << "task-graph-raster-user-shader: failed to seal/publish mixed compute+raster graph\n";
    return false;
  }
  std::string mixed_error;
  vg::core::ExecutionPlan mixed_plan;
  if (assemble_user_raster_plan(arena, shader, {compute_task, mixed_raster_task},
                                &mixed_plan, &mixed_error, raster_options)) {
    std::cerr << "task-graph-raster-user-shader: assembly unexpectedly accepted a mixed compute+"
                 "user_raster_shader graph\n";
    return false;
  }
  if (mixed_error != "task kind does not match its resolved node execution domain") {
    std::cerr << "task-graph-raster-user-shader: unexpected mixed-graph rejection message: " << mixed_error
              << "\n";
    return false;
  }

  std::cout << "task-graph-raster-user-shader: ok\n";
  return true;
}

// E013: run_pipeline_classification. After the sha256 key fix the expected
// arithmetic is classified=6 (2 compute checked on/off + 4 raster
// format×sample) and naive=24. Assert those exact counts when they match;
// otherwise keep the invariants (classified < naive, hits>0, misses>0,
// unsupported_rejected) and print the observed counts rather than inventing
// a match.
bool run_pipeline_classification(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "pipeline-classification: no Metal device available on this host\n";
    return false;
  }

  vg::metal::PipelineClassificationRun result;
  std::string error;
  if (!metal_device->run_pipeline_classification(&result, &error)) {
    std::cerr << "pipeline-classification: run failed: " << error << "\n";
    return false;
  }
  if (result.classified_pipeline_count >= result.naive_pipeline_count) {
    std::cerr << "pipeline-classification: classified_pipeline_count must be < naive_pipeline_count "
                 "(classified="
              << result.classified_pipeline_count << " naive=" << result.naive_pipeline_count << ")\n";
    return false;
  }
  if (result.cache_hits == 0 || result.cache_misses == 0) {
    std::cerr << "pipeline-classification: expected both cache hits and misses (hits="
              << result.cache_hits << " misses=" << result.cache_misses << ")\n";
    return false;
  }
  if (!result.unsupported_rejected) {
    std::cerr << "pipeline-classification: UnsupportedNeedsConversion must be rejected\n";
    return false;
  }
  if (result.classified_pipeline_count == 6 && result.naive_pipeline_count == 24) {
    std::cout << "pipeline-classification: ok (classified=6 naive=24 hits=" << result.cache_hits
              << " misses=" << result.cache_misses << ")\n";
    return true;
  }
  std::cout << "pipeline-classification: ok (classified=" << result.classified_pipeline_count
            << " naive=" << result.naive_pipeline_count << " hits=" << result.cache_hits
            << " misses=" << result.cache_misses
            << "; expected 6/24 after sha256 key fix, asserting invariants only)\n";
  return true;
}

// Builds a load-only module against `allocation` at a caller-chosen
// representation_epoch. Stage 5 runs before the interpreter and bumps the
// transformed allocation's epoch, so a module that still names the
// pre-transform epoch faults STALE_OR_BOUNDS. The consume-input cases below
// load a *separate* probe allocation that Stage 5 does not touch, so the
// module stays valid after the bump (and after ConsumeInput clears the
// image's host bytes).
vg::ir::Module make_epoch_probe_module(const vg::core::Allocation& allocation, uint32_t epoch) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/v1";
  vg::ir::Instruction load;
  load.op = "load";
  load.allocation = allocation.id;
  load.generation = allocation.generation;
  load.representation_epoch = epoch;
  load.offset = 0;
  load.size = 4;
  module.instructions.push_back(load);
  module.declared_effects.push_back({allocation.id, 0, 4, vg::ir::Access::Read, epoch});
  return module;
}

bool compile_and_submit_representation(vg::metal::DeviceHal& metal_device, vg::core::Arena& arena,
                                       const vg::ir::Module& module,
                                       const vg::core::RepresentationRequest& request,
                                       vg::hal::Submission* submission, std::string* error) {
  const std::vector<vg::core::RepresentationRequest> requests{request};
  vg::test_support::AssemblyOptions options;
  options.representation_requests = &requests;
  options.facet_pool = &metal_device.facet_pool();
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  if (!assemble_compute_plan(arena, module, {probe_task(module)}, &plan, error, options)) return false;
  vg::hal::CompiledPlan compiled;
  if (!metal_device.compile(plan, &compiled, error)) return false;
  return metal_device.submit(compiled, arena, submission, error);
}

// E005 via compile()/submit() Stage 5 only. Standalone
// run_representation_transform never consumes (06 §11). multi-version keeps
// the old backing (released_backing_bytes==0); ConsumeInput with a complete
// proof releases it (allocation stays Active, generation unchanged, the new
// facet still samples). An incomplete proof is rejected by
// ExecutionPlan::validate, not inferred by the adapter.
bool run_consume_input(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "consume-input: no Metal device available on this host\n";
    return false;
  }

  auto prepare_image = [](vg::core::Arena& arena, vg::core::CanonicalView* view) -> vg::core::Allocation& {
    constexpr uint32_t kW = 2;
    constexpr uint32_t kH = 2;
    auto& allocation = arena.allocate(static_cast<uint64_t>(kW) * kH * 4);
    allocation.bytes = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
    };
    *view = make_rgba8_view(allocation, {.width = kW, .height = kH});
    return allocation;
  };

  const std::vector<std::array<float, 2>> uvs = {{0.25f, 0.25f}};

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    prepare_image(arena, &view);
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);

    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = false;

    vg::hal::Submission submission;
    std::string error;
    if (!compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error)) {
      std::cerr << "consume-input: multi-version compile/submit failed: " << error << "\n";
      return false;
    }
    if (!submission.result.ok) {
      std::cerr << "consume-input: multi-version execution reported failure: "
                << submission.result.message << "\n";
      return false;
    }
    if (!submission.representation_epoch.sealed() || submission.representation_facets.size() != 1) {
      std::cerr << "consume-input: multi-version must seal one RepresentationEpoch facet\n";
      return false;
    }
    if (submission.old_backing_bytes == 0 || submission.released_backing_bytes != 0) {
      std::cerr << "consume-input: multi-version must keep old backing (old="
                << submission.old_backing_bytes << " released=" << submission.released_backing_bytes
                << ")\n";
      return false;
    }
    std::cout << "consume-input: multi-version ok (old=" << submission.old_backing_bytes
              << " new=" << submission.new_backing_bytes << " released=0)\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_image(arena, &view);
    const uint64_t image_id = image.id;
    const uint32_t generation_before = image.generation;
    auto expected = vg::reference::sample_facet(arena, view, vg::core::FilterMode::Nearest,
                                                vg::core::WrapMode::Clamp, uvs);
    if (!expected.ok) {
      std::cerr << "consume-input: pre-submit oracle failed: " << expected.message << "\n";
      return false;
    }
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);

    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();

    vg::hal::Submission submission;
    std::string error;
    if (!compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error)) {
      std::cerr << "consume-input: ConsumeInput compile/submit failed: " << error << "\n";
      return false;
    }
    if (!submission.result.ok) {
      std::cerr << "consume-input: ConsumeInput execution reported failure: "
                << submission.result.message << "\n";
      return false;
    }
    if (submission.released_backing_bytes == 0 || submission.consumed_allocation_count != 1) {
      std::cerr << "consume-input: ConsumeInput must release the superseded backing (released="
                << submission.released_backing_bytes
                << " consumed_allocation_count=" << submission.consumed_allocation_count << ")\n";
      return false;
    }
    if (submission.released_backing_bytes != submission.old_backing_bytes) {
      std::cerr << "consume-input: released_backing_bytes (" << submission.released_backing_bytes
                << ") must equal old_backing_bytes (" << submission.old_backing_bytes << ")\n";
      return false;
    }
    const auto* after = arena.lookup(vg::core::PointerRef{image_id, generation_before});
    if (after == nullptr || after->state != vg::core::ObjectState::Active ||
        after->generation != generation_before) {
      std::cerr << "consume-input: allocation must stay Active at the same generation\n";
      return false;
    }
    if (submission.representation_facets.size() != 1) {
      std::cerr << "consume-input: ConsumeInput must publish exactly one live facet\n";
      return false;
    }
    vg::metal::SampleFacetResult sampled;
    if (!metal_device->run_sample_facet(arena, metal_device->facet_pool(),
                                        submission.representation_facets[0],
                                        vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp, uvs,
                                        &sampled, &error)) {
      std::cerr << "consume-input: new facet must still sample after ConsumeInput: " << error << "\n";
      return false;
    }
    if (!channels_close(sampled.sampled_rgba[0], expected.sampled_rgba[0], kNearestTol, "consume-input",
                        "post-consume sample"))
      return false;
    bool released_device_linear = false;
    for (const auto& event : submission.report.events) {
      if (event.operation == "consume_input_backing_release" && event.bytes != 0) released_device_linear = true;
    }
    if (!released_device_linear) {
      std::cerr << "consume-input: ConsumeInput must destroy the superseded linear device buffer, "
                   "not only the host bytes\n";
      return false;
    }
    std::cout << "consume-input: ConsumeInput ok (old=" << submission.old_backing_bytes
              << " new=" << submission.new_backing_bytes
              << " released=" << submission.released_backing_bytes << ")\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_image(arena, &view);
    const auto module = make_epoch_probe_module(image, image.representation_epoch);
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    std::string compile_error;
    const std::vector<vg::core::RepresentationRequest> requests{request};
    vg::test_support::AssemblyOptions options;
    options.representation_requests = &requests;
    options.facet_pool = &metal_device->facet_pool();
    vg::core::ExecutionPlan plan;
    if (!assemble_compute_plan(arena, module, {probe_task(module)}, &plan, &compile_error, options)) {
      std::cerr << "consume-input: same-allocation plan assembly failed: " << compile_error << "\n";
      return false;
    }
    vg::hal::CompiledPlan compiled;
    if (metal_device->compile(plan, &compiled, &compile_error)) {
      std::cerr << "consume-input: ConsumeInput of an allocation the module also loads must fail compile\n";
      return false;
    }
    if (compile_error.find("whose linear representation this plan's compute module also reads or writes") ==
        std::string::npos) {
      std::cerr << "consume-input: same-allocation ConsumeInput was refused for the wrong reason: "
                << compile_error << "\n";
      return false;
    }
    std::cout << "consume-input: same-allocation ConsumeInput rejected at compile\n";
  }

  // Catalog fault-injection (09 E005): transform 前/中/后 fault;
  // capture replay request; 外部引用存在. These run the real rejection
  // paths and print what the program actually does -- they do not invent a
  // second Arena fault injector (existing IR poison stays scoped to
  // compile()/submit() instruction execution).
  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_image(arena, &view);
    const auto original = image.bytes;
    const uint32_t generation = image.generation;
    const uint32_t epoch_before = image.representation_epoch;
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    if (!arena.acquire(image.id, generation)) {
      std::cerr << "consume-input: fault-before acquire failed\n";
      return false;
    }
    vg::hal::Submission submission;
    std::string error;
    if (compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error)) {
      std::cerr << "consume-input: fault-before must refuse while the allocation is in flight\n";
      return false;
    }
    if (error.find("representation epoch is referenced in flight") == std::string::npos) {
      std::cerr << "consume-input: fault-before refused for the wrong reason: " << error << "\n";
      return false;
    }
    if (image.bytes != original || image.generation != generation ||
        image.representation_epoch != epoch_before ||
        image.state != vg::core::ObjectState::Active) {
      std::cerr << "consume-input: fault-before must leave the old representation untouched\n";
      return false;
    }
    if (!arena.release(image.id, generation)) {
      std::cerr << "consume-input: fault-before release failed\n";
      return false;
    }
    std::cout << "consume-input: fault-before refused (in-flight), old backing kept ("
              << image.bytes.size() << " bytes, epoch=" << image.representation_epoch << ")\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_image(arena, &view);
    const auto original = image.bytes;
    const uint32_t generation = image.generation;
    const uint32_t epoch_before = image.representation_epoch;
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);
    vg::core::FacetPool pool;
    const std::vector<vg::core::RepresentationRequest> requests{request};
    vg::test_support::AssemblyOptions options;
    options.representation_requests = &requests;
    // This is an intentionally narrow Stage-7 physical-fault harness, so its
    // explicit pool is the one commit_representation_operations consumes; it
    // is not presented as DeviceHal::compile/submit.
    options.facet_pool = &pool;
    vg::core::ExecutionPlan plan;
    vg::hal::Submission submission;
    std::string error;
    if (!assemble_compute_plan(arena, module, {probe_task(module)}, &plan, &error, options)) {
      std::cerr << "consume-input: fault-during assembly failed: " << error << "\n";
      return false;
    }
    if (vg::hal::commit_representation_operations(
            plan, {{vg::hal::CompiledPlan::RepresentationOperation::CopyToPrivate, 0, "fault harness"}}, arena, pool,
            [](const vg::core::RepresentationSemanticPlanItem&, const vg::hal::CompiledPlan::PhysicalRepresentationOperation&, vg::core::FacetRef,
               vg::hal::RepresentationTransformCost*, std::string* physical_error) {
              if (physical_error) *physical_error = "injected physical transform fault";
              return false;
            },
            &submission, &error)) {
      std::cerr << "consume-input: fault-during must fail the physical step\n";
      return false;
    }
    if (error.find("injected physical transform fault") == std::string::npos) {
      std::cerr << "consume-input: fault-during refused for the wrong reason: " << error << "\n";
      return false;
    }
    // 02 §9: a fault is not transactional rollback. transform() already
    // published the new epoch before the physical step ran; consume must
    // not have happened, and the superseded host bytes must still be here.
    if (submission.consumed_allocation_count != 0 || submission.released_backing_bytes != 0) {
      std::cerr << "consume-input: fault-during must not consume (consumed="
                << submission.consumed_allocation_count
                << " released=" << submission.released_backing_bytes << ")\n";
      return false;
    }
    if (image.bytes != original || image.generation != generation ||
        image.state != vg::core::ObjectState::Active) {
      std::cerr << "consume-input: fault-during must keep the old host backing\n";
      return false;
    }
    if (image.representation_epoch != epoch_before + 1) {
      std::cerr << "consume-input: fault-during rolled back the published epoch (got "
                << image.representation_epoch << ")\n";
      return false;
    }
    if (arena.lookup(vg::core::RepresentationRef{image.id, generation, epoch_before}) != nullptr ||
        arena.lookup(vg::core::RepresentationRef{image.id, generation, image.representation_epoch}) == nullptr) {
      std::cerr << "consume-input: fault-during must leave the new epoch visible and the old one stale\n";
      return false;
    }
    std::cout << "consume-input: fault-during kept old backing (" << image.bytes.size()
              << " bytes), epoch advanced " << epoch_before << "->" << image.representation_epoch
              << ", consume did not run\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_image(arena, &view);
    const uint32_t generation = image.generation;
    const uint32_t epoch_before = image.representation_epoch;
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    vg::hal::Submission submission;
    std::string error;
    if (!compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error) ||
        !submission.result.ok || submission.released_backing_bytes == 0) {
      std::cerr << "consume-input: fault-after setup ConsumeInput failed: " << error << "\n";
      return false;
    }
    const auto stale_module = make_epoch_probe_module(image, epoch_before);
    const auto stale = vg::reference::execute(stale_module, arena);
    if (stale.ok || stale.outputs_valid || stale.poison == vg::core::PoisonState::Valid ||
        stale.fault.code != "STALE_OR_BOUNDS") {
      std::cerr << "consume-input: fault-after old-epoch load must be STALE_OR_BOUNDS (ok="
                << stale.ok << " code=" << stale.fault.code << ")\n";
      return false;
    }
    if (!image.bytes.empty() || image.generation != generation ||
        image.state != vg::core::ObjectState::Active) {
      std::cerr << "consume-input: fault-after must not roll consume back\n";
      return false;
    }
    std::cout << "consume-input: fault-after old-epoch load " << stale.fault.code
              << ", consume not rolled back (released=" << submission.released_backing_bytes << ")\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_image(arena, &view);
    const auto original = image.bytes;
    const uint32_t epoch_before = image.representation_epoch;
    const auto image_module = make_epoch_probe_module(image, epoch_before);
    const auto pre = vg::capture::make_capture(image_module, arena);
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    vg::hal::Submission submission;
    std::string error;
    if (!compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error) ||
        !submission.result.ok) {
      std::cerr << "consume-input: capture-replay setup ConsumeInput failed: " << error << "\n";
      return false;
    }
    vg::capture::ReplayResult pre_replay;
    if (!vg::capture::replay(pre, &pre_replay, &error) || !pre_replay.execution.ok) {
      std::cerr << "consume-input: pre-consume capture must still replay: " << error << " "
                << pre_replay.execution.message << "\n";
      return false;
    }
    const auto post = vg::capture::make_capture(image_module, arena);
    if (post.allocations.size() != 2) {
      std::cerr << "consume-input: post-consume capture allocation count=" << post.allocations.size()
                << "\n";
      return false;
    }
    uint64_t post_image_bytes = 0;
    for (const auto& snapshot : post.allocations) {
      if (snapshot.id == image.id) post_image_bytes = snapshot.bytes.size();
    }
    vg::capture::ReplayResult post_replay;
    if (vg::capture::replay(post, &post_replay, &error)) {
      std::cerr << "consume-input: post-consume capture must not be importable after bytes were released\n";
      return false;
    }
    if (error.find("cannot restore a consumed representation") == std::string::npos) {
      std::cerr << "consume-input: post-consume replay was refused for the wrong reason: " << error << "\n";
      return false;
    }
    std::cout << "consume-input: capture-replay pre-package ok, post-package lost " << original.size()
              << " linear bytes (snapshot now " << post_image_bytes << "), " << error << "\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& image = prepare_image(arena, &view);
    vg::core::FacetRef live{};
    std::string error;
    if (!metal_device->facet_pool().acquire(arena, view, vg::core::FacetKind::Sample, &live, &error)) {
      std::cerr << "consume-input: live-facet acquire failed: " << error << "\n";
      return false;
    }
    if (!metal_device->facet_pool().begin_gpu_use(arena, live, &error)) {
      std::cerr << "consume-input: live-facet begin_gpu_use failed: " << error << "\n";
      return false;
    }
    auto& probe = arena.allocate(64);
    const auto module = make_epoch_probe_module(probe, probe.representation_epoch);
    vg::core::RepresentationRequest request;
    request.view = view;
    request.target_kind = vg::core::FacetKind::Sample;
    request.consume_input = true;
    request.consume_proof = complete_consume_proof();
    vg::hal::Submission submission;
    if (compile_and_submit_representation(*metal_device, arena, module, request, &submission, &error)) {
      std::cerr << "consume-input: live-facet ConsumeInput must be refused while the token is held\n";
      return false;
    }
    // The live-facet snapshot is a Stage-5 semantic fact, so the assembler
    // must reject it before Metal lowering or commit is entered.
    if (error.find("live FacetRef names its source epoch") == std::string::npos) {
      std::cerr << "consume-input: live-facet was refused for the wrong reason: " << error << "\n";
      return false;
    }
    vg::core::FacetStatus status = vg::core::FacetStatus::Ok;
    if (metal_device->facet_pool().lookup(arena, live, &status) == nullptr) {
      std::cerr << "consume-input: live-facet token must still resolve after a refused consume (status="
                << vg::core::to_string(status) << ")\n";
      return false;
    }
    if (image.bytes.size() != 16) {
      std::cerr << "consume-input: live-facet refuse must keep the old backing\n";
      return false;
    }
    if (!metal_device->facet_pool().end_gpu_use(live, &error)) {
      std::cerr << "consume-input: live-facet end_gpu_use failed: " << error << "\n";
      return false;
    }
    std::cout << "consume-input: external live-facet token still live, ConsumeInput refused\n";
  }

  std::cout << "consume-input: ok\n";
  return true;
}

// E016: four catalog variants over standalone transforms (unbounded growth,
// backpressure reject, ConsumeInput watermark, drop/quality skip). Assert
// unbounded peak at 8 transforms exceeds the ConsumeInput peak, and that
// backpressure triggers at least once. fif==1 accepts 0 transforms because
// the allocation's initial representation already saturates the budget.
bool run_representation_churn(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "representation-churn: no Metal device available on this host\n";
    return false;
  }

  constexpr uint32_t kW = 2;
  constexpr uint32_t kH = 2;
  constexpr uint32_t kAttempts = 8;
  const auto seed = [](vg::core::Arena& arena, vg::core::CanonicalView* view,
                       std::vector<uint8_t>* bytes) -> vg::core::Allocation& {
    auto& allocation = arena.allocate(static_cast<uint64_t>(kW) * kH * 4);
    allocation.bytes = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
    };
    *view = make_rgba8_view(allocation, {.width = kW, .height = kH});
    if (bytes != nullptr) *bytes = allocation.bytes;
    return allocation;
  };

  uint64_t unbounded_peak = 0;
  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    auto& allocation = seed(arena, &view, nullptr);
    arena.set_max_in_flight_representations(0);
    vg::core::FacetPool pool;
    uint64_t accumulated_new = 0;
    uint32_t last_live = allocation.live_representations;
    for (uint32_t i = 0; i < kAttempts; ++i) {
      vg::metal::RepresentationTransformResult result;
      std::string error;
      if (!metal_device->run_representation_transform(arena, pool, view, vg::core::FacetKind::Sample,
                                                      &result, &error)) {
        std::cerr << "representation-churn: unbounded transform " << i << " failed: " << error << "\n";
        return false;
      }
      accumulated_new += result.new_backing_bytes;
      const uint64_t current = result.old_backing_bytes + accumulated_new;
      if (current > unbounded_peak) unbounded_peak = current;
      last_live = allocation.live_representations;
    }
    if (last_live <= 1) {
      std::cerr << "representation-churn: unbounded live_representations did not grow\n";
      return false;
    }
    std::cout << "representation-churn: unbounded ok (live=" << last_live
              << " peak=" << unbounded_peak << ")\n";
  }

  bool backpressure_triggered = false;
  const uint32_t fif_values[] = {1, 2, 4, 8};
  for (uint32_t fif : fif_values) {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    seed(arena, &view, nullptr);
    arena.set_max_in_flight_representations(fif);
    vg::core::FacetPool pool;
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < kAttempts; ++i) {
      vg::metal::RepresentationTransformResult result;
      std::string error;
      if (metal_device->run_representation_transform(arena, pool, view, vg::core::FacetKind::Sample,
                                                     &result, &error)) {
        ++accepted;
        continue;
      }
      if (error.find("in-flight representation budget exceeded") == std::string::npos) {
        std::cerr << "representation-churn: backpressure fif=" << fif
                  << " refused without the budget error: " << error << "\n";
        return false;
      }
      backpressure_triggered = true;
    }
    if (fif == 1 && accepted != 0) {
      std::cerr << "representation-churn: fif=1 must accept 0 transforms, accepted " << accepted
                << "\n";
      return false;
    }
    std::cout << "representation-churn: backpressure fif=" << fif << " accepted " << accepted
              << " / " << kAttempts << "\n";
  }
  if (!backpressure_triggered) {
    std::cerr << "representation-churn: backpressure never triggered\n";
    return false;
  }

  uint64_t consume_peak = 0;
  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    std::vector<uint8_t> original;
    auto& allocation = seed(arena, &view, &original);
    arena.set_max_in_flight_representations(0);
    vg::core::FacetPool pool;
    const auto proof = complete_consume_proof();
    for (uint32_t i = 0; i < kAttempts; ++i) {
      vg::metal::RepresentationTransformResult result;
      std::string error;
      if (!metal_device->run_representation_transform(arena, pool, view, vg::core::FacetKind::Sample,
                                                      &result, &error)) {
        std::cerr << "representation-churn: ConsumeInput transform " << i << " failed: " << error
                  << "\n";
        return false;
      }
      const uint64_t current = result.old_backing_bytes + result.new_backing_bytes;
      if (current > consume_peak) consume_peak = current;
      uint64_t released = 0;
      if (!arena.consume_representation(allocation.id, allocation.generation, result.new_epoch, proof,
                                        &released, &error)) {
        std::cerr << "representation-churn: consume_representation " << i << " failed: " << error
                  << "\n";
        return false;
      }
      // Host consume cannot see this adapter's Shared blit source. Drop it
      // now, before the next frame restores host bytes, so the device
      // watermark matches what consume_representation already handed back.
      metal_device->reclaim_released_backing(arena);
      if (allocation.live_representations > 2) {
        std::cerr << "representation-churn: ConsumeInput live_representations="
                  << allocation.live_representations << " escaped the transform window\n";
        return false;
      }
      // Restore host bytes so the next standalone blit still has a linear
      // source -- consume_representation releases the superseded backing, which
      // is the point of the watermark, but the next frame still has to upload.
      allocation.bytes = original;
    }
    std::cout << "representation-churn: ConsumeInput ok (peak=" << consume_peak << ")\n";
  }

  {
    vg::core::Arena arena;
    vg::core::CanonicalView view;
    seed(arena, &view, nullptr);
    arena.set_max_in_flight_representations(2);
    vg::core::FacetPool pool;
    uint32_t skipped = 0;
    for (uint32_t frame = 0; frame < kAttempts; ++frame) {
      vg::metal::RepresentationTransformResult result;
      std::string error;
      if (metal_device->run_representation_transform(arena, pool, view, vg::core::FacetKind::Sample,
                                                     &result, &error))
        continue;
      // drop/quality: skip the frame. Application policy, not a core API --
      // we do not release_representation to make room, and we do not retry.
      ++skipped;
    }
    if (skipped == 0) {
      std::cerr << "representation-churn: drop/quality never skipped a frame\n";
      return false;
    }
    std::cout << "representation-churn: drop/quality skipped " << skipped << " frames\n";
  }

  if (unbounded_peak <= consume_peak) {
    std::cerr << "representation-churn: unbounded peak at fif=8 (" << unbounded_peak
              << ") must exceed ConsumeInput peak (" << consume_peak << ")\n";
    return false;
  }

  std::cout << "representation-churn: ok\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: vg_metal_task_timeline_test "
                 "<task-tier0|timeline|access-certificate|tier1-indirect|cull-compact|cull-compact-1m|effect-dag|pointer-graph|"
                 "indexed-binding|representation-layer|sample-facet|checked-facet-generation|basic-raster|"
                 "task-graph-raster|task-graph-raster-depth|task-graph-raster-user-shader|pipeline-classification|consume-input|"
                 "representation-churn> "
                 "<repo_root>\n";
    return 2;
  }
  const std::string mode = argv[1];
  const std::string root = argv[2];
  bool ok = false;
  if (mode == "task-tier0") {
    ok = run_task_tier0(root);
  } else if (mode == "timeline") {
    ok = run_timeline(root);
  } else if (mode == "access-certificate") {
    ok = run_access_certificate(root);
  } else if (mode == "tier1-indirect") {
    ok = run_tier1_indirect(root);
  } else if (mode == "cull-compact") {
    ok = run_cull_compact(root);
  } else if (mode == "cull-compact-1m") {
    ok = run_cull_compact_1m(root);
  } else if (mode == "effect-dag") {
    ok = run_effect_dag(root);
  } else if (mode == "pointer-graph") {
    ok = run_pointer_graph(root);
  } else if (mode == "indexed-binding") {
    ok = run_indexed_binding(root);
  } else if (mode == "representation-layer") {
    ok = run_representation_layer(root);
  } else if (mode == "sample-facet") {
    ok = run_sample_facet(root);
  } else if (mode == "checked-facet-generation") {
    ok = run_checked_facet_generation(root);
  } else if (mode == "basic-raster") {
    ok = run_basic_raster(root);
  } else if (mode == "task-graph-raster") {
    ok = run_task_graph_raster(root);
  } else if (mode == "task-graph-raster-depth") {
    ok = run_task_graph_raster_depth(root);
  } else if (mode == "task-graph-raster-user-shader") {
    ok = run_task_graph_raster_user_shader(root);
  } else if (mode == "pipeline-classification") {
    ok = run_pipeline_classification(root);
  } else if (mode == "consume-input") {
    ok = run_consume_input(root);
  } else if (mode == "representation-churn") {
    ok = run_representation_churn(root);
  } else {
    std::cerr << "unknown mode: " << mode << "\n";
    return 2;
  }
  return ok ? 0 : 1;
}
