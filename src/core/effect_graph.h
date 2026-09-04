#ifndef VG_CORE_EFFECT_GRAPH_H_
#define VG_CORE_EFFECT_GRAPH_H_

#include "ir/ir.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vg::core {

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

}  // namespace vg::core

#endif
