#include "compiler/shader_sources.h"

namespace vg::compiler {
std::string cull_compact_metal_source() {
  // One thread per instance. Threads that fail the visibility check return
  // immediately and never touch visible_count/compact_ids, so no atomic op
  // is spent on invisible instances. relaxed ordering is sufficient: the
  // only cross-thread communication is the slot claim itself (the counter),
  // and each thread only ever writes to the exact slot it just claimed, so
  // there is no other memory this kernel needs a stronger fence for.
  return
      "#include <metal_stdlib>\n"
      "using namespace metal;\n\n"
      "kernel void vg_cull_compact(device const uint* instance_visible [[buffer(0)]],\n"
      "                            device const uint* instance_ids [[buffer(1)]],\n"
      "                            device atomic_uint* visible_count [[buffer(2)]],\n"
      "                            device uint* compact_ids [[buffer(3)]],\n"
      "                            constant uint& instance_count [[buffer(4)]],\n"
      "                            uint gid [[thread_position_in_grid]]) {\n"
      "  if (gid >= instance_count) return;\n"
      "  if (instance_visible[gid] == 0u) return;\n"
      "  uint slot = atomic_fetch_add_explicit(visible_count, 1u, memory_order_relaxed);\n"
      "  compact_ids[slot] = instance_ids[gid];\n"
      "}\n";
}

std::string cull_compact_vulkan_source() {
  // GLSL analogue of cull_compact_metal_source(): same one-thread-per-
  // instance atomic-append compaction, addressed via buffer_reference (BDA)
  // through push constants like task_ring_vulkan_source(). Compile-review-
  // only -- not wired into any Vulkan dispatch call in this project.
  return
      "#version 450\n"
      "#extension GL_EXT_buffer_reference2 : require\n"
      "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n\n"
      "layout(buffer_reference, std430, buffer_reference_align = 4) buffer VgInstanceVisibleRef { uint visible[]; };\n"
      "layout(buffer_reference, std430, buffer_reference_align = 4) buffer VgInstanceIdsRef { uint ids[]; };\n"
      "layout(buffer_reference, std430, buffer_reference_align = 4) buffer VgVisibleCountRef { uint count; };\n"
      "layout(buffer_reference, std430, buffer_reference_align = 4) buffer VgCompactIdsRef { uint ids[]; };\n\n"
      "layout(push_constant) uniform VgCullCompactPushConstants {\n"
      "  VgInstanceVisibleRef instance_visible;\n"
      "  VgInstanceIdsRef instance_ids;\n"
      "  VgVisibleCountRef visible_count;\n"
      "  VgCompactIdsRef compact_ids;\n"
      "  uint instance_count;\n"
      "} vg_pc;\n\n"
      "void main() {\n"
      "  uint gid = gl_GlobalInvocationID.x;\n"
      "  if (gid >= vg_pc.instance_count) return;\n"
      "  if (vg_pc.instance_visible.visible[gid] == 0u) return;\n"
      "  uint slot = atomicAdd(vg_pc.visible_count.count, 1u);\n"
      "  vg_pc.compact_ids.ids[slot] = vg_pc.instance_ids.ids[gid];\n"
      "}\n";
}

}  // namespace vg::compiler
