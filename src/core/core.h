#ifndef VG_CORE_CORE_H_
#define VG_CORE_CORE_H_

#include "ir/ir.h"

#include <cstdint>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

namespace vg::core {

enum class ObjectState { Active, Retired };
enum class PoisonState { Valid, PartiallyProduced, Poisoned };

struct Allocation {
  uint64_t id{};
  uint32_t generation{1};
  uint64_t size{};
  ObjectState state{ObjectState::Active};
  uint32_t representation_epoch{0};
  uint32_t in_flight{};
  std::vector<uint8_t> bytes;
};

class Arena {
 public:
  explicit Arena(uint64_t id = 1) : id_(id) {}
  Allocation& allocate(uint64_t size);
  bool retire(uint64_t id, uint32_t generation);
  Allocation* lookup(uint64_t id, uint32_t generation);
  const Allocation* lookup(uint64_t id, uint32_t generation) const;
  Allocation* lookup(uint64_t id, uint32_t generation, uint32_t representation_epoch);
  const Allocation* lookup(uint64_t id, uint32_t generation, uint32_t representation_epoch) const;
  bool acquire(uint64_t id, uint32_t generation);
  bool release(uint64_t id, uint32_t generation);
  bool transform(uint64_t id, uint32_t generation, uint32_t* new_epoch, std::string* error = nullptr);
  bool transform(uint64_t id, uint32_t generation, uint32_t expected_epoch, uint32_t* new_epoch, std::string* error = nullptr);
  bool consume(uint64_t id, uint32_t generation, uint32_t expected_epoch, std::string* error = nullptr);
  const std::unordered_map<uint64_t, Allocation>& allocations() const { return allocations_; }
  bool import_allocation(uint64_t id, uint32_t generation, uint64_t size, uint32_t representation_epoch,
                         ObjectState state, const std::vector<uint8_t>& bytes, std::string* error = nullptr);
  uint64_t id() const { return id_; }
  uint64_t topology_epoch() const { return topology_epoch_; }

 private:
  uint64_t id_;
  uint64_t next_id_{1};
  uint64_t topology_epoch_{};
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
enum class PixelFormat : uint32_t { RGBA8Unorm, R32Float };
enum class ViewDimension : uint32_t { Texture2D, Texture2DArray };

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

  bool identity() const {
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
};

// index+generation capability reference into a FacetPool slot -- never a
// raw backend texture/sampler pointer (03-system-architecture.md Sec.10).
struct FacetRef {
  uint32_t index{};
  uint32_t generation{};
};

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
  uint32_t in_flight(FacetRef ref) const;

 private:
  void retire_slot(uint32_t index);

  std::vector<FacetSlot> slots_;
  std::vector<uint32_t> free_list_;
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
};

struct PointerRef {
  uint64_t allocation{};
  uint32_t generation{};
};

class GraphEpoch {
 public:
  uint64_t value() const { return value_; }
  bool sealed() const { return sealed_; }
  const std::vector<PointerRef>& references() const { return references_; }
  bool contains(PointerRef reference) const;

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
  bool sealed() const { return sealed_; }

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
  const std::vector<Edge>& edges() const { return edges_; }
  bool reachable(PointerRef from, uint64_t field_offset, PointerRef to) const;
  // Cycle-safe multi-hop reachability over any field offset. Cycles are not
  // rejected -- they are valid graph shapes here, so this only guards
  // against infinite loops, unlike EffectGraph::valid()'s cycle rejection.
  bool reachable(PointerRef from, PointerRef to) const;

 private:
  friend class PointerGraphBuilder;
  std::vector<Edge> edges_;
};

class PointerGraphBuilder {
 public:
  bool add_edge(PointerRef from, uint64_t field_offset, PointerRef to, std::string* error = nullptr);
  bool build(PointerGraph* out, std::string* error = nullptr);
  bool built() const { return built_; }

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
  bool valid() const;
  const std::vector<EffectEdge>& edges() const { return edges_; }
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
  bool sealed() const { return sealed_; }

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
  const std::vector<TaskRecord>& tasks() const { return tasks_; }
  const std::vector<std::pair<uint32_t, uint32_t>>& dependencies() const { return dependencies_; }
  const class EffectGraph& effect_graph() const { return effect_graph_; }
  bool sealed() const { return sealed_; }
  bool published() const { return published_; }
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
  bool set_effects(uint32_t task, std::vector<ir::Effect> effects, std::string* error = nullptr);
  bool set_quota(uint32_t max_tasks, uint64_t max_payload_bytes, std::string* error = nullptr);
  bool append_published(PublicationRing& ring, uint32_t slot, std::string* error = nullptr);
  bool seal(TaskGraph* out, std::string* error = nullptr);
  bool sealed() const { return sealed_; }

 private:
  std::vector<TaskRecord> tasks_;
  std::vector<std::pair<uint32_t, uint32_t>> dependencies_;
  std::vector<std::vector<ir::Effect>> effects_;
  uint32_t max_tasks_{UINT32_MAX};
  uint64_t max_payload_bytes_{UINT64_MAX};
  uint64_t payload_bytes_{};
  bool sealed_{};
};

class Timeline {
 public:
  explicit Timeline(uint64_t value = 0) : value_(value) {}
  bool signal(uint64_t value, std::string* error = nullptr);
  bool wait(uint64_t value) const { return value <= value_; }
  bool validate_wait(uint64_t value, std::string* error = nullptr) const;
  uint64_t value() const { return value_; }

 private:
  uint64_t value_{};
};

struct Certificate {
  std::vector<ir::Effect> ranges;
  bool covers(const ir::Effect& effect) const;
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
  const std::vector<WitnessEntry>& entries() const { return entries_; }
  WitnessDiff diff(const Certificate& certificate) const;

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
};

bool validate_certificate(const Certificate& certificate,
                          const std::vector<ir::Effect>& effects,
                          std::string* error = nullptr);

}  // namespace vg::core

#endif
