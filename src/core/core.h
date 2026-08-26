#ifndef VG_CORE_CORE_H_
#define VG_CORE_CORE_H_

#include "ir/ir.h"

#include <array>
#include <cstdint>
#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vg::hal { struct ExecutionPlan; }

namespace vg::core {

enum class ObjectState { Active, Retired };
enum class PoisonState { Valid, PartiallyProduced, Poisoned };

// 03-system-architecture.md Sec.12's four profiles. A profile never changes
// what a legal program means -- it only decides how much diagnosis and
// instrumentation is paid for, which is why this is a plain input to backends
// rather than a field of any sealed object.
enum class ValidationProfile : uint32_t { CheckedNative, FastNative, ReferenceStrict, Capture };

struct Allocation {
  uint64_t id{};
  uint32_t generation{1};
  uint64_t size{};
  ObjectState state{ObjectState::Active};
  uint32_t representation_epoch{0};
  uint32_t in_flight{};
  // How many representation versions of this allocation are still live. Starts
  // at 1 (the allocation's initial representation); transform() adds one and
  // release_representation() removes one. This is what a non-zero
  // Arena::max_in_flight_representations() budget is checked against (E016:
  // "禁止无界创建版本").
  uint32_t live_representations{1};
  std::vector<uint8_t> bytes;
};

// The four obligations 02-principles-and-semantics.md Sec.4.2 requires a
// caller to discharge before ConsumeInput may run: the old envelope has
// completed, nothing outside holds a reference, the old version will never be
// replayed, and the caller accepts that a fault leaves no rollback. Passed as
// an argument to Arena::consume rather than inferred, because none of the four
// is observable from arena state alone -- consume() refuses unless all four
// hold, which is 10-validation-and-benchmarks.md Sec.3's "ConsumeInput proof"
// conformance row and Sec.4's "illegal consume" negative case.
struct ConsumeProof {
  bool envelope_complete{};
  bool no_external_references{};
  bool no_replay_required{};
  bool failure_semantics_accepted{};

  [[nodiscard]] bool complete() const {
    return envelope_complete && no_external_references && no_replay_required &&
           failure_semantics_accepted;
  }
  // Names the first unmet obligation so a rejection says which proof failed
  // rather than only that one did; nullptr when complete().
  [[nodiscard]] const char* first_unmet() const;
};

// Identity of a live allocation version (id + generation). Defined here so
// Arena lookup/retire can take it by name; GraphEpoch is the type that
// *produces* these refs.
struct PointerRef;
// Allocation + generation + representation epoch. Same idea for the
// representation-aware lookup overloads.
struct RepresentationRef;

class Arena {
 public:
  explicit Arena(uint64_t id = 1) : id_(id) {}
  Allocation& allocate(uint64_t size);
  bool retire(const PointerRef& ref);
  Allocation* lookup(const PointerRef& ref);
  [[nodiscard]] const Allocation* lookup(const PointerRef& ref) const;
  Allocation* lookup(const RepresentationRef& ref);
  [[nodiscard]] const Allocation* lookup(const RepresentationRef& ref) const;
  bool acquire(uint64_t id, uint32_t generation);
  bool release(uint64_t id, uint32_t generation);
  bool transform(uint64_t id, uint32_t generation, uint32_t* new_epoch, std::string* error = nullptr);
  bool transform(uint64_t id, uint32_t generation, uint32_t expected_epoch, uint32_t* new_epoch, std::string* error = nullptr);
  // ConsumeInput (02 Sec.4.2). Destructive: the allocation is retired, its
  // generation bumped so no old token can ever resolve again, and its backing
  // bytes actually released -- there is no rollback to the pre-consume
  // representation afterwards, which is exactly what `proof` attests the
  // caller accepts. Refuses an incomplete proof before touching any state.
  bool consume(uint64_t id, uint32_t generation, uint32_t expected_epoch, const ConsumeProof& proof,
               std::string* error = nullptr);
  // ConsumeInput applied to a representation transform (06 Sec.11: the proof
  // buys "reuse heap range、in-place compute transform 或立即释放旧 backing").
  // The object survives -- its identity, generation and freshly published
  // epoch stay live, so facets acquired against the new representation keep
  // resolving -- but the superseded backing is released at once instead of
  // being retained until the relevant command buffer completes. That released
  // byte count is the watermark reduction E005 measures, and it is the whole
  // difference from the default retain-until-completion behaviour.
  //
  // Distinct from consume() above, which retires the object entirely: these
  // are two different destructive operations and collapsing them would make a
  // transform's target facet stale the moment its own transform succeeded.
  bool consume_representation(uint64_t id, uint32_t generation, uint32_t expected_epoch,
                              const ConsumeProof& proof, uint64_t* released_bytes = nullptr,
                              std::string* error = nullptr);
  // E016 backpressure. 0 (the default) means unbounded; a non-zero budget makes
  // transform() refuse -- predictably, with an explicit error -- once an
  // allocation already has that many live representations, instead of letting
  // versions accumulate until the process thrashes.
  void set_max_in_flight_representations(uint32_t budget) { max_in_flight_representations_ = budget; }
  [[nodiscard]] uint32_t max_in_flight_representations() const { return max_in_flight_representations_; }
  // Drops one live representation version of `id` without retiring the
  // allocation, i.e. the producer observed that an older version's readers are
  // done. Never drops the last one: an Active allocation always has at least
  // its current representation.
  bool release_representation(uint64_t id, uint32_t generation, std::string* error = nullptr);
  [[nodiscard]] const std::unordered_map<uint64_t, Allocation>& allocations() const { return allocations_; }
  // Pointer-identity liveness check for a raw Allocation* obtained earlier
  // (e.g. via arena_allocate's reinterpret_cast out-param) and now of unknown
  // provenance to the caller. allocate()/retire()/consume() etc. never erase
  // an allocations_ entry -- only destroying the whole Arena invalidates a
  // pointer into this map -- so this exists to let a caller that still has
  // this Arena alive confirm `ptr` actually points at one of its own live
  // map entries *before* dereferencing it, rather than trusting an untrusted
  // pointer's own bytes. Never dereferences `ptr` itself; only compares its
  // address against `&kv.second` for entries this Arena owns.
  [[nodiscard]] bool is_live_allocation(const Allocation* ptr) const {
    for (const auto& kv : allocations_) {
      if (&kv.second == ptr) return true;
    }
    return false;
  }
  bool import_allocation(const RepresentationRef& ref, uint64_t size, ObjectState state,
                         const std::vector<uint8_t>& bytes, std::string* error = nullptr);
  [[nodiscard]] uint64_t id() const { return id_; }
  [[nodiscard]] uint64_t topology_epoch() const { return topology_epoch_; }
  // Arena-wide monotone counter of representation transitions, sibling to
  // topology_epoch(): that one ticks when the pointer-bearing topology changes,
  // this one ticks when any allocation's backing/facet interpretation does
  // (02 Sec.4.1). RepresentationEpochBuilder::seal() stamps it, the same way
  // GraphEpochBuilder::seal() stamps topology_epoch().
  [[nodiscard]] uint64_t representation_clock() const { return representation_clock_; }

 private:
  uint64_t id_;
  uint64_t next_id_{1};
  uint64_t topology_epoch_{};
  uint64_t representation_clock_{};
  uint32_t max_in_flight_representations_{};
  std::unordered_map<uint64_t, Allocation> allocations_;
};

// A CanonicalView per-usage generates AddressFacet/SampleFacet/StorageFacet/
// AttachmentFacet/TransferFacet (02-principles-and-semantics.md §3.3). It
// names an allocation's shape/format contract; it does not own the
// allocation -- FacetPool slots reference it by value. Facets must not be
// folded into one maximal ViewRecord, and active facet refs must not be
// mutated in place.
enum class FacetKind : uint32_t { Address, Sample, Storage, Attachment, Transfer };

// Format/dimension inputs that Metal Sample/Storage/Attachment facets need
// (06-backend-macos-metal.md §6.1–6.3). Extend only when a real path requires it.
enum class PixelFormat : uint32_t { RGBA8Unorm, R32Float, Depth32Float, R16Uint, R32Uint };
enum class ViewDimension : uint32_t { Texture2D, Texture2DArray };

// Both formats this milestone models are 4 bytes wide, but the two reach that
// width differently (4x8-bit vs. 1x32-bit), so callers that pack rows must ask
// rather than assume.
uint32_t bytes_per_texel(PixelFormat format);

// Sampler policy for SampleFacet only (06 §6.1). Kept off CanonicalView so the
// same view can be sampled under different filter/wrap without a new view.
enum class FilterMode : uint32_t { Nearest, Bilinear };
enum class WrapMode : uint32_t { Clamp, Repeat };

// Per-channel source selection (06 §6.1 lists swizzle among the inputs a facet
// compiles from). Part of the view contract rather than sampler policy: it
// changes what the shader reads, so two views differing only in swizzle are
// different contracts and must not share a cached facet.
enum class Swizzle : uint32_t { Red, Green, Blue, Alpha, Zero, One };

struct SwizzleChannels {
  Swizzle red{Swizzle::Red};
  Swizzle green{Swizzle::Green};
  Swizzle blue{Swizzle::Blue};
  Swizzle alpha{Swizzle::Alpha};

  [[nodiscard]] bool identity() const {
    return red == Swizzle::Red && green == Swizzle::Green && blue == Swizzle::Blue &&
           alpha == Swizzle::Alpha;
  }
};

struct CanonicalView {
  uint64_t allocation{};
  uint32_t allocation_generation{};
  PixelFormat format{PixelFormat::RGBA8Unorm};
  ViewDimension dimension{ViewDimension::Texture2D};
  uint32_t width{};
  uint32_t height{};
  uint32_t array_layers{1};
  uint32_t mip_levels{1};
  SwizzleChannels swizzle;

  // Half-and-clamp, the sizing rule every graphics API's mip chain uses.
  [[nodiscard]] uint32_t mip_width(uint32_t level) const;
  [[nodiscard]] uint32_t mip_height(uint32_t level) const;
  [[nodiscard]] uint32_t subresource_count() const { return array_layers * mip_levels; }
  // Byte layout of the linear allocation this view names: slice-major, then
  // ascending mip level, each level tightly packed at
  // mip_width(level) * bytes_per_texel rows. This is the single contract the
  // Metal upload path and the reference sampling oracle both encode against --
  // if they disagreed on it, an image-correctness comparison between them
  // would be meaningless.
  struct SubresourceIndex {
    uint32_t array_layer{};
    uint32_t mip_level{};
  };
  [[nodiscard]] uint64_t bytes_per_row(uint32_t level) const;
  [[nodiscard]] uint64_t subresource_byte_size(uint32_t level) const;
  [[nodiscard]] uint64_t subresource_byte_offset(SubresourceIndex index) const;
  [[nodiscard]] uint64_t byte_size() const;
  // Rejects a view whose extents/counts cannot describe a real image (zero
  // extent, zero layers/levels, a mip chain longer than the extent supports,
  // or array_layers > 1 without the array dimension). Shape validity is a
  // property of the view contract, so every backend gets the same answer.
  bool valid(std::string* error = nullptr) const;
};

// index+generation capability reference into a FacetPool slot -- never a
// raw backend texture/sampler pointer (03-system-architecture.md Sec.10).
struct FacetRef {
  uint32_t index{};
  uint32_t generation{};
};

// Sample source + attachment target for one raster pass. Adjacent FacetRef
// parameters are otherwise interchangeable at every draw call site.
struct RasterFacetPair {
  FacetRef source{};
  FacetRef target{};
};

// F4's depth comparison is intentionally a core task property: Reference and
// Metal must make the same per-fragment decision, while their state objects
// remain backend-private.
enum class DepthCompareOp : uint32_t {
  Never,
  Less,
  Equal,
  LessEqual,
  Greater,
  NotEqual,
  GreaterEqual,
  Always,
};

// F2 (ADR-043 Decision #3, ADR-046): discriminates the shape a TaskRecord
// carries. Defaults to Compute so every pre-F2 caller's x/y/z dispatch
// meaning is unchanged.
enum class TaskKind : uint32_t { Compute, Raster };

// F2: the only topology F2 supports. Indexed/strip/fan draws are F5+; a
// raster TaskRecord requesting one is rejected at compile() time rather
// than silently reinterpreted (START.md Sec.4 invariant 10).
enum class Topology : uint32_t { TriangleList };

struct FacetSlot {
  uint32_t generation{1};
  bool active{};
  FacetKind kind{FacetKind::Address};
  CanonicalView view;
  uint32_t representation_epoch{};
  // Outstanding begin_gpu_use() calls not yet matched by end_gpu_use().
  // Non-zero means a command buffer may still dereference this slot, so its
  // index must not be handed to a new acquire() (06 §6.4, §11).
  uint32_t in_flight{};
  // Generation the outstanding uses were begun under. Retirement bumps
  // `generation` immediately, so end_gpu_use() matches against this instead.
  uint32_t in_flight_generation{};
};

// Why a lookup() failed, so callers can act on the reason instead of parsing
// a diagnostic string (04-public-c-abi.md §4: every failure programmatically
// determinable). Retired/GenerationMismatch/EpochStale are the three shapes
// of a stale capability token.
enum class FacetStatus : uint32_t {
  Ok,
  UnknownIndex,
  Retired,
  GenerationMismatch,
  EpochStale,
  AllocationLost,
};

const char* to_string(FacetStatus status);

// Independent index+generation pool for facets. Sibling to Arena's
// allocation table, not a reuse of its id space
// (02-principles-and-semantics.md Sec.3.3: "不能把所有 facet 拼成一个最大
// ViewRecord，也不能在活动引用中原地修改") -- mirrors how GraphEpoch and
// PointerGraph already exist as separate parallel epoch concepts rather
// than being unified into one structure.
class FacetPool {
 public:
  // Snapshots view.allocation's current representation_epoch (read from
  // `arena`) into the new slot. Fails if that allocation is not active in
  // `arena`.
  bool acquire(const Arena& arena, const CanonicalView& view, FacetKind kind, FacetRef* out,
               std::string* error = nullptr);
  // A slot is stale once its backing allocation's representation_epoch in
  // `arena` no longer matches the epoch snapshotted at acquire() time --
  // this is the "facet generation vs epoch = stale token" check from
  // 02-principles-and-semantics.md Sec.10. Returns nullptr for a stale or
  // retired/generation-mismatched ref rather than the slot's last-known
  // contents.
  const FacetSlot* lookup(const Arena& arena, FacetRef ref, FacetStatus* status = nullptr) const;
  // Retires a slot so its index becomes eligible for reuse by a future
  // acquire(). Only a generation-matched, still-active ref may be retired.
  // Reuse is additionally gated on in_flight reaching zero.
  bool retire(FacetRef ref, std::string* error = nullptr);
  // Retires every active slot whose snapshotted representation_epoch no longer
  // matches the live allocation in `arena` (06 §6.4: slots become reusable only
  // after the referenced RepresentationEpoch is no longer current).
  size_t retire_stale(const Arena& arena);
  // Brackets a command buffer's use of a facet. Between begin and end the
  // slot's index stays out of the free list even after retirement, which is
  // what "默认保留旧 facet/backing 至相关 command buffer 完成" (06 §11) buys:
  // a retired-but-in-flight token stops resolving immediately, yet its backend
  // resource is not handed to an unrelated facet underneath the GPU.
  bool begin_gpu_use(const Arena& arena, FacetRef ref, std::string* error = nullptr);
  bool end_gpu_use(FacetRef ref, std::string* error = nullptr);
  [[nodiscard]] uint32_t in_flight(FacetRef ref) const;
  [[nodiscard]] uint32_t slot_count() const { return static_cast<uint32_t>(slots_.size()); }
  // 06 Sec.6.4 asks the checked profile to verify facet generation *in the
  // shader*. A shader cannot call lookup(), so the checked profile uploads this
  // table instead: entry[index] is the generation that index currently
  // resolves at, or 0 for a retired slot, so a kernel holding a FacetRef can
  // reject a stale token itself before dereferencing it. Snapshot semantics --
  // it is only as fresh as the submission that uploaded it, which is why the
  // host-side lookup() check stays authoritative.
  void snapshot_generations(std::vector<uint32_t>* out) const;
  // Host-side mirror of the comparison the checked-profile shader performs, so
  // a test can state the expected in-shader verdict without a GPU. Deliberately
  // weaker than lookup(): it sees only what the uploaded table encodes, and so
  // cannot observe an epoch that went stale after the snapshot.
  [[nodiscard]] bool generation_valid(FacetRef ref) const;
  // 02 §4.2 / 06 §11: a live token or an outstanding GPU use of this
  // allocation at `epoch` is an external reference ConsumeInput must see.
  // Retired slots still count while in_flight > 0, because the command buffer
  // that called begin_gpu_use() may still dereference the old backing.
  [[nodiscard]] bool references(const RepresentationRef& ref) const;

 private:
  void retire_slot(uint32_t index);

  std::vector<FacetSlot> slots_;
  std::vector<uint32_t> free_list_;
};

// One frozen interpretation of a set of allocations' backing/metadata/facets
// (02 Sec.4.1). Sibling to GraphEpoch, not a generalization of it: GraphEpoch
// freezes which pointers a topology may dereference, this freezes how the bytes
// behind those allocations are to be read. A representation transform produces
// a new one rather than editing this one, which is 02 Sec.8's "transform 不是
// 纯 barrier" stated as a data structure.
struct RepresentationRef {
  uint64_t allocation{};
  uint32_t allocation_generation{};
  uint32_t representation_epoch{};
};

class RepresentationEpoch {
 public:
  [[nodiscard]] uint64_t value() const { return value_; }
  [[nodiscard]] bool sealed() const { return sealed_; }
  [[nodiscard]] const std::vector<RepresentationRef>& representations() const { return representations_; }
  [[nodiscard]] const std::vector<FacetRef>& facets() const { return facets_; }
  [[nodiscard]] bool contains(RepresentationRef reference) const;
  [[nodiscard]] bool contains(FacetRef ref) const;
  // 02 Sec.10's "facet generation vs epoch = stale token" at epoch granularity:
  // true once any frozen representation no longer matches `arena`, meaning
  // every facet this epoch authorized has to be rebuilt rather than reused.
  [[nodiscard]] bool stale(const Arena& arena) const;

 private:
  friend class RepresentationEpochBuilder;
  uint64_t value_{1};
  std::vector<RepresentationRef> representations_;
  std::vector<FacetRef> facets_;
  bool sealed_{};
};

// Mirrors GraphEpochBuilder's shape (add references, seal once, stamp the
// arena's clock) so the two epoch kinds stay recognizably the same mechanism
// applied to different state.
class RepresentationEpochBuilder {
 public:
  explicit RepresentationEpochBuilder(uint64_t next_epoch = 1) : next_epoch_(next_epoch) {}
  RepresentationEpochBuilder(const Arena* arena, uint64_t next_epoch = 1)
      : next_epoch_(next_epoch), arena_(arena) {}
  bool add_representation(RepresentationRef reference, std::string* error = nullptr);
  // Snapshots the allocation's *current* representation_epoch, so a caller
  // cannot freeze a version the arena is not actually at.
  bool add_representation(const Arena& arena, uint64_t allocation, uint32_t generation,
                          std::string* error = nullptr);
  // Adds the facet and the representation it was acquired against together --
  // a facet whose slot is already stale is refused, since freezing it would
  // authorize a token that is dead on arrival.
  bool add_facet(const Arena& arena, const FacetPool& pool, FacetRef ref, std::string* error = nullptr);
  bool seal(RepresentationEpoch* out, std::string* error = nullptr);
  [[nodiscard]] bool sealed() const { return sealed_; }

 private:
  uint64_t next_epoch_;
  const Arena* arena_{};
  std::vector<RepresentationRef> representations_;
  std::vector<FacetRef> facets_;
  bool sealed_{};
};

struct TaskRecord {
  uint32_t node_index{};
  uint32_t node_generation{1};
  uint64_t root_allocation{};
  uint32_t root_generation{1};
  uint32_t x{1}, y{1}, z{1};
  uint32_t flags{};
  uint32_t contract_index{};
  uint32_t payload_size{};
  uint64_t payload_or_offset{};
  // F2 (ADR-046): raster is a shape of TaskRecord, not a parallel API.
  // Everything below defaults to a no-op for a Compute task.
  TaskKind kind{TaskKind::Compute};
  Topology topology{Topology::TriangleList};
  RasterFacetPair raster_facets{};
  // F4: an Attachment-kind facet over a Depth32Float CanonicalView. A zero
  // ref means this remains the F3 depth-free raster task.
  FacetRef depth_attachment_ref{};
  bool depth_test_enable{};
  bool depth_write_enable{};
  DepthCompareOp depth_compare_op{DepthCompareOp::Always};
  // Address-kind facet whose backing bytes are a tightly packed
  // RasterVertex array; vertex count is derived from its byte length
  // (Allocation::bytes.size() / sizeof(RasterVertex)), not stored here.
  FacetRef vertex_buffer_ref{};
  FacetRef index_buffer_ref{};
  // >0 selects F5 indexed TriangleList draw. index_buffer_ref must name an
  // Address facet over R16Uint or R32Uint; that format supplies the element
  // type without extending this frozen task layout.
  uint32_t index_count{};
  FilterMode raster_filter{FilterMode::Bilinear};
  WrapMode raster_wrap{WrapMode::Clamp};
  std::array<float, 4> raster_tint{1.0f, 1.0f, 1.0f, 1.0f};
};

struct PointerRef {
  uint64_t allocation{};
  uint32_t generation{};
};

class GraphEpoch {
 public:
  [[nodiscard]] uint64_t value() const { return value_; }
  [[nodiscard]] bool sealed() const { return sealed_; }
  [[nodiscard]] const std::vector<PointerRef>& references() const { return references_; }
  [[nodiscard]] bool contains(PointerRef reference) const;

 private:
  friend class GraphEpochBuilder;
  uint64_t value_{1};
  std::vector<PointerRef> references_;
  bool sealed_{};
};

class GraphEpochBuilder {
 public:
  explicit GraphEpochBuilder(uint64_t next_epoch = 1) : next_epoch_(next_epoch) {}
  GraphEpochBuilder(const Arena* arena, uint64_t next_epoch = 1) : arena_(arena), next_epoch_(next_epoch) {}
  bool add_reference(PointerRef reference, std::string* error = nullptr);
  bool add_reference(const Arena& arena, PointerRef reference, std::string* error = nullptr);
  bool seal(GraphEpoch* out, std::string* error = nullptr);
  [[nodiscard]] bool sealed() const { return sealed_; }

 private:
  uint64_t next_epoch_;
  const Arena* arena_{};
  std::vector<PointerRef> references_;
  bool sealed_{};
};

// One declared hop of a typed pointer graph (E002): "the ref value at
// (from, field_offset) may be dereferenced to reach to". Sibling to
// GraphEpoch/GraphEpochBuilder, not a generalization of them -- pointer
// graphs can legitimately be cyclic (e.g. linked lists), unlike the
// acyclic-only EffectGraph.
struct Edge {
  PointerRef from{};
  uint64_t field_offset{};
  PointerRef to{};
};

class PointerGraph {
 public:
  [[nodiscard]] const std::vector<Edge>& edges() const { return edges_; }
  [[nodiscard]] bool reachable(PointerRef from, uint64_t field_offset, PointerRef to) const;
  // Cycle-safe multi-hop reachability over any field offset. Cycles are not
  // rejected -- they are valid graph shapes here, so this only guards
  // against infinite loops, unlike EffectGraph::valid()'s cycle rejection.
  struct ReachQuery {
    PointerRef from{};
    PointerRef to{};
  };
  [[nodiscard]] bool reachable(ReachQuery query) const;

 private:
  friend class PointerGraphBuilder;
  std::vector<Edge> edges_;
};

class PointerGraphBuilder {
 public:
  bool add_edge(PointerRef from, uint64_t field_offset, PointerRef to, std::string* error = nullptr);
  bool build(PointerGraph* out, std::string* error = nullptr);
  [[nodiscard]] bool built() const { return built_; }

 private:
  std::vector<Edge> edges_;
  bool built_{};
};

enum class PublicationState : uint32_t { Empty, Writing, Published, Consumed };

struct PublicationSlot {
  TaskRecord task{};
  std::atomic<PublicationState> state{PublicationState::Empty};
};

enum class EffectEdgeKind { Explicit, InferredConflict, Timeline, Publication };
struct EffectEdge { uint32_t before{}, after{}; EffectEdgeKind kind{EffectEdgeKind::Explicit}; uint64_t timeline_value{}; };

class EffectGraph {
 public:
  bool add_edge(uint32_t before, uint32_t after, std::string* error = nullptr);
  bool add_edge(uint32_t before, uint32_t after, EffectEdgeKind kind, uint64_t timeline_value = 0,
                std::string* error = nullptr);
  bool add_timeline_edge(uint32_t before, uint32_t after, uint64_t required_value,
                         uint64_t signaled_value, std::string* error = nullptr);
  static bool conflicts(const ir::Effect& before, const ir::Effect& after);
  bool validate_happens_before(const std::vector<std::vector<ir::Effect>>& effects,
                               std::string* error = nullptr) const;
  [[nodiscard]] bool valid() const;
  [[nodiscard]] const std::vector<EffectEdge>& edges() const { return edges_; }
 private:
  std::vector<EffectEdge> edges_;
};

// The 3 in-scope Effect DAG shapes a backend can lower without cross-queue
// or representation-transition machinery (ADR-027). Unsupported covers
// every graph EffectGraphBuilder can seal but this milestone's classifier
// does not recognize a lowering strategy for -- callers must report it as
// an honest Unsupported/Deferred result, never guess a fence placement.
enum class EffectGraphShape { LinearChain, IndependentBranches, ForkJoin, Unsupported };

// Sibling to TaskGraphBuilder, not a generalization of it: TaskGraphBuilder
// bakes in Task-specific invariants (quota, PublicationRing integration)
// that a general-purpose Effect DAG builder must not inherit. Builds an
// EffectGraph from generic per-node ir::Effect lists using the exact same
// conflicts()/validate_happens_before() algorithm TaskGraphBuilder::seal
// already uses, over nodes that represent arbitrary lowering units (e.g.
// Metal encoder passes) rather than TaskRecords.
class EffectGraphBuilder {
 public:
  uint32_t add_node(std::vector<ir::Effect> effects, std::string* error = nullptr);
  bool add_dependency(uint32_t before, uint32_t after, std::string* error = nullptr);
  bool seal(EffectGraph* out, uint32_t* node_count, std::string* error = nullptr);
  [[nodiscard]] bool sealed() const { return sealed_; }

 private:
  std::vector<std::vector<ir::Effect>> effects_;
  std::vector<std::pair<uint32_t, uint32_t>> dependencies_;
  bool sealed_{};
};

// Classifies a sealed EffectGraph's shape from its edges' in/out-degree
// structure, over `node_count` nodes (0 and 1 node graphs are LinearChain
// trivially). Pure structural analysis, no I/O: safe to call from both
// compile()-time lowering-strategy selection and tests.
EffectGraphShape classify_effect_graph_shape(const EffectGraph& graph, uint32_t node_count);

// Deterministic topological order over Explicit+InferredConflict edges
// (same Kahn's-algorithm, lowest-ready-index-first shape as
// TaskGraph::deterministic_order, generalized to an arbitrary sealed
// EffectGraph rather than a TaskGraph). For IndependentBranches every node
// has in-degree 0, so this returns plain ascending node-index order.
bool effect_graph_deterministic_order(const EffectGraph& graph, uint32_t node_count,
                                     std::vector<uint32_t>* out, std::string* error = nullptr);

// Identifies the fan-out source, fan-in join, and the remaining independent
// middle nodes of a ForkJoin-shaped graph. Only meaningful when
// classify_effect_graph_shape(graph, node_count) == ForkJoin -- callers
// must check the shape first.
struct EffectGraphForkJoin {
  uint32_t source{};
  uint32_t join{};
  std::vector<uint32_t> middle;
};
EffectGraphForkJoin describe_fork_join(const EffectGraph& graph, uint32_t node_count);

class PublicationRing {
 public:
  explicit PublicationRing(uint32_t capacity);
  int32_t reserve();
  bool write(uint32_t slot, const TaskRecord& task, std::string* error = nullptr);
  bool publish(uint32_t slot, std::string* error = nullptr);
  bool acquire(uint32_t slot, TaskRecord* out, std::string* error = nullptr) const;
  bool consume(uint32_t slot, std::string* error = nullptr);
  bool abort(uint32_t slot, std::string* error = nullptr);
  bool publish_task(const TaskRecord& task, uint32_t* slot = nullptr, std::string* error = nullptr);
  uint32_t capacity() const { return static_cast<uint32_t>(slots_.size()); }

 private:
  std::vector<PublicationSlot> slots_;
  std::atomic<uint32_t> next_slot_{};
};

class TaskGraph {
 public:
  [[nodiscard]] const std::vector<TaskRecord>& tasks() const { return tasks_; }
  [[nodiscard]] const std::vector<std::pair<uint32_t, uint32_t>>& dependencies() const { return dependencies_; }
  [[nodiscard]] const class EffectGraph& effect_graph() const { return effect_graph_; }
  [[nodiscard]] bool sealed() const { return sealed_; }
  [[nodiscard]] bool published() const { return published_; }
  bool publish(std::string* error = nullptr);
  bool validate_execution(std::string* error = nullptr) const;
  // Deterministic topological order over Explicit+InferredConflict edges
  // (Kahn's algorithm, lowest-ready-index-first tie-breaking). Every backend
  // (reference, Metal, Vulkan) must use this exact ordering when publishing
  // tasks so their outputs are byte/order-comparable against each other.
  bool deterministic_order(std::vector<uint32_t>* out, std::string* error = nullptr) const;

 private:
  friend class TaskGraphBuilder;
  std::vector<TaskRecord> tasks_;
  std::vector<std::pair<uint32_t, uint32_t>> dependencies_;
  class EffectGraph effect_graph_;
  bool sealed_{true};
  bool published_{};
};

class TaskGraphBuilder {
 public:
  bool append(const TaskRecord& task, std::string* error = nullptr);
  bool add_dependency(uint32_t before, uint32_t after, std::string* error = nullptr);
  bool add_effect(uint32_t task, const ir::Effect& effect, std::string* error = nullptr);
  bool set_effects(uint32_t task, const std::vector<ir::Effect>& effects, std::string* error = nullptr);
  bool set_quota(uint32_t max_tasks, uint64_t max_payload_bytes, std::string* error = nullptr);
  bool append_published(PublicationRing& ring, uint32_t slot, std::string* error = nullptr);
  bool seal(TaskGraph* out, std::string* error = nullptr);
  [[nodiscard]] bool sealed() const { return sealed_; }

 private:
  std::vector<TaskRecord> tasks_;
  std::vector<std::pair<uint32_t, uint32_t>> dependencies_;
  std::vector<std::vector<ir::Effect>> effects_;
  uint32_t max_tasks_{UINT32_MAX};
  uint64_t max_payload_bytes_{UINT64_MAX};
  uint64_t payload_bytes_{};
  bool sealed_{};
};

// Wait-then-signal pair for one submission. Adjacent uint64_t wait/signal
// parameters are otherwise interchangeable at every dispatch call site.
struct TimelineGate {
  uint64_t wait{};
  uint64_t signal{};
};

class Timeline {
 public:
  explicit Timeline(uint64_t value = 0) : value_(value) {}
  bool signal(uint64_t value, std::string* error = nullptr);
  [[nodiscard]] bool wait(uint64_t value) const { return value <= value_; }
  bool validate_wait(uint64_t value, std::string* error = nullptr) const;
  [[nodiscard]] uint64_t value() const { return value_; }

 private:
  uint64_t value_{};
};

struct Certificate {
  std::vector<ir::Effect> ranges;
  [[nodiscard]] bool covers(const ir::Effect& effect) const;
};

struct WitnessEntry {
  ir::Effect effect;
  uint32_t instruction_index{};
};

struct WitnessDiff {
  std::vector<ir::Effect> missing;
  std::vector<ir::Effect> unused;
};

class AccessWitness {
 public:
  void record(ir::Effect effect, uint32_t instruction_index);
  [[nodiscard]] const std::vector<WitnessEntry>& entries() const { return entries_; }
  [[nodiscard]] WitnessDiff diff(const Certificate& certificate) const;

 private:
  std::vector<WitnessEntry> entries_;
};

// The five E004 adapter memory-visibility policies (09-experiment-catalog.md).
// CertifiedPinned/Universe/DiscoverThenLease have real implementations on
// reference and Metal (see build_access_certificate); SoftwarePaged and
// FaultManaged are pre-sanctioned Unsupported results (docs/START.md §4) —
// build_access_certificate refuses to fabricate a certificate for either.
enum class AccessCertificateMode { CertifiedPinned, Universe, DiscoverThenLease, SoftwarePaged, FaultManaged };

struct AccessCertificate {
  AccessCertificateMode mode{AccessCertificateMode::CertifiedPinned};
  GraphEpoch epoch;
  uint64_t discovery_host_ns{};
  uint64_t discovery_gpu_ns{};
  uint64_t scanned_bytes{};
  uint64_t result_bytes{};
  uint64_t working_set_bytes{};
};

// Builds an AccessCertificate for `mode` over `arena`. `touched` is the set
// of allocations the compiled module statically references, used as the
// CertifiedPinned working set; Universe and DiscoverThenLease instead scan
// every Active allocation in `arena` (DiscoverThenLease additionally times
// the scan as a real host-side rescan and reports it via discovery_host_ns).
// Returns false for SoftwarePaged/FaultManaged — callers must classify those
// modes Unsupported themselves rather than treat a false return as a bug.
bool build_access_certificate(const Arena& arena, AccessCertificateMode mode,
                              const std::vector<PointerRef>& touched,
                              AccessCertificate* out, std::string* error = nullptr);

// TASK-D2 / ADR-036: seed-topology discovery (02 §7.2). Distinct from
// build_access_certificate's B-era DiscoverThenLease, which still scans
// every Active allocation. This walk reads 12-byte PointerRef slots
// packed the same way load_ref does ({u64 allocation, u32 generation},
// not sizeof(PointerRef) which may pad) and follows only refs that are
// well-formed and resolve to Active allocations. Result = seeds +
// reachable, which can be strictly smaller than Universe on the same
// Arena. This is a semantic reachable set / proxy, not an OS page
// migration (06 §10).
//
// topology_epoch is frozen at the start of the walk; a change mid-walk
// refuses rather than certifying a mixed-epoch set.
struct DiscoveryResult {
  std::vector<PointerRef> reachable;
  uint64_t frozen_topology_epoch{};
  uint64_t scanned_bytes{};
  uint64_t result_bytes{};
  uint64_t discovery_host_ns{};
};

// `after_visit` is invoked after each newly reached allocation is
// recorded and before the next hop. Production callers leave it empty.
// Tests use it to bump topology_epoch mid-walk so the freeze check in
// 02 §7.2 is observable.
bool discover_reachable(const Arena& arena, const std::vector<PointerRef>& seeds,
                        DiscoveryResult* out, std::string* error = nullptr,
                        const std::function<void()>& after_visit = {});

// Seals a DiscoverThenLease AccessCertificate over `discovery.reachable`
// only -- not the B-era full-arena scan. Callers that still want that
// scan must keep using build_access_certificate.
bool build_discovered_certificate(const Arena& arena, const DiscoveryResult& discovery,
                                  AccessCertificate* out, std::string* error = nullptr);

// 02 §10: certificate (proof) covers witness (observation). An extra
// forged allocation in `witness` is a refuse, not a silent enlarge.
bool certificate_covers_discovery_witness(const AccessCertificate& certificate,
                                          const std::vector<PointerRef>& witness,
                                          std::string* error = nullptr);

// Conservative composition of effect certificates (12 §8 open question 3 /
// Task children and indirect calls). Union of ranges is a sound
// over-approximation: the result covers every input range. It does not try
// to stay tight; overlapping ranges are kept rather than merged into a
// smaller cover. Empty `parts` is refused -- there is no implicit Universe.
bool compose_certificates(const std::vector<Certificate>& parts, Certificate* out,
                          std::string* error = nullptr);

// Conservative composition of AccessCertificates: union of GraphEpoch
// references on one Arena. Mixed topology epochs are refused. If the union
// names every Active allocation while some input did not, `exploded` is
// true -- that is the honest "composition can become Universe" outcome,
// not a silent success. `exploded` may be null.
bool compose_access_certificates(const Arena& arena, const std::vector<AccessCertificate>& parts,
                                 AccessCertificate* out, bool* exploded = nullptr,
                                 std::string* error = nullptr);

// Phase D shared contracts (ADR-035). Independent of AccessCertificate
// (sound over-approximation) and of Allocation eviction: this is the
// residency hold and overflow bookkeeping D2/D3/D5 fill in. Default
// construction is "not applied" so existing callers stay unchanged.

// Unset (has_limit == false) is distinct from a set limit of 0. A set
// limit that requested bytes exceed is a predictable refusal, not a clamp.
struct WorkingSetBudget {
  bool has_limit{};
  uint64_t byte_limit{};

  static WorkingSetBudget unlimited() { return {}; }
  static WorkingSetBudget limited(uint64_t bytes) {
    WorkingSetBudget budget;
    budget.has_limit = true;
    budget.byte_limit = bytes;
    return budget;
  }
  bool allows(uint64_t bytes, std::string* error = nullptr) const;
};

// This submission's residency hold. A lease cannot name an allocation
// absent from the caller-supplied proven set (certificate or discovery
// witness). `complete` is the caller's claim that the named set is the
// whole hold -- it is not inferred from Arena state.
struct WorkingSetLease {
  std::vector<PointerRef> allocations;
  uint64_t byte_limit{};
  bool complete{};

  [[nodiscard]] bool covers(PointerRef ref) const;
  bool add(PointerRef ref, const std::vector<PointerRef>& proven, std::string* error = nullptr);
  bool valid(const std::vector<PointerRef>& proven, std::string* error = nullptr) const;
};

// Work this submit could not fit. Rejected cannot be reported as
// continued. Deferred leftover requires a non-zero continuation token
// for the next submit -- not a silent quota increase (ADR-010's
// set_quota is build-time only).
enum class EnvelopeOverflowDisposition { None, Rejected, Deferred };

struct EnvelopeOverflow {
  uint32_t overflow_task_count{};
  EnvelopeOverflowDisposition disposition{EnvelopeOverflowDisposition::None};
  uint64_t continuation_token{};

  bool valid(std::string* error = nullptr) const;
  // True only for a valid Deferred leftover. A Rejected record never
  // answers true, even if a caller stuffed a token in.
  [[nodiscard]] bool continued() const;
};

// Per-device leftover buffer for E017 / ADR-039. Tokens are minted here
// so a second submit can present ExecutionPlan::pending_overflow without
// turning leftover into an implicit global queue. Token 0 is never issued.
class EnvelopeContinuationTable {
 public:
  // Stores leftover deterministic-order indices and returns a non-zero
  // token. Empty leftover is not a Deferred record -- mint returns 0 and
  // stores nothing.
  uint64_t mint(std::vector<uint32_t> leftover_order);
  [[nodiscard]] bool contains(uint64_t token) const;
  bool lookup(uint64_t token, std::vector<uint32_t>* leftover, std::string* error = nullptr) const;
  // Removes the leftover so a later submit cannot drain it twice.
  bool take(uint64_t token, std::vector<uint32_t>* leftover, std::string* error = nullptr);

 private:
  uint64_t next_token_{1};
  std::unordered_map<uint64_t, std::vector<uint32_t>> leftover_;
};

struct FaultRecord {
  uint32_t instruction_index{};
  ir::Effect effect{};
  std::string code;
  std::string message;
  uint32_t task_index{};
};

struct ExecutionResult {
  bool ok{};
  PoisonState poison{PoisonState::Valid};
  std::string message;
  std::vector<ir::Effect> trace;
  std::vector<ir::Effect> missing_effects;
  FaultRecord fault;
  AccessWitness witness;
  bool outputs_valid{true};

  // v1.2 (ADR-045): serializes the full execution outcome -- ok/poison/
  // message/fault/witness entries/missing_effects/outputs_valid -- so the
  // public C ABI's getSubmissionExecutionResult can surface it without a
  // second ABI-fragile struct mirror, matching LoweringReport::canonical_json().
  [[nodiscard]] std::string canonical_json() const;
};

bool validate_certificate(const Certificate& certificate,
                          const std::vector<ir::Effect>& effects,
                          std::string* error = nullptr);

// ---- v1.1 / ADR-044 (F1): minimal C++ backing for the public C ABI's
// Device/AddressDomain/CodeObject/Node/ExecutionEnvelope handles. Each type
// here is the smallest object needed to reach the existing, already
// hardware-verified Arena/TaskGraph/DeviceHal machinery from a thin C
// wrapper -- not a redesign of any of it. Disclosed v1 narrowings are noted
// per class; see ADR-044 for the full list. ----

// One address domain per Device for v1.1 (ADR-044 disclosed narrowing);
// multiple domains (e.g. a distinct host-visible staging domain) are
// deferred past F1.
struct AddressDomain {
  uint32_t kind{};
};

// Owns the raw bytes handed to loadCodeObject plus a format tag. No caching
// or precompilation for v1.1 -- submit() still parses fresh via
// ir::parse_module every time, exactly matching pre-F1 behavior; this only
// gives the bytes a handle and a lifetime (ADR-044 disclosed narrowing).
struct CodeObject {
  std::vector<uint8_t> bytes;
  std::string format_tag;
};

// A CodeObject has exactly one runnable entry for v1.1, but the
// index+generation pair is what the public VgNodeRef/VgTaskRecord.node
// fields carry, mirroring VgFacetRef's staleness-checked-token model rather
// than a bare caller-picked integer.
struct NodeEntry {
  std::string entry_name;
  uint32_t generation{1};
  bool live{true};
};

class NodeTable {
 public:
  struct Ref {
    uint32_t index{};
    uint32_t generation{};
  };

  Ref create(const std::string& entry_name);
  bool destroy(Ref ref);
  [[nodiscard]] NodeEntry* lookup(Ref ref);
  [[nodiscard]] const NodeEntry* lookup(Ref ref) const;

 private:
  std::vector<NodeEntry> entries_;
};

// Combines what 04-public-c-abi.md Sec.17 calls the envelope's
// "authorization + certificate + epoch + quota + timeline": which Node
// classes may run, an access certificate mode, a per-submit task quota and
// the timeline wait/signal values one submit() call authorizes. ADR-044
// disclosed narrowing: certificate_touched is a whole-allocation PointerRef
// set derived from the caller's VgAccessRange array (offset/size/access_mask
// are recorded on the C-side VgAccessRange for a future range-granular
// certificate but not yet enforced at that granularity here); one timeline
// per device, so timeline_wait/timeline_signal are the only ones submit()
// honors.
class ExecutionEnvelope {
 public:
  std::vector<uint32_t> allowed_node_classes;
  std::vector<PointerRef> certificate_touched;
  bool has_certificate_mode{};
  AccessCertificateMode certificate_mode{AccessCertificateMode::CertifiedPinned};
  bool has_task_quota{};
  uint32_t task_quota{};
  uint64_t timeline_wait{};
  uint64_t timeline_signal{};

  // Splices this envelope's fields into `plan` ahead of compile()/submit().
  // Defined in src/api/vg_api_execution.cpp, not core.cpp: that is the one
  // place core-layer state legitimately reaches into backends::ExecutionPlan,
  // and keeping the definition there avoids giving vg_core a backends/
  // header dependency (core.h only forward-declares hal::ExecutionPlan).
  void apply_to(hal::ExecutionPlan& plan) const;
};

}  // namespace vg::core

#endif
