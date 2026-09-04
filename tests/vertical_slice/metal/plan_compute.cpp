#include "fixture.h"

namespace vg::tests::metal {

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

}  // namespace vg::tests::metal
