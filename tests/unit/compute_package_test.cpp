#include "compiler/compiler.h"

#include <cassert>
#include <string>

int main() {
  auto compiled = vg::compiler::compile_c_like("@node @effects store(7,0,4,9) atomic_add(7,8,8,3)");
  assert(compiled.ok);
  const auto package = vg::compiler::build_linear_compute_package(compiled.module);
  assert(package.ok);
  assert(package.package.bindings.size() == 1);
  assert(package.package.bindings[0].allocation == 7);
  assert(package.package.source_map.size() == 2);
  assert(package.package.metal_source.find("vg_linear_compute") != std::string::npos);
  assert(package.package.vulkan_glsl_source.find("atomicAdd") != std::string::npos);

  auto unsupported = vg::compiler::compile_c_like("@node @effects store(7,0,8,9)");
  assert(unsupported.ok);
  assert(!vg::compiler::build_linear_compute_package(unsupported.module).ok);

  // Phase C facet kernels. These are emitted as text and bound by index, so
  // the binding ABI declared in compiler.h and the entry-point names the host
  // looks up are pinned here rather than re-derived from the emitted source by
  // each caller. Backend-agnostic assertions only -- whether the MSL actually
  // compiles is a Metal-hardware question and belongs to the Metal tests.
  const std::string sample_metal = vg::compiler::sample_facet_metal_source();
  assert(sample_metal.find("kernel void vg_sample_facet(") != std::string::npos);
  assert(sample_metal.find("texture2d<float, access::sample> tex") != std::string::npos);
  // Explicit LOD: a compute kernel has no implicit derivatives, so E008's mip
  // axis needs a real level() argument rather than a level-0-only path.
  assert(sample_metal.find("level(lod)") != std::string::npos);
  // 06 §6.4's in-shader facet-generation check, gated on a function constant so
  // a fast-native pipeline pays nothing for a check it did not ask for (03 §12).
  assert(sample_metal.find("is_function_constant_defined(vg_checked_profile)") != std::string::npos);
  assert(sample_metal.find("facet_generation_table[slot] == facet_token.generation") != std::string::npos);

  const std::string sample_array_metal = vg::compiler::sample_facet_array_metal_source();
  assert(sample_array_metal.find("kernel void vg_sample_facet_array(") != std::string::npos);
  assert(sample_array_metal.find("texture2d_array<float, access::sample> tex") != std::string::npos);
  assert(sample_array_metal.find("array_slices[gid]") != std::string::npos);
  // The array kernel is the only one that binds the array-slice buffer; the 2D
  // kernel leaves that slot unused rather than renumbering the shared table.
  assert(sample_array_metal.find("buffer(" + std::to_string(vg::compiler::kSampleFacetArraySliceBufferIndex) +
                                 ")") != std::string::npos);
  assert(sample_metal.find("array_slices") == std::string::npos);

  const std::string sample_vulkan = vg::compiler::sample_facet_vulkan_source();
  assert(sample_vulkan.find("uniform sampler2D vg_tex") != std::string::npos);
  assert(sample_vulkan.find("textureLod(vg_tex") != std::string::npos);
  assert(sample_vulkan.find("layout(constant_id = 0) const bool vg_checked_profile = false;") !=
         std::string::npos);
  const std::string sample_array_vulkan = vg::compiler::sample_facet_array_vulkan_source();
  assert(sample_array_vulkan.find("uniform sampler2DArray vg_tex") != std::string::npos);

  // 05 §9's `region.attachment.store` lowering: a vertex+fragment pair, with
  // vertex *input* read as pointer-indexed root data (no MTLVertexDescriptor)
  // so vertex layout stays out of the pipeline cache key (06 §7).
  const std::string raster_metal = vg::compiler::raster_facet_metal_source();
  assert(raster_metal.find("vertex VgRasterVaryings vg_raster_vertex(") != std::string::npos);
  assert(raster_metal.find("fragment VgRasterFragment vg_raster_fragment(") != std::string::npos);
  assert(raster_metal.find("device const VgRasterVertex* vertices") != std::string::npos);
  assert(raster_metal.find("[[stage_in]]") != std::string::npos);
  assert(raster_metal.find("[[color(0)]]") != std::string::npos);
  assert(raster_metal.find("tex.sample(samp, varyings.uv) * tint") != std::string::npos);

  // One GLSL translation unit has one entry point, so both stages live behind
  // the two guards the host compiles with.
  const std::string raster_vulkan = vg::compiler::raster_facet_vulkan_source();
  assert(raster_vulkan.find("#ifdef VG_RASTER_VERTEX_STAGE") != std::string::npos);
  assert(raster_vulkan.find("#ifdef VG_RASTER_FRAGMENT_STAGE") != std::string::npos);
  // vec4[] gives the identical 16-byte std430 stride the two-vec2 MSL struct
  // has, so host vertex data is byte-compatible across the two backends.
  assert(raster_vulkan.find("vec4 vertices[]") != std::string::npos);
  return 0;
}
