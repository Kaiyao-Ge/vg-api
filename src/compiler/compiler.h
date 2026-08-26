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

// Binding ABI shared by every sample-facet kernel below, declared here so the
// Metal backend and the vertical-slice tests bind the same slots without
// re-deriving them from the emitted MSL text. Buffer indices 0/1 keep their
// historical meaning (uv coordinates, float4 output); everything added for
// explicit LOD, array slices and the 06 §6.4 generation guard occupies
// strictly higher indices. Index 3 (array slices) is only present in
// sample_facet_array_metal_source(); the 2D kernel leaves that slot unused
// rather than renumbering the shared table.
constexpr uint32_t kSampleFacetTextureIndex = 0;
constexpr uint32_t kSampleFacetSamplerIndex = 0;
constexpr uint32_t kSampleFacetUvBufferIndex = 0;
constexpr uint32_t kSampleFacetOutputBufferIndex = 1;
constexpr uint32_t kSampleFacetLodBufferIndex = 2;
constexpr uint32_t kSampleFacetArraySliceBufferIndex = 3;
constexpr uint32_t kSampleFacetTokenBufferIndex = 4;
constexpr uint32_t kSampleFacetGenerationTableBufferIndex = 5;
constexpr uint32_t kSampleFacetSlotCountBufferIndex = 6;
constexpr uint32_t kSampleFacetViolationCounterBufferIndex = 7;

// 03 §12: a profile changes instrumentation, never meaning. The facet-
// generation guard 06 §6.4 requires ("checked profile 在 shader 中验证
// generation") is therefore gated on an MSL function constant / GLSL
// specialization constant rather than a uniform branch, so a `fast-native`
// pipeline compiles the guard, its four extra bindings and its atomic away
// entirely and pays nothing for a check it did not ask for. A pipeline that
// leaves the constant undefined behaves exactly like `fast-native`: the
// kernels use is_function_constant_defined() so an unset constant means
// "guard off", never a pipeline compile failure.
constexpr uint32_t kFacetCheckedProfileFunctionConstant = 0;

// Written to every channel of the failing thread's output slot when the
// checked-profile guard rejects the facet token. Chosen finite (not NaN) so
// it survives readback and byte-comparison unchanged, and far outside the
// range any format this project samples can produce -- RGBA8Unorm yields
// [0,1] and the R32Float fixtures are small integers -- so a poisoned slot
// can never be confused with a legitimately sampled value. A poisoned
// thread does not sample at all, matching 02's rule that a rejected access
// produces poison rather than a plausible-looking substitute.
constexpr float kFacetGenerationPoisonValue = -3.0e38f;

// Standalone SampleFacet readback kernel, same "independent hand-written
// kernel + dedicated pipeline" precedent as cull_compact_metal_source()
// above -- texture/sampler binding is a different resource class than the
// buffer-only IR taxonomy every other ComputePackage here compiles, so this
// is backend-private infrastructure rather than a new IR opcode with a
// single consumer.
// One thread per uv coordinate: samples `tex` at `uv_coords[gid]` through
// `samp` at the explicit level(lod) taken from buffer(2) and writes the
// result to `output[gid]` as a float4, so a host-side readback can compare
// it against the reference CPU oracle (reference::sample_facet). The
// MTLSamplerState itself (filter/wrap mode) is configured host-side when the
// sampler is created, not by this kernel. LOD is explicit because a compute
// kernel has no implicit derivatives -- with lod == 0 this samples exactly
// what the pre-mip version of this kernel sampled, so the existing Metal
// vertical-slice oracle comparison is unaffected; a non-zero lod is what
// gives E008's "2D/array/mip" input axis a real mip path instead of a
// silently level-0-only one.
// Under the checked profile (kFacetCheckedProfileFunctionConstant) the
// kernel first validates the caller's facet token against the host-supplied
// generation table: `index < slot_count && table[index] != 0 &&
// table[index] == generation`, i.e. exactly core::FacetPool's own
// generation_valid() predicate evaluated on the GPU. A thread that fails it
// does not sample; it writes kFacetGenerationPoisonValue to its output slot
// and atomically increments the violation counter at buffer(7), so the host
// can prove the shader itself rejected a stale token rather than inferring
// it from a suspicious-looking sampled value.
std::string sample_facet_metal_source();
// texture2d_array<float> analogue of sample_facet_metal_source(), taking a
// per-coordinate `array_slices[gid]` at buffer(3) in addition to the shared
// uv/lod inputs. It exists so CanonicalView's Texture2DArray dimension has a
// real sampling path (E008 explicitly lists array inputs) instead of being
// rejected by the backend for want of a kernel -- a rejection would have been
// an honest but avoidable Unsupported, and 05 §9 asks facet lowering to reach
// the specialized hardware unit, not to narrow what the unified Region can
// express. Same checked-profile guard, same poison value, same violation
// counter as the 2D kernel.
std::string sample_facet_array_metal_source();
// GLSL analogue of sample_facet_metal_source(). Deliberately NOT
// buffer_reference/push-constant-addressed like the other *_vulkan_source()
// functions above -- combined image samplers are always descriptor-set
// bound in Vulkan regardless of BDA use elsewhere, so a push-constant-only
// scheme cannot express this kernel's inputs honestly. The generation guard
// is gated on a `layout(constant_id = 0)` specialization constant, the GLSL
// equivalent of the MSL function constant, and defaults to false so an
// unspecialized module is the fast-native shape. Compile-review-only on this
// project (no Vulkan hardware reachable here); not wired into any Vulkan
// dispatch call.
std::string sample_facet_vulkan_source();
// GLSL analogue of sample_facet_array_metal_source(), sampler2DArray-based.
// Compile-review-only on this project (no Vulkan hardware reachable here);
// not wired into any Vulkan dispatch call.
std::string sample_facet_array_vulkan_source();

// Binding ABI of the basic raster pair below. Metal keeps a separate binding
// table per stage, so vertex buffer(0) and fragment buffer(0) are different
// slots and both may legitimately be index 0.
constexpr uint32_t kRasterVertexBufferIndex = 0;
constexpr uint32_t kRasterTintBufferIndex = 0;
constexpr uint32_t kRasterTextureIndex = 0;
constexpr uint32_t kRasterSamplerIndex = 0;

// Phase C basic raster path: the minimal vertex+fragment pair that lowers
// 05 §9's `region.attachment.store` onto a real Metal render attachment
// (06 §6.3), giving the software-rasterizer oracle a GPU counterpart to be
// compared against instead of a semantics-only reference.
// Vertex stage `vg_raster_vertex` reads an interleaved
// `struct { packed_float3 position; packed_float2 uv; }` array (clip-space
// x/y, normalized z, uv in [0,1]) from a `device` buffer at vertex buffer(0), indexed
// by [[vertex_id]]. Deliberately no [[stage_in]] / MTLVertexDescriptor on
// the vertex *input* side: pointer-indexed root data is this project's
// addressing philosophy (04 §8, 06 §5), it keeps vertex layout out of the
// pipeline cache key (06 §7), and it spares the host an entire vertex-
// descriptor object. Fragment varyings still arrive via [[stage_in]] --
// interpolation is a fixed-function stage interface, not a resource binding,
// and there is no pointer-shaped way to express it.
// Fragment stage `vg_raster_fragment` samples `texture(0)` through
// `sampler(0)`, multiplies by the per-draw `float4` tint at fragment
// buffer(0), and returns it through a struct member tagged [[color(0)]].
// Deliberately out of scope: depth/stencil, blending, MSAA resolve and
// instancing -- 06 §7's raster-state-in-key question is answered by
// pipeline_classification.h, not by growing this shader.
std::string raster_facet_metal_source();
// GLSL analogue of raster_facet_metal_source(). Both stages live in one
// string guarded by VG_RASTER_VERTEX_STAGE / VG_RASTER_FRAGMENT_STAGE,
// because a GLSL translation unit has exactly one entry point and this
// project's *_source() functions return exactly one string; the host
// compiles it twice with the matching define and --stage. Like
// sample_facet_vulkan_source() the combined image sampler is descriptor-set
// bound rather than BDA-addressed, and the tint is a plain uniform block for
// the same reason. The vertex array is declared as `vec4[]` (xy = clip
// position, zw = uv), which is the identical 16-byte stride std430 gives the
// two-vec2 struct, so host data is byte-compatible with the MSL side.
// Honest discrepancy, not compensated here: Vulkan's clip space has +Y
// pointing down where Metal's points up, so identical vertex data draws
// vertically mirrored between the two backends unless the host flips the
// viewport. Compile-review-only on this project (no Vulkan hardware
// reachable here); not wired into any Vulkan draw call.
std::string raster_facet_vulkan_source();

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
