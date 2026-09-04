#include "fixture.h"

namespace vg::tests::execution_plan {

void test_submission_lifetime_hold_is_transactional_and_repeatable() {
  vg::core::Arena arena;
  const auto& root = arena.allocate(32);
  const uint64_t root_id = root.id;
  const uint32_t root_generation = root.generation;
  vg::core::FacetPool pool;
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(vg::test_support::assemble_single_node_plan(
      arena, canonical_module(root_id),
      {vg::test_support::compute_task(root_id, root_generation)}, &fixture, &plan, &error));
  CHECK(plan.lifetime_plan_derived);
  CHECK(plan.lifetime_facets.empty());
  CHECK(plan.touched_allocations.size() == 1);  // root/effect dedup

  for (int submit_index = 0; submit_index < 2; ++submit_index) {
    vg::hal::SubmissionLifetimeHold hold;
    CHECK(hold.prepare(plan, arena, pool, &error));
    CHECK(hold.acquire({}, &error));
    CHECK(hold.held());
    CHECK(hold.allocation_count() == 1);
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 1);
    uint32_t ignored_epoch = 0;
    CHECK(!arena.transform(root_id, root_generation, &ignored_epoch, &error));
    CHECK(error == "representation epoch is referenced in flight");
    CHECK(!arena.retire({root_id, root_generation}));
    CHECK(!arena.consume(root_id, root_generation, 0, {true, true, true, true}, &error));
    CHECK(error == "consume requires exclusive ownership");
    hold.release();
    CHECK(!hold.held());
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);
  }

  // A future asynchronous backend may move the owner into a completion
  // object. Moving over an already-held destination releases that
  // destination first, then transfers the source without duplicating either
  // count; explicit release plus both destructors remains exactly-once.
  {
    vg::hal::SubmissionLifetimeHold source_hold;
    vg::hal::SubmissionLifetimeHold destination_hold;
    CHECK(source_hold.prepare(plan, arena, pool, &error));
    CHECK(source_hold.acquire({}, &error));
    CHECK(destination_hold.prepare(plan, arena, pool, &error));
    CHECK(destination_hold.acquire({}, &error));
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 2);
    destination_hold = std::move(source_hold);
    CHECK(!source_hold.held());
    CHECK(destination_hold.held());
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 1);
    vg::hal::SubmissionLifetimeHold completion_hold(std::move(destination_hold));
    CHECK(!destination_hold.held());
    CHECK(completion_hold.held());
    completion_hold.release();
    completion_hold.release();
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);
  }
  CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);

  // prepare() is read-only. If a later identity goes stale, acquire() may
  // already have retained earlier sorted entries; it must roll them all back.
  const auto& second = arena.allocate(32);
  const uint64_t second_id = second.id;
  const uint32_t second_generation = second.generation;
  vg::test_support::AssembledPlanFixture partial_fixture;
  vg::core::ExecutionPlan partial_plan;
  CHECK(vg::test_support::assemble_single_node_plan(
      arena, canonical_module(second_id),
      {vg::test_support::compute_task(root_id, root_generation)},
      &partial_fixture, &partial_plan, &error));
  CHECK(partial_plan.touched_allocations.size() == 2);
  vg::hal::SubmissionLifetimeHold partial;
  CHECK(partial.prepare(partial_plan, arena, pool, &error));
  CHECK(arena.retire({second_id, second_generation}));
  CHECK(!partial.acquire({}, &error));
  CHECK(error.find("stale or retired") != std::string::npos);
  CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);
  CHECK(partial.allocation_count() == 0);
  CHECK(partial.facet_count() == 0);
}

void test_submission_lifetime_hold_deduplicates_facets_and_backing() {
  vg::core::Arena arena;
  const auto& source = arena.allocate(4);
  const auto& target = arena.allocate(4);
  const auto& vertex = arena.allocate(12);  // Address view is a 4-byte prefix.
  vg::core::FacetPool pool;
  vg::core::FacetRef source_ref{};
  vg::core::FacetRef target_ref{};
  vg::core::FacetRef vertex_ref{};
  std::string error;
  CHECK(pool.acquire(arena, rgba_view(source), vg::core::FacetKind::Sample,
                     &source_ref, &error));
  CHECK(pool.acquire(arena, rgba_view(target), vg::core::FacetKind::Attachment,
                     &target_ref, &error));
  CHECK(pool.acquire(arena, rgba_view(vertex), vg::core::FacetKind::Address,
                     &vertex_ref, &error));

  vg::core::TaskRecord raster;
  raster.kind = vg::core::TaskKind::Raster;
  raster.raster_facets = {source_ref, target_ref};
  raster.vertex_buffer_ref = vertex_ref;
  const vg::ir::UserRasterShaderContract shader{
      "vg.test.raster/v1", "vertex_main", "fragment_main",
      vg::ir::kRasterVertexAbiXyzuvPackedV1, "kernel source is opaque to core"};
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions raster_options;
  raster_options.facet_pool = &pool;
  CHECK(vg::test_support::assemble_single_user_raster_plan(
      arena, shader, {raster, raster}, &fixture, &plan, &error, raster_options));
  CHECK(plan.lifetime_plan_derived);
  CHECK(plan.lifetime_facets.size() == 3);  // two Tasks, one capability set
  CHECK(plan.task_effects.size() == 2);
  CHECK(plan.task_facet_uses.size() == 2);
  CHECK(plan.task_facet_uses[0].size() == 3);
  CHECK(plan.task_facet_uses[1].size() == 3);
  CHECK(plan.task_facet_uses[0][0].ref.index == source_ref.index);
  CHECK(plan.task_facet_uses[0][0].kind == vg::core::FacetKind::Sample);
  CHECK(plan.task_facet_uses[0][0].view.allocation == source.id);
  CHECK(plan.task_facet_uses[0][1].kind == vg::core::FacetKind::Attachment);
  CHECK(plan.task_facet_uses[0][2].kind == vg::core::FacetKind::Address);
  CHECK(plan.task_effects[0].size() == 3);  // source read, target write, vertex read
  CHECK(plan.task_effects[1].size() == 3);
  CHECK(plan.task_effects[0][2].size == vertex.size);
  CHECK(plan.validated_effect_graph.edges().size() == 1);
  CHECK(plan.validated_effect_graph.edges()[0].kind ==
        vg::core::EffectEdgeKind::InferredConflict);
  CHECK(plan.task_order == std::vector<uint32_t>({0, 1}));
  CHECK(plan.execution_schedule_derived);
  CHECK(plan.execution_schedule.components.size() == 1);
  CHECK(plan.execution_schedule.components[0].waves.size() == 2);
  CHECK(plan.execution_schedule.components[0].waves[0].tasks ==
        std::vector<uint32_t>({0}));
  CHECK(plan.execution_schedule.components[0].waves[1].tasks ==
        std::vector<uint32_t>({1}));
  CHECK(plan.execution_schedule.structural_successors[0] ==
        std::vector<uint32_t>({1}));
  CHECK(plan.resolved_nodes.size() == 1);
  CHECK(plan.resolved_nodes[0].execution_domain == vg::core::TaskKind::Raster);

  auto schedule_tampered = plan;
  schedule_tampered.execution_schedule.components[0].waves[0].tasks[0] = 1;
  CHECK(!schedule_tampered.validate(&error));
  CHECK(error == "assembled execution plan semantic facts disagree with the immutable assembler seal");

  auto transition_tampered = plan;
  transition_tampered.execution_schedule.transitions.back().requires_execution_completion = false;
  CHECK(!transition_tampered.validate(&error));
  CHECK(error == "assembled execution plan semantic facts disagree with the immutable assembler seal");

  auto successor_tampered = plan;
  successor_tampered.execution_schedule.structural_successors[0].clear();
  CHECK(!successor_tampered.validate(&error));
  CHECK(error == "assembled execution plan semantic facts disagree with the immutable assembler seal");

  auto domain_tampered = plan;
  domain_tampered.resolved_nodes[0].execution_domain = vg::core::TaskKind::Compute;
  CHECK(!domain_tampered.validate(&error));
  CHECK(error == "resolved Node execution domain disagrees with its Tasks or contract");

  auto facet_tampered = plan;
  ++facet_tampered.task_facet_uses[0][0].view.width;
  CHECK(!facet_tampered.validate(&error));
  CHECK(error == "assembled execution plan semantic facts disagree with the immutable assembler seal");
  auto raster_tampered = plan;
  --raster_tampered.task_effects[0][1].size;
  --raster_tampered.task_effects[1][1].size;
  raster_tampered.instantiated_effects.clear();
  for (uint32_t task_index : raster_tampered.task_order)
    raster_tampered.instantiated_effects.insert(
        raster_tampered.instantiated_effects.end(),
        raster_tampered.task_effects[task_index].begin(),
        raster_tampered.task_effects[task_index].end());
  raster_tampered.certificate.ranges = raster_tampered.instantiated_effects;
  CHECK(!raster_tampered.validate(&error));
  CHECK(error == "assembled execution plan semantic facts disagree with the immutable assembler seal");

  // A NodeRef owns exactly one execution domain even though a complete plan
  // may now contain both domains through distinct NodeRefs.
  vg::test_support::AssembledPlanFixture mixed_fixture;
  vg::core::ExecutionPlan mixed_plan;
  auto compute = vg::test_support::compute_task(source.id, source.generation);
  vg::test_support::AssemblyOptions mixed_options;
  mixed_options.facet_pool = &pool;
  error.clear();
  CHECK(!vg::test_support::assemble_single_node_plan(
      arena, canonical_module(source.id), {compute, raster}, &mixed_fixture,
      &mixed_plan, &error, mixed_options));
  CHECK(error == "one resolved NodeRef is used by multiple execution domains");

  const auto expect_raster_assembly_rejection = [&](vg::core::TaskRecord invalid,
                                                     const char* expected) {
    vg::test_support::AssembledPlanFixture invalid_fixture;
    vg::core::ExecutionPlan invalid_plan;
    error.clear();
    CHECK(!vg::test_support::assemble_single_user_raster_plan(
        arena, shader, {invalid}, &invalid_fixture, &invalid_plan, &error,
        raster_options));
    CHECK(error == expected);
  };
  auto missing_source = raster;
  missing_source.raster_facets.source = {};
  expect_raster_assembly_rejection(missing_source, "raster source facet is required");
  auto missing_target = raster;
  missing_target.raster_facets.target = {};
  expect_raster_assembly_rejection(missing_target, "raster target facet is required");
  auto missing_vertex = raster;
  missing_vertex.vertex_buffer_ref = {};
  expect_raster_assembly_rejection(missing_vertex, "raster vertex facet is required");
  auto missing_index = raster;
  missing_index.index_count = 3;
  expect_raster_assembly_rejection(
      missing_index, "raster index facet is required when index_count is non-zero");
  auto missing_depth = raster;
  missing_depth.depth_test_enable = true;
  expect_raster_assembly_rejection(
      missing_depth, "raster depth state requires a depth attachment facet");
  vg::test_support::AssembledPlanFixture no_pool_fixture;
  vg::core::ExecutionPlan no_pool_plan;
  error.clear();
  CHECK(!vg::test_support::assemble_single_user_raster_plan(
      arena, shader, {raster}, &no_pool_fixture, &no_pool_plan, &error));
  CHECK(error ==
        "raster semantic assembly requires the submitting FacetPool to resolve raster source facet");

  // An unrelated caller may already retain the facet. The plan adds exactly
  // one independent use and returns the external count unchanged.
  CHECK(pool.begin_gpu_use(arena, source_ref, &error));
  vg::hal::SubmissionLifetimeHold hold;
  CHECK(hold.prepare(plan, arena, pool, &error));
  CHECK(hold.acquire({}, &error));
  CHECK(hold.facet_count() == 3);
  CHECK(hold.allocation_count() == 3);
  CHECK(pool.in_flight(source_ref) == 2);
  CHECK(pool.in_flight(target_ref) == 1);
  CHECK(pool.in_flight(vertex_ref) == 1);

  // Retiring a token invalidates it immediately, but its slot must not be
  // reused while this submission still owns a GPU-use count.
  CHECK(pool.retire(target_ref, &error));
  vg::core::FacetRef while_held{};
  CHECK(pool.acquire(arena, rgba_view(target), vg::core::FacetKind::Attachment,
                     &while_held, &error));
  CHECK(while_held.index != target_ref.index);
  hold.release();
  CHECK(pool.in_flight(source_ref) == 1);
  CHECK(pool.in_flight(vertex_ref) == 0);
  CHECK(pool.end_gpu_use(source_ref, &error));
  CHECK(pool.in_flight(source_ref) == 0);
  vg::hal::SubmissionLifetimeHold stale_facet_hold;
  CHECK(!stale_facet_hold.prepare(plan, arena, pool, &error));
  CHECK(error.find("retired") != std::string::npos);

  // The task's source token names the pre-transform epoch. Since a concrete
  // target token is not available until the physical operation publishes it,
  // the same submit cannot silently transform that backing and then execute
  // through the now-stale TaskRecord token.
  vg::core::RepresentationRequest conflicting_transform;
  conflicting_transform.view = rgba_view(source);
  conflicting_transform.target_kind = vg::core::FacetKind::Storage;
  const std::vector<vg::core::RepresentationRequest> transforms{conflicting_transform};
  vg::test_support::AssemblyOptions options;
  options.representation_requests = &transforms;
  options.facet_pool = &pool;
  vg::test_support::AssembledPlanFixture conflicting_fixture;
  vg::core::ExecutionPlan conflicting_plan;
  auto live_raster = raster;
  live_raster.raster_facets.target = while_held;
  CHECK(vg::test_support::assemble_single_user_raster_plan(
      arena, shader, {live_raster}, &conflicting_fixture, &conflicting_plan, &error, options));
  CHECK(conflicting_plan.execution_schedule.transitions.size() == 1);
  CHECK(conflicting_plan.execution_schedule.transitions[0].before_wave ==
        vg::core::kExecutionSchedulePrelude);
  CHECK(conflicting_plan.execution_schedule.transitions[0].representation_operations ==
        std::vector<uint32_t>({0}));
  vg::hal::SubmissionLifetimeHold conflicting_hold;
  CHECK(!conflicting_hold.prepare(conflicting_plan, arena, pool, &error));
  CHECK(error ==
        "a same-submit representation transform would invalidate a sealed Task FacetRef before Stage 7");
  CHECK(arena.lookup(vg::core::PointerRef{source.id, source.generation})->representation_epoch == 0);

  // The SceneRoot schema moves source authority into root.material.albedo.
  // Seal one plan that also uses every optional explicit raster capability so
  // the inventory proves all five Task fields plus the indirect albedo path.
  auto& scene_root = arena.allocate(VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE);
  auto& albedo = arena.allocate(4);
  auto& scene_target = arena.allocate(4);
  auto& scene_vertex = arena.allocate(4);
  auto& scene_index = arena.allocate(6);
  auto& scene_depth = arena.allocate(4);
  vg::core::FacetRef albedo_ref{};
  vg::core::FacetRef scene_target_ref{};
  vg::core::FacetRef scene_vertex_ref{};
  vg::core::FacetRef scene_index_ref{};
  vg::core::FacetRef scene_depth_ref{};
  CHECK(pool.acquire(arena, rgba_view(albedo), vg::core::FacetKind::Sample,
                     &albedo_ref, &error));
  CHECK(pool.acquire(arena, rgba_view(scene_target), vg::core::FacetKind::Attachment,
                     &scene_target_ref, &error));
  CHECK(pool.acquire(arena, rgba_view(scene_vertex), vg::core::FacetKind::Address,
                     &scene_vertex_ref, &error));
  auto index_view = rgba_view(scene_index);
  index_view.format = vg::core::PixelFormat::R16Uint;
  CHECK(pool.acquire(arena, index_view, vg::core::FacetKind::Address,
                     &scene_index_ref, &error));
  auto depth_view = rgba_view(scene_depth);
  depth_view.format = vg::core::PixelFormat::Depth32Float;
  CHECK(pool.acquire(arena, depth_view, vg::core::FacetKind::Attachment,
                     &scene_depth_ref, &error));

  VgSchemaLayout_SceneRootRaster scene_bytes{};
  scene_bytes.camera_clip_from_local[0] = 1.0f;
  scene_bytes.camera_clip_from_local[5] = 1.0f;
  scene_bytes.camera_clip_from_local[10] = 1.0f;
  scene_bytes.camera_clip_from_local[15] = 1.0f;
  scene_bytes.material.base_color[0] = 1.0f;
  scene_bytes.material.base_color[1] = 1.0f;
  scene_bytes.material.base_color[2] = 1.0f;
  scene_bytes.material.base_color[3] = 1.0f;
  scene_bytes.material.albedo.index = albedo_ref.index;
  scene_bytes.material.albedo.generation = albedo_ref.generation;
  std::memcpy(scene_root.bytes.data(), &scene_bytes, sizeof(scene_bytes));

  vg::core::TaskRecord scene_task;
  scene_task.kind = vg::core::TaskKind::Raster;
  scene_task.root_allocation = scene_root.id;
  scene_task.root_generation = scene_root.generation;
  scene_task.raster_facets.target = scene_target_ref;
  scene_task.vertex_buffer_ref = scene_vertex_ref;
  scene_task.index_buffer_ref = scene_index_ref;
  scene_task.index_count = 3;
  scene_task.depth_attachment_ref = scene_depth_ref;
  scene_task.depth_test_enable = true;
  scene_task.depth_write_enable = true;
  const vg::ir::UserRasterShaderContract scene_shader{
      VG_SCHEMA_SCENEROOTRASTER_CONTRACT_NAME, "vertex_main", "fragment_main",
      vg::ir::kRasterVertexAbiXyzuvPackedV1, "kernel source is opaque to core"};
  vg::test_support::AssembledPlanFixture scene_fixture;
  vg::core::ExecutionPlan scene_plan;
  CHECK(vg::test_support::assemble_single_user_raster_plan(
      arena, scene_shader, {scene_task}, &scene_fixture, &scene_plan, &error,
      raster_options));
  CHECK(scene_plan.lifetime_facets.size() == 5);
  CHECK(scene_plan.task_facet_uses.size() == 1);
  CHECK(scene_plan.task_facet_uses[0].size() == 6);  // depth contributes read + write
  CHECK(scene_plan.task_facet_uses[0][0].ref.index == albedo_ref.index);
  CHECK(scene_plan.task_facet_uses[0][0].view.allocation == albedo.id);
  CHECK(std::ranges::any_of(scene_plan.task_effects[0], [&](const auto& effect) {
    return effect.allocation == scene_index.id && effect.size == 6 &&
           effect.access == vg::ir::Access::Read;
  }));
  CHECK(std::ranges::any_of(scene_plan.task_effects[0], [&](const auto& effect) {
    return effect.allocation == scene_depth.id &&
           effect.access == vg::ir::Access::Read;
  }));
  CHECK(std::ranges::any_of(scene_plan.task_effects[0], [&](const auto& effect) {
    return effect.allocation == scene_depth.id &&
           effect.access == vg::ir::Access::Write;
  }));
  const auto contains_lifetime_use = [&](vg::core::FacetRef ref, vg::core::FacetKind kind) {
    return std::ranges::any_of(scene_plan.lifetime_facets, [&](const auto& use) {
      return use.ref.index == ref.index && use.ref.generation == ref.generation && use.kind == kind;
    });
  };
  CHECK(contains_lifetime_use(albedo_ref, vg::core::FacetKind::Sample));
  CHECK(contains_lifetime_use(scene_target_ref, vg::core::FacetKind::Attachment));
  CHECK(contains_lifetime_use(scene_vertex_ref, vg::core::FacetKind::Address));
  CHECK(contains_lifetime_use(scene_index_ref, vg::core::FacetKind::Address));
  CHECK(contains_lifetime_use(scene_depth_ref, vg::core::FacetKind::Attachment));
  vg::hal::SubmissionLifetimeHold scene_hold;
  CHECK(scene_hold.prepare(scene_plan, arena, pool, &error));
  CHECK(scene_hold.acquire({}, &error));
  CHECK(scene_hold.facet_count() == 5);
  CHECK(scene_hold.allocation_count() == 6);  // root plus five distinct facet backings
  scene_hold.release();
  for (const auto ref : {albedo_ref, scene_target_ref, scene_vertex_ref,
                         scene_index_ref, scene_depth_ref})
    CHECK(pool.in_flight(ref) == 0);
}

void test_representation_outputs_join_lifetime_after_physical_stage() {
  vg::core::Arena arena;
  const auto& probe = arena.allocate(32);
  const auto& image = arena.allocate(4);
  const uint64_t image_id = image.id;
  const uint32_t image_generation = image.generation;
  vg::core::FacetPool pool;
  vg::core::RepresentationRequest request;
  request.view = rgba_view(image);
  request.target_kind = vg::core::FacetKind::Sample;
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(assemble_representation_case(arena, probe.id, probe.generation, {request},
                                     pool, &plan, &error));
  CHECK(plan.touched_allocations.size() == 2);  // probe + representation target

  vg::hal::CompiledPlan compiled;
  compiled.representation_operations.push_back({
      vg::hal::CompiledPlan::RepresentationOperation::Identity, 0, "unit physical operation"});
  vg::hal::Submission submission;
  vg::hal::SubmissionLifetimeHold hold;
  CHECK(hold.prepare(plan, arena, pool, &error));
  CHECK(vg::hal::commit_representation_operations(
      plan, compiled.representation_operations, arena, pool,
      [](const vg::core::RepresentationSemanticPlanItem& request,
         const vg::hal::CompiledPlan::PhysicalRepresentationOperation&,
         vg::core::FacetRef, vg::hal::RepresentationTransformCost* cost,
         std::string*) {
        cost->new_backing_bytes = request.view.byte_size();
        return true;
      },
      &submission, &error));
  CHECK(submission.representation_facets.size() == 1);
  const auto target_facet = submission.representation_facets.front();
  CHECK(hold.acquire(submission.representation_facets, &error));
  CHECK(hold.allocation_count() == 2);
  CHECK(hold.facet_count() == 1);
  CHECK(arena.lookup(vg::core::PointerRef{image_id, image_generation})->in_flight == 1);
  CHECK(pool.in_flight(target_facet) == 1);
  uint32_t ignored_epoch = 0;
  CHECK(!arena.transform(image_id, image_generation, &ignored_epoch, &error));
  CHECK(error == "representation epoch is referenced in flight");
  hold.release();
  CHECK(arena.lookup(vg::core::PointerRef{image_id, image_generation})->in_flight == 0);
  CHECK(pool.in_flight(target_facet) == 0);

  // A physical failure happens before hold acquisition. It may have published
  // an epoch (the transform is not transactional), but it must never leak an
  // allocation/facet use count into the next submission.
  const auto& failing_image = arena.allocate(4);
  const uint64_t failing_id = failing_image.id;
  vg::core::RepresentationRequest failing_request;
  failing_request.view = rgba_view(failing_image);
  failing_request.target_kind = vg::core::FacetKind::Storage;
  vg::core::ExecutionPlan failing_plan;
  CHECK(assemble_representation_case(arena, probe.id, probe.generation, {failing_request},
                                     pool, &failing_plan, &error));
  vg::hal::SubmissionLifetimeHold failing_hold;
  CHECK(failing_hold.prepare(failing_plan, arena, pool, &error));
  vg::hal::Submission failed_submission;
  CHECK(!vg::hal::commit_representation_operations(
      failing_plan, compiled.representation_operations, arena, pool,
      [](const vg::core::RepresentationSemanticPlanItem&,
         const vg::hal::CompiledPlan::PhysicalRepresentationOperation&,
         vg::core::FacetRef, vg::hal::RepresentationTransformCost*,
         std::string* physical_error) {
        if (physical_error) *physical_error = "injected physical failure";
        return false;
      },
      &failed_submission, &error));
  CHECK(error.find("injected physical failure") != std::string::npos);
  CHECK(arena.lookup(vg::core::PointerRef{failing_id, failing_image.generation})->in_flight == 0);
  CHECK(!failing_hold.held());
}

}  // namespace vg::tests::execution_plan
