#include "backends/reference/reference_executor.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
vg::core::CanonicalView view(const vg::core::Allocation &allocation,
                             vg::core::PixelFormat format) {
  vg::core::CanonicalView result;
  result.allocation = allocation.id;
  result.allocation_generation = allocation.generation;
  result.format = format;
  result.dimension = vg::core::ViewDimension::Texture2D;
  result.width = 2;
  result.height = 2;
  return result;
}

template <typename Index> bool indexed_depth_oracle(const char *label) {
  vg::core::Arena arena;
  auto &source = arena.allocate(16);
  source.bytes = {255, 0, 0, 255, 255, 0, 0, 255,
                  255, 0, 0, 255, 255, 0, 0, 255};
  auto &target = arena.allocate(16);
  auto &depth = arena.allocate(4 * sizeof(float));
  const auto source_view = view(source, vg::core::PixelFormat::RGBA8Unorm);
  const auto target_view = view(target, vg::core::PixelFormat::RGBA8Unorm);
  const auto depth_view = view(depth, vg::core::PixelFormat::Depth32Float);

  const std::vector<vg::reference::RasterVertex> vertices{
      {-1, -1, 0.25f, 0.25f, 0.25f},
      {3, -1, 0.25f, 0.25f, 0.25f},
      {-1, 3, 0.25f, 0.25f, 0.25f}};
  const std::vector<Index> indices{0, 1, 2};
  std::vector<vg::reference::RasterVertex> indexed_vertices;
  indexed_vertices.reserve(indices.size());
  for (const Index index : indices) {
    if (static_cast<size_t>(index) >= vertices.size()) {
      std::cerr << label << ": index fixture is invalid\n";
      return false;
    }
    indexed_vertices.push_back(vertices[index]);
  }

  vg::reference::RasterDesc desc;
  desc.attachment.load = vg::reference::AttachmentLoadAction::Clear;
  desc.attachment.store = vg::reference::AttachmentStoreAction::Store;
  desc.attachment.clear_rgba = {0, 0, 0, 1};
  desc.depth_attachment = &depth_view;
  desc.depth_test_enable = true;
  desc.depth_write_enable = true;
  desc.depth_compare_op = vg::core::DepthCompareOp::Less;
  const auto result = vg::reference::raster_triangles(
      arena, source_view, target_view, desc, indexed_vertices);
  if (!result.ok || result.resolved_rgba.size() != 4 ||
      result.resolved_depth.size() != 4 ||
      std::fabs(result.resolved_rgba.front()[0] - 1.0f) > 1e-4f ||
      std::fabs(result.resolved_depth.front() - 0.25f) > 1e-4f ||
      !result.stored) {
    std::cerr << label << ": Reference indexed color/depth oracle failed: "
              << result.message << "\n";
    return false;
  }
  return true;
}

bool cpu_fixture() {
  const bool u16 = indexed_depth_oracle<uint16_t>("cpu-fixture-u16");
  const bool u32 = indexed_depth_oracle<uint32_t>("cpu-fixture-u32");
  if (!u16 || !u32)
    return false;
  std::cout << "cpu-fixture: Reference indexed color+Depth32Float oracle ok\n";
  return true;
}

bool contains(const std::string &text, const char *needle) {
  return text.find(needle) != std::string::npos;
}

bool source_contract(const char *root) {
  std::ifstream plan_file(std::string(root) +
                          "/src/backends/vulkan/vulkan_plan_raster.cpp");
  std::stringstream plan_stream;
  plan_stream << plan_file.rdbuf();
  const std::string plan = plan_stream.str();
  std::ifstream device_file(std::string(root) +
                            "/src/backends/vulkan/vulkan_device_hal.cpp");
  std::stringstream device_stream;
  device_stream << device_file.rdbuf();
  const std::string device = device_stream.str();
  const auto raster_gate = device.find("if (raster) {");
  const auto raster_gate_end = raster_gate == std::string::npos
                                   ? std::string::npos
                                   : device.find("\n  }", raster_gate);
  const auto user_import = device.find("Capability::UserShaderImport");
  const bool ok =
      plan_file && device_file && contains(plan, "vkCmdDrawIndexed") &&
      contains(plan, "vkCmdDraw(") && contains(plan, "index_count != 0") &&
      contains(plan, "resolve_scene_root_raster") &&
      contains(plan, "const float identity[16]") &&
      contains(plan, "VK_FORMAT_D32_SFLOAT") &&
      contains(plan, "VK_IMAGE_ASPECT_DEPTH_BIT") &&
      contains(plan, "VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL") &&
      contains(plan, "VkPipelineDepthStencilStateCreateInfo") &&
      contains(plan, "depthAttachmentFormat") &&
      contains(plan, "resolved_depth") && contains(plan, "depth_readback") &&
      contains(plan, "arena.mark_content_modified(*depth_allocation)") &&
      contains(plan, "ensure_plan_user_raster_pipeline") &&
      contains(plan, "vg.glsl.raster/v1") &&
      contains(plan, "vg.msl.raster/v1 is Unsupported") &&
      raster_gate != std::string::npos &&
      raster_gate_end != std::string::npos && user_import > raster_gate &&
      user_import < raster_gate_end;
  if (!ok)
    std::cerr << "source-contract: formal Vulkan Raster coverage changed\n";
  return ok;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  if (std::string(argv[1]) == "cpu-fixture")
    return cpu_fixture() ? 0 : 1;
  if (std::string(argv[1]) == "source-contract")
    return source_contract(argv[2]) ? 0 : 1;
  return 2;
}
