#include "core/pointer_graph.h"

#include "core/arena.h"

#include <algorithm>

namespace vg::core {

bool GraphEpoch::contains(PointerRef reference) const {
  return std::ranges::any_of(references_, [&](PointerRef candidate) {
    return candidate.allocation == reference.allocation && candidate.generation == reference.generation;
  });
}

bool GraphEpochBuilder::add_reference(PointerRef reference, std::string* error) {
  if (sealed_) { if (error) *error = "graph epoch is sealed"; return false; }
  if (reference.generation == 0) { if (error) *error = "graph reference generation must be non-zero"; return false; }
  if (std::ranges::any_of(references_, [&](PointerRef candidate) {
        return candidate.allocation == reference.allocation && candidate.generation == reference.generation;
      })) return true;
  references_.push_back(reference);
  return true;
}

bool GraphEpochBuilder::add_reference(const Arena& arena, PointerRef reference, std::string* error) {
  if (arena.lookup(reference) == nullptr) {
    if (error) *error = "graph reference is not active in arena";
    return false;
  }
  if (arena_ == nullptr) arena_ = &arena;
  return add_reference(reference, error);
}

bool GraphEpochBuilder::seal(GraphEpoch* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "graph epoch output is required"; return false; }
  if (sealed_) { if (error) *error = "graph epoch builder is already sealed"; return false; }
  out->value_ = arena_ != nullptr ? arena_->topology_epoch() : next_epoch_;
  out->references_ = references_;
  out->sealed_ = true;
  sealed_ = true;
  return true;
}

namespace {
bool pointer_ref_equal(PointerRef a, PointerRef b) { return a.allocation == b.allocation && a.generation == b.generation; }
}

bool PointerGraph::reachable(PointerRef from, uint64_t field_offset, PointerRef to) const {
  return std::ranges::any_of(edges_, [&](const Edge& edge) {
    return pointer_ref_equal(edge.from, from) && edge.field_offset == field_offset && pointer_ref_equal(edge.to, to);
  });
}

bool PointerGraph::reachable(ReachQuery query) const {
  std::vector<PointerRef> worklist{query.from};
  std::vector<PointerRef> seen{query.from};
  while (!worklist.empty()) {
    PointerRef current = worklist.back();
    worklist.pop_back();
    for (const auto& edge : edges_) {
      if (!pointer_ref_equal(edge.from, current)) continue;
      if (pointer_ref_equal(edge.to, query.to)) return true;
      if (std::ranges::any_of(seen, [&](PointerRef candidate) { return pointer_ref_equal(candidate, edge.to); })) continue;
      seen.push_back(edge.to);
      worklist.push_back(edge.to);
    }
  }
  return false;
}

bool PointerGraphBuilder::add_edge(PointerRef from, uint64_t field_offset, PointerRef to, std::string* error) {
  if (built_) { if (error) *error = "pointer graph builder is already built"; return false; }
  if (from.generation == 0 || to.generation == 0) { if (error) *error = "pointer edge generation must be non-zero"; return false; }
  edges_.push_back({from, field_offset, to});
  return true;
}

bool PointerGraphBuilder::build(PointerGraph* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "pointer graph output is required"; return false; }
  if (built_) { if (error) *error = "pointer graph builder is already built"; return false; }
  out->edges_ = edges_;
  built_ = true;
  return true;
}

}  // namespace vg::core
