#include "../support/assembled_plan_fixture.h"
#include "backends/vulkan/vulkan_user_raster.h"
#include "ir/ir.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const std::string good =
      R"({"root_schema":"x","vertex_entry":"v","fragment_entry":"f","vertex_abi":"vg.raster.vertex.xyzuv-packed/v1","source":"#version 450\n#ifdef VG_VERTEX_STAGE\nvoid v(){}\n#endif\n#ifdef VG_FRAGMENT_STAGE\nvoid f(){}\n#endif"})";
  auto glsl = vg::ir::parse_glsl_raster_envelope(good);
  try {
    (void)vg::ir::parse_glsl_raster_envelope(
        R"({"root_schema":"x","vertex_entry":"v","fragment_entry":"f","vertex_abi":"vg.raster.vertex.xyzuv-packed/v1","source":"void v(){}"})");
    return 1;
  } catch (...) {
  }
  vg::vulkan::UserRasterSpirv stages;
  std::string error;
  if (vg::vulkan::compile_user_raster_glsl(glsl, &stages, &error) ||
      error.find("unavailable") == std::string::npos) {
    std::cerr
        << "stub compiler did not report its explicit unavailable state\n";
    return 1;
  }
  vg::vulkan::UserRasterSpirvCache cache;
  const vg::vulkan::UserRasterSpirv *cached = nullptr;
  if (vg::vulkan::get_or_compile_user_raster_glsl(&cache, glsl, &cached,
                                                  &error) ||
      cached != nullptr || error.find("unavailable") == std::string::npos) {
    std::cerr << "stub cache did not report its explicit unavailable state\n";
    return 1;
  }
  for (const std::string &bad :
       {std::string("#version 450\n#include <metal_stdlib>\n#ifdef "
                    "VG_VERTEX_STAGE\nvoid v(){}\n#endif\n#ifdef "
                    "VG_FRAGMENT_STAGE\nvoid f(){}\n#endif"),
        std::string(
            "#version 450\nvoid main(){}\n#ifdef VG_VERTEX_STAGE\nvoid "
            "v(){}\n#endif\n#ifdef VG_FRAGMENT_STAGE\nvoid f(){}\n#endif"),
        std::string("#version 450\nvoid v(){}")}) {
    auto c = glsl;
    c.source = bad;
    if (vg::vulkan::compile_user_raster_glsl(c, &stages, &error))
      return 1;
  }
  auto bad_abi = glsl;
  bad_abi.vertex_abi = "vg.raster.vertex.xyuv/v1";
  if (vg::vulkan::compile_user_raster_glsl(bad_abi, &stages, &error))
    return 1;
  vg::core::Arena arena;
  auto make_view = [](const vg::core::Allocation &a) {
    vg::core::CanonicalView v;
    v.allocation = a.id;
    v.allocation_generation = a.generation;
    v.format = vg::core::PixelFormat::RGBA8Unorm;
    v.dimension = vg::core::ViewDimension::Texture2D;
    v.width = 2;
    v.height = 2;
    return v;
  };
  auto &source = arena.allocate(16);
  auto &target = arena.allocate(16);
  auto &vertices = arena.allocate(60);
  vg::core::FacetPool pool;
  vg::core::FacetRef sample{}, attachment{}, vertex{};
  auto source_view = make_view(source), target_view = make_view(target),
       vertex_view = make_view(vertices);
  vertex_view.width = 15;
  vertex_view.height = 1;
  if (!pool.acquire(arena, source_view, vg::core::FacetKind::Sample, &sample,
                    &error) ||
      !pool.acquire(arena, target_view, vg::core::FacetKind::Attachment,
                    &attachment, &error) ||
      !pool.acquire(arena, vertex_view, vg::core::FacetKind::Address, &vertex,
                    &error))
    return 1;
  vg::core::TaskRecord task{};
  task.kind = vg::core::TaskKind::Raster;
  task.root_allocation = source.id;
  task.root_generation = source.generation;
  task.raster_facets = {sample, attachment};
  task.vertex_buffer_ref = vertex;
  task.x = task.y = task.z = 1;
  const vg::ir::UserRasterShaderContract msl{
      "vg.test.raster/v1", "vg_vertex", "vg_fragment",
      "vg.raster.vertex.xyzuv-packed/v1", "#include <metal_stdlib>"};
  // This Core fixture uses its parsed immutable result directly; public API
  // routing is checked below against its two distinct source-format branches.
  constexpr const char kLoadedFormat[] = "vg.msl.raster/v1";
  constexpr const char kVulkanLoadedFormat[] = "vg.glsl.raster/v1";
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions options;
  options.facet_pool = &pool;
  if (!vg::test_support::assemble_single_user_raster_plan(
          arena, msl, {task}, &fixture, &plan, &error, options) ||
      plan.resolved_nodes.size() != 1 ||
      !plan.resolved_nodes.front().user_raster_shader.has_value() ||
      std::find(plan.required_capabilities.begin(),
                plan.required_capabilities.end(),
                vg::core::CapabilityRequirement::Raster) ==
          plan.required_capabilities.end() ||
      std::find(plan.required_capabilities.begin(),
                plan.required_capabilities.end(),
                vg::core::CapabilityRequirement::UserShaderImport) ==
          plan.required_capabilities.end()) {
    std::cerr << "restricted MSL plan was not assembled: " << error << "\n";
    return 1;
  }
  std::ifstream api(std::string(argv[1]) + "/src/api/vg_api_code.cpp");
  const std::string api_source((std::istreambuf_iterator<char>(api)), {});
  std::ifstream device(std::string(argv[1]) +
                       "/src/backends/vulkan/vulkan_device_hal.cpp");
  const std::string device_source((std::istreambuf_iterator<char>(device)), {});
  std::ifstream user_header(std::string(argv[1]) +
                            "/src/backends/vulkan/vulkan_user_raster.h");
  const std::string header_source((std::istreambuf_iterator<char>(user_header)),
                                  {});
  std::ifstream user_component(std::string(argv[1]) +
                               "/src/backends/vulkan/vulkan_user_raster.cpp");
  const std::string component_source(
      (std::istreambuf_iterator<char>(user_component)), {});
  std::ifstream plan_raster(std::string(argv[1]) +
                            "/src/backends/vulkan/vulkan_plan_raster.cpp");
  const std::string plan_raster_source(
      (std::istreambuf_iterator<char>(plan_raster)), {});
  const auto msl_dispatch = api_source.find(kLoadedFormat);
  const auto glsl_dispatch = api_source.find(kVulkanLoadedFormat);
  const auto raster_gate = device_source.find("if (raster) {");
  const auto raster_gate_end = raster_gate == std::string::npos
                                   ? std::string::npos
                                   : device_source.find("\n  }", raster_gate);
  const auto user_shader_capability =
      device_source.find("Capability::UserShaderImport");
  if (msl_dispatch == std::string::npos || glsl_dispatch == std::string::npos ||
      msl_dispatch >= glsl_dispatch ||
      api_source.find("parse_msl_raster_envelope", msl_dispatch) ==
          std::string::npos ||
      api_source.find("parse_glsl_raster_envelope", glsl_dispatch) ==
          std::string::npos ||
      header_source.find("UserRasterSpirvCache") == std::string::npos ||
      header_source.find("get_or_compile_user_raster_glsl") ==
          std::string::npos ||
      header_source.find("create_user_raster_shader_modules") ==
          std::string::npos ||
      header_source.find("destroy_user_raster_shader_modules") ==
          std::string::npos ||
      component_source.find("metal_stdlib") == std::string::npos ||
      component_source.find("Vulkan user raster compiler is unavailable") ==
          std::string::npos ||
      component_source.find("vkCreateShaderModule") == std::string::npos ||
      plan_raster_source.find("vg.glsl.raster/v1") == std::string::npos ||
      plan_raster_source.find("vg.msl.raster/v1 is Unsupported") ==
          std::string::npos ||
      plan_raster_source.find("get_or_compile_user_raster_glsl") ==
          std::string::npos ||
      plan_raster_source.find("ensure_plan_user_raster_pipeline") ==
          std::string::npos ||
      plan_raster_source.find("LoweringClass::HostAssisted") ==
          std::string::npos ||
      raster_gate == std::string::npos ||
      raster_gate_end == std::string::npos ||
      user_shader_capability == std::string::npos ||
      user_shader_capability <= raster_gate ||
      user_shader_capability >= raster_gate_end) {
    std::cerr << "Vulkan GLSL format separation or reusable user-raster "
                 "component contract changed\n";
    return 1;
  }
  std::cout << "GLSL route, formal plan-raster cache path, and reusable "
               "Vulkan user-raster interfaces verified; UserShaderImport "
               "is Raster-gated\n";
  return 0;
}
