#include "backends/device_hal.h"
#include "backends/metal/metal_device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "golden_format.h"
#include "ir/ir.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

namespace {

// Parses the fixture and rebinds its fixture-local allocation ids onto real
// arena.allocate() calls, mirroring tests/conformance/device_hal_conformance.cpp's
// pattern. Two independently-constructed Arenas fed the same fixture text
// produce identical remapped ids (Arena::allocate() assigns ids from a
// counter starting at 1, in the order allocations are first requested), so
// calling this once per arena is sufficient to keep both sides addressing
// the "same" allocation without any shared state between them.
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

bool run_fixture(const std::string& name, const std::string& root) {
  vg::core::Arena metal_arena;
  vg::core::Arena reference_arena;
  const std::string path = root + "/tests/fixtures/ir/" + name + ".vgir.json";
  const auto metal_module = load_and_bind(path, metal_arena);
  const auto reference_module = load_and_bind(path, reference_arena);

  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << name << ": no Metal device available on this host\n";
    return false;
  }
  auto reference_device = vg::reference::make_device_hal();

  vg::hal::ExecutionPlan metal_plan;
  metal_plan.capabilities = metal_device->capabilities();
  metal_plan.module = metal_module;
  metal_plan.published = true;

  vg::hal::ExecutionPlan reference_plan;
  reference_plan.capabilities = reference_device->capabilities();
  reference_plan.module = reference_module;
  reference_plan.published = true;

  std::string error;
  vg::hal::CompiledPlan metal_compiled;
  if (!metal_device->compile(metal_plan, &metal_compiled, &error)) {
    std::cerr << name << ": Metal compile failed: " << error << "\n";
    return false;
  }
  if (!metal_compiled.report.supported) {
    std::cerr << name << ": Metal reported unsupported: " << metal_compiled.report.diagnostic << "\n";
    return false;
  }

  vg::hal::CompiledPlan reference_compiled;
  if (!reference_device->compile(reference_plan, &reference_compiled, &error)) {
    std::cerr << name << ": reference compile failed: " << error << "\n";
    return false;
  }

  vg::hal::Submission metal_submission;
  if (!metal_device->submit(metal_compiled, metal_arena, &metal_submission, &error)) {
    std::cerr << name << ": Metal submit failed: " << error << "\n";
    return false;
  }
  if (!metal_submission.result.ok) {
    std::cerr << name << ": Metal execution reported failure: " << metal_submission.result.message << "\n";
    return false;
  }

  vg::hal::Submission reference_submission;
  if (!reference_device->submit(reference_compiled, reference_arena, &reference_submission, &error)) {
    std::cerr << name << ": reference submit failed: " << error << "\n";
    return false;
  }
  if (!reference_submission.result.ok) {
    std::cerr << name << ": reference execution reported failure: " << reference_submission.result.message << "\n";
    return false;
  }

  bool bytes_match = true;
  for (const auto& [id, allocation] : reference_arena.allocations()) {
    const auto* metal_allocation = metal_arena.lookup(vg::core::RepresentationRef{id, allocation.generation, allocation.representation_epoch});
    if (metal_allocation == nullptr || metal_allocation->bytes != allocation.bytes) {
      std::cerr << name << ": byte mismatch for allocation " << id << "\n";
      bytes_match = false;
    }
  }
  if (!bytes_match) return false;

  const bool host_assisted =
      std::ranges::any_of(metal_submission.report.events,
                 [](const vg::hal::LoweringEvent& event) {
                   return event.operation == "metal_pipeline" &&
                          event.classification == vg::hal::LoweringClass::HostAssisted;
                 });
  std::cout << name << ": ok (" << (host_assisted ? "host-assisted" : "direct") << ")\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: vg_metal_vertical_slice_test <repo_root>\n";
    return 2;
  }
  const std::string root = argv[1];
  bool all_ok = true;
  for (const char* name : vg::golden::kFixtureNames) all_ok = run_fixture(name, root) && all_ok;
  return all_ok ? 0 : 1;
}
