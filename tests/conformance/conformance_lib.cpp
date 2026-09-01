#include "conformance_lib.h"

#include "backends/reference/reference_device_hal.h"
#include "../support/assembled_plan_fixture.h"
#include "compiler/compiler.h"
#include "golden_format.h"
#include "ir/ir.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
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

uint64_t event_count(const vg::hal::LoweringReport& report, const std::string& operation) {
  uint64_t total = 0;
  for (const auto& event : report.events)
    if (event.operation == operation) total += event.count;
  return total;
}

bool has_one_package_per_resolved_node(const vg::hal::CompiledPlan& compiled) {
  if (compiled.per_node_packages.size() != compiled.plan.resolved_nodes.size()) return false;
  std::set<std::pair<uint32_t, uint32_t>> package_refs;
  for (const auto& package : compiled.per_node_packages)
    package_refs.emplace(package.ref.index, package.ref.generation);
  if (package_refs.size() != compiled.per_node_packages.size()) return false;
  for (const auto& node : compiled.plan.resolved_nodes)
    if (!package_refs.contains({node.ref.index, node.ref.generation})) return false;
  return true;
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

  std::string error;
  vg::core::ExecutionPlan plan;
  vg::test_support::AssembledPlanFixture plan_fixture;
  const auto root_ref = vg::core::PointerRef{allocation.id, allocation.generation};
  if (!check(vg::test_support::assemble_single_node_plan(
                 arena, compiled_module.module,
                 {vg::test_support::compute_task(root_ref.allocation, root_ref.generation)},
                 &plan_fixture, &plan, &error), backend_name, "assembles a valid linear-subset plan", &all_ok))
    return all_ok;

  vg::hal::CompiledPlan compiled;
  const bool compiled_ok = device.compile(plan, &compiled, &error);
  if (!caps.supports(vg::hal::Capability::TaskPublication)) {
    check(!compiled_ok && !compiled.report.supported &&
              error.find("TaskPublication") != std::string::npos,
          backend_name, "rejects a TaskPublication plan when that capability is absent", &all_ok);
  } else if (check(compiled_ok, backend_name, "compiles a valid linear-subset plan", &all_ok)) {
    // Every backend emits this event for a linear-subset compile (the B4
    // shared contract); backends that implement more than the linear subset
    // (currently only reference) additionally instrument the underlying
    // canonical-IR interpretation step. A backend that has NOT implemented
    // more than the linear subset must not claim that extra event.
    check(has_event(compiled.report, "node_compute_package"),
         backend_name, "reports a compute package lowering event",
         &all_ok);
    check(has_one_package_per_resolved_node(compiled), backend_name,
          "has exactly one package for every resolved NodeRef", &all_ok);
    check(event_count(compiled.report, "node_compute_package") == compiled.per_node_packages.size(),
          backend_name, "reports the exact count of compute NodeRef package lowerings", &all_ok);

    auto bad_abi = compiled;
    ++bad_abi.abi_version;
    vg::hal::Submission tampered_submission;
    error.clear();
    check(!device.submit(bad_abi, arena, &tampered_submission, &error) &&
              error == "compiled plan ABI version is unsupported",
          backend_name, "rejects a CompiledPlan with a wrong ABI version before Stage 7", &all_ok);

    auto wrong_backend = compiled;
    wrong_backend.report.backend = vg::hal::BackendKind::Reference;
    if (device.capabilities().backend == vg::hal::BackendKind::Reference)
      wrong_backend.report.backend = vg::hal::BackendKind::Metal;
    error.clear();
    check(!device.submit(wrong_backend, arena, &tampered_submission, &error) &&
              error == "compiled plan backend does not match adapter",
          backend_name, "rejects a CompiledPlan from another backend before Stage 7", &all_ok);

    auto tampered_order = compiled;
    tampered_order.plan.task_order[0] =
        static_cast<uint32_t>(tampered_order.plan.task_graph.tasks().size());
    error.clear();
    check(!device.submit(tampered_order, arena, &tampered_submission, &error),
          backend_name, "rejects a CompiledPlan with a tampered sealed task order", &all_ok);

    auto tampered_effects = compiled;
    ++tampered_effects.plan.task_effects[0][0].offset;
    error.clear();
    check(!device.submit(tampered_effects, arena, &tampered_submission, &error),
          backend_name, "rejects a CompiledPlan with tampered sealed effects", &all_ok);

    auto tampered_package = compiled;
    tampered_package.per_node_packages[0].kind =
        vg::hal::CompiledPlan::NodePackageKind::Raster;
    error.clear();
    check(!device.submit(tampered_package, arena, &tampered_submission, &error),
          backend_name, "rejects a CompiledPlan with a mismatched Node package kind", &all_ok);
  }

  // The shared fixture exercises the public Stage 0--6 path for two distinct
  // CodeObjects.  A backend may either prove its Node-aware lowering with
  // complete package evidence or decline it explicitly; it must never report
  // a successful compile with a partial Node/package mapping.
  auto second_module = vg::compiler::compile_c_like("@node @effects store(1,0,4,9)");
  if (!check(second_module.ok, backend_name, "second compile_c_like fixture parses", &all_ok)) return all_ok;
  auto& second_allocation = arena.allocate(16);
  second_module.module.instructions[0].allocation = second_allocation.id;
  second_module.module.declared_effects[0].allocation = second_allocation.id;
  second_module.module.canonical_json = vg::ir::serialize_module(second_module.module);
  vg::core::ExecutionPlan multi_node_plan;
  vg::test_support::MultiNodePlanFixture multi_node_fixture;
  error.clear();
  if (check(vg::test_support::assemble_multi_node_plan(
                arena, {compiled_module.module, second_module.module},
                {vg::test_support::compute_task(root_ref.allocation, root_ref.generation),
                 vg::test_support::compute_task(second_allocation.id, second_allocation.generation)},
                {{0, 1}}, &multi_node_fixture, &multi_node_plan, &error), backend_name,
            "assembles a two-CodeObject NodeRef plan", &all_ok)) {
    vg::hal::CompiledPlan multi_node_compiled;
    error.clear();
    const bool multi_node_compiled_ok = device.compile(multi_node_plan, &multi_node_compiled, &error);
    if (!caps.supports(vg::hal::Capability::EffectDag)) {
      check(!multi_node_compiled_ok && !multi_node_compiled.report.supported &&
                error.find("EffectDag") != std::string::npos,
            backend_name, "rejects an EffectDag plan when that capability is absent", &all_ok);
    } else if (multi_node_compiled_ok) {
      check(multi_node_compiled.report.supported, backend_name,
            "successful multi-Node compile reports supported", &all_ok);
      check(has_one_package_per_resolved_node(multi_node_compiled), backend_name,
            "multi-Node compile has complete unique NodeRef packages", &all_ok);
      check(event_count(multi_node_compiled.report, "node_compute_package") ==
                multi_node_compiled.per_node_packages.size(),
            backend_name, "multi-Node compile reports the exact count of compute packages", &all_ok);
    } else {
      check(!multi_node_compiled.report.supported &&
                multi_node_compiled.report.count(vg::hal::LoweringClass::Unsupported) != 0,
            backend_name, "unsupported multi-Node lowering is explicit", &all_ok);
    }
  }


  vg::core::ExecutionPlan bad_timeline;
  vg::test_support::AssembledPlanFixture bad_timeline_fixture;
  vg::test_support::AssemblyOptions bad_timeline_options;
  bad_timeline_options.timeline_wait = 4;
  bad_timeline_options.timeline_signal = 4;
  error.clear();
  check(!vg::test_support::assemble_single_node_plan(
            arena, compiled_module.module,
            {vg::test_support::compute_task(root_ref.allocation, root_ref.generation)},
            &bad_timeline_fixture, &bad_timeline, &error, bad_timeline_options),
        backend_name, "rejects non-advancing timeline", &all_ok);
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

    vg::core::ExecutionPlan device_plan;
    vg::core::ExecutionPlan reference_plan;
    vg::test_support::AssembledPlanFixture device_fixture;
    vg::test_support::AssembledPlanFixture reference_fixture;
    const auto device_root = vg::core::PointerRef{device_module.instructions.front().allocation,
                                                  device_module.instructions.front().generation};
    const auto reference_root = vg::core::PointerRef{reference_module.instructions.front().allocation,
                                                     reference_module.instructions.front().generation};

    std::string error;
    if (!vg::test_support::assemble_single_node_plan(
            device_arena, device_module,
            {vg::test_support::compute_task(device_root.allocation, device_root.generation)},
            &device_fixture, &device_plan, &error) ||
        !vg::test_support::assemble_single_node_plan(
            reference_arena, reference_module,
            {vg::test_support::compute_task(reference_root.allocation, reference_root.generation)},
            &reference_fixture, &reference_plan, &error)) {
      check(false, backend_name, std::string(name) + ": core assembly", &all_ok);
      continue;
    }
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
