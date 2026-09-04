#ifndef VG_CORE_TASK_GRAPH_H_
#define VG_CORE_TASK_GRAPH_H_

#include "core/effect_graph.h"
#include "core/facet.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vg::core {

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

enum class PublicationState : uint32_t { Empty, Writing, Published, Consumed };

struct PublicationSlot {
  TaskRecord task{};
  std::atomic<PublicationState> state{PublicationState::Empty};
};

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

}  // namespace vg::core

#endif
