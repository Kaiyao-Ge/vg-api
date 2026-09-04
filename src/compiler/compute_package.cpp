#include "compiler/compute_package.h"
#include "compiler/compute_codegen.h"
#include "ir/sha256.h"

#include <algorithm>
#include <set>
#include <utility>

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

  for (size_t index = 0; index < module.instructions.size(); ++index) {
    package.source_map.push_back({static_cast<uint32_t>(index), static_cast<uint32_t>(5 + index),
                                  module.instructions[index].source});
  }
  auto sources = detail::emit_pointer_graph_compute_sources(module, package.bindings);
  package.metal_source = std::move(sources.metal_source);
  package.vulkan_glsl_source = std::move(sources.vulkan_glsl_source);
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

  for (size_t index = 0; index < module.instructions.size(); ++index) {
    package.source_map.push_back({static_cast<uint32_t>(index), static_cast<uint32_t>(5 + index),
                                  module.instructions[index].source});
  }
  auto sources = detail::emit_linear_compute_sources(module, package.bindings, has_atomic);
  package.metal_source = std::move(sources.metal_source);
  package.vulkan_glsl_source = std::move(sources.vulkan_glsl_source);
  return {true, "", std::move(package)};
}

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
    const auto it = std::ranges::find(package.referenced_allocations,
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

  for (size_t index = 0; index < module.instructions.size(); ++index) {
    package.source_map.push_back({static_cast<uint32_t>(index), static_cast<uint32_t>(4 + index),
                                  module.instructions[index].source});
  }
  auto sources = detail::emit_indexed_compute_sources(module, package.binding.count, table_index_by_instruction);
  package.metal_source = std::move(sources.metal_source);
  package.vulkan_glsl_source = std::move(sources.vulkan_glsl_source);
  return {true, "", std::move(package)};
}

}  // namespace vg::compiler
