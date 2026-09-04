#include "compiler/shader_sources.h"

#include <iomanip>
#include <sstream>

namespace vg::compiler {
namespace {
// The poison literal is formatted from kFacetGenerationPoisonValue rather
// than spelled out in the shader text, so the value the host compares
// against and the value the shader writes cannot drift apart.
std::string poison_literal() {
  std::ostringstream out;
  out << std::scientific << std::setprecision(8) << kFacetGenerationPoisonValue << "f";
  return out.str();
}

// Emitted identically into the 2D and array kernels so the two sampling
// paths can never end up with different guard semantics.
std::string metal_facet_guard_prelude() {
  std::ostringstream out;
  out << "struct VgFacetToken { uint index; uint generation; };\n"
      << "constant bool vg_checked_profile [[function_constant("
      << kFacetCheckedProfileFunctionConstant << ")]];\n\n";
  return out.str();
}

std::string metal_facet_guard_arguments(const std::string& indent) {
  std::ostringstream out;
  out << indent << "constant VgFacetToken& facet_token [[buffer(" << kSampleFacetTokenBufferIndex << ")]],\n"
      << indent << "device const uint* facet_generation_table [[buffer(" << kSampleFacetGenerationTableBufferIndex
      << ")]],\n"
      << indent << "constant uint& facet_slot_count [[buffer(" << kSampleFacetSlotCountBufferIndex << ")]],\n"
      << indent << "device atomic_uint* facet_violation_count [[buffer(" << kSampleFacetViolationCounterBufferIndex
      << ")]],\n";
  return out.str();
}

// is_function_constant_defined() rather than a bare read of the constant:
// a pipeline that never sets it must compile and behave as fast-native, not
// fail pipeline creation. Both branches fold away at specialization time.
std::string metal_facet_guard_body() {
  std::ostringstream out;
  out << "  bool vg_checked = false;\n"
      << "  if (is_function_constant_defined(vg_checked_profile)) vg_checked = vg_checked_profile;\n"
      << "  if (vg_checked) {\n"
      << "    const uint slot = facet_token.index;\n"
      << "    const bool live = slot < facet_slot_count && facet_generation_table[slot] != 0u &&\n"
      << "                      facet_generation_table[slot] == facet_token.generation;\n"
      << "    if (!live) {\n"
      << "      output[gid] = float4(" << poison_literal() << ");\n"
      << "      atomic_fetch_add_explicit(facet_violation_count, 1u, memory_order_relaxed);\n"
      << "      return;\n"
      << "    }\n"
      << "  }\n";
  return out.str();
}

// GLSL descriptor slots continue the numbering sample_facet_vulkan_source()
// already used (0 = combined image sampler, 1 = uv, 2 = output), so they are
// offset from the Metal buffer indices by the sampler occupying slot 0. This
// side is compile-review-only, so the divergence costs nothing and renumbering
// would break the existing reviewed source.
std::string vulkan_facet_guard_declarations() {
  std::ostringstream out;
  out << "layout(constant_id = " << kFacetCheckedProfileFunctionConstant
      << ") const bool vg_checked_profile = false;\n"
      << "layout(set = 0, binding = 5) uniform VgFacetToken { uint index; uint generation; } vg_facet;\n"
      << "layout(set = 0, binding = 6, std430) readonly buffer VgFacetGenerationTable { uint table[]; };\n"
      << "layout(set = 0, binding = 7) uniform VgFacetSlotCount { uint count; } vg_slots;\n"
      << "layout(set = 0, binding = 8, std430) buffer VgFacetViolationCount { uint count; } vg_violation;\n";
  return out.str();
}

std::string vulkan_facet_guard_body() {
  std::ostringstream out;
  out << "  if (vg_checked_profile) {\n"
      << "    uint slot = vg_facet.index;\n"
      << "    bool live = slot < vg_slots.count && table[slot] != 0u && table[slot] == vg_facet.generation;\n"
      << "    if (!live) {\n"
      << "      outp[gid] = vec4(" << poison_literal() << ");\n"
      << "      atomicAdd(vg_violation.count, 1u);\n"
      << "      return;\n"
      << "    }\n"
      << "  }\n";
  return out.str();
}
}  // namespace

std::string sample_facet_metal_source() {
  std::ostringstream out;
  out << "#include <metal_stdlib>\n"
      << "using namespace metal;\n\n"
      << metal_facet_guard_prelude()
      << "kernel void vg_sample_facet(texture2d<float, access::sample> tex [[texture("
      << kSampleFacetTextureIndex << ")]],\n"
      << "                            sampler samp [[sampler(" << kSampleFacetSamplerIndex << ")]],\n"
      << "                            device const float2* uv_coords [[buffer(" << kSampleFacetUvBufferIndex
      << ")]],\n"
      << "                            device float4* output [[buffer(" << kSampleFacetOutputBufferIndex << ")]],\n"
      << "                            constant float& lod [[buffer(" << kSampleFacetLodBufferIndex << ")]],\n"
      << metal_facet_guard_arguments("                            ")
      << "                            uint gid [[thread_position_in_grid]]) {\n"
      << metal_facet_guard_body()
      << "  output[gid] = tex.sample(samp, uv_coords[gid], level(lod));\n"
      << "}\n";
  return out.str();
}

std::string sample_facet_array_metal_source() {
  std::ostringstream out;
  out << "#include <metal_stdlib>\n"
      << "using namespace metal;\n\n"
      << metal_facet_guard_prelude()
      << "kernel void vg_sample_facet_array(texture2d_array<float, access::sample> tex [[texture("
      << kSampleFacetTextureIndex << ")]],\n"
      << "                                  sampler samp [[sampler(" << kSampleFacetSamplerIndex << ")]],\n"
      << "                                  device const float2* uv_coords [[buffer(" << kSampleFacetUvBufferIndex
      << ")]],\n"
      << "                                  device float4* output [[buffer(" << kSampleFacetOutputBufferIndex
      << ")]],\n"
      << "                                  constant float& lod [[buffer(" << kSampleFacetLodBufferIndex << ")]],\n"
      << "                                  device const uint* array_slices [[buffer("
      << kSampleFacetArraySliceBufferIndex << ")]],\n"
      << metal_facet_guard_arguments("                                  ")
      << "                                  uint gid [[thread_position_in_grid]]) {\n"
      << metal_facet_guard_body()
      << "  output[gid] = tex.sample(samp, uv_coords[gid], array_slices[gid], level(lod));\n"
      << "}\n";
  return out.str();
}

std::string sample_facet_vulkan_source() {
  std::ostringstream out;
  out << "#version 450\n"
      << "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n\n"
      << vulkan_facet_guard_declarations()
      << "layout(set = 0, binding = 0) uniform sampler2D vg_tex;\n"
      << "layout(set = 0, binding = 1, std430) readonly buffer VgUvBuffer { vec2 uv[]; };\n"
      << "layout(set = 0, binding = 2, std430) buffer VgOutputBuffer { vec4 outp[]; };\n"
      << "layout(set = 0, binding = 3) uniform VgSampleLod { float lod; } vg_lod;\n\n"
      << "void main() {\n"
      << "  uint gid = gl_GlobalInvocationID.x;\n"
      << vulkan_facet_guard_body()
      << "  outp[gid] = textureLod(vg_tex, uv[gid], vg_lod.lod);\n"
      << "}\n";
  return out.str();
}

std::string sample_facet_array_vulkan_source() {
  std::ostringstream out;
  out << "#version 450\n"
      << "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n\n"
      << vulkan_facet_guard_declarations()
      << "layout(set = 0, binding = 0) uniform sampler2DArray vg_tex;\n"
      << "layout(set = 0, binding = 1, std430) readonly buffer VgUvBuffer { vec2 uv[]; };\n"
      << "layout(set = 0, binding = 2, std430) buffer VgOutputBuffer { vec4 outp[]; };\n"
      << "layout(set = 0, binding = 3) uniform VgSampleLod { float lod; } vg_lod;\n"
      << "layout(set = 0, binding = 4, std430) readonly buffer VgArraySliceBuffer { uint slices[]; };\n\n"
      << "void main() {\n"
      << "  uint gid = gl_GlobalInvocationID.x;\n"
      << vulkan_facet_guard_body()
      << "  outp[gid] = textureLod(vg_tex, vec3(uv[gid], float(slices[gid])), vg_lod.lod);\n"
      << "}\n";
  return out.str();
}

}  // namespace vg::compiler
