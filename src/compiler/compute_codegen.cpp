#include "compiler/compute_codegen.h"

#include <algorithm>
#include <sstream>

namespace vg::compiler::detail {
namespace {
std::string binding_name(uint32_t binding) { return "allocation_" + std::to_string(binding); }

// reference_executor.cpp's store fills every byte in [offset, offset+size)
// with the low byte of `value` (a byte-broadcast, not a little-endian
// encoding of `value`). Codegen must reproduce that exact bit pattern so
// GPU-executed stores read back identically to the reference oracle.
uint32_t store_word_pattern(int64_t value) {
  const auto low_byte = static_cast<uint32_t>(static_cast<uint8_t>(value));
  return low_byte * 0x01010101u;
}

}  // namespace

ComputeSources emit_pointer_graph_compute_sources(const ir::Module& module,
                                                  std::span<const ComputeBinding> bindings) {
  std::ostringstream metal;
  metal << "#include <metal_stdlib>\nusing namespace metal;\n\n";
  metal << "// CachedObject lowering (ADR-028): load_via/store_via targets are bound\n";
  metal << "// directly by static resolution against declared_pointer_edges; load_ref's\n";
  metal << "// value is never read on the GPU in this lowering.\n";
  metal << "kernel void vg_pointer_graph_compute(";
  for (size_t index = 0; index < bindings.size(); ++index) {
    if (index != 0) metal << ", ";
    metal << "device uint* " << binding_name(bindings[index].binding)
          << " [[buffer(" << bindings[index].binding << ")]]";
  }
  if (!bindings.empty()) metal << ", ";
  metal << "uint3 gid [[thread_position_in_grid]]) {\n  if (any(gid != uint3(0))) return;\n";

  std::ostringstream glsl;
  glsl << "#version 450\n#extension GL_EXT_buffer_reference2 : require\n";
  glsl << "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n\n";
  glsl << "layout(buffer_reference, std430, buffer_reference_align = 4) buffer VgAllocationRef {\n  uint words[];\n};\n\n";
  glsl << "layout(push_constant) uniform VgPushConstants {\n  VgAllocationRef allocations[" << bindings.size() << "];\n} vg_pc;\n\n";
  glsl << "void main() {\n  if (gl_GlobalInvocationID != uvec3(0)) return;\n";

  for (size_t index = 0; index < module.instructions.size(); ++index) {
    const auto& instruction = module.instructions[index];
    if (instruction.op == "load_ref") continue;
    const auto binding = std::ranges::find_if(bindings,
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
  return {metal.str(), glsl.str()};
}

ComputeSources emit_linear_compute_sources(const ir::Module& module,
                                           std::span<const ComputeBinding> bindings,
                                           bool has_atomic) {
  std::ostringstream metal;
  metal << "#include <metal_stdlib>\nusing namespace metal;\n\nkernel void vg_linear_compute(";
  for (size_t index = 0; index < bindings.size(); ++index) {
    if (index != 0) metal << ", ";
    metal << "device uint* " << binding_name(bindings[index].binding)
          << " [[buffer(" << bindings[index].binding << ")]]";
  }
  if (!bindings.empty()) metal << ", ";
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
  glsl << "\nlayout(push_constant) uniform VgPushConstants {\n  VgAllocationRef allocations[" << bindings.size() << "];\n} vg_pc;\n\n";
  glsl << "void main() {\n  if (gl_GlobalInvocationID != uvec3(0)) return;\n";

  for (size_t index = 0; index < module.instructions.size(); ++index) {
    const auto& instruction = module.instructions[index];
    const auto binding = std::ranges::find_if(bindings,
                                      [&](const ComputeBinding& item) { return item.allocation == instruction.allocation; });
    const std::string name = binding_name(binding->binding);
    const std::string glsl_ref = "vg_pc.allocations[" + std::to_string(binding->binding) + "]";

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
      const auto operand = static_cast<uint64_t>(instruction.value);
      metal << "  atomic_fetch_add_explicit((device atomic<ulong>*)((device uchar*)" << name << " + " << byte_offset
            << "), (ulong)" << operand << "UL, memory_order_relaxed);\n";
      glsl << "  atomicAdd(VgAllocationRef64(uint64_t(" << glsl_ref << ")).words64[" << word64 << "], uint64_t(" << operand << "UL));\n";
    }
  }
  metal << "}\n";
  glsl << "}\n";
  return {metal.str(), glsl.str()};
}

ComputeSources emit_indexed_compute_sources(const ir::Module& module,
                                            uint32_t binding_count,
                                            std::span<const uint32_t> table_index_by_instruction) {
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
       << std::max<uint32_t>(binding_count, 1) << "];\n} vg_pc;\n\n";
  glsl << "void main() {\n  if (gl_GlobalInvocationID != uvec3(0)) return;\n";

  for (size_t index = 0; index < module.instructions.size(); ++index) {
    const auto& instruction = module.instructions[index];
    const uint32_t table_index = table_index_by_instruction[index];
    const uint64_t word = instruction.offset / 4;

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
  return {metal.str(), glsl.str()};
}

}  // namespace vg::compiler::detail
