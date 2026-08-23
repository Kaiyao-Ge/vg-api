#include "backends/device_hal.h"
#include "backends/metal/metal_device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "ir/ir.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using vg::core::TaskGraph;
using vg::core::TaskGraphBuilder;
using vg::core::TaskRecord;

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
         a.payload_size == b.payload_size && a.payload_or_offset == b.payload_or_offset;
}

// Two tasks with an explicit dependency (1 depends on 0), non-trivial x/y/z
// so the GPU dispatch-sizing path (TaskRecord.x/y/z, not a hardcoded
// (1,1,1)) is actually exercised. Metal's GPU task ring publication must
// report published_tasks byte-identical, in the same order, to the
// reference oracle (reference::execute_task_graph()).
bool run_task_tier0(const std::string& root) {
  (void)root;
  TaskGraphBuilder builder;
  TaskRecord task0{};
  task0.node_index = 0;
  task0.root_allocation = 42;
  task0.x = 3;
  task0.y = 2;
  task0.z = 1;
  task0.payload_size = 8;
  TaskRecord task1{};
  task1.node_index = 1;
  task1.root_allocation = 42;
  task1.x = 1;
  task1.y = 1;
  task1.z = 1;
  task1.flags = 7;
  task1.contract_index = 3;
  task1.payload_or_offset = 0x1'0000'0001ULL;
  if (!builder.append(task0) || !builder.append(task1) || !builder.add_dependency(0, 1)) {
    std::cerr << "task-tier0: failed to build task graph\n";
    return false;
  }
  TaskGraph graph;
  if (!builder.seal(&graph) || !graph.publish()) {
    std::cerr << "task-tier0: failed to seal/publish task graph\n";
    return false;
  }

  auto oracle = vg::reference::execute_task_graph(graph);
  if (!oracle.ok || oracle.published_tasks.size() != 2) {
    std::cerr << "task-tier0: reference oracle failed: " << oracle.message << "\n";
    return false;
  }

  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "task-tier0: no Metal device available on this host\n";
    return false;
  }

  vg::core::Arena arena;
  const auto module = make_probe_module(arena);

  vg::hal::ExecutionPlan plan;
  plan.capabilities = metal_device->capabilities();
  plan.module = module;
  plan.published = true;
  plan.task_graph = graph;
  plan.graph_epoch = arena.topology_epoch();

  vg::hal::CompiledPlan compiled;
  std::string error;
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

  vg::hal::ExecutionPlan signal_plan;
  signal_plan.capabilities = metal_device->capabilities();
  signal_plan.module = module;
  signal_plan.published = true;
  signal_plan.timeline_signal = 5;
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

  vg::hal::ExecutionPlan wait_plan;
  wait_plan.capabilities = metal_device->capabilities();
  wait_plan.module = module;
  wait_plan.published = true;
  wait_plan.timeline_wait = 5;
  wait_plan.timeline_signal = 10;
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

  vg::hal::ExecutionPlan stuck_plan;
  stuck_plan.capabilities = metal_device->capabilities();
  stuck_plan.module = module;
  stuck_plan.published = true;
  stuck_plan.timeline_wait = 999;
  stuck_plan.timeline_signal = 1000;
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
    arena.allocate(16);  // untouched by the module -- only Universe/DiscoverThenLease should see it

    vg::hal::ExecutionPlan plan;
    plan.capabilities = metal_device->capabilities();
    plan.module = module;
    plan.published = true;
    plan.requested_certificate_mode = mode;

    vg::hal::CompiledPlan compiled;
    std::string error;
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

    vg::hal::ExecutionPlan plan;
    plan.capabilities = metal_device->capabilities();
    plan.module = module;
    plan.published = true;
    plan.requested_certificate_mode = mode;

    vg::hal::CompiledPlan compiled;
    std::string error;
    if (metal_device->compile(plan, &compiled, &error)) {
      std::cerr << "access-certificate: " << label << " unexpectedly compiled successfully\n";
      return false;
    }
    if (compiled.report.supported) {
      std::cerr << "access-certificate: " << label << " report claims supported\n";
      return false;
    }
    bool found_unsupported = false;
    for (const auto& event : compiled.report.events) {
      if (event.operation == "access_certificate" && event.classification == vg::hal::LoweringClass::Unsupported)
        found_unsupported = true;
    }
    if (!found_unsupported) {
      std::cerr << "access-certificate: " << label << " missing honest Unsupported report event\n";
      return false;
    }
    std::cout << "access-certificate: " << label << " honestly unsupported\n";
    return true;
  };

  if (!check_unsupported_mode(vg::core::AccessCertificateMode::SoftwarePaged, "software-paged")) return false;
  if (!check_unsupported_mode(vg::core::AccessCertificateMode::FaultManaged, "fault-managed")) return false;

  std::cout << "access-certificate: ok\n";
  return true;
}

// TASK-B13 (E009): ExecutionPlan::request_tier1_indirect drives a second,
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

  TaskGraphBuilder builder;
  TaskRecord task0{};
  task0.node_index = 0;
  task0.root_allocation = 42;
  task0.x = 4;
  task0.y = 1;
  task0.z = 1;
  TaskRecord task1{};
  task1.node_index = 1;
  task1.root_allocation = 42;
  task1.x = 2;
  task1.y = 3;
  task1.z = 1;
  if (!builder.append(task0) || !builder.append(task1) || !builder.add_dependency(0, 1)) {
    std::cerr << "tier1-indirect: failed to build task graph\n";
    return false;
  }
  TaskGraph graph;
  if (!builder.seal(&graph) || !graph.publish()) {
    std::cerr << "tier1-indirect: failed to seal/publish task graph\n";
    return false;
  }

  vg::core::Arena arena;
  const auto module = make_probe_module(arena);

  vg::hal::ExecutionPlan plan;
  plan.capabilities = metal_device->capabilities();
  plan.module = module;
  plan.published = true;
  plan.task_graph = graph;
  plan.graph_epoch = arena.topology_epoch();
  plan.request_tier1_indirect = true;

  vg::hal::CompiledPlan compiled;
  std::string error;
  if (!metal_device->compile(plan, &compiled, &error)) {
    std::cerr << "tier1-indirect: Metal compile failed: " << error << "\n";
    return false;
  }

  vg::hal::Submission submission;
  if (!metal_device->submit(compiled, arena, &submission, &error)) {
    std::cerr << "tier1-indirect: Metal submit failed: " << error << "\n";
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
  std::sort(got.begin(), got.end());
  std::sort(want.begin(), want.end());
  if (got != want) {
    std::cerr << "cull-compact: compacted id set mismatches reference oracle\n";
    return false;
  }

  std::cout << "cull-compact: ok\n";
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

vg::ir::Instruction make_store_instruction(const vg::core::Allocation& allocation, uint64_t offset, int64_t value) {
  vg::ir::Instruction instruction;
  instruction.op = "store";
  instruction.allocation = allocation.id;
  instruction.generation = allocation.generation;
  instruction.representation_epoch = allocation.representation_epoch;
  instruction.offset = offset;
  instruction.size = 4;
  instruction.value = value;
  return instruction;
}

vg::ir::Module make_store_pass(const vg::core::Allocation& allocation, uint64_t offset, int64_t value) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/v1";
  module.instructions.push_back(make_store_instruction(allocation, offset, value));
  module.declared_effects.push_back(
      {allocation.id, offset, 4, vg::ir::Access::Write, allocation.representation_epoch});
  return module;
}

// reference_executor.cpp/compute_package.cpp's store fills every byte of
// [offset, offset+size) with the low byte of `value`, broadcast -- not a
// little-endian encoding of `value`. Mirrors compute_package.cpp's private
// store_word_pattern() so this test can check GPU-written bytes directly.
uint32_t store_word_pattern(int64_t value) {
  const uint32_t low_byte = static_cast<uint32_t>(static_cast<uint8_t>(value));
  return low_byte * 0x01010101u;
}

bool bytes_match_pattern(const std::vector<uint8_t>& bytes, uint64_t offset, uint32_t pattern) {
  if (offset + 4 > bytes.size()) return false;
  uint32_t got = 0;
  std::memcpy(&got, bytes.data() + offset, 4);
  return got == pattern;
}

// TASK-B14 (E012): exercises all 3 in-scope Effect DAG shapes
// (classify_effect_graph_shape, ADR-027) end to end through a real Metal
// submit(), plus one construction confirmed to fall outside those 3 shapes
// so compile() must honestly report it Unsupported rather than guess a
// fence placement. The ForkJoin construction is not a "textbook diamond":
// classify_effect_graph_shape's edge-count invariant (structural_edges ==
// 2*(node_count-1)) is only satisfiable for node_count==4 when every node
// pair conflicts, so all 4 passes deliberately write the *same* allocation
// with mutually-conflicting effects and zero explicit dependencies, letting
// seal() generate the full C(4,2)=6-edge transitive closure automatically
// (see dispatch_effect_dag's ForkJoin branch doc comment in
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
                         vg::core::EffectGraphShape expected_shape, uint64_t expected_encoder_count,
                         uint64_t expected_barrier_count, vg::core::Arena& arena,
                         const std::vector<std::pair<uint64_t, uint32_t>>& expect_final_bytes) {
    vg::hal::ExecutionPlan plan;
    plan.capabilities = metal_device->capabilities();
    plan.module = passes[0];
    plan.published = true;
    plan.effect_dag_passes = passes;
    plan.effect_dag_dependencies = dependencies;

    vg::hal::CompiledPlan compiled;
    std::string error;
    if (!metal_device->compile(plan, &compiled, &error)) {
      std::cerr << "effect-dag: " << label << " compile failed: " << error << "\n";
      return false;
    }
    if (compiled.effect_dag_shape != expected_shape) {
      std::cerr << "effect-dag: " << label << " classified as unexpected shape\n";
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
    for (const auto& expectation : expect_final_bytes) {
      const auto* allocation = arena.lookup(expectation.first, 1);
      if (allocation == nullptr) {
        std::cerr << "effect-dag: " << label << " missing allocation " << expectation.first << " after submit\n";
        return false;
      }
      if (!bytes_match_pattern(allocation->bytes, 0, store_word_pattern(expectation.second))) {
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
    if (!check_shape("independent-branches", passes, {}, vg::core::EffectGraphShape::IndependentBranches, 3, 0,
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
    if (!check_shape("linear-chain", passes, {{0, 1}, {1, 2}}, vg::core::EffectGraphShape::LinearChain, 1, 0, arena,
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
    if (!check_shape("fork-join", passes, {}, vg::core::EffectGraphShape::ForkJoin, 4, 3, arena, {{a.id, 33}}))
      return false;
  }

  {
    // Same source/middle/middle/join topology as a textbook diamond, but
    // with explicit dependencies AND overlapping conflicting effects between
    // adjacent nodes -- seal() adds one Explicit edge plus a duplicate
    // InferredConflict edge per such pair (core.cpp's seal() never
    // deduplicates against an existing explicit edge), which inflates
    // structural_edges past what classify_effect_graph_shape recognizes as
    // any of the 3 in-scope shapes. Confirmed empirically (scratch probing
    // this milestone) to classify Unsupported -- compile() must report that
    // honestly rather than guess a fence placement (ADR-027).
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
    middle1.instructions.push_back(make_store_instruction(b, 0, 41));
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
    middle2.instructions.push_back(make_store_instruction(c, 0, 42));
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

    vg::hal::ExecutionPlan plan;
    plan.capabilities = metal_device->capabilities();
    plan.module = source;
    plan.published = true;
    plan.effect_dag_passes = {source, middle1, middle2, join};
    plan.effect_dag_dependencies = {{0, 1}, {0, 2}, {1, 3}, {2, 3}};

    vg::hal::CompiledPlan compiled;
    std::string error;
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
      if (event.operation == "effect_dag_lowering" && event.classification == vg::hal::LoweringClass::Unsupported)
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

  vg::hal::ExecutionPlan plan;
  plan.capabilities = metal_device->capabilities();
  plan.module = module;
  plan.published = true;

  vg::hal::CompiledPlan compiled;
  std::string error;
  if (!metal_device->compile(plan, &compiled, &error)) {
    std::cerr << "pointer-graph: compile failed: " << error << "\n";
    return false;
  }

  bool found_event = false;
  for (const auto& event : compiled.report.events) {
    if (event.operation != "compute_package") continue;
    found_event = true;
    if (event.classification != vg::hal::LoweringClass::CachedObject) {
      std::cerr << "pointer-graph: expected CachedObject classification for compute_package\n";
      return false;
    }
  }
  if (!found_event) {
    std::cerr << "pointer-graph: missing compute_package report event\n";
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

  vg::hal::ExecutionPlan plan;
  plan.capabilities = metal_device->capabilities();
  plan.module = module;
  plan.published = true;
  plan.request_indexed_binding = true;

  vg::hal::CompiledPlan compiled;
  std::string error;
  if (!metal_device->compile(plan, &compiled, &error)) {
    if (error.find("gpuAddress") != std::string::npos) {
      std::cout << "indexed-binding: device does not support MTLBuffer.gpuAddress, skipping\n";
      return true;
    }
    std::cerr << "indexed-binding: compile failed: " << error << "\n";
    return false;
  }

  if (!compiled.indexed_compute_package.has_value()) {
    std::cerr << "indexed-binding: compiled plan has no indexed compute package\n";
    return false;
  }
  if (compiled.indexed_compute_package->referenced_allocations.size() != 2) {
    std::cerr << "indexed-binding: expected 2 referenced allocations\n";
    return false;
  }

  bool found_event = false;
  for (const auto& event : compiled.report.events) {
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

  vg::hal::Submission submission;
  if (!metal_device->submit(compiled, arena, &submission, &error)) {
    std::cerr << "indexed-binding: submit failed: " << error << "\n";
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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: vg_metal_task_timeline_test "
                 "<task-tier0|timeline|access-certificate|tier1-indirect|cull-compact|effect-dag|pointer-graph|"
                 "indexed-binding|representation-layer> "
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
  } else if (mode == "effect-dag") {
    ok = run_effect_dag(root);
  } else if (mode == "pointer-graph") {
    ok = run_pointer_graph(root);
  } else if (mode == "indexed-binding") {
    ok = run_indexed_binding(root);
  } else if (mode == "representation-layer") {
    ok = run_representation_layer(root);
  } else {
    std::cerr << "unknown mode: " << mode << "\n";
    return 2;
  }
  return ok ? 0 : 1;
}
