#include "core/task_graph.h"

#include <algorithm>
#include <utility>

namespace vg::core {

PublicationRing::PublicationRing(uint32_t capacity) : slots_(capacity) {}

int32_t PublicationRing::reserve() {
  if (slots_.empty()) return -1;
  const uint32_t start = next_slot_.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; i < slots_.size(); ++i) {
    const uint32_t index = (start + i) % static_cast<uint32_t>(slots_.size());
    auto expected = PublicationState::Empty;
    if (slots_[index].state.compare_exchange_strong(expected, PublicationState::Writing,
                                                     std::memory_order_acquire,
                                                     std::memory_order_relaxed)) return static_cast<int32_t>(index);
  }
  return -1;
}

bool PublicationRing::write(uint32_t slot, const TaskRecord& task, std::string* error) {
  if (slot >= slots_.size()) { if (error) *error = "publication slot is out of range"; return false; }
  if (slots_[slot].state.load(std::memory_order_relaxed) != PublicationState::Writing) { if (error) *error = "publication slot is not writable"; return false; }
  if (task.node_generation == 0 || task.root_generation == 0) { if (error) *error = "task generation must be non-zero"; return false; }
  slots_[slot].task = task;
  return true;
}

bool PublicationRing::publish(uint32_t slot, std::string* error) {
  if (slot >= slots_.size()) { if (error) *error = "publication slot is out of range"; return false; }
  auto expected = PublicationState::Writing;
  if (!slots_[slot].state.compare_exchange_strong(expected, PublicationState::Published,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)) { if (error) *error = "publication slot is not in writing state"; return false; }
  return true;
}

bool PublicationRing::acquire(uint32_t slot, TaskRecord* out, std::string* error) const {
  if (slot >= slots_.size() || out == nullptr) { if (error) *error = "publication acquire arguments are invalid"; return false; }
  if (slots_[slot].state.load(std::memory_order_acquire) != PublicationState::Published) { if (error) *error = "publication slot is not published"; return false; }
  *out = slots_[slot].task;
  return true;
}

bool PublicationRing::consume(uint32_t slot, std::string* error) {
  if (slot >= slots_.size()) { if (error) *error = "publication slot is out of range"; return false; }
  auto expected = PublicationState::Published;
  if (!slots_[slot].state.compare_exchange_strong(expected, PublicationState::Consumed,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed)) { if (error) *error = "publication slot is not consumable"; return false; }
  return true;
}

bool PublicationRing::abort(uint32_t slot, std::string* error) {
  if (slot >= slots_.size()) { if (error) *error = "publication slot is out of range"; return false; }
  auto expected = PublicationState::Writing;
  if (!slots_[slot].state.compare_exchange_strong(expected, PublicationState::Empty,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
    if (error) *error = "publication slot is not abortable";
    return false;
  }
  return true;
}

bool PublicationRing::publish_task(const TaskRecord& task, uint32_t* slot, std::string* error) {
  const int32_t reserved = reserve();
  if (reserved < 0) { if (error) *error = "publication ring quota overflow"; return false; }
  const auto index = static_cast<uint32_t>(reserved);
  if (!write(index, task, error)) { abort(index); return false; }
  if (!publish(index, error)) { abort(index); return false; }
  if (slot != nullptr) *slot = index;
  return true;
}

bool TaskGraphBuilder::append(const TaskRecord& task, std::string* error) {
  if (sealed_) { if (error) *error = "task graph builder is sealed"; return false; }
  if (task.node_generation == 0 || task.root_generation == 0) { if (error) *error = "task generation must be non-zero"; return false; }
  if (task.kind == TaskKind::Raster && task.topology != Topology::TriangleList) {
    if (error) *error = "raster topology is Unsupported; triangle-list is required";
    return false;
  }
  if (task.kind == TaskKind::Raster &&
      static_cast<uint32_t>(task.depth_compare_op) > static_cast<uint32_t>(DepthCompareOp::Always)) {
    if (error) *error = "raster depth compare op is invalid";
    return false;
  }
  const bool has_depth_attachment = task.depth_attachment_ref.index != 0 || task.depth_attachment_ref.generation != 0;
  // Facet slot zero is valid, so `{0, nonzero}` is a legitimate token.  The
  // converse `{nonzero, 0}` can never be one: every issued FacetRef has a
  // non-zero generation. Reject it here rather than letting one backend
  // mistake the malformed token for an omitted depth attachment.
  if (task.kind == TaskKind::Raster && task.depth_attachment_ref.index != 0 &&
      task.depth_attachment_ref.generation == 0) {
    if (error) *error = "raster depth attachment facet generation must be non-zero";
    return false;
  }
  if (task.kind == TaskKind::Raster && !has_depth_attachment &&
      (task.depth_test_enable || task.depth_write_enable || task.depth_compare_op != DepthCompareOp::Always)) {
    if (error) *error = "raster depth state requires a depth attachment facet";
    return false;
  }
  if (tasks_.size() >= max_tasks_) { if (error) *error = "task graph quota overflow"; return false; }
  if (task.payload_size > max_payload_bytes_ - payload_bytes_) { if (error) *error = "task payload quota overflow"; return false; }
  tasks_.push_back(task);
  effects_.emplace_back();
  if (task.kind == TaskKind::Raster && has_depth_attachment) {
    // TaskGraphBuilder deliberately has no Arena/FacetPool, so it cannot
    // resolve a depth facet to its backing allocation here. Encode the full
    // capability token instead: generation occupies the high 32 bits and
    // slot index the low 32 bits. This is deterministic and injective for
    // FacetRef, hence two tasks using the same live depth capability receive
    // the same synthetic write effect and seal() infers a WAW edge. This is a
    // conservative capability-token-level identity (it can only add an
    // extra dependency if it collides with a caller-supplied allocation id),
    // until a future builder owns enough context to resolve actual backing
    // allocation ranges.
    const uint64_t synthetic_depth_identity =
        (static_cast<uint64_t>(task.depth_attachment_ref.generation) << 32) |
        static_cast<uint64_t>(task.depth_attachment_ref.index);
    effects_.back().push_back({synthetic_depth_identity, 0, 1, ir::Access::Write, 0});
  }
  payload_bytes_ += task.payload_size;
  return true;
}

bool TaskGraphBuilder::add_dependency(uint32_t before, uint32_t after, std::string* error) {
  if (sealed_) { if (error) *error = "task graph builder is sealed"; return false; }
  if (before >= tasks_.size() || after >= tasks_.size() || before == after) { if (error) *error = "invalid task dependency"; return false; }
  dependencies_.push_back({before, after});
  return true;
}

bool TaskGraphBuilder::add_effect(uint32_t task, const ir::Effect& effect, std::string* error) {
  if (sealed_) { if (error) *error = "task graph builder is sealed"; return false; }
  if (task >= effects_.size()) { if (error) *error = "effect task index is out of range"; return false; }
  if (effect.allocation == 0 || effect.size == 0) {
    if (error) *error = "task effect identity and size must be non-zero";
    return false;
  }
  if (effect.offset > UINT64_MAX - effect.size) { if (error) *error = "task effect range overflows"; return false; }
  effects_[task].push_back(effect);
  return true;
}

bool TaskGraphBuilder::set_effects(uint32_t task, const std::vector<ir::Effect>& effects, std::string* error) {
  if (sealed_) { if (error) *error = "task graph builder is sealed"; return false; }
  if (task >= effects_.size()) { if (error) *error = "effect task index is out of range"; return false; }
  effects_[task].clear();
  return std::ranges::all_of(effects, [&](const ir::Effect& effect) { return add_effect(task, effect, error); });
}

bool TaskGraphBuilder::set_quota(uint32_t max_tasks, uint64_t max_payload_bytes, std::string* error) {
  if (sealed_) { if (error) *error = "task graph builder is sealed"; return false; }
  if (max_tasks < tasks_.size() || max_payload_bytes < payload_bytes_) {
    if (error) *error = "task graph quota is below current usage";
    return false;
  }
  max_tasks_ = max_tasks;
  max_payload_bytes_ = max_payload_bytes;
  return true;
}

bool TaskGraphBuilder::append_published(PublicationRing& ring, uint32_t slot, std::string* error) {
  TaskRecord task;
  if (!ring.acquire(slot, &task, error)) return false;
  if (!append(task, error)) return false;
  return ring.consume(slot, error);
}

bool TaskGraphBuilder::seal(TaskGraph* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "sealed graph output is required"; return false; }
  if (sealed_) { if (error) *error = "task graph builder is already sealed"; return false; }
  EffectGraph graph;
  for (const auto& edge : dependencies_) if (!graph.add_edge(edge.first, edge.second, EffectEdgeKind::Explicit, 0, error)) return false;
  for (uint32_t before = 0; before < effects_.size(); ++before) {
    for (uint32_t after = before + 1; after < effects_.size(); ++after) {
      bool hazard = false;
      for (const auto& lhs : effects_[before]) for (const auto& rhs : effects_[after])
        hazard = hazard || EffectGraph::conflicts(lhs, rhs);
      if (hazard && !graph.add_edge(before, after, EffectEdgeKind::InferredConflict, 0, error)) return false;
    }
  }
  if (!graph.valid()) { if (error) *error = "task graph dependency cycle"; return false; }
  if (!graph.validate_happens_before(effects_, error)) return false;
  out->tasks_ = tasks_;
  out->dependencies_.clear();
  for (const auto& edge : graph.edges()) out->dependencies_.push_back({edge.before, edge.after});
  out->effect_graph_ = std::move(graph);
  out->published_ = false;
  sealed_ = true;
  return true;
}

bool TaskGraph::publish(std::string* error) {
  if (!sealed_) { if (error) *error = "task graph must be sealed before publication"; return false; }
  if (published_) { if (error) *error = "task graph is already published"; return false; }
  published_ = true;
  return true;
}

bool TaskGraph::validate_execution(std::string* error) const {
  if (!sealed_) { if (error) *error = "task graph must be sealed before execution"; return false; }
  if (!published_) { if (error) *error = "task graph must be published before execution"; return false; }
  if (!std::ranges::all_of(tasks_, [](const TaskRecord& task) {
        return task.node_generation != 0 && task.root_generation != 0;
      })) {
    if (error) *error = "task generation is stale";
    return false;
  }
  return true;
}

bool TaskGraph::deterministic_order(std::vector<uint32_t>* out, std::string* error) const {
  if (out == nullptr) { if (error) *error = "deterministic order output is required"; return false; }
  const auto count = static_cast<uint32_t>(tasks_.size());
  std::vector<std::vector<uint32_t>> adjacency(count);
  std::vector<uint32_t> in_degree(count, 0);
  for (const auto& dependency : dependencies_) {
    adjacency[dependency.first].push_back(dependency.second);
    ++in_degree[dependency.second];
  }
  std::vector<uint32_t> ready;
  for (uint32_t i = 0; i < count; ++i) if (in_degree[i] == 0) ready.push_back(i);
  out->clear();
  out->reserve(count);
  while (!ready.empty()) {
    std::ranges::sort(ready);
    const uint32_t node = ready.front();
    ready.erase(ready.begin());
    out->push_back(node);
    for (uint32_t next : adjacency[node]) if (--in_degree[next] == 0) ready.push_back(next);
  }
  if (out->size() != count) {
    if (error) *error = "task graph dependency cycle detected during execution ordering";
    return false;
  }
  return true;
}

}  // namespace vg::core
