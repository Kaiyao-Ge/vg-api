#include "core/effect_graph.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <utility>

namespace vg::core {

bool EffectGraph::add_edge(uint32_t before, uint32_t after, std::string* error) {
  return add_edge(before, after, EffectEdgeKind::Explicit, 0, error);
}

bool EffectGraph::add_edge(uint32_t before, uint32_t after, EffectEdgeKind kind,
                           uint64_t timeline_value, std::string* error) {
  if (before == after) { if (error) *error = "effect graph self-cycle"; return false; }
  edges_.push_back({before, after, kind, timeline_value});
  return true;
}

bool EffectGraph::add_timeline_edge(uint32_t before, uint32_t after, uint64_t required_value,
                                    uint64_t signaled_value, std::string* error) {
  if (required_value == 0) { if (error) *error = "timeline dependency value must be non-zero"; return false; }
  if (signaled_value < required_value) { if (error) *error = "timeline wait point is unsatisfied"; return false; }
  return add_edge(before, after, EffectEdgeKind::Timeline, required_value, error);
}

bool EffectGraph::conflicts(const ir::Effect& before, const ir::Effect& after) {
  if (before.allocation != after.allocation || before.representation_epoch != after.representation_epoch) return false;
  if (before.size == 0 || after.size == 0 || before.offset > UINT64_MAX - before.size || after.offset > UINT64_MAX - after.size) return false;
  const bool overlap = before.offset < after.offset + after.size && after.offset < before.offset + before.size;
  if (!overlap) return false;
  return before.access != ir::Access::Read || after.access != ir::Access::Read;
}

bool EffectGraph::validate_happens_before(const std::vector<std::vector<ir::Effect>>& effects,
                                          std::string* error) const {
  const auto count = static_cast<uint32_t>(effects.size());
  std::vector<std::vector<uint32_t>> adjacency(count);
  for (const auto& edge : edges_) {
    if (edge.before >= count || edge.after >= count) {
      if (error) *error = "effect edge references an unknown task";
      return false;
    }
    adjacency[edge.before].push_back(edge.after);
  }
  const auto reaches = [&](uint32_t source, uint32_t destination) {
    std::vector<uint8_t> seen(count);
    std::vector<uint32_t> work{source};
    seen[source] = 1;
    for (size_t i = 0; i < work.size(); ++i)
      for (uint32_t next : adjacency[work[i]])
        if (!seen[next]) { seen[next] = 1; work.push_back(next); }
    return seen[destination] != 0;
  };
  for (uint32_t before = 0; before < count; ++before) {
    for (uint32_t after = before + 1; after < count; ++after) {
      bool conflict = false;
      for (const auto& lhs : effects[before]) for (const auto& rhs : effects[after])
        conflict = conflict || conflicts(lhs, rhs);
      if (!conflict) continue;
      if (!reaches(before, after) && !reaches(after, before)) {
        if (error) *error = "conflicting task effects have no happens-before edge";
        return false;
      }
    }
  }
  return true;
}

bool EffectGraph::valid() const {
  std::unordered_map<uint32_t, std::vector<uint32_t>> adjacency;
  for (const auto& edge : edges_) adjacency[edge.before].push_back(edge.after);
  std::unordered_map<uint32_t, uint8_t> mark;
  std::function<bool(uint32_t)> visit = [&](uint32_t node) {
    if (mark[node] == 1) return false;
    if (mark[node] == 2) return true;
    mark[node] = 1;
    if (!std::ranges::all_of(adjacency[node], [&](uint32_t next) { return visit(next); })) return false;
    mark[node] = 2;
    return true;
  };
  return std::ranges::all_of(adjacency, [&](const auto& pair) { return visit(pair.first); });
}

uint32_t EffectGraphBuilder::add_node(std::vector<ir::Effect> effects, std::string* error) {
  if (sealed_) { if (error) *error = "effect graph builder is sealed"; return UINT32_MAX; }
  effects_.push_back(std::move(effects));
  return static_cast<uint32_t>(effects_.size() - 1);
}

bool EffectGraphBuilder::add_dependency(uint32_t before, uint32_t after, std::string* error) {
  if (sealed_) { if (error) *error = "effect graph builder is sealed"; return false; }
  if (before >= effects_.size() || after >= effects_.size() || before == after) {
    if (error) *error = "invalid effect graph dependency";
    return false;
  }
  dependencies_.push_back({before, after});
  return true;
}

bool EffectGraphBuilder::seal(EffectGraph* out, uint32_t* node_count, std::string* error) {
  if (out == nullptr) { if (error) *error = "sealed effect graph output is required"; return false; }
  if (sealed_) { if (error) *error = "effect graph builder is already sealed"; return false; }
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
  if (!graph.valid()) { if (error) *error = "effect graph dependency cycle"; return false; }
  if (!graph.validate_happens_before(effects_, error)) return false;
  if (node_count != nullptr) *node_count = static_cast<uint32_t>(effects_.size());
  *out = std::move(graph);
  sealed_ = true;
  return true;
}

EffectGraphShape classify_effect_graph_shape(const EffectGraph& graph, uint32_t node_count) {
  if (node_count <= 1) return EffectGraphShape::LinearChain;
  // Timeline/Publication edges are cross-cutting metadata, not structural
  // ordering the shape classifier reasons about -- only Explicit and
  // InferredConflict edges shape encoder/fence lowering (ADR-027).
  std::vector<uint32_t> out_degree(node_count, 0), in_degree(node_count, 0);
  uint32_t structural_edges = 0;
  for (const auto& edge : graph.edges()) {
    if (edge.kind != EffectEdgeKind::Explicit && edge.kind != EffectEdgeKind::InferredConflict) continue;
    if (edge.before >= node_count || edge.after >= node_count) return EffectGraphShape::Unsupported;
    ++out_degree[edge.before];
    ++in_degree[edge.after];
    ++structural_edges;
  }

  if (structural_edges == 0) return EffectGraphShape::IndependentBranches;

  // Linear chain: exactly node_count - 1 edges, every node has in/out
  // degree <= 1, forming a single path (checked by walking from the sole
  // in-degree-0 node and requiring every step to have exactly one option).
  if (structural_edges == node_count - 1) {
    bool is_chain = true;
    uint32_t sources = 0, start = 0;
    for (uint32_t i = 0; i < node_count; ++i) {
      if (in_degree[i] > 1 || out_degree[i] > 1) { is_chain = false; break; }
      if (in_degree[i] == 0) { ++sources; start = i; }
    }
    if (is_chain && sources == 1) {
      std::vector<std::vector<uint32_t>> adjacency(node_count);
      for (const auto& edge : graph.edges())
        if (edge.kind == EffectEdgeKind::Explicit || edge.kind == EffectEdgeKind::InferredConflict)
          adjacency[edge.before].push_back(edge.after);
      uint32_t node = start;
      uint32_t visited = 1;
      while (!adjacency[node].empty()) { node = adjacency[node].front(); ++visited; }
      if (visited == node_count) return EffectGraphShape::LinearChain;
    }
  }

  // Fork-join: exactly one source node fanning out to every other node
  // except a single join node, and exactly one join node fanning in from
  // every other node except the source, with no edges among the "middle"
  // nodes themselves.
  uint32_t source = UINT32_MAX, join = UINT32_MAX;
  for (uint32_t i = 0; i < node_count; ++i) {
    if (out_degree[i] == node_count - 1 && in_degree[i] == 0) source = i;
    if (in_degree[i] == node_count - 1 && out_degree[i] == 0) join = i;
  }
  if (source != UINT32_MAX && join != UINT32_MAX && source != join &&
      structural_edges == 2 * (node_count - 1)) {
    return EffectGraphShape::ForkJoin;
  }

  return EffectGraphShape::Unsupported;
}

bool effect_graph_deterministic_order(const EffectGraph& graph, uint32_t node_count,
                                      std::vector<uint32_t>* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "effect graph deterministic order output is required"; return false; }
  std::vector<std::vector<uint32_t>> adjacency(node_count);
  std::vector<uint32_t> in_degree(node_count, 0);
  for (const auto& edge : graph.edges()) {
    if (edge.kind != EffectEdgeKind::Explicit && edge.kind != EffectEdgeKind::InferredConflict) continue;
    if (edge.before >= node_count || edge.after >= node_count) {
      if (error) *error = "effect graph edge references an unknown node";
      return false;
    }
    adjacency[edge.before].push_back(edge.after);
    ++in_degree[edge.after];
  }
  std::vector<uint32_t> ready;
  for (uint32_t i = 0; i < node_count; ++i) if (in_degree[i] == 0) ready.push_back(i);
  out->clear();
  out->reserve(node_count);
  while (!ready.empty()) {
    std::ranges::sort(ready);
    const uint32_t node = ready.front();
    ready.erase(ready.begin());
    out->push_back(node);
    for (uint32_t next : adjacency[node]) if (--in_degree[next] == 0) ready.push_back(next);
  }
  if (out->size() != node_count) {
    if (error) *error = "effect graph dependency cycle detected during execution ordering";
    return false;
  }
  return true;
}

EffectGraphForkJoin describe_fork_join(const EffectGraph& graph, uint32_t node_count) {
  EffectGraphForkJoin result;
  std::vector<uint32_t> out_degree(node_count, 0), in_degree(node_count, 0);
  for (const auto& edge : graph.edges()) {
    if (edge.kind != EffectEdgeKind::Explicit && edge.kind != EffectEdgeKind::InferredConflict) continue;
    if (edge.before >= node_count || edge.after >= node_count) continue;
    ++out_degree[edge.before];
    ++in_degree[edge.after];
  }
  for (uint32_t i = 0; i < node_count; ++i) {
    if (out_degree[i] == node_count - 1 && in_degree[i] == 0) result.source = i;
    if (in_degree[i] == node_count - 1 && out_degree[i] == 0) result.join = i;
  }
  for (uint32_t i = 0; i < node_count; ++i) if (i != result.source && i != result.join) result.middle.push_back(i);
  return result;
}

}  // namespace vg::core
