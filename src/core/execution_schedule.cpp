#include "core/execution_schedule.h"

#include <algorithm>

namespace vg::core {
namespace {

bool structural(EffectEdgeKind kind) {
  return kind == EffectEdgeKind::Explicit ||
         kind == EffectEdgeKind::InferredConflict;
}

bool overlap(const ir::Effect &producer, const ir::Effect &consumer,
             uint64_t *offset, uint64_t *size) {
  if (producer.allocation != consumer.allocation ||
      producer.representation_epoch != consumer.representation_epoch ||
      producer.size == 0 || consumer.size == 0 ||
      producer.offset > UINT64_MAX - producer.size ||
      consumer.offset > UINT64_MAX - consumer.size)
    return false;
  const uint64_t begin = std::max(producer.offset, consumer.offset);
  const uint64_t end = std::min(producer.offset + producer.size,
                                consumer.offset + consumer.size);
  if (begin >= end || (producer.access == ir::Access::Read &&
                       consumer.access == ir::Access::Read))
    return false;
  *offset = begin;
  *size = end - begin;
  return true;
}

bool task_uses_allocation(
    uint32_t task, uint64_t allocation,
    const std::vector<std::vector<ir::Effect>> &effects,
    const std::vector<std::vector<TaskFacetSemanticUse>> &facets) {
  return std::ranges::any_of(effects[task],
                             [&](const ir::Effect &effect) {
                               return effect.allocation == allocation;
                             }) ||
         std::ranges::any_of(facets[task],
                             [&](const TaskFacetSemanticUse &use) {
                               return use.view.allocation == allocation;
                             });
}

} // namespace

bool derive_execution_schedule(
    const EffectGraph &graph,
    const std::vector<std::vector<ir::Effect>> &task_effects,
    const std::vector<std::vector<TaskFacetSemanticUse>> &task_facet_uses,
    const std::vector<ScheduleRepresentationFact> &representation_facts,
    const std::vector<uint32_t> &canonical_task_order, ExecutionSchedule *out,
    std::string *error) {
  if (out == nullptr) {
    if (error)
      *error = "execution schedule output is required";
    return false;
  }
  const uint32_t count = static_cast<uint32_t>(task_effects.size());
  if (task_facet_uses.size() != count || canonical_task_order.size() != count) {
    if (error)
      *error = "execution schedule inputs do not cover every task";
    return false;
  }

  std::vector<uint8_t> order_seen(count);
  for (uint32_t task : canonical_task_order) {
    if (task >= count || order_seen[task] != 0) {
      if (error)
        *error = "execution schedule canonical task order is malformed";
      return false;
    }
    order_seen[task] = 1;
  }
  std::vector<uint32_t> expected_order;
  if (!effect_graph_deterministic_order(graph, count, &expected_order, error) ||
      expected_order != canonical_task_order) {
    if (error && error->empty())
      *error =
          "execution schedule canonical order disagrees with its EffectGraph";
    return false;
  }

  std::vector<std::vector<uint32_t>> successors(count), undirected(count);
  std::vector<std::pair<uint32_t, uint32_t>> structural_edges;
  for (const auto &edge : graph.edges()) {
    if (edge.before >= count || edge.after >= count) {
      if (error)
        *error = "execution schedule edge references an unknown task";
      return false;
    }
    if (!structural(edge.kind))
      continue;
    if (std::ranges::find(successors[edge.before], edge.after) !=
        successors[edge.before].end()) {
      if (error)
        *error = "execution schedule contains a duplicate structural edge";
      return false;
    }
    successors[edge.before].push_back(edge.after);
    undirected[edge.before].push_back(edge.after);
    undirected[edge.after].push_back(edge.before);
    structural_edges.push_back({edge.before, edge.after});
  }
  for (auto &next : successors)
    std::ranges::sort(next);
  for (auto &neighbours : undirected)
    std::ranges::sort(neighbours);
  std::vector<std::vector<uint8_t>> reaches(count, std::vector<uint8_t>(count));
  for (uint32_t source = 0; source < count; ++source) {
    std::vector<uint32_t> work{source};
    reaches[source][source] = 1;
    for (size_t index = 0; index < work.size(); ++index) {
      for (uint32_t next : successors[work[index]]) {
        if (!reaches[source][next]) {
          reaches[source][next] = 1;
          work.push_back(next);
        }
      }
    }
  }

  ExecutionSchedule schedule;
  schedule.task_order = canonical_task_order;
  schedule.structural_successors = successors;

  std::vector<uint8_t> visited(count);
  for (uint32_t seed = 0; seed < count; ++seed) {
    if (visited[seed])
      continue;
    std::vector<uint32_t> work{seed};
    visited[seed] = 1;
    for (size_t i = 0; i < work.size(); ++i) {
      for (uint32_t neighbour : undirected[work[i]]) {
        if (!visited[neighbour]) {
          visited[neighbour] = 1;
          work.push_back(neighbour);
        }
      }
    }
    std::ranges::sort(work);
    ExecutionComponent component;
    component.tasks = work;

    std::vector<uint8_t> member(count), emitted(count);
    std::vector<uint32_t> in_degree(count);
    for (uint32_t task : work)
      member[task] = 1;
    for (const auto &[before, after] : structural_edges)
      if (member[before] && member[after])
        ++in_degree[after];
    size_t emitted_count = 0;
    while (emitted_count != work.size()) {
      ExecutionWave wave;
      for (uint32_t task : work)
        if (!emitted[task] && in_degree[task] == 0)
          wave.tasks.push_back(task);
      if (wave.tasks.empty()) {
        if (error)
          *error = "execution schedule structural graph contains a cycle";
        return false;
      }
      for (uint32_t task : wave.tasks) {
        emitted[task] = 1;
        ++emitted_count;
      }
      for (uint32_t task : wave.tasks)
        for (uint32_t next : successors[task])
          if (member[next])
            --in_degree[next];
      component.waves.push_back(std::move(wave));
    }
    schedule.components.push_back(std::move(component));
  }

  for (uint32_t component_index = 0;
       component_index < schedule.components.size(); ++component_index) {
    const auto &component = schedule.components[component_index];
    std::vector<uint32_t> task_wave(count, UINT32_MAX);
    for (uint32_t wave_index = 0; wave_index < component.waves.size();
         ++wave_index)
      for (uint32_t task : component.waves[wave_index].tasks)
        task_wave[task] = wave_index;

    for (uint32_t after_wave = 0; after_wave < component.waves.size();
         ++after_wave) {
      WaveTransition transition;
      transition.component = component_index;
      transition.before_wave =
          after_wave == 0 ? kExecutionSchedulePrelude : after_wave - 1;
      transition.after_wave = after_wave;
      transition.requires_execution_completion = after_wave != 0;

      for (uint32_t task : component.waves[after_wave].tasks)
        for (const auto &use : task_facet_uses[task])
          transition.facet_requirements.push_back({task, use});

      for (const auto &fact : representation_facts) {
        if (std::ranges::any_of(
                component.waves[after_wave].tasks, [&](uint32_t task) {
                  return task_uses_allocation(task, fact.view.allocation,
                                              task_effects, task_facet_uses);
                })) {
          // The first consumer wave is sufficient: the operation is sealed as
          // a precondition and all later waves are ordered after it.
          bool used_earlier = false;
          for (uint32_t earlier = 0; earlier < after_wave && !used_earlier;
               ++earlier)
            used_earlier = std::ranges::any_of(
                component.waves[earlier].tasks, [&](uint32_t task) {
                  return task_uses_allocation(task, fact.view.allocation,
                                              task_effects, task_facet_uses);
                });
          if (!used_earlier)
            transition.representation_operations.push_back(fact.operation);
        }
      }

      if (after_wave != 0) {
        for (uint32_t after : component.waves[after_wave].tasks) {
          for (uint32_t before : component.tasks) {
            if (task_wave[before] >= after_wave || !reaches[before][after])
              continue;
            for (const auto &producer : task_effects[before]) {
              for (const auto &consumer : task_effects[after]) {
                uint64_t offset = 0, size = 0;
                if (!overlap(producer, consumer, &offset, &size))
                  continue;
                transition.region_visibility.push_back(
                    {before, after, producer.allocation, offset, size,
                     producer.representation_epoch, producer.access,
                     consumer.access});
              }
            }
          }
        }
      }

      // Prelude transitions are retained only when they carry Stage-5
      // representation/facet facts. Every later wave has a completion fact.
      if (transition.requires_execution_completion ||
          !transition.facet_requirements.empty() ||
          !transition.representation_operations.empty())
        schedule.transitions.push_back(std::move(transition));
    }
  }

  *out = std::move(schedule);
  return true;
}

} // namespace vg::core
