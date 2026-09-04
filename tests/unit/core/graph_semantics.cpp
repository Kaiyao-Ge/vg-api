#include "cases.h"
#include <cassert>
#include "core/task_schema.h"

namespace vg::tests::core {

void test_arena_task_graph(vg::core::Arena& arena, const vg::core::ConsumeProof& discharged) {
  auto& allocation = arena.allocate(16);
  assert(allocation.id == 1 && allocation.generation == 1);
  assert(arena.topology_epoch() == 1);
  assert(arena.lookup(vg::core::PointerRef{allocation.id, allocation.generation}) != nullptr);
  assert(arena.acquire(allocation.id, allocation.generation));
  std::string transform_error;
  assert(!arena.transform(allocation.id, allocation.generation, nullptr, &transform_error));
  assert(arena.release(allocation.id, allocation.generation));
  uint32_t representation_epoch = 0;
  assert(arena.transform(allocation.id, allocation.generation, &representation_epoch) && representation_epoch == 1);
  assert(arena.lookup(vg::core::RepresentationRef{allocation.id, allocation.generation, representation_epoch}) != nullptr);
  assert(!arena.transform(allocation.id, allocation.generation, 0, nullptr, &transform_error));
  assert(!arena.consume(allocation.id, allocation.generation, representation_epoch,
                        vg::core::ConsumeProof{}, &transform_error) &&
         transform_error.rfind("ConsumeInput proof incomplete", 0) == 0);
  assert(arena.consume(allocation.id, allocation.generation, representation_epoch, discharged,
                       &transform_error));
  assert(arena.lookup(vg::core::PointerRef{allocation.id, 1}) == nullptr);
  auto& second_allocation = arena.allocate(16);

  // F2 (ADR-043 Decision #3, ADR-046): a default-constructed TaskRecord must
  // read as a plain Compute task with F2's fixed raster defaults, so every
  // pre-F2 caller that never touches these fields keeps its old meaning.
  {
    vg::core::TaskRecord default_task{};
    assert(default_task.kind == vg::core::TaskKind::Compute);
    assert(default_task.topology == vg::core::Topology::TriangleList);
    assert(default_task.index_count == 0);
    assert(default_task.raster_filter == vg::core::FilterMode::Bilinear);
    assert(default_task.raster_wrap == vg::core::WrapMode::Clamp);
    assert(default_task.raster_tint[0] == 1.0f && default_task.raster_tint[1] == 1.0f &&
           default_task.raster_tint[2] == 1.0f && default_task.raster_tint[3] == 1.0f);
  }

  vg::core::TaskGraphBuilder builder;
  vg::core::TaskRecord first{}; first.node_index = 1; first.root_allocation = 1;
  vg::core::TaskRecord second{}; second.node_index = 2; second.root_allocation = 1;
  assert(builder.append(first)); assert(builder.append(second));
  assert(builder.add_dependency(0, 1));
  vg::core::TaskGraph graph;
  assert(builder.seal(&graph));
  assert(graph.sealed() && graph.tasks().size() == 2);
  assert(graph.publish());
  assert(graph.published());
  std::string error;
  assert(!builder.append(first, &error) && error == "task graph builder is sealed");

  vg::core::TaskGraphBuilder quota_builder;
  assert(quota_builder.set_quota(1, 4));
  assert(quota_builder.append(first));
  assert(!quota_builder.append(second, &error) && error == "task graph quota overflow");
  vg::core::TaskGraphBuilder payload_quota_builder;
  assert(payload_quota_builder.set_quota(2, 3));
  auto payload_task = first; payload_task.payload_size = 4;
  assert(!payload_quota_builder.append(payload_task, &error) && error == "task payload quota overflow");

  // F4: slot zero is a valid FacetRef index, but a non-zero index with a
  // zero generation is never an issued capability token. Reject it before a
  // backend can disagree on whether depth was requested.
  vg::core::TaskRecord malformed_depth{};
  malformed_depth.kind = vg::core::TaskKind::Raster;
  malformed_depth.depth_attachment_ref = {7, 0};
  malformed_depth.depth_test_enable = true;
  vg::core::TaskGraphBuilder malformed_depth_builder;
  assert(!malformed_depth_builder.append(malformed_depth, &error) &&
         error == "raster depth attachment facet generation must be non-zero");
  vg::core::TaskRecord slot_zero_depth{};
  slot_zero_depth.kind = vg::core::TaskKind::Raster;
  slot_zero_depth.depth_attachment_ref = {0, 1};
  slot_zero_depth.depth_test_enable = true;
  vg::core::TaskGraphBuilder slot_zero_depth_builder;
  assert(slot_zero_depth_builder.append(slot_zero_depth));
  VgSchema_TaskRecord schema_task{};
  schema_task.node.index = 7; schema_task.node.generation = 3; schema_task.root = 11;
  schema_task.shape.x = 2; schema_task.shape.y = 3; schema_task.shape.z = 4;
  schema_task.payload_size = 4; schema_task.payload_or_offset = 99;
  const auto converted = vg::core::task_from_schema(schema_task);
  assert(converted.node_index == 7 && converted.node_generation == 3 && converted.root_allocation == 11);

  vg::core::TaskGraphBuilder effects_builder;
  assert(effects_builder.append(first));
  assert(effects_builder.append(second));
  assert(effects_builder.add_effect(0, {second_allocation.id, 0, 8, vg::ir::Access::Write, 0}));
  assert(effects_builder.add_effect(1, {second_allocation.id, 4, 8, vg::ir::Access::Read, 0}));
  vg::core::TaskGraph effects_graph;
  assert(effects_builder.seal(&effects_graph));
  assert(effects_graph.effect_graph().edges().size() == 1);
  assert(effects_graph.effect_graph().edges().front().kind == vg::core::EffectEdgeKind::InferredConflict);

  // F4: TaskGraphBuilder has no FacetPool to resolve depth attachment backing
  // allocations. It therefore derives a deterministic capability-token write
  // effect; identical depth refs must serialize as a WAW conflict at seal.
  vg::core::TaskRecord depth_writer_a{};
  depth_writer_a.kind = vg::core::TaskKind::Raster;
  depth_writer_a.depth_attachment_ref = {42, 7};
  depth_writer_a.depth_write_enable = true;
  vg::core::TaskRecord depth_writer_b = depth_writer_a;
  vg::core::TaskGraphBuilder depth_effect_builder;
  assert(depth_effect_builder.append(depth_writer_a));
  assert(depth_effect_builder.append(depth_writer_b));
  vg::core::TaskGraph depth_effect_graph;
  assert(depth_effect_builder.seal(&depth_effect_graph));
  assert(depth_effect_graph.effect_graph().edges().size() == 1);
  const auto& depth_edge = depth_effect_graph.effect_graph().edges().front();
  assert(depth_edge.before == 0 && depth_edge.after == 1);
  assert(depth_edge.kind == vg::core::EffectEdgeKind::InferredConflict);

  vg::core::TaskGraphBuilder reads_builder;
  assert(reads_builder.append(first)); assert(reads_builder.append(second));
  assert(reads_builder.add_effect(0, {second_allocation.id, 0, 8, vg::ir::Access::Read, 0}));
  assert(reads_builder.add_effect(1, {second_allocation.id, 0, 8, vg::ir::Access::Read, 0}));
  vg::core::TaskGraph reads_graph;
  assert(reads_builder.seal(&reads_graph));
  assert(reads_graph.effect_graph().edges().empty());
  vg::core::EffectGraph missing_hb;
  std::string hb_error;
  assert(!missing_hb.validate_happens_before(
      {{ {second_allocation.id, 0, 8, vg::ir::Access::Write, 0} },
         { {second_allocation.id, 0, 8, vg::ir::Access::Read, 0} } }, &hb_error));
  assert(hb_error == "conflicting task effects have no happens-before edge");

  vg::core::Timeline timeline;
  assert(timeline.signal(2)); assert(timeline.wait(1));
  assert(timeline.validate_wait(2));
  assert(!timeline.validate_wait(3, &error) && error == "timeline wait point is unsatisfied");
  assert(!timeline.signal(2, &error));

  vg::core::EffectGraph effects;
  assert(effects.add_edge(0, 1)); assert(!effects.add_edge(1, 0) || !effects.valid());
  vg::core::EffectGraph timeline_graph;
  assert(timeline_graph.add_timeline_edge(0, 1, 2, timeline.value()));
  assert(timeline_graph.edges().front().kind == vg::core::EffectEdgeKind::Timeline);
  assert(!timeline_graph.add_timeline_edge(1, 2, 3, timeline.value(), &error));

  vg::core::GraphEpochBuilder epoch_builder(&arena, 7);
  assert(!epoch_builder.add_reference(arena, {allocation.id, allocation.generation}));
  assert(epoch_builder.add_reference(arena, {second_allocation.id, second_allocation.generation}));
  assert(epoch_builder.add_reference(arena, {second_allocation.id, second_allocation.generation}));
  vg::core::GraphEpoch graph_epoch;
  assert(epoch_builder.seal(&graph_epoch));
  assert(graph_epoch.value() == arena.topology_epoch() && graph_epoch.references().size() == 1);
  assert(graph_epoch.contains({second_allocation.id, second_allocation.generation}));
  assert(!epoch_builder.add_reference({2, 1}));
}

}  // namespace vg::tests::core
