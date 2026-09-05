#include "backends/vulkan/vulkan_device_hal.h"
#include "core/execution_plan.h"
#include "ir/sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
using vg::core::NodeTable;

vg::core::CanonicalView view(const vg::core::Allocation &a,
                             vg::core::PixelFormat format, uint32_t width,
                             uint32_t height) {
  vg::core::CanonicalView v;
  v.allocation = a.id;
  v.allocation_generation = a.generation;
  v.format = format;
  v.dimension = vg::core::ViewDimension::Texture2D;
  v.width = width;
  v.height = height;
  v.array_layers = 1;
  v.mip_levels = 1;
  return v;
}

std::shared_ptr<const vg::core::CodeObject>
raster_object(const vg::core::Allocation &source, const char *schema) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = schema;
  module.instructions.push_back({"load", source.id, 0, 4, 0, source.generation,
                                 source.representation_epoch, 0, "load"});
  module.declared_effects.push_back(
      {source.id, 0, 4, vg::ir::Access::Read, source.representation_epoch});
  module.canonical_json = vg::ir::serialize_module(module);
  module.hash = vg::ir::sha256_hex(module.canonical_json);
  auto object = std::make_shared<vg::core::CodeObject>();
  object->module = std::move(module);
  return object;
}

bool build_plan(vg::vulkan::DeviceHal &device, vg::core::Arena &arena,
                bool authorize_b, bool indexed, bool mixed,
                vg::core::ExecutionPlan *out,
                std::array<NodeTable::Ref, 2> *refs, std::string *error) {
  auto &source_allocation = arena.allocate(16);
  const vg::core::PointerRef source_ref{source_allocation.id,
                                        source_allocation.generation};
  auto &target_allocation = arena.allocate(16);
  const vg::core::PointerRef target_ref{target_allocation.id,
                                        target_allocation.generation};
  auto &vertex_allocation = arena.allocate(6 * 5 * sizeof(float));
  const vg::core::PointerRef vertex_ref{vertex_allocation.id,
                                        vertex_allocation.generation};
  auto *source = arena.lookup(source_ref);
  auto *target = arena.lookup(target_ref);
  auto *vertices = arena.lookup(vertex_ref);
  if (source == nullptr || target == nullptr || vertices == nullptr)
    return false;
  source->bytes = {255, 0, 0, 255, 255, 0, 0, 255,
                   255, 0, 0, 255, 255, 0, 0, 255};
  const float quad[] = {-1, 1, 0, 0, 0, 1, 1,  0, 1, 0, -1, -1, 0, 0, 1,
                        1,  1, 0, 1, 0, 1, -1, 0, 1, 1, -1, -1, 0, 0, 1};
  std::memcpy(vertices->bytes.data(), quad, sizeof(quad));
  vg::core::FacetRef sample{}, attachment{}, address{}, index_address{};
  vg::core::Allocation *indices = nullptr;
  if (indexed || mixed) {
    auto &index_allocation = arena.allocate(3 * sizeof(uint16_t));
    const vg::core::PointerRef index_ref{index_allocation.id, index_allocation.generation};
    indices = arena.lookup(index_ref);
    const uint16_t triangle[] = {0, 1, 2};
    if (indices == nullptr) return false;
    std::memcpy(indices->bytes.data(), triangle, sizeof(triangle));
  }
  if (!device.facet_pool().acquire(
          arena, view(*source, vg::core::PixelFormat::RGBA8Unorm, 2, 2),
          vg::core::FacetKind::Sample, &sample, error) ||
      !device.facet_pool().acquire(
          arena, view(*target, vg::core::PixelFormat::RGBA8Unorm, 2, 2),
          vg::core::FacetKind::Attachment, &attachment, error) ||
      !device.facet_pool().acquire(
          arena,
          view(*vertices, vg::core::PixelFormat::RGBA8Unorm, sizeof(quad) / 4,
               1),
          vg::core::FacetKind::Address, &address, error) ||
      (indices != nullptr && !device.facet_pool().acquire(
          arena, view(*indices, vg::core::PixelFormat::R16Uint, 3, 1),
          vg::core::FacetKind::Address, &index_address, error)))
    return false;

  vg::core::NodeTable nodes;
  const auto a = nodes.create(raster_object(*source, "vg.test.tier2.a"), "a");
  const auto b = nodes.create(raster_object(*source, "vg.test.tier2.b"), "b");
  *refs = {a, b};
  vg::core::TaskGraphBuilder builder;
  uint32_t task_number = 0;
  for (const NodeTable::Ref ref : {a, a, a, b}) {
    vg::core::TaskRecord task{};
    task.kind = vg::core::TaskKind::Raster;
    task.node_index = ref.index;
    task.node_generation = ref.generation;
    task.root_allocation = source->id;
    task.root_generation = source->generation;
    task.raster_facets = {sample, attachment};
    task.vertex_buffer_ref = address;
    if (indexed || (mixed && task_number == 0)) {
      task.index_buffer_ref = index_address;
      task.index_count = 3;
    }
    task.raster_filter = vg::core::FilterMode::Nearest;
    task.raster_wrap = vg::core::WrapMode::Clamp;
    task.raster_tint = {1, 1, 1, 1};
    if (!builder.append(task, error))
      return false;
    ++task_number;
  }
  vg::core::TaskGraph graph;
  if (!builder.seal(&graph, error) || !graph.publish())
    return false;
  vg::core::ExecutionEnvelope envelope;
  envelope.allowed_nodes = authorize_b ? std::vector<NodeTable::Ref>{a, b}
                                       : std::vector<NodeTable::Ref>{a};
  const std::vector<NodeTable::Ref> selected{a, b};
  vg::core::ExecutionPlanAssemblerInputs inputs{
      &graph, &nodes, &envelope, &arena, nullptr, nullptr, nullptr, 0};
  inputs.facet_pool = &device.facet_pool();
  inputs.tier2_selection_nodes = &selected;
  return vg::core::ExecutionPlanAssembler::assemble(inputs, out, error);
}

bool positive(bool indexed) {
  std::string error;
  auto device = vg::vulkan::make_device_hal(&error);
  if (!device) {
    std::cerr << "no Vulkan device: " << error << "\n";
    return false;
  }
  if (!device->capabilities().supports(vg::hal::Capability::Raster) ||
      !device->capabilities().supports(
          vg::hal::Capability::IndirectTier2Select)) {
    std::cerr << "llvmpipe lacks Raster or IndirectTier2Select capability\n";
    return false;
  }
  vg::core::Arena arena;
  vg::core::ExecutionPlan plan;
  std::array<NodeTable::Ref, 2> refs{};
  if (!build_plan(*device, arena, true, indexed, false, &plan, &refs, &error)) {
    std::cerr << "assemble: " << error << "\n";
    return false;
  }
  if (plan.tier2_selection_nodes.size() != 2 ||
      plan.tier2_selection_nodes[0].index != refs[0].index ||
      plan.tier2_selection_nodes[0].generation != refs[0].generation ||
      plan.tier2_selection_nodes[1].index != refs[1].index ||
      plan.tier2_selection_nodes[1].generation != refs[1].generation)
    return false;
  vg::hal::CompiledPlan compiled;
  vg::hal::Submission submission;
  if (!device->compile(plan, &compiled, &error)) {
    std::cerr << "compile/submit: " << error << "\n";
    return false;
  }
  if (!device->submit(compiled, arena, &submission, &error)) {
    std::cerr << "compile/submit: " << error << "\n";
    return false;
  }
  const bool four_gpu_commands = std::ranges::any_of(
      submission.report.events, [](const vg::hal::LoweringEvent &event) {
        return event.operation == "tier2_bucket_fill_draw_commands" &&
               event.classification == vg::hal::LoweringClass::EmulatedDevicePass &&
               event.count == 4 &&
               event.reason.find("host did not read back or re-encode") != std::string::npos;
      });
  if (!submission.result.ok || submission.raster_results.size() != 4 ||
      submission.published_tasks.size() != 4 || !four_gpu_commands)
    return false;
  bool nonzero = false;
  for (const auto &result : submission.raster_results)
    for (const auto &pixel : result.resolved_rgba)
      nonzero = nonzero || pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0;
  if (!nonzero)
    return false;
  std::cout << "llvmpipe Tier2 repeated/skewed " << (indexed ? "indexed" : "direct") << " Raster ok\n";
  return true;
}

bool negative() {
  std::string error;
  auto device = vg::vulkan::make_device_hal(&error);
  if (!device)
    return false;
  vg::core::Arena arena;
  vg::core::ExecutionPlan plan;
  std::array<NodeTable::Ref, 2> refs{};
  if (build_plan(*device, arena, false, false, false, &plan, &refs, &error)) {
    std::cerr << "unauthorized Tier2 Node assembled\n";
    return false;
  }
  bool untouched_zero_target = false;
  for (const auto &[id, allocation] : arena.allocations()) {
    (void)id;
    if (allocation.content_epoch != 1)
      return false;
    if (allocation.bytes.size() == 16 &&
        std::ranges::all_of(allocation.bytes, [](uint8_t byte) { return byte == 0; }))
      untouched_zero_target = true;
  }
  return untouched_zero_target && error.find("unauthorized") != std::string::npos;
}

bool mixed_abi_rejected() {
  std::string error;
  auto device = vg::vulkan::make_device_hal(&error);
  if (!device) return false;
  vg::core::Arena arena;
  vg::core::ExecutionPlan plan;
  std::array<NodeTable::Ref, 2> refs{};
  if (!build_plan(*device, arena, true, false, true, &plan, &refs, &error)) return false;
  vg::hal::CompiledPlan compiled;
  if (device->compile(plan, &compiled, &error)) return false;
  return std::ranges::all_of(arena.allocations(), [](const auto &entry) {
    return entry.second.content_epoch == 1;
  });
}
} // namespace

int main() { return positive(false) && positive(true) && negative() && mixed_abi_rejected() ? 0 : 1; }
