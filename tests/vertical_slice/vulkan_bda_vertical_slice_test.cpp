#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/vulkan/vulkan_device_hal.h"
#include "golden_format.h"
#include "ir/ir.h"
#include "../support/assembled_plan_fixture.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

namespace {

// Parses the fixture and rebinds its fixture-local allocation ids onto real
// arena.allocate() calls, mirroring tests/vertical_slice/metal_vertical_slice_test.cpp
// and tests/conformance/device_hal_conformance.cpp's pattern. Two
// independently-constructed Arenas fed the same fixture text produce
// identical remapped ids (Arena::allocate() assigns ids from a counter
// starting at 1, in the order allocations are first requested), so calling
// this once per arena is sufficient to keep both sides addressing the "same"
// allocation without any shared state between them.
vg::ir::Module load_and_bind(const std::string& path, vg::core::Arena& arena) {
  std::ifstream input(path);
  std::stringstream buffer;
  buffer << input.rdbuf();
  auto module = vg::ir::parse_module(buffer.str());

  std::vector<uint64_t> original_ids;
  for (const auto& instruction : module.instructions)
    if (std::find(original_ids.begin(), original_ids.end(), instruction.allocation) == original_ids.end())
      original_ids.push_back(instruction.allocation);

  std::map<uint64_t, uint64_t> remap;
  for (uint64_t original : original_ids) remap[original] = arena.allocate(64).id;

  for (auto& instruction : module.instructions) instruction.allocation = remap.at(instruction.allocation);
  for (auto& effect : module.declared_effects) effect.allocation = remap.at(effect.allocation);
  return module;
}

bool run_fixture(const std::string& name, const std::string& root) {
  vg::core::Arena vulkan_arena;
  vg::core::Arena reference_arena;
  const std::string path = root + "/tests/fixtures/ir/" + name + ".vgir.json";
  const auto vulkan_module = load_and_bind(path, vulkan_arena);
  const auto reference_module = load_and_bind(path, reference_arena);

  std::string device_error;
  auto vulkan_device = vg::vulkan::make_device_hal(&device_error);
  if (vulkan_device == nullptr) {
    std::cerr << name << ": no Vulkan device available on this host: " << device_error << "\n";
    return false;
  }
  auto reference_device = vg::reference::make_device_hal();

  vg::test_support::AssembledPlanFixture vulkan_fixture;
  vg::test_support::AssembledPlanFixture reference_fixture;
  vg::core::ExecutionPlan vulkan_plan;
  vg::core::ExecutionPlan reference_plan;
  std::string assembly_error;
  const auto vulkan_root = vulkan_module.instructions.front();
  const auto reference_root = reference_module.instructions.front();
  if (!vg::test_support::assemble_single_node_plan(
          vulkan_arena, vulkan_module,
          {vg::test_support::compute_task(vulkan_root.allocation, vulkan_root.generation)},
          &vulkan_fixture, &vulkan_plan, &assembly_error)) {
    std::cerr << name << ": Vulkan plan assembly failed: " << assembly_error << "\n";
    return false;
  }
  if (!vg::test_support::assemble_single_node_plan(
          reference_arena, reference_module,
          {vg::test_support::compute_task(reference_root.allocation, reference_root.generation)},
          &reference_fixture, &reference_plan, &assembly_error)) {
    std::cerr << name << ": Reference plan assembly failed: " << assembly_error << "\n";
    return false;
  }

  std::string error;
  vg::hal::CompiledPlan vulkan_compiled;
  if (!vulkan_device->compile(vulkan_plan, &vulkan_compiled, &error)) {
    std::cerr << name << ": Vulkan compile failed: " << error << "\n";
    return false;
  }
  if (!vulkan_compiled.report.supported) {
    std::cerr << name << ": Vulkan reported unsupported: " << vulkan_compiled.report.diagnostic << "\n";
    return false;
  }

  vg::hal::CompiledPlan reference_compiled;
  if (!reference_device->compile(reference_plan, &reference_compiled, &error)) {
    std::cerr << name << ": reference compile failed: " << error << "\n";
    return false;
  }

  vg::hal::Submission vulkan_submission;
  if (!vulkan_device->submit(vulkan_compiled, vulkan_arena, &vulkan_submission, &error)) {
    std::cerr << name << ": Vulkan submit failed: " << error << "\n";
    return false;
  }
  if (!vulkan_submission.result.ok) {
    std::cerr << name << ": Vulkan execution reported failure: " << vulkan_submission.result.message << "\n";
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
    const auto* vulkan_allocation = vulkan_arena.lookup(vg::core::RepresentationRef{id, allocation.generation, allocation.representation_epoch});
    if (vulkan_allocation == nullptr || vulkan_allocation->bytes != allocation.bytes) {
      std::cerr << name << ": byte mismatch for allocation " << id << "\n";
      bytes_match = false;
    }
  }
  if (!bytes_match) return false;

  std::cout << name << ": ok (direct)\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: vg_vulkan_bda_vertical_slice_test <repo_root>\n";
    return 2;
  }
  const std::string root = argv[1];
  bool all_ok = true;
  for (const char* name : vg::golden::kFixtureNames) all_ok = run_fixture(name, root) && all_ok;
  return all_ok ? 0 : 1;
}
