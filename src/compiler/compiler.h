#ifndef VG_COMPILER_COMPILER_H_
#define VG_COMPILER_COMPILER_H_
#include "ir/ir.h"
#include <string>
#include <vector>
namespace vg::compiler {
struct CompileResult { bool ok{}; std::string message; ir::Module module; };

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

CompileResult compile_c_like(const std::string& source);
ComputePackageResult build_linear_compute_package(const ir::Module& module);

// Task Tier0 publication-protocol kernel source, shared across backends so
// their GPU-side Empty->Writing->Published state machines stay identical.
// Each task record is packed as 14 little-endian uint32 words (matches
// core::TaskRecord field order; 64-bit fields split into lo/hi words) --
// see backends/metal/metal_device_hal.mm's pack_task_record/unpack_task_record
// for the authoritative layout. `task_state[i]` is 0=Empty,1=Writing,
// 2=Published,3=Consumed; only Empty->Writing->Published are ever written by
// this kernel (Consumed is host-side bookkeeping after read-back).
constexpr uint32_t kTaskRingWordsPerRecord = 14;
std::string task_ring_metal_source();
// GLSL analogue of task_ring_metal_source(): identical Empty->Writing->
// Published state machine and 14-word record layout, addressed via
// buffer_reference (BDA) through push constants rather than buffer(N)
// slots, matching this compiler's existing GLSL codegen convention.
std::string task_ring_vulkan_source();

// TASK-B13 (E009): standalone GPU cull/compact kernel, following the same
// "independent hand-written kernel + dedicated pipeline" precedent as
// task_ring_metal_source() above -- this is not part of the target-neutral
// ComputePackage, just backend-private infrastructure for one experiment.
// One thread per instance; instance_visible[gid]==0 returns immediately,
// otherwise the thread claims an output slot via atomic_fetch_add on
// visible_count and writes instance_ids[gid] into compact_ids[slot]. Because
// slot assignment is ordered by whichever thread's atomic op lands first
// (not by gid), the compacted output's *set* of ids is well-defined but its
// *order* is not -- callers must compare as a set/sorted-multiset against a
// CPU oracle, never by position. This is a correct, expected property of
// atomic-append stream compaction, not a defect.
std::string cull_compact_metal_source();
// GLSL analogue of cull_compact_metal_source(): same one-thread-per-instance
// atomic-append compaction, addressed via buffer_reference (BDA) through
// push constants like task_ring_vulkan_source(). Compile-review-only on this
// project (no Vulkan hardware reachable here); provided for design symmetry
// and is not wired into any Vulkan dispatch call.
std::string cull_compact_vulkan_source();

// Standalone SampleFacet readback kernel, same "independent hand-written
// kernel + dedicated pipeline" precedent as cull_compact_metal_source()
// above -- texture/sampler binding is a different resource class than the
// buffer-only IR taxonomy every other ComputePackage here compiles, so this
// is backend-private infrastructure rather than a new IR opcode with a
// single consumer.
// One thread per uv coordinate: samples `tex` at `uv_coords[gid]` through
// `samp` and writes the result to `output[gid]` as a float4, so a host-side
// readback can compare it against the reference CPU oracle
// (reference::sample_facet). The MTLSamplerState itself (filter/wrap mode)
// is configured host-side when the sampler is created, not by this kernel.
std::string sample_facet_metal_source();
// GLSL analogue of sample_facet_metal_source(). Deliberately NOT
// buffer_reference/push-constant-addressed like the other *_vulkan_source()
// functions above -- combined image samplers are always descriptor-set
// bound in Vulkan regardless of BDA use elsewhere, so a push-constant-only
// scheme cannot express this kernel's inputs honestly. Compile-review-only
// on this project (no Vulkan hardware reachable here); not wired into any
// Vulkan dispatch call.
std::string sample_facet_vulkan_source();

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
}
#endif
