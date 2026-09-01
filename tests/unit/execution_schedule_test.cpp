#include "core/execution_schedule.h"

#include <cstdlib>
#include <iostream>

namespace {

[[noreturn]] void check_failed(const char *expression, const char *file,
                               int line) {
  std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
  std::exit(EXIT_FAILURE);
}

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition))                                                          \
      check_failed(#condition, __FILE__, __LINE__);                            \
  } while (false)

using vg::core::EffectEdgeKind;
using vg::core::EffectGraph;
using vg::core::ExecutionSchedule;

ExecutionSchedule
derive(const EffectGraph &graph,
       std::vector<std::vector<vg::ir::Effect>> effects,
       std::vector<std::vector<vg::core::TaskFacetSemanticUse>> facets = {},
       std::vector<vg::core::ScheduleRepresentationFact> representations = {}) {
  if (facets.empty())
    facets.resize(effects.size());
  std::vector<uint32_t> order;
  std::string error;
  CHECK(vg::core::effect_graph_deterministic_order(
      graph, static_cast<uint32_t>(effects.size()), &order, &error));
  ExecutionSchedule schedule;
  CHECK(vg::core::derive_execution_schedule(
      graph, effects, facets, representations, order, &schedule, &error));
  return schedule;
}

void test_deterministic_components_waves_and_canonical_order() {
  EffectGraph graph;
  CHECK(graph.add_edge(1, 2, EffectEdgeKind::Explicit));
  CHECK(graph.add_edge(0, 2, EffectEdgeKind::Explicit));
  CHECK(graph.add_edge(3, 4, EffectEdgeKind::Explicit));
  const auto schedule =
      derive(graph, std::vector<std::vector<vg::ir::Effect>>(5));
  CHECK(schedule.components.size() == 2);
  CHECK(schedule.components[0].tasks == std::vector<uint32_t>({0, 1, 2}));
  CHECK(schedule.components[0].waves.size() == 2);
  CHECK(schedule.components[0].waves[0].tasks == std::vector<uint32_t>({0, 1}));
  CHECK(schedule.components[0].waves[1].tasks == std::vector<uint32_t>({2}));
  CHECK(schedule.components[1].tasks == std::vector<uint32_t>({3, 4}));
  CHECK(schedule.components[1].waves[0].tasks == std::vector<uint32_t>({3}));
  CHECK(schedule.components[1].waves[1].tasks == std::vector<uint32_t>({4}));
  CHECK(schedule.task_order == std::vector<uint32_t>({0, 1, 2, 3, 4}));
  CHECK(schedule.structural_successors[0] == std::vector<uint32_t>({2}));
  CHECK(schedule.structural_successors[1] == std::vector<uint32_t>({2}));
  CHECK(schedule.structural_successors[3] == std::vector<uint32_t>({4}));
}

void test_read_only_and_non_overlapping_ranges_are_independent() {
  const vg::ir::Effect read_a{7, 0, 16, vg::ir::Access::Read, 3};
  const vg::ir::Effect read_b{7, 0, 16, vg::ir::Access::Read, 3};
  EffectGraph read_graph;
  const auto reads = derive(read_graph, {{read_a}, {read_b}});
  CHECK(reads.components.size() == 2);
  CHECK(reads.transitions.empty());

  const vg::ir::Effect write_left{9, 0, 8, vg::ir::Access::Write, 4};
  const vg::ir::Effect write_right{9, 8, 8, vg::ir::Access::Write, 4};
  EffectGraph range_graph;
  const auto ranges = derive(range_graph, {{write_left}, {write_right}});
  CHECK(ranges.components.size() == 2);
  CHECK(ranges.transitions.empty());
}

void test_visibility_and_explicit_completion_transitions() {
  EffectGraph conflict;
  CHECK(conflict.add_edge(0, 1, EffectEdgeKind::InferredConflict));
  const auto visibility =
      derive(conflict, {{{31, 4, 12, vg::ir::Access::Write, 6}},
                        {{31, 8, 8, vg::ir::Access::Read, 6}}});
  CHECK(visibility.transitions.size() == 1);
  const auto &transition = visibility.transitions[0];
  CHECK(transition.requires_execution_completion);
  CHECK(transition.before_wave == 0 && transition.after_wave == 1);
  CHECK(transition.region_visibility.size() == 1);
  CHECK(transition.region_visibility[0].allocation == 31);
  CHECK(transition.region_visibility[0].offset == 8);
  CHECK(transition.region_visibility[0].size == 8);

  EffectGraph explicit_only;
  CHECK(explicit_only.add_edge(0, 1, EffectEdgeKind::Explicit));
  const auto completion =
      derive(explicit_only, {{{41, 0, 4, vg::ir::Access::Read, 0}},
                             {{42, 0, 4, vg::ir::Access::Read, 0}}});
  CHECK(completion.transitions.size() == 1);
  CHECK(completion.transitions[0].requires_execution_completion);
  CHECK(completion.transitions[0].region_visibility.empty());

  // A conflicting producer/consumer may be ordered by a transitive explicit
  // path.  The visibility fact still belongs to the consumer boundary even
  // though Core correctly avoided adding a redundant direct edge.
  EffectGraph transitive;
  CHECK(transitive.add_edge(0, 1, EffectEdgeKind::Explicit));
  CHECK(transitive.add_edge(1, 2, EffectEdgeKind::Explicit));
  const auto transitive_visibility =
      derive(transitive, {{{77, 0, 4, vg::ir::Access::Write, 2}},
                          {{88, 0, 4, vg::ir::Access::Read, 2}},
                          {{77, 0, 4, vg::ir::Access::Read, 2}}});
  CHECK(transitive_visibility.transitions.size() == 2);
  CHECK(transitive_visibility.transitions[0].region_visibility.empty());
  CHECK(transitive_visibility.transitions[1].region_visibility.size() == 1);
  CHECK(
      transitive_visibility.transitions[1].region_visibility[0].producer_task ==
      0);
  CHECK(
      transitive_visibility.transitions[1].region_visibility[0].consumer_task ==
      2);
}

void test_fork_join_and_multiple_chains() {
  EffectGraph fork_join;
  CHECK(fork_join.add_edge(0, 1, EffectEdgeKind::Explicit));
  CHECK(fork_join.add_edge(0, 2, EffectEdgeKind::Explicit));
  CHECK(fork_join.add_edge(1, 3, EffectEdgeKind::Explicit));
  CHECK(fork_join.add_edge(2, 3, EffectEdgeKind::Explicit));
  const auto forked =
      derive(fork_join, std::vector<std::vector<vg::ir::Effect>>(4));
  CHECK(forked.components.size() == 1);
  CHECK(forked.components[0].waves.size() == 3);
  CHECK(forked.components[0].waves[0].tasks == std::vector<uint32_t>({0}));
  CHECK(forked.components[0].waves[1].tasks == std::vector<uint32_t>({1, 2}));
  CHECK(forked.components[0].waves[2].tasks == std::vector<uint32_t>({3}));

  EffectGraph chains;
  CHECK(chains.add_edge(0, 2, EffectEdgeKind::Explicit));
  CHECK(chains.add_edge(1, 3, EffectEdgeKind::Explicit));
  const auto independent =
      derive(chains, std::vector<std::vector<vg::ir::Effect>>(4));
  CHECK(independent.components.size() == 2);
  CHECK(independent.components[0].tasks == std::vector<uint32_t>({0, 2}));
  CHECK(independent.components[1].tasks == std::vector<uint32_t>({1, 3}));
}

void test_non_structural_edges_do_not_partition() {
  EffectGraph graph;
  CHECK(graph.add_edge(0, 1, EffectEdgeKind::Timeline, 7));
  CHECK(graph.add_edge(1, 2, EffectEdgeKind::Publication));
  const auto schedule =
      derive(graph, std::vector<std::vector<vg::ir::Effect>>(3));
  CHECK(schedule.components.size() == 3);
  CHECK(schedule.transitions.empty());
  CHECK(schedule.structural_successors ==
        std::vector<std::vector<uint32_t>>(3));
}

void test_facet_and_representation_prerequisites() {
  vg::core::CanonicalView view;
  view.allocation = 55;
  view.allocation_generation = 2;
  view.width = 1;
  view.height = 1;
  vg::core::TaskFacetSemanticUse use{
      {4, 9}, vg::core::FacetKind::Sample, view, 8, vg::ir::Access::Read};
  vg::core::ScheduleRepresentationFact representation{
      3, view, vg::core::FacetKind::Sample, 9};
  EffectGraph graph;
  const auto schedule = derive(graph, {{{55, 0, 4, vg::ir::Access::Read, 8}}},
                               {{use}}, {representation});
  CHECK(schedule.transitions.size() == 1);
  const auto &prelude = schedule.transitions[0];
  CHECK(prelude.before_wave == vg::core::kExecutionSchedulePrelude);
  CHECK(prelude.after_wave == 0);
  CHECK(!prelude.requires_execution_completion);
  CHECK(prelude.facet_requirements.size() == 1);
  CHECK(prelude.facet_requirements[0].task == 0);
  CHECK(prelude.facet_requirements[0].use.view.allocation == 55);
  CHECK(prelude.facet_requirements[0].use.representation_epoch == 8);
  CHECK(prelude.representation_operations == std::vector<uint32_t>({3}));
}

void test_shared_representation_operation_is_one_prerequisite_identity() {
  // Read-only sharing creates two independent components. Both first waves
  // reference the same frozen operation identity; these references must not
  // be interpreted as two executions of the physical operation.
  const vg::ir::Effect read{66, 0, 4, vg::ir::Access::Read, 5};
  vg::core::CanonicalView view;
  view.allocation = 66;
  view.allocation_generation = 1;
  view.width = 1;
  view.height = 1;
  const vg::core::ScheduleRepresentationFact representation{
      7, view, vg::core::FacetKind::Sample, 6};
  EffectGraph graph;
  const auto schedule = derive(graph, {{read}, {read}}, {}, {representation});
  CHECK(schedule.components.size() == 2);
  CHECK(schedule.transitions.size() == 2);
  CHECK(schedule.transitions[0].before_wave ==
        vg::core::kExecutionSchedulePrelude);
  CHECK(schedule.transitions[1].before_wave ==
        vg::core::kExecutionSchedulePrelude);
  CHECK(schedule.transitions[0].representation_operations ==
        std::vector<uint32_t>({7}));
  CHECK(schedule.transitions[1].representation_operations ==
        std::vector<uint32_t>({7}));
}

void test_malformed_inputs_are_rejected() {
  std::string error;
  ExecutionSchedule schedule;
  EffectGraph graph;
  CHECK(graph.add_edge(0, 3, EffectEdgeKind::Explicit));
  CHECK(!vg::core::derive_execution_schedule(
      graph, std::vector<std::vector<vg::ir::Effect>>(2),
      std::vector<std::vector<vg::core::TaskFacetSemanticUse>>(2), {}, {0, 1},
      &schedule, &error));
  CHECK(error.find("unknown") != std::string::npos);

  EffectGraph invalid_metadata;
  CHECK(invalid_metadata.add_edge(0, 8, EffectEdgeKind::Publication));
  error.clear();
  CHECK(!vg::core::derive_execution_schedule(
      invalid_metadata, std::vector<std::vector<vg::ir::Effect>>(2),
      std::vector<std::vector<vg::core::TaskFacetSemanticUse>>(2), {}, {0, 1},
      &schedule, &error));
  CHECK(error == "execution schedule edge references an unknown task");

  EffectGraph valid;
  error.clear();
  CHECK(!vg::core::derive_execution_schedule(
      valid, std::vector<std::vector<vg::ir::Effect>>(2),
      std::vector<std::vector<vg::core::TaskFacetSemanticUse>>(2), {}, {0, 0},
      &schedule, &error));
  CHECK(error == "execution schedule canonical task order is malformed");

  EffectGraph duplicate;
  CHECK(duplicate.add_edge(0, 1, EffectEdgeKind::Explicit));
  CHECK(duplicate.add_edge(0, 1, EffectEdgeKind::InferredConflict));
  error.clear();
  CHECK(!vg::core::derive_execution_schedule(
      duplicate, std::vector<std::vector<vg::ir::Effect>>(2),
      std::vector<std::vector<vg::core::TaskFacetSemanticUse>>(2), {}, {0, 1},
      &schedule, &error));
  CHECK(error == "execution schedule contains a duplicate structural edge");
}

} // namespace

int main() {
  test_deterministic_components_waves_and_canonical_order();
  test_read_only_and_non_overlapping_ranges_are_independent();
  test_visibility_and_explicit_completion_transitions();
  test_fork_join_and_multiple_chains();
  test_non_structural_edges_do_not_partition();
  test_facet_and_representation_prerequisites();
  test_shared_representation_operation_is_one_prerequisite_identity();
  test_malformed_inputs_are_rejected();
  return 0;
}
