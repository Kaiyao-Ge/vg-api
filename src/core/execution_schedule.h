#ifndef VG_CORE_EXECUTION_SCHEDULE_H_
#define VG_CORE_EXECUTION_SCHEDULE_H_

#include "core/core.h"

namespace vg::core {

// Immutable Stage-5 snapshot of one facet capability a Task will use.  The
// full view and representation epoch are captured while the FacetPool is
// available to Core so Stage 6 never has to rediscover Task semantics from a
// mutable pool.
struct TaskFacetSemanticUse {
  FacetRef ref;
  FacetKind kind{FacetKind::Address};
  CanonicalView view;
  uint32_t representation_epoch{};
  ir::Access access{ir::Access::Read};
};

struct ExecutionWave {
  std::vector<uint32_t> tasks;
};

struct ExecutionComponent {
  // Deterministic ascending membership.  This is an execution island, not a
  // promise that a backend uses a separate hardware queue for it.
  std::vector<uint32_t> tasks;
  std::vector<ExecutionWave> waves;
};

struct RegionVisibilityRequirement {
  uint32_t producer_task{};
  uint32_t consumer_task{};
  uint64_t allocation{};
  uint64_t offset{};
  uint64_t size{};
  uint32_t representation_epoch{};
  ir::Access producer_access{ir::Access::Read};
  ir::Access consumer_access{ir::Access::Read};
};

struct FacetTransitionRequirement {
  uint32_t task{};
  TaskFacetSemanticUse use;
};

// A transition with before_wave == kExecutionSchedulePrelude names semantic
// prerequisites of a component's first wave.  Other transitions are the
// domain-neutral facts between consecutive ready waves.  Stage 6 may lower a
// boundary conservatively, but it must preserve every fact recorded here.
inline constexpr uint32_t kExecutionSchedulePrelude = UINT32_MAX;
struct WaveTransition {
  uint32_t component{};
  uint32_t before_wave{kExecutionSchedulePrelude};
  uint32_t after_wave{};
  bool requires_execution_completion{};
  std::vector<RegionVisibilityRequirement> region_visibility;
  std::vector<FacetTransitionRequirement> facet_requirements;
  // Indices into the one frozen representation plan, not commands owned by
  // this transition. The same operation may be referenced by the first
  // consumer wave of multiple independent components. Stage 6/7 must compile
  // and execute that physical operation once per submission, then make every
  // referencing wave depend on its completion.
  std::vector<uint32_t> representation_operations;
};

struct ExecutionSchedule {
  std::vector<ExecutionComponent> components;
  std::vector<WaveTransition> transitions;
  // Direct structural successors are the immutable failure-cancellation
  // authority.  Backends may walk this sealed adjacency but must not rebuild
  // it from EffectGraph.
  std::vector<std::vector<uint32_t>> structural_successors;
  // Stable lowest-ready-index observation/publication order.  It is not a
  // serial hardware schedule.
  std::vector<uint32_t> task_order;
};

// Lightweight view of an already-frozen Stage-5 representation operation.
// This avoids making the schedule algorithm depend on HAL physical types.
struct ScheduleRepresentationFact {
  uint32_t operation{};
  CanonicalView view;
  FacetKind target_kind{FacetKind::Sample};
  uint32_t target_representation_epoch{};
};

bool derive_execution_schedule(
    const EffectGraph &graph,
    const std::vector<std::vector<ir::Effect>> &task_effects,
    const std::vector<std::vector<TaskFacetSemanticUse>> &task_facet_uses,
    const std::vector<ScheduleRepresentationFact> &representation_facts,
    const std::vector<uint32_t> &canonical_task_order, ExecutionSchedule *out,
    std::string *error = nullptr);

} // namespace vg::core

#endif
