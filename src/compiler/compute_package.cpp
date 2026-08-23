#include "compiler/compiler.h"

#include "ir/sha256.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace vg::compiler {
namespace {
// B4 linear subset: the only instructions build_linear_compute_package()
// accepts. load/store operate on 4-byte, 4-byte-aligned words; atomic_add
// operates on 8-byte, 8-byte-aligned words to match the reference
// executor's int64 atomic_add contract (reference_executor.cpp requires
// exactly sizeof(int64_t)). publish and any other op are rejected: GPU-
// generated publication is out of scope for the linear compute package.
bool supported_instruction(const ir::Instruction& instruction, std::string* error) {
  if (instruction.op != "load" && instruction.op != "store" && instruction.op != "atomic_add") {
    if (error) *error = "linear compute package does not support instruction: " + instruction.op;
    return false;
  }
  if (instruction.op == "atomic_add") {
    if (instruction.size != 8 || (instruction.offset % 8) != 0) {
      if (error) *error = "linear compute package atomic_add requires 8-byte aligned accesses";
      return false;
    }
    return true;
  }
  if (instruction.size != 4 || (instruction.offset % 4) != 0) {
    if (error) *error = "linear compute package currently requires 4-byte aligned accesses";
    return false;
  }
  return true;
}
std::string binding_name(uint32_t binding) { return "allocation_" + std::to_string(binding); }

// reference_executor.cpp's store fills every byte in [offset, offset+size)
// with the low byte of `value` (a byte-broadcast, not a little-endian
// encoding of `value`). Codegen must reproduce that exact bit pattern so
// GPU-executed stores read back identically to the reference oracle.
uint32_t store_word_pattern(int64_t value) {
  const uint32_t low_byte = static_cast<uint32_t>(static_cast<uint8_t>(value));
  return low_byte * 0x01010101u;
}

// TASK-B15 (E002): the CachedObject lowering only accepts load_ref (12-byte,
// 4-byte-aligned) and load_via/store_via (4-byte, 4-byte-aligned, matching
// the linear package's own load/store granularity). A separate function
// from supported_instruction() above, not an extension of it -- these are a
// disjoint opcode set with disjoint size/alignment rules.
bool supported_pointer_instruction(const ir::Instruction& instruction, std::string* error) {
  if (instruction.op == "load_ref") {
    if (instruction.size != 12 || (instruction.offset % 4) != 0) {
      if (error) *error = "pointer graph compute package load_ref requires a 12-byte, 4-byte-aligned access";
      return false;
    }
    return true;
  }
  if (instruction.op == "load_via" || instruction.op == "store_via") {
    if (instruction.size != 4 || (instruction.offset % 4) != 0) {
      if (error) *error = "pointer graph compute package requires 4-byte aligned load_via/store_via";
      return false;
    }
    return true;
  }
  if (error) *error = "pointer graph compute package does not support instruction: " + instruction.op;
  return false;
}
}  // namespace

ComputePackageResult build_pointer_graph_compute_package(const ir::Module& module) {
  const auto verification = ir::verify(module);
  if (!verification.ok) return {false, verification.message, {}};
  std::set<uint64_t> allocation_ids;
  for (const auto& instruction : module.instructions) {
    std::string error;
    if (!supported_pointer_instruction(instruction, &error)) return {false, error, {}};
    if (instruction.op != "load_ref") allocation_ids.insert(instruction.allocation);
  }

  ComputePackage package;
  package.root_schema = module.root_schema;
  package.canonical_ir_hash = module.hash.empty() ? ir::sha256_hex(ir::serialize_module(module)) : module.hash;
  for (uint64_t allocation : allocation_ids)
    package.bindings.push_back({allocation, static_cast<uint32_t>(package.bindings.size())});

  std::ostringstream metal;
  metal << "#include <metal_stdlib>\nusing namespace metal;\n\n";
  metal << "// CachedObject lowering (ADR-028): load_via/store_via targets are bound\n";
  metal << "// directly by static resolution against declared_pointer_edges; load_ref's\n";
  metal << "// value is never read on the GPU in this lowering.\n";
  metal << "kernel void vg_pointer_graph_compute(";
  for (size_t index = 0; index < package.bindings.size(); ++index) {
    if (index != 0) metal << ", ";
    metal << "device uint* " << binding_name(package.bindings[index].binding)
          << " [[buffer(" << package.bindings[index].binding << ")]]";
  }
  if (!package.bindings.empty()) metal << ", ";
  metal << "uint3 gid [[thread_position_in_grid]]) {\n  if (any(gid != uint3(0))) return;\n";

  std::ostringstream glsl;
  glsl << "#version 450\n#extension GL_EXT_buffer_reference2 : require\n";
  glsl << "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n\n";
  glsl << "layout(buffer_reference, std430, buffer_reference_align = 4) buffer VgAllocationRef {\n  uint words[];\n};\n\n";
  glsl << "layout(push_constant) uniform VgPushConstants {\n  VgAllocationRef allocations[" << package.bindings.size() << "];\n} vg_pc;\n\n";
  glsl << "void main() {\n  if (gl_GlobalInvocationID != uvec3(0)) return;\n";

  for (size_t index = 0; index < module.instructions.size(); ++index) {
    const auto& instruction = module.instructions[index];
    package.source_map.push_back({static_cast<uint32_t>(index), static_cast<uint32_t>(5 + index), instruction.source});
    if (instruction.op == "load_ref") continue;
    const auto binding = std::find_if(package.bindings.begin(), package.bindings.end(),
                                      [&](const ComputeBinding& item) { return item.allocation == instruction.allocation; });
    const std::string name = binding_name(binding->binding);
    const std::string glsl_ref = "vg_pc.allocations[" + std::to_string(binding->binding) + "]";
    const uint64_t word = instruction.offset / 4;
    if (instruction.op == "load_via") {
      metal << "  volatile uint value_" << index << " = " << name << "[" << word << "];\n";
      glsl << "  uint value_" << index << " = " << glsl_ref << ".words[" << word << "];\n";
    } else {
      const uint32_t pattern = store_word_pattern(instruction.value);
      metal << "  " << name << "[" << word << "] = " << pattern << "u;\n";
      glsl << "  " << glsl_ref << ".words[" << word << "] = " << pattern << "u;\n";
    }
  }
  metal << "}\n";
  glsl << "}\n";
  package.metal_source = metal.str();
  package.vulkan_glsl_source = glsl.str();
  return {true, "", std::move(package)};
}

ComputePackageResult build_linear_compute_package(const ir::Module& module) {
  const auto verification = ir::verify(module);
  if (!verification.ok) return {false, verification.message, {}};
  std::set<uint64_t> allocation_ids;
  bool has_atomic = false;
  for (const auto& instruction : module.instructions) {
    std::string error;
    if (!supported_instruction(instruction, &error)) return {false, error, {}};
    allocation_ids.insert(instruction.allocation);
    if (instruction.op == "atomic_add") has_atomic = true;
  }

  ComputePackage package;
  package.root_schema = module.root_schema;
  package.canonical_ir_hash = module.hash.empty() ? ir::sha256_hex(ir::serialize_module(module)) : module.hash;
  for (uint64_t allocation : allocation_ids)
    package.bindings.push_back({allocation, static_cast<uint32_t>(package.bindings.size())});

  std::ostringstream metal;
  metal << "#include <metal_stdlib>\nusing namespace metal;\n\nkernel void vg_linear_compute(";
  for (size_t index = 0; index < package.bindings.size(); ++index) {
    if (index != 0) metal << ", ";
    metal << "device uint* " << binding_name(package.bindings[index].binding)
          << " [[buffer(" << package.bindings[index].binding << ")]]";
  }
  if (!package.bindings.empty()) metal << ", ";
  // MSL's != on vector types is component-wise and yields a bool3, unlike
  // GLSL where != on a vector yields a single bool -- any() collapses it.
  metal << "uint3 gid [[thread_position_in_grid]]) {\n  if (any(gid != uint3(0))) return;\n";

  // GLSL side is BDA-ready from the start: buffer_reference blocks addressed
  // via push-constant GPU VAs, no descriptor sets. A second buffer_reference
  // type (8-byte aligned) aliases the same allocation for atomic_add so the
  // 4-byte word view (load/store) and 8-byte view (atomic_add) can coexist.
  std::ostringstream glsl;
  glsl << "#version 450\n#extension GL_EXT_buffer_reference2 : require\n";
  if (has_atomic) glsl << "#extension GL_EXT_shader_atomic_int64 : require\n";
  glsl << "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n\n";
  glsl << "layout(buffer_reference, std430, buffer_reference_align = 4) buffer VgAllocationRef {\n  uint words[];\n};\n";
  if (has_atomic)
    glsl << "layout(buffer_reference, std430, buffer_reference_align = 8) buffer VgAllocationRef64 {\n  uint64_t words64[];\n};\n";
  glsl << "\nlayout(push_constant) uniform VgPushConstants {\n  VgAllocationRef allocations[" << package.bindings.size() << "];\n} vg_pc;\n\n";
  glsl << "void main() {\n  if (gl_GlobalInvocationID != uvec3(0)) return;\n";

  for (size_t index = 0; index < module.instructions.size(); ++index) {
    const auto& instruction = module.instructions[index];
    const auto binding = std::find_if(package.bindings.begin(), package.bindings.end(),
                                      [&](const ComputeBinding& item) { return item.allocation == instruction.allocation; });
    const std::string name = binding_name(binding->binding);
    const std::string glsl_ref = "vg_pc.allocations[" + std::to_string(binding->binding) + "]";
    package.source_map.push_back({static_cast<uint32_t>(index), static_cast<uint32_t>(5 + index), instruction.source});

    if (instruction.op == "load") {
      const uint64_t word = instruction.offset / 4;
      metal << "  volatile uint value_" << index << " = " << name << "[" << word << "];\n";
      glsl << "  uint value_" << index << " = " << glsl_ref << ".words[" << word << "];\n";
    } else if (instruction.op == "store") {
      const uint64_t word = instruction.offset / 4;
      const uint32_t pattern = store_word_pattern(instruction.value);
      metal << "  " << name << "[" << word << "] = " << pattern << "u;\n";
      glsl << "  " << glsl_ref << ".words[" << word << "] = " << pattern << "u;\n";
    } else {
      const uint64_t byte_offset = instruction.offset;
      const uint64_t word64 = byte_offset / 8;
      const uint64_t operand = static_cast<uint64_t>(instruction.value);
      metal << "  atomic_fetch_add_explicit((device atomic<ulong>*)((device uchar*)" << name << " + " << byte_offset
            << "), (ulong)" << operand << "UL, memory_order_relaxed);\n";
      glsl << "  atomicAdd(VgAllocationRef64(uint64_t(" << glsl_ref << ")).words64[" << word64 << "], uint64_t(" << operand << "UL));\n";
    }
  }
  metal << "}\n";
  glsl << "}\n";
  package.metal_source = metal.str();
  package.vulkan_glsl_source = glsl.str();
  return {true, "", std::move(package)};
}

std::string task_ring_metal_source() {
  // gid indexes one task per threadgroup (dispatch grid is (task_count,1,1)
  // threadgroups of (1,1,1) threads); each thread publishes exactly one
  // task, so no two threads ever race on the same slot. MSL only exposes
  // the "weak" compare-exchange (may fail spuriously even when *object ==
  // expected), so this loops while the observed value is still 0 -- a
  // single un-looped weak CAS would let a spurious failure silently drop
  // that task's publication forever, since no other thread ever revisits
  // this slot to retry it.
  return
      "#include <metal_stdlib>\n"
      "using namespace metal;\n\n"
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
      "  for (uint word = 0u; word < 14u; ++word) {\n"
      "    task_fields[gid * 14u + word] = task_inputs[gid * 14u + word];\n"
      "  }\n"
      "  atomic_store_explicit(&task_state[gid], 2u, memory_order_relaxed);\n"
      "}\n";
}

std::string task_ring_vulkan_source() {
  // GLSL's atomicCompSwap is a single hardware CAS with no "weak" spurious-
  // failure mode (unlike MSL's atomic_compare_exchange_weak_explicit above),
  // so a single un-looped comparison is sufficient here: the returned
  // original value is authoritative, not merely a retry hint.
  return
      "#version 450\n"
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
      "  for (uint word = 0u; word < 14u; ++word) {\n"
      "    vg_pc.task_fields.fields[gid * 14u + word] = vg_pc.task_inputs.inputs[gid * 14u + word];\n"
      "  }\n"
      "  atomicExchange(vg_pc.task_state.state[gid], 2u);\n"
      "}\n";
}

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
      "                            uint gid [[thread_position_in_grid]]) {\n"
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
      "} vg_pc;\n\n"
      "void main() {\n"
      "  uint gid = gl_GlobalInvocationID.x;\n"
      "  if (vg_pc.instance_visible.visible[gid] == 0u) return;\n"
      "  uint slot = atomicAdd(vg_pc.visible_count.count, 1u);\n"
      "  vg_pc.compact_ids.ids[slot] = vg_pc.instance_ids.ids[gid];\n"
      "}\n";
}

std::string sample_facet_metal_source() {
  return
      "#include <metal_stdlib>\n"
      "using namespace metal;\n\n"
      "kernel void vg_sample_facet(texture2d<float, access::sample> tex [[texture(0)]],\n"
      "                            sampler samp [[sampler(0)]],\n"
      "                            device const float2* uv_coords [[buffer(0)]],\n"
      "                            device float4* output [[buffer(1)]],\n"
      "                            uint gid [[thread_position_in_grid]]) {\n"
      "  output[gid] = tex.sample(samp, uv_coords[gid]);\n"
      "}\n";
}

std::string sample_facet_vulkan_source() {
  return
      "#version 450\n"
      "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n\n"
      "layout(set = 0, binding = 0) uniform sampler2D vg_tex;\n"
      "layout(set = 0, binding = 1, std430) readonly buffer VgUvBuffer { vec2 uv[]; };\n"
      "layout(set = 0, binding = 2, std430) writeonly buffer VgOutputBuffer { vec4 outp[]; };\n\n"
      "void main() {\n"
      "  uint gid = gl_GlobalInvocationID.x;\n"
      "  outp[gid] = texture(vg_tex, uv[gid]);\n"
      "}\n";
}

namespace {
// TASK-B16 (E007): load/store only, 4-byte/4-byte-aligned -- same
// granularity as build_linear_compute_package's own load/store subset, but
// a binding-cost experiment has no use for atomic_add's separate 8-byte
// contract, so it is simply rejected here rather than carried along.
bool supported_indexed_instruction(const ir::Instruction& instruction, std::string* error) {
  if (instruction.op != "load" && instruction.op != "store") {
    if (error) *error = "indexed compute package does not support instruction: " + instruction.op;
    return false;
  }
  if (instruction.size != 4 || (instruction.offset % 4) != 0) {
    if (error) *error = "indexed compute package currently requires 4-byte aligned accesses";
    return false;
  }
  return true;
}
}  // namespace

IndexedComputePackageResult build_indexed_compute_package(const ir::Module& module) {
  const auto verification = ir::verify(module);
  if (!verification.ok) return {false, verification.message, {}};

  IndexedComputePackage package;
  package.root_schema = module.root_schema;
  package.canonical_ir_hash = module.hash.empty() ? ir::sha256_hex(ir::serialize_module(module)) : module.hash;

  // First-seen distinct-allocation order becomes the table's row order --
  // unlike build_linear_compute_package's std::set<uint64_t> (sorted by
  // allocation id), this order only needs to be stable, not sorted, since
  // it is never round-tripped through anything that assumes numeric order.
  std::vector<uint32_t> table_index_by_instruction(module.instructions.size());
  for (size_t index = 0; index < module.instructions.size(); ++index) {
    const auto& instruction = module.instructions[index];
    std::string error;
    if (!supported_indexed_instruction(instruction, &error)) return {false, error, {}};
    const auto it = std::find(package.referenced_allocations.begin(), package.referenced_allocations.end(),
                              instruction.allocation);
    if (it == package.referenced_allocations.end()) {
      table_index_by_instruction[index] = static_cast<uint32_t>(package.referenced_allocations.size());
      package.referenced_allocations.push_back(instruction.allocation);
    } else {
      table_index_by_instruction[index] = static_cast<uint32_t>(it - package.referenced_allocations.begin());
    }
  }

  package.binding.table_binding = 0;
  package.binding.stride = sizeof(uint64_t);
  package.binding.count = static_cast<uint32_t>(package.referenced_allocations.size());

  // Metal side: a single constant table of real GPU virtual addresses
  // (hal::Capability::LinearAddress), one buffer(0) binding regardless of
  // how many distinct allocations are referenced -- the contrast this
  // experiment measures against build_linear_compute_package's N separate
  // buffer(N) parameters / N setBuffer:atIndex: calls. Backends populate
  // table[K] with referenced_allocations[K]'s real [buffer gpuAddress];
  // every underlying buffer still needs a residency declaration
  // (useResource: on Metal) even though only the table itself is bound at
  // an index -- that residency cost is exactly what this experiment
  // reports, not something elided.
  std::ostringstream metal;
  metal << "#include <metal_stdlib>\nusing namespace metal;\n\n";
  metal << "kernel void vg_indexed_compute(constant uint64_t* vg_table [[buffer(0)]],\n";
  metal << "                                uint3 gid [[thread_position_in_grid]]) {\n";
  metal << "  if (any(gid != uint3(0))) return;\n";

  // GLSL side: the same design expressed through this project's existing
  // BDA convention (VgAllocationRef, per build_linear_compute_package) --
  // vg_pc.table holds real device addresses directly rather than a
  // buffer_reference wrapper per slot, so each use casts the raw
  // uint64_t address to VgAllocationRef at the point of dereference.
  std::ostringstream glsl;
  glsl << "#version 450\n#extension GL_EXT_buffer_reference2 : require\n";
  glsl << "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n\n";
  glsl << "layout(buffer_reference, std430, buffer_reference_align = 4) buffer VgAllocationRef {\n  uint words[];\n};\n\n";
  glsl << "layout(push_constant) uniform VgIndexedPushConstants {\n  uint64_t table["
       << std::max<uint32_t>(package.binding.count, 1) << "];\n} vg_pc;\n\n";
  glsl << "void main() {\n  if (gl_GlobalInvocationID != uvec3(0)) return;\n";

  for (size_t index = 0; index < module.instructions.size(); ++index) {
    const auto& instruction = module.instructions[index];
    const uint32_t table_index = table_index_by_instruction[index];
    const uint64_t word = instruction.offset / 4;
    package.source_map.push_back({static_cast<uint32_t>(index), static_cast<uint32_t>(4 + index), instruction.source});

    const std::string metal_ptr = "((device uint*)vg_table[" + std::to_string(table_index) + "])";
    const std::string glsl_ref = "VgAllocationRef(vg_pc.table[" + std::to_string(table_index) + "])";
    if (instruction.op == "load") {
      metal << "  volatile uint value_" << index << " = " << metal_ptr << "[" << word << "];\n";
      glsl << "  uint value_" << index << " = " << glsl_ref << ".words[" << word << "];\n";
    } else {
      const uint32_t pattern = store_word_pattern(instruction.value);
      metal << "  " << metal_ptr << "[" << word << "] = " << pattern << "u;\n";
      glsl << "  " << glsl_ref << ".words[" << word << "] = " << pattern << "u;\n";
    }
  }
  metal << "}\n";
  glsl << "}\n";
  package.metal_source = metal.str();
  package.vulkan_glsl_source = glsl.str();
  return {true, "", std::move(package)};
}

}  // namespace vg::compiler
