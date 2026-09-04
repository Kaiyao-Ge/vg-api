#ifndef VG_CORE_POINTER_GRAPH_H_
#define VG_CORE_POINTER_GRAPH_H_

#include "core/resource_types.h"

#include <string>
#include <vector>

namespace vg::core {

class Arena;

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

}  // namespace vg::core

#endif
