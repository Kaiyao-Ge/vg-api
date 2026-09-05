#include "backends/vulkan/vulkan_device_hal.h"
#include "core/execution_plan.h"
#include "ir/sha256.h"

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

namespace {
struct Fixture {
  vg::vulkan::DeviceHal &device;
  vg::core::Arena arena;
  vg::core::Allocation *source{};
  vg::core::Allocation *target{};
  vg::core::FacetRef sample{}, attachment{}, vertex{};

  explicit Fixture(vg::vulkan::DeviceHal &d) : device(d) {
    auto &s = arena.allocate(16);
    const vg::core::PointerRef sr{s.id, s.generation};
    auto &t = arena.allocate(16);
    const vg::core::PointerRef tr{t.id, t.generation};
    auto &v = arena.allocate(6 * 5 * sizeof(float));
    const vg::core::PointerRef vr{v.id, v.generation};
    source = arena.lookup(sr);
    target = arena.lookup(tr);
    auto *vertices = arena.lookup(vr);
    source->bytes = {255, 0, 0, 255, 255, 0, 0, 255,
                     255, 0, 0, 255, 255, 0, 0, 255};
    const float quad[] = {-1, 1, 0, 0, 0, 1, 1,  0, 1, 0, -1, -1, 0, 0, 1,
                          1,  1, 0, 1, 0, 1, -1, 0, 1, 1, -1, -1, 0, 0, 1};
    std::memcpy(vertices->bytes.data(), quad, sizeof(quad));
    auto image = [](const vg::core::Allocation &a, uint32_t width,
                    uint32_t height) {
      vg::core::CanonicalView view;
      view.allocation = a.id;
      view.allocation_generation = a.generation;
      view.format = vg::core::PixelFormat::RGBA8Unorm;
      view.dimension = vg::core::ViewDimension::Texture2D;
      view.width = width;
      view.height = height;
      return view;
    };
    std::string error;
    if (!device.facet_pool().acquire(arena, image(*source, 2, 2),
                                     vg::core::FacetKind::Sample, &sample,
                                     &error) ||
        !device.facet_pool().acquire(arena, image(*target, 2, 2),
                                     vg::core::FacetKind::Attachment,
                                     &attachment, &error) ||
        !device.facet_pool().acquire(
            arena, image(*vertices, sizeof(quad) / 4, 1),
            vg::core::FacetKind::Address, &vertex, &error))
      throw std::runtime_error(error);
  }
};

const char *glsl_source() {
  return R"(#version 450
#ifdef VG_VERTEX_STAGE
struct V { float x; float y; float z; float u; float v; };
layout(set=0,binding=0,std430) readonly buffer VB { V v[]; };
layout(set=0,binding=3,std140) uniform Root { mat4 camera; vec4 color; uvec2 albedo; } root;
layout(location=0) out vec2 uv;
void vertex_main(){ V a=v[gl_VertexIndex]; gl_Position=root.camera*vec4(a.x,a.y,a.z,1); uv=vec2(a.u,a.v); }
#endif
#ifdef VG_FRAGMENT_STAGE
layout(set=0,binding=1) uniform sampler2D tex;
layout(set=0,binding=2,std140) uniform Tint { vec4 tint; };
layout(location=0) in vec2 uv; layout(location=0) out vec4 out_color;
void fragment_main(){ out_color=texture(tex,uv)*tint; }
#endif
)";
}

const char *bad_binding_glsl_source() {
  return R"(#version 450
#ifdef VG_VERTEX_STAGE
struct V { float x; float y; float z; float u; float v; };
layout(set=0,binding=1,std430) readonly buffer WrongBinding { V v[]; };
layout(location=0) out vec2 uv;
void vertex_main(){ V a=v[gl_VertexIndex]; gl_Position=vec4(a.x,a.y,a.z,1); uv=vec2(a.u,a.v); }
#endif
#ifdef VG_FRAGMENT_STAGE
layout(location=0) in vec2 uv; layout(location=0) out vec4 out_color;
void fragment_main(){ out_color=vec4(uv,0,1); }
#endif
)";
}

std::shared_ptr<const vg::core::CodeObject> object(bool msl, bool bad_binding) {
  auto out = std::make_shared<vg::core::CodeObject>();
  out->format_tag = msl ? "vg.msl.raster/v1" : "vg.glsl.raster/v1";
  out->user_raster_shader = vg::ir::UserRasterShaderContract{
      "vg.test.user-glsl/v1", "vertex_main", "fragment_main",
      vg::ir::kRasterVertexAbiXyzuvPackedV1,
      msl ? "#include <metal_stdlib>"
          : (bad_binding ? bad_binding_glsl_source() : glsl_source())};
  return out;
}

bool run(bool msl, bool bad_binding = false) {
  std::string error;
  auto device = vg::vulkan::make_device_hal(&error);
  if (!device) {
    std::cerr << error << "\n";
    return false;
  }
  if (!device->capabilities().supports(vg::hal::Capability::Raster) ||
      !device->capabilities().supports(vg::hal::Capability::UserShaderImport))
    return false;
  Fixture fixture(*device);
  vg::core::NodeTable nodes;
  const auto ref = nodes.create(object(msl, bad_binding), "user-glsl");
  vg::core::TaskRecord task{};
  task.kind = vg::core::TaskKind::Raster;
  task.node_index = ref.index;
  task.node_generation = ref.generation;
  task.root_allocation = fixture.source->id;
  task.root_generation = fixture.source->generation;
  task.raster_facets = {fixture.sample, fixture.attachment};
  task.vertex_buffer_ref = fixture.vertex;
  task.raster_filter = vg::core::FilterMode::Nearest;
  task.raster_wrap = vg::core::WrapMode::Clamp;
  task.raster_tint = {1, 1, 1, 1};
  vg::core::TaskGraphBuilder builder;
  if (!builder.append(task, &error))
    return false;
  vg::core::TaskGraph graph;
  if (!builder.seal(&graph, &error) || !graph.publish())
    return false;
  vg::core::ExecutionEnvelope envelope;
  envelope.allowed_nodes = {ref};
  vg::core::ExecutionPlanAssemblerInputs inputs{
      &graph, &nodes, &envelope, &fixture.arena, nullptr, nullptr, nullptr, 0};
  inputs.facet_pool = &device->facet_pool();
  vg::core::ExecutionPlan plan;
  if (!vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error)) {
    std::cerr << "assemble: " << error << "\n";
    return false;
  }
  const auto before = fixture.target->bytes;
  vg::hal::CompiledPlan compiled;
  if (msl || bad_binding) {
    const bool rejected = !device->compile(plan, &compiled, &error);
    const bool precise =
        msl ? error.find("vg.glsl.raster/v1") != std::string::npos
            : error.find("descriptor set/binding ABI") != std::string::npos;
    return rejected && precise && fixture.target->bytes == before;
  }
  vg::hal::Submission submission;
  if (!device->compile(plan, &compiled, &error) ||
      !device->submit(compiled, fixture.arena, &submission, &error)) {
    std::cerr << "compile/submit: " << error << "\n";
    return false;
  }
  bool nonzero = false;
  for (const auto &pixel : submission.raster_results.front().resolved_rgba)
    nonzero = nonzero || pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0;
  return submission.result.ok && submission.raster_results.size() == 1 &&
         submission.published_tasks.size() == 1 && nonzero &&
         submission.report.count(vg::hal::LoweringClass::HostAssisted) != 0;
}
} // namespace

int main() { return run(false) && run(true) && run(false, true) ? 0 : 1; }
