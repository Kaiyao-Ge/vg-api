#include "compiler/shader_sources.h"
#include "compiler/compute_task_ring.h"

namespace vg::compiler {
std::string task_ring_metal_source() {
  // gid indexes one task per threadgroup (dispatch grid is (task_count,1,1)
  // threadgroups of (1,1,1) threads); each thread publishes exactly one
  // task, so no two threads ever race on the same slot. MSL only exposes
  // the "weak" compare-exchange (may fail spuriously even when *object ==
  // expected), so this loops while the observed value is still 0 -- a
  // single un-looped weak CAS would let a spurious failure silently drop
  // that task's publication forever, since no other thread ever revisits
  // this slot to retry it.
  return std::string(
      "#include <metal_stdlib>\n"
      "using namespace metal;\n\n") + schema::compute_task_ring::kShaderLayout +
      "\n"
      "kernel void vg_task_publish(device atomic_uint* task_state [[buffer(0)]],\n"
      "                             device uint* task_fields [[buffer(1)]],\n"
      "                             constant uint* task_inputs [[buffer(2)]],\n"
      "                             uint gid [[thread_position_in_grid]]) {\n"
      "  uint expected = 0u;\n"
      "  bool won = false;\n"
      "  while (!won && expected == 0u) {\n"
      "    won = atomic_compare_exchange_weak_explicit(&task_state[gid], &expected, 1u,\n"
      "                                                memory_order_relaxed, memory_order_relaxed);\n"
      "  }\n"
      "  if (!won) return;\n"
      "  for (uint word = 0u; word < VG_TASK_RING_WORD_COUNT; ++word) {\n"
      "    task_fields[gid * VG_TASK_RING_WORD_COUNT + word] =\n"
      "        task_inputs[gid * VG_TASK_RING_WORD_COUNT + word];\n"
      "  }\n"
      "  atomic_store_explicit(&task_state[gid], 2u, memory_order_relaxed);\n"
      "}\n";
}

std::string task_ring_vulkan_source() {
  // GLSL's atomicCompSwap is a single hardware CAS with no "weak" spurious-
  // failure mode (unlike MSL's atomic_compare_exchange_weak_explicit above),
  // so a single un-looped comparison is sufficient here: the returned
  // original value is authoritative, not merely a retry hint.
  return std::string("#version 450\n") + schema::compute_task_ring::kShaderLayout +
      "\n"
      "#extension GL_EXT_buffer_reference2 : require\n"
      "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n\n"
      "layout(buffer_reference, std430, buffer_reference_align = 4) buffer VgTaskStateRef { uint state[]; };\n"
      "layout(buffer_reference, std430, buffer_reference_align = 4) buffer VgTaskFieldsRef { uint fields[]; };\n"
      "layout(buffer_reference, std430, buffer_reference_align = 4) buffer VgTaskInputsRef { uint inputs[]; };\n\n"
      "layout(push_constant) uniform VgTaskPushConstants {\n"
      "  VgTaskStateRef task_state;\n"
      "  VgTaskFieldsRef task_fields;\n"
      "  VgTaskInputsRef task_inputs;\n"
      "} vg_pc;\n\n"
      "void main() {\n"
      "  uint gid = gl_GlobalInvocationID.x;\n"
      "  if (atomicCompSwap(vg_pc.task_state.state[gid], 0u, 1u) != 0u) return;\n"
      "  for (uint word = 0u; word < VG_TASK_RING_WORD_COUNT; ++word) {\n"
      "    vg_pc.task_fields.fields[gid * VG_TASK_RING_WORD_COUNT + word] =\n"
      "        vg_pc.task_inputs.inputs[gid * VG_TASK_RING_WORD_COUNT + word];\n"
      "  }\n"
      "  atomicExchange(vg_pc.task_state.state[gid], 2u);\n"
      "}\n";
}

}  // namespace vg::compiler
