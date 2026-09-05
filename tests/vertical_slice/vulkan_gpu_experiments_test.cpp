// Vulkan F's compute-only physical experiments.  GPU modes fail when no real
// Vulkan device is available; `cpu-oracle` is intentionally a separately
// named, unregistered fixture mode rather than a substitute success path.
#include "backends/reference/reference_executor.h"
#include "backends/reference/tier2_oracle.h"
#include "compiler/compute_package.h"
#include "vulkan_adapter_harness.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <vector>

namespace {

vg::core::TaskGraph graph_for(const std::vector<uint32_t> &classes) {
  vg::core::TaskGraphBuilder builder;
  for (uint32_t node : classes) {
    vg::core::TaskRecord task{};
    task.node_index = node;
    task.root_allocation = 1;
    if (!builder.append(task))
      return {};
  }
  vg::core::TaskGraph graph;
  if (!builder.seal(&graph) || !graph.publish())
    return {};
  return graph;
}

std::vector<uint32_t> sorted(std::vector<uint32_t> values) {
  std::ranges::sort(values);
  return values;
}

bool cpu_oracle() {
  const std::vector<uint32_t> visible{1, 0, 1, 1, 0, 1, 0, 1};
  const std::vector<uint32_t> ids{101, 102, 103, 104, 105, 106, 107, 108};
  const auto compact = vg::reference::cull_compact(visible, ids);
  if (!compact.ok ||
      compact.compact_ids != std::vector<uint32_t>({101, 103, 104, 106, 108})) {
    std::cerr << "cpu-oracle: cull/compact fixture failed\n";
    return false;
  }
  const auto graph = graph_for({0, 1, 0, 1, 1, 0});
  const auto tier2 = vg::reference::select_tier2_nodes(graph, {0, 1});
  if (!tier2.ok || tier2.bucket_count != 2 || tier2.command_count != 2 ||
      tier2.selected_classes != std::vector<uint32_t>({0, 1, 0, 1, 1, 0})) {
    std::cerr << "cpu-oracle: tier2 fixture failed\n";
    return false;
  }
  const auto refused =
      vg::reference::select_tier2_nodes(graph_for({0, 2}), {0, 1});
  if (refused.ok || !refused.unauthorized) {
    std::cerr << "cpu-oracle: unauthorized tier2 fixture was accepted\n";
    return false;
  }
  // This executes the restricted load/store shape used by indexed-address,
  // then builds the compiler-owned Vulkan GLSL package from that same module.
  vg::core::Arena indexed_arena;
  auto &input_allocation = indexed_arena.allocate(4);
  auto &output_allocation = indexed_arena.allocate(4);
  input_allocation.bytes[0] = 7;
  vg::ir::Module indexed;
  indexed.version = 1;
  indexed.root_schema = "vg.test/vulkan-indexed-address";
  indexed.instructions.push_back(
      {"load", input_allocation.id, 0, 4, 0, input_allocation.generation,
       input_allocation.representation_epoch, 0, "input"});
  indexed.instructions.push_back(
      {"store", output_allocation.id, 0, 4, 0x5a, output_allocation.generation,
       output_allocation.representation_epoch, 0, "output"});
  indexed.declared_effects = {{input_allocation.id, 0, 4, vg::ir::Access::Read,
                               input_allocation.representation_epoch},
                              {output_allocation.id, 0, 4,
                               vg::ir::Access::Write,
                               output_allocation.representation_epoch}};
  const auto package = vg::compiler::build_indexed_compute_package(indexed);
  if (!package.ok || package.package.referenced_allocations.size() != 2 ||
      package.package.vulkan_glsl_source.empty()) {
    std::cerr << "cpu-oracle: indexed compiler package fixture failed\n";
    return false;
  }
  const auto executed = vg::reference::execute(indexed, indexed_arena);
  if (!executed.ok || output_allocation.bytes[0] != 0x5a ||
      output_allocation.bytes[3] != 0x5a) {
    std::cerr << "cpu-oracle: indexed Reference execution fixture failed\n";
    return false;
  }
  std::cout << "cpu-oracle: ok\n";
  return true;
}

bool report_ok(const vg::hal::LoweringReport &report, uint64_t barriers) {
  return report.supported && report.backend == vg::hal::BackendKind::Vulkan &&
         report.command_buffer_count == 1 && report.encoder_count == 1 &&
         report.queue_wait_count == 1 && report.barrier_count == barriers;
}

bool indirect() {
  auto device = vg::vulkan::make_device_hal();
  if (!device) {
    std::cerr << "indirect: no real Vulkan device\n";
    return false;
  }
  vg::vulkan::GpuIndirectExperimentResult result;
  std::string error;
  const std::vector<std::array<uint32_t, 3>> dims{
      {3, 1, 1}, {5, 1, 1}, {2, 1, 1}};
  if (!vg::vulkan::AdapterHarness(*device).run_gpu_indirect_experiment(
          dims, &result, &error)) {
    std::cerr << "indirect: " << error << "\n";
    return false;
  }
  if (result.gpu_written_dims != dims || result.gpu_invocation_count != 10 ||
      result.indirect_dispatch_count != 3 || !report_ok(result.report, 2)) {
    std::cerr << "indirect: GPU-written commands or dispatch output mismatch\n";
    return false;
  }
  if (vg::vulkan::AdapterHarness(*device).run_gpu_indirect_experiment(
          {{0, 1, 1}}, &result, &error)) {
    std::cerr << "indirect: zero dimension was accepted\n";
    return false;
  }
  std::cout << "indirect: ok\n";
  return true;
}

bool cull_compact() {
  auto device = vg::vulkan::make_device_hal();
  if (!device) {
    std::cerr << "cull-compact: no real Vulkan device\n";
    return false;
  }
  const std::vector<uint32_t> visible{1, 0, 1, 1, 0, 1, 0, 1, 0};
  const std::vector<uint32_t> ids{10, 11, 12, 13, 14, 15, 16, 17, 18};
  const auto oracle = vg::reference::cull_compact(visible, ids);
  vg::vulkan::GpuCullCompactExperimentResult result;
  std::string error;
  if (!oracle.ok ||
      !vg::vulkan::AdapterHarness(*device).run_gpu_cull_compact_experiment(
          visible, ids, &result, &error)) {
    std::cerr << "cull-compact: " << (oracle.ok ? error : oracle.message)
              << "\n";
    return false;
  }
  if (result.visible_count != oracle.compact_ids.size() ||
      !report_ok(result.report, 1) ||
      sorted(result.compact_ids) != sorted(oracle.compact_ids)) {
    std::cerr << "cull-compact: GPU result differs from reference oracle\n";
    return false;
  }
  const auto first = sorted(result.compact_ids);
  if (!vg::vulkan::AdapterHarness(*device).run_gpu_cull_compact_experiment(
          visible, ids, &result, &error) ||
      sorted(result.compact_ids) != first ||
      vg::vulkan::AdapterHarness(*device).run_gpu_cull_compact_experiment(
          {}, {}, &result, &error)) {
    std::cerr << "cull-compact: repeat or empty-input contract failed\n";
    return false;
  }
  std::cout << "cull-compact: ok\n";
  return true;
}

bool indexed_address() {
  auto device = vg::vulkan::make_device_hal();
  if (!device) {
    std::cerr << "indexed-address: no real Vulkan device\n";
    return false;
  }
  vg::vulkan::GpuIndexedAddressExperimentResult result;
  std::string error;
  if (!vg::vulkan::AdapterHarness(*device).run_gpu_indexed_address_experiment(
          {7, 8, 9, 10}, &result, &error)) {
    std::cerr << "indexed-address: " << error << "\n";
    return false;
  }
  if (result.referenced_allocation_count != 2 ||
      result.gpu_dispatch_count != 1 || !report_ok(result.report, 1)) {
    std::cerr << "indexed-address: compiler package result mismatch\n";
    return false;
  }
  std::cout << "indexed-address: ok\n";
  return true;
}

bool tier2() {
  auto device = vg::vulkan::make_device_hal();
  if (!device) {
    std::cerr << "tier2: no real Vulkan device\n";
    return false;
  }
  const std::vector<uint32_t> classes{0, 1, 0, 1, 1, 0, 1, 0};
  const std::vector<uint32_t> authorized{0, 1};
  const auto oracle =
      vg::reference::select_tier2_nodes(graph_for(classes), authorized);
  vg::vulkan::GpuTier2ExperimentResult result;
  std::string error;
  if (!oracle.ok ||
      !vg::vulkan::AdapterHarness(*device).run_gpu_tier2_bucket_experiment(
          classes, authorized, &result, &error)) {
    std::cerr << "tier2: " << (oracle.ok ? error : oracle.message) << "\n";
    return false;
  }
  if (sorted(result.selected_classes) != sorted(oracle.selected_classes) ||
      result.bucket_counts != std::vector<uint32_t>({4, 4}) ||
      result.indirect_dispatch_count != authorized.size() ||
      result.host_preprocessed_task_count != classes.size() ||
      !report_ok(result.report, 3)) {
    std::cerr << "tier2: GPU bucket result differs from oracle\n";
    return false;
  }
  // Invalid authorization must be a refuse, never a host-produced result.
  if (vg::vulkan::AdapterHarness(*device).run_gpu_tier2_bucket_experiment(
          {0, 2}, authorized, &result, &error)) {
    std::cerr << "tier2: unauthorized class was accepted\n";
    return false;
  }
  if (vg::vulkan::AdapterHarness(*device).run_gpu_tier2_bucket_experiment(
          classes, {0, 0}, &result, &error)) {
    std::cerr << "tier2: duplicate authorization was accepted\n";
    return false;
  }
  if (!vg::vulkan::AdapterHarness(*device).run_gpu_tier2_bucket_experiment(
          std::vector<uint32_t>(8, 0), authorized, &result, &error) ||
      result.bucket_counts != std::vector<uint32_t>({8, 0})) {
    std::cerr << "tier2: zero bucket fixture failed\n";
    return false;
  }
  std::cout << "tier2: ok\n";
  return true;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: vulkan_gpu_experiments_test "
                 "<indirect|cull-compact|indexed-address|tier2|cpu-oracle> "
                 "<repo_root>\n";
    return 2;
  }
  const std::string mode = argv[1];
  if (mode == "cpu-oracle")
    return cpu_oracle() ? 0 : 1;
  if (mode == "indirect")
    return indirect() ? 0 : 1;
  if (mode == "cull-compact")
    return cull_compact() ? 0 : 1;
  if (mode == "indexed-address")
    return indexed_address() ? 0 : 1;
  if (mode == "tier2")
    return tier2() ? 0 : 1;
  std::cerr << "unknown mode: " << mode << "\n";
  return 2;
}
