#include "compiler/shader_sources.h"
#include "vg_scene_root_msl.h"

#include <sstream>

namespace vg::compiler {
std::string raster_facet_metal_source() {
  std::ostringstream out;
  out << "#include <metal_stdlib>\n"
      << "using namespace metal;\n\n"
      // F4's public raster vertex ABI is the tightly packed five-float tuple
      // {x,y,z,u,v}.  `packed_float3`, rather than `float3`, is required here:
      // Metal aligns float3 and float2 to 16/8 bytes whereas the C++ ABI intentionally has
      // no padding between z and u. Both members must therefore be packed.
      // This is an intentional F3 contract break;
      // old four-float user MSL vertex declarations must be rebuilt.
      << "struct VgRasterVertex { packed_float3 position; packed_float2 uv; };\n"
      // Generated from scene-root-raster.vg.json. The declaration names and
      // packed types are no longer a second handwritten layout source.
      << VG_SCHEMA_SCENEROOTRASTER_MSL_DECLARATIONS
      << "struct VgRasterVaryings { float4 position [[position]]; float2 uv; };\n"
      << "struct VgRasterFragment { float4 color [[color(0)]]; };\n\n"
      << "vertex VgRasterVaryings vg_raster_vertex(device const VgRasterVertex* vertices [[buffer("
      << kRasterVertexBufferIndex << ")]],\n"
      << "                                         constant VgSchema_SceneRootRaster& root [[buffer("
      << kRasterSceneRootBufferIndex << ")]],\n"
      << "                                         uint vid [[vertex_id]]) {\n"
      << "  VgRasterVaryings varyings;\n"
      << "  float4x4 camera(float4(root.camera_clip_from_local[0]), float4(root.camera_clip_from_local[1]), "
      << "float4(root.camera_clip_from_local[2]), float4(root.camera_clip_from_local[3]));\n"
      << "  varyings.position = camera * float4(float3(vertices[vid].position), 1.0f);\n"
      << "  varyings.uv = float2(vertices[vid].uv);\n"
      << "  return varyings;\n"
      << "}\n\n"
      << "fragment VgRasterFragment vg_raster_fragment(VgRasterVaryings varyings [[stage_in]],\n"
      << "                                             texture2d<float, access::sample> tex [[texture("
      << kRasterTextureIndex << ")]],\n"
      << "                                             sampler samp [[sampler(" << kRasterSamplerIndex << ")]],\n"
      << "                                             constant float4& tint [[buffer(" << kRasterTintBufferIndex
      << ")]]) {\n"
      << "  VgRasterFragment result;\n"
      << "  result.color = tex.sample(samp, varyings.uv) * tint;\n"
      << "  return result;\n"
      << "}\n";
  return out.str();
}

std::string raster_facet_vulkan_source() {
  return
      "#version 450\n\n"
      "#ifdef VG_RASTER_VERTEX_STAGE\n"
      "layout(set = 0, binding = 0, std430) readonly buffer VgRasterVertexBuffer { vec4 vertices[]; };\n"
      "layout(location = 0) out vec2 vg_uv;\n"
      "void main() {\n"
      "  vec4 vertex = vertices[gl_VertexIndex];\n"
      "  gl_Position = vec4(vertex.xy, 0.0, 1.0);\n"
      "  vg_uv = vertex.zw;\n"
      "}\n"
      "#endif\n\n"
      "#ifdef VG_RASTER_FRAGMENT_STAGE\n"
      "layout(set = 0, binding = 1) uniform sampler2D vg_tex;\n"
      "layout(set = 0, binding = 2) uniform VgRasterTint { vec4 tint; } vg_tint;\n"
      "layout(location = 0) in vec2 vg_uv;\n"
      "layout(location = 0) out vec4 vg_color;\n"
      "void main() {\n"
      "  vg_color = texture(vg_tex, vg_uv) * vg_tint.tint;\n"
      "}\n"
      "#endif\n";
}

}  // namespace vg::compiler
