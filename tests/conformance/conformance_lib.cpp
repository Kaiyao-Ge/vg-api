#include "conformance_lib.h"

#include "backends/reference/reference_device_hal.h"
#include "compiler/compiler.h"
#include "golden_format.h"
#include "ir/ir.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

namespace vg::conformance {
namespace {

bool check(bool condition, const std::string& backend_name, const std::string& what, bool* all_ok) {
  if (!condition) {
    std::cerr << backend_name << ": FAILED " << what << "\n";
    *all_ok = false;
  }
  return condition;
}

bool has_event(const vg::hal::LoweringReport& report, const std::string& operation) {
  return std::ranges::any_of(report.events, [&](const vg::hal::LoweringEvent& event) {
    return event.operation == operation;
  });
}

// Parses the fixture and rebinds its fixture-local allocation ids onto real
// arena.allocate() calls, mirroring tests/vertical_slice/*_vertical_slice_test.cpp.
// Two independently-constructed Arenas fed the same fixture text produce
// identical remapped ids (Arena::allocate() assigns ids from a counter
// starting at 1, in the order allocations are first requested), so calling
// this once per arena keeps both sides addressing the "same" allocation
// without any shared state between them.
vg::ir::Module load_and_bind(const std::string& path, vg::core::Arena& arena) {
  std::ifstream input(path);
  std::stringstream buffer;
  buffer << input.rdbuf();
  auto module = vg::ir::parse_module(buffer.str());

  std::vector<uint64_t> original_ids;
  for (const auto& instruction : module.instructions)
    if (std::ranges::find(original_ids, instruction.allocation) == original_ids.end())
      original_ids.push_back(instruction.allocation);

  std::map<uint64_t, uint64_t> remap;
  for (uint64_t original : original_ids) remap[original] = arena.allocate(64).id;

  for (auto& instruction : module.instructions) instruction.allocation = remap.at(instruction.allocation);
  for (auto& effect : module.declared_effects) effect.allocation = remap.at(effect.allocation);
  return module;
}

bool run_contract_checks(vg::hal::DeviceHal& device, const std::string& backend_name,
                         const ConformanceExpectation& expectation) {
  bool all_ok = true;
  const auto& caps = device.capabilities();
  check(caps.abi_version == vg::hal::kDeviceHalAbiVersion, backend_name, "capability snapshot ABI version", &all_ok);
  check(caps.supports(vg::hal::Capability::LinearAddress), backend_name, "LinearAddress capability", &all_ok);
  if (expectation.expect_task_publication)
    check(caps.supports(vg::hal::Capability::TaskPublication), backend_name, "TaskPublication capability", &all_ok);
  if (expectation.expect_timeline)
    check(caps.supports(vg::hal::Capability::Timeline), backend_name, "Timeline capability", &all_ok);

  auto compiled_module = vg::compiler::compile_c_like("@node @effects store(1,0,4,7)");
  if (!check(compiled_module.ok, backend_name, "compile_c_like fixture parses", &all_ok)) return all_ok;
  vg::core::Arena arena;
  auto& allocation = arena.allocate(16);
  compiled_module.module.instructions[0].allocation = allocation.id;
  compiled_module.module.declared_effects[0].allocation = allocation.id;
  compiled_module.module.canonical_json = vg::ir::serialize_module(compiled_module.module);

  vg::hal::ExecutionPlan plan;
  plan.capabilities = caps;
  plan.module = compiled_module.module;
  plan.published = true;

  vg::hal::CompiledPlan compiled;
  std::string error;
  if (check(device.compile(plan, &compiled, &error), backend_name, "compiles a valid linear-subset plan", &all_ok)) {
    // Every backend emits this event for a linear-subset compile (the B4
    // shared contract); backends that implement more than the linear subset
    // (currently only reference) additionally instrument the underlying
    // canonical-IR interpretation step. A backend that has NOT implemented
    // more than the linear subset must not claim that extra event.
    check(has_event(compiled.report, "compute_package"), backend_name, "reports a compute_package lowering event",
         &all_ok);
    if (expectation.expect_linear_subset_only)
      check(!has_event(compiled.report, "canonical_ir"), backend_name,
           "linear-subset-only backend does not claim canonical_ir instrumentation", &all_ok);
    else
      check(has_event(compiled.report, "canonical_ir"), backend_name,
           "full backend reports canonical_ir instrumentation", &all_ok);
  }

  auto stale_plan = plan;
  stale_plan.abi_version = vg::hal::kDeviceHalAbiVersion + 1;
  check(!device.compile(stale_plan, &compiled, &error), backend_name, "rejects stale ABI version", &all_ok);
  check(error == "execution plan ABI version is unsupported", backend_name, "stale ABI version error message",
       &all_ok);

  auto bad_timeline = plan;
  bad_timeline.timeline_wait = 4;
  bad_timeline.timeline_signal = 4;
  check(!device.compile(bad_timeline, &compiled, &error), backend_name, "rejects non-advancing timeline", &all_ok);
  check(error == "timeline signal does not advance past wait", backend_name, "non-advancing timeline error message",
       &all_ok);

  return all_ok;
}

bool run_golden_fixture_invariant(vg::hal::DeviceHal& device, const std::string& backend_name,
                                  const std::filesystem::path& repo_root) {
  bool all_ok = true;
  auto reference_device = vg::reference::make_device_hal();
  for (const char* name : vg::golden::kFixtureNames) {
    vg::core::Arena device_arena;
    vg::core::Arena reference_arena;
    const std::string path = (repo_root / "tests/fixtures/ir" / (std::string(name) + ".vgir.json")).string();
    const auto device_module = load_and_bind(path, device_arena);
    const auto reference_module = load_and_bind(path, reference_arena);

    vg::hal::ExecutionPlan device_plan;
    device_plan.capabilities = device.capabilities();
    device_plan.module = device_module;
    device_plan.published = true;

    vg::hal::ExecutionPlan reference_plan;
    reference_plan.capabilities = reference_device->capabilities();
    reference_plan.module = reference_module;
    reference_plan.published = true;

    std::string error;
    vg::hal::CompiledPlan device_compiled;
    const bool device_compiled_ok = device.compile(device_plan, &device_compiled, &error);

    vg::hal::CompiledPlan reference_compiled;
    if (!check(reference_device->compile(reference_plan, &reference_compiled, &error), backend_name,
              std::string(name) + ": reference compile", &all_ok))
      continue;
    vg::hal::Submission reference_submission;
    if (!check(reference_device->submit(reference_compiled, reference_arena, &reference_submission, &error),
              backend_name, std::string(name) + ": reference submit", &all_ok))
      continue;
    if (!check(reference_submission.result.ok, backend_name, std::string(name) + ": reference execution", &all_ok))
      continue;

    // The one universal invariant: a claimed-successful lowering is bound to
    // produce byte-identical output to the reference oracle. An honest
    // "unsupported" is not a conformance failure -- silently wrong bytes are.
    if (!device_compiled_ok || !device_compiled.report.supported) {
      std::cout << backend_name << " " << name << ": skipped (unsupported)\n";
      continue;
    }

    vg::hal::Submission device_submission;
    if (!check(device.submit(device_compiled, device_arena, &device_submission, &error), backend_name,
              std::string(name) + ": device submit", &all_ok))
      continue;
    if (!check(device_submission.result.ok, backend_name, std::string(name) + ": device execution", &all_ok))
      continue;

    bool bytes_match = true;
    for (const auto& [id, allocation] : reference_arena.allocations()) {
      const auto* device_allocation = device_arena.lookup(vg::core::RepresentationRef{id, allocation.generation, allocation.representation_epoch});
      if (device_allocation == nullptr || device_allocation->bytes != allocation.bytes) bytes_match = false;
    }
    check(bytes_match, backend_name, std::string(name) + ": byte-exact match against reference oracle", &all_ok);
    if (bytes_match) std::cout << backend_name << " " << name << ": ok\n";
  }
  return all_ok;
}

}  // namespace

bool run(vg::hal::DeviceHal& device, const std::string& backend_name,
        const ConformanceExpectation& expectation, const std::string& repo_root) {
  bool all_ok = run_contract_checks(device, backend_name, expectation);
  all_ok = run_golden_fixture_invariant(device, backend_name, repo_root) && all_ok;
  return all_ok;
}

}  // namespace vg::conformance
