#include "backends/metal/metal_shader_sources.h"
namespace vg::metal {
const char* storage_facet_metal_source() {
  const char* source =
      "#include <metal_stdlib>\n"
      "using namespace metal;\n"
      "kernel void vg_storage_facet_write(texture2d<float, access::write> tex [[texture(0)]],\n"
      "                                   constant float4& rgba [[buffer(0)]],\n"
      "                                   constant uint4& target [[buffer(1)]],\n"
      "                                   uint2 gid [[thread_position_in_grid]]) {\n"
      "  if (gid.x != 0 || gid.y != 0) return;\n"
      "  tex.write(rgba, uint2(target.x, target.y), target.w);\n"
      "}\n"
      "kernel void vg_storage_facet_write_array(texture2d_array<float, access::write> tex [[texture(0)]],\n"
      "                                         constant float4& rgba [[buffer(0)]],\n"
      "                                         constant uint4& target [[buffer(1)]],\n"
      "                                         uint2 gid [[thread_position_in_grid]]) {\n"
      "  if (gid.x != 0 || gid.y != 0) return;\n"
      "  tex.write(rgba, uint2(target.x, target.y), target.z, target.w);\n"
      "}\n"
      // format: 0 = RGBA8Unorm, 1 = R32Float, matching core::PixelFormat.
      // The write is encoded in the view's own format so the linear target
      // never silently changes precision (06 §6.2). texel_index is the
      // caller's texel offset in units of core::bytes_per_texel, which is 4
      // for both formats this milestone models.
      "kernel void vg_storage_facet_write_buffer(device uint* texels [[buffer(0)]],\n"
      "                                          constant float4& rgba [[buffer(1)]],\n"
      "                                          constant uint& format [[buffer(2)]],\n"
      "                                          constant uint& texel_index [[buffer(3)]],\n"
      "                                          uint gid [[thread_position_in_grid]]) {\n"
      "  if (gid != 0) return;\n"
      "  if (format == 0) {\n"
      "    texels[texel_index] = pack_float_to_unorm4x8(rgba);\n"
      "  } else {\n"
      "    device float* floats = (device float*)texels;\n"
      "    floats[texel_index] = rgba.x;\n"
      "  }\n"
      "}\n";
  return source;
}
}
