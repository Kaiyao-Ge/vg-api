#ifndef VG_COMPILER_COMPUTE_PACKAGE_H_
#define VG_COMPILER_COMPUTE_PACKAGE_H_
#include "ir/ir.h"
#include <cstdint>
#include <string>
#include <vector>
namespace vg::compiler {
// Backend-neutral and inspectable input for B5/B6.  Backends compile the
// target source and own their pipeline objects; Core never sees those handles.
//
// B4 linear subset (build_linear_compute_package): the only IR this
// accepts is load/store on 4-byte, 4-byte-aligned words, and atomic_add
// on 8-byte, 8-byte-aligned words (matching the reference executor's
// int64 atomic_add contract byte-for-byte). publish and any other op are
// rejected -- GPU-driven publication is out of scope for this package.
// vulkan_glsl_source is BDA-addressed via push constants (GL_EXT_buffer_
// reference2), never classic descriptor sets, so B6 only needs to compile
// and dispatch it.
struct ComputeBinding { uint64_t allocation{}; uint32_t binding{}; };
struct ComputeSourceMapEntry { uint32_t instruction_index{}; uint32_t generated_line{}; std::string source; };
struct ComputePackage {
  uint32_t version{1};
  std::string canonical_ir_hash;
  std::string root_schema;
  std::vector<ComputeBinding> bindings;
  std::vector<ComputeSourceMapEntry> source_map;
  std::string metal_source;
  std::string vulkan_glsl_source;
};
struct ComputePackageResult { bool ok{}; std::string message; ComputePackage package; };

ComputePackageResult build_linear_compute_package(const ir::Module& module);

// TASK-B15 (E002): typed pointer graph. Only load_ref/load_via/store_via.
// This is the CachedObject lowering (ADR-028): a load_via/store_via's
// target allocation is bound directly, by binding(N) index, because its
// identity is already statically resolved via ir::Module::declared_pointer_edges
// (checked host-side in ir::verify() before this ever runs) -- so nothing
// dynamic needs to happen on the GPU to find it. load_ref's value is
// elided from the generated kernel entirely: it is never read on the GPU in
// this lowering, and exists only for the reference executor's dynamic
// dangling-ref check (does the loaded ref actually match the statically
// resolved target?) and for a possible future real-device-pointer lowering
// that would reinterpret it as a raw GPU virtual address instead.
ComputePackageResult build_pointer_graph_compute_package(const ir::Module& module);

// TASK-B16 (E007): root pointer vs. bindless binding cost. Same load/store-
// only, 4-byte/4-byte-aligned instruction contract as build_linear_compute_
// package (atomic_add is out of scope here -- disproportionate for a
// binding-cost experiment), but instead of one ComputeBinding per distinct
// allocation (N separate buffer(N) slots, hence N real setBuffer:atIndex:
// calls at encode time -- build_linear_compute_package's actual cost
// profile), every distinct allocation referenced is collapsed into ONE
// argument-buffer-style table binding: a small host-populated array of real
// GPU virtual addresses (one per distinct allocation, gated on
// hal::Capability::LinearAddress), bound at a single buffer(table_binding)
// slot. The generated kernel dereferences each instruction's target through
// its compile-time-known slot in that table (`(device uint*)table[K]`)
// rather than through its own dedicated buffer(N) parameter -- a real
// device-pointer dereference, unlike E002's CachedObject static-index
// lowering (ADR-028). `referenced_allocations` records the stable
// first-seen order backends must populate the table in (table[K] must hold
// referenced_allocations[K]'s real GPU address). This does not eliminate
// the need to mark every referenced allocation's buffer resident on the
// GPU (Metal's useResource, or the Vulkan equivalent) even though only the
// table is bound at an index -- that residency cost is exactly what this
// experiment measures against the traditional per-object binding cost, not
// something elided for convenience.
struct IndexedComputeBinding { uint32_t table_binding{}; uint32_t stride{}; uint32_t count{}; };
struct IndexedComputePackage {
  uint32_t version{1};
  std::string canonical_ir_hash;
  std::string root_schema;
  std::vector<uint64_t> referenced_allocations;
  IndexedComputeBinding binding;
  std::vector<ComputeSourceMapEntry> source_map;
  std::string metal_source;
  std::string vulkan_glsl_source;
};
struct IndexedComputePackageResult { bool ok{}; std::string message; IndexedComputePackage package; };
IndexedComputePackageResult build_indexed_compute_package(const ir::Module& module);
}  // namespace vg::compiler
#endif
