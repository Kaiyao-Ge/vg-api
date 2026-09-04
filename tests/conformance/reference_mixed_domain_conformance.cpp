#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "compiler/compiler.h"
#include "assembled_plan_fixture.h"
#include "mixed_continuation_contract.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
uint64_t event_count(const vg::hal::LoweringReport& report, const char* operation) {
  uint64_t count = 0;
  for (const auto& event : report.events)
    if (event.operation == operation) count += event.count;
  return count;
}

vg::core::CanonicalView view(uint64_t allocation, uint32_t width, uint32_t height) {
  vg::core::CanonicalView result;
  result.allocation = allocation;
  result.allocation_generation = 1;
  result.format = vg::core::PixelFormat::RGBA8Unorm;
  result.dimension = vg::core::ViewDimension::Texture2D;
  result.width = width;
  result.height = height;
  return result;
}

vg::ir::Module compute_store(const vg::core::Allocation& allocation) {
  auto compiled = vg::compiler::compile_c_like("@node @effects store(1,0,4,0)");
  assert(compiled.ok);
  auto result = std::move(compiled.module);
  result.instructions[0].allocation = allocation.id;
  result.instructions[0].generation = allocation.generation;
  result.instructions[0].representation_epoch = allocation.representation_epoch;
  result.declared_effects[0].allocation = allocation.id;
  result.declared_effects[0].representation_epoch = allocation.representation_epoch;
  result.canonical_json = vg::ir::serialize_module(result);
  result.hash = vg::ir::sha256_hex(result.canonical_json);
  return result;
}

vg::ir::Module compute_store_value(const vg::core::Allocation& allocation, int64_t value) {
  auto result = compute_store(allocation);
  result.instructions[0].value = value;
  result.canonical_json = vg::ir::serialize_module(result);
  result.hash = vg::ir::sha256_hex(result.canonical_json);
  return result;
}

vg::ir::Module atomic_add(const vg::core::Allocation& allocation, uint64_t offset = 0) {
  auto compiled = vg::compiler::compile_c_like("@node @effects atomic_add(1,0,8,1)");
  assert(compiled.ok);
  auto result = std::move(compiled.module);
  result.instructions[0].allocation = allocation.id;
  result.instructions[0].generation = allocation.generation;
  result.instructions[0].representation_epoch = allocation.representation_epoch;
  result.instructions[0].offset = offset;
  result.declared_effects[0].allocation = allocation.id;
  result.declared_effects[0].offset = offset;
  result.declared_effects[0].representation_epoch = allocation.representation_epoch;
  result.canonical_json = vg::ir::serialize_module(result);
  result.hash = vg::ir::sha256_hex(result.canonical_json);
  return result;
}

vg::ir::Module runtime_pointer_fault(const vg::core::Allocation& holder,
                                     const vg::core::Allocation& target) {
  vg::ir::Module result;
  result.version = 1;
  result.root_schema = "vg.root/v1";
  result.instructions = {{"load_ref", holder.id, 0, 12, 0, holder.generation,
                          holder.representation_epoch, 0, ""},
                         {"store_via", target.id, 0, 4, 9, target.generation,
                          target.representation_epoch, 1, ""}};
  result.declared_effects = {{holder.id, 0, 12, vg::ir::Access::Read,
                              holder.representation_epoch}};
  result.declared_pointer_edges = {{holder.id, 0, target.id}};
  result.canonical_json = vg::ir::serialize_module(result);
  result.hash = vg::ir::sha256_hex(result.canonical_json);
  return result;
}

void write_pointer_ref(vg::core::Allocation& holder, const vg::core::PointerRef& ref) {
  std::memcpy(holder.bytes.data(), &ref.allocation, sizeof(ref.allocation));
  std::memcpy(holder.bytes.data() + sizeof(ref.allocation), &ref.generation, sizeof(ref.generation));
}
}  // namespace

void runtime_failure_matrix() {
  // A failed task cancels only its sealed structural descendants.  An
  // independent component still executes and makes the final poison partial.
  {
    auto device = vg::reference::make_device_hal();
    vg::core::Arena arena;
    auto& holder = arena.allocate(16); auto& pointed = arena.allocate(16);
    auto& descendant = arena.allocate(16); auto& independent = arena.allocate(16);
    write_pointer_ref(holder, {pointed.id, pointed.generation});
    vg::test_support::MultiNodePlanFixture fixture; vg::core::ExecutionPlan plan; std::string error;
    assert(vg::test_support::assemble_multi_node_plan(
        arena, {runtime_pointer_fault(holder, pointed), compute_store_value(descendant, 7), compute_store_value(independent, 9)},
        {vg::test_support::compute_task(holder.id, holder.generation),
         vg::test_support::compute_task(descendant.id, descendant.generation),
         vg::test_support::compute_task(independent.id, independent.generation)}, {{0, 1}},
        &fixture, &plan, &error));
    vg::hal::CompiledPlan compiled; assert(device->compile(plan, &compiled, &error));
    const uint32_t stale = pointed.generation + 1;
    std::memcpy(holder.bytes.data() + sizeof(uint64_t), &stale, sizeof(stale));
    vg::hal::Submission submission; assert(device->submit(compiled, arena, &submission, &error));
    assert(!submission.result.ok && submission.result.poison == vg::core::PoisonState::PartiallyProduced);
    assert(submission.result.fault.task_index == 0);
    assert(descendant.bytes[0] == 0 && independent.bytes[0] == 9);
    assert(compiled.report.transition_serialized_fallback_count == 1);
    assert(submission.report.transition_serialized_fallback_count == 0);
    assert(event_count(compiled.report, "schedule_program_order") == 3);
    assert(event_count(submission.report, "schedule_program_order") == 2);
    assert(submission.published_tasks.size() == plan.execution_schedule.task_order.size());
    for (size_t i = 0; i < submission.published_tasks.size(); ++i)
      assert(submission.published_tasks[i].node_index ==
             plan.task_graph.tasks()[plan.execution_schedule.task_order[i]].node_index);
  }
  // With no preceding or independent output, the same genuine runtime fault
  // is Poisoned rather than PartiallyProduced.
  {
    auto device = vg::reference::make_device_hal(); vg::core::Arena arena;
    auto& holder = arena.allocate(16); auto& pointed = arena.allocate(16);
    auto& other_holder = arena.allocate(16); auto& other_pointed = arena.allocate(16);
    write_pointer_ref(holder, {pointed.id, pointed.generation});
    write_pointer_ref(other_holder, {other_pointed.id, other_pointed.generation});
    vg::test_support::MultiNodePlanFixture fixture; vg::core::ExecutionPlan plan; std::string error;
    assert(vg::test_support::assemble_multi_node_plan(arena,
        {runtime_pointer_fault(holder, pointed), runtime_pointer_fault(other_holder, other_pointed)},
        {vg::test_support::compute_task(holder.id, holder.generation),
         vg::test_support::compute_task(other_holder.id, other_holder.generation)}, {}, &fixture, &plan, &error));
    vg::hal::CompiledPlan compiled; assert(device->compile(plan, &compiled, &error));
    const uint32_t stale = pointed.generation + 1;
    std::memcpy(holder.bytes.data() + sizeof(uint64_t), &stale, sizeof(stale));
    const uint32_t other_stale = other_pointed.generation + 1;
    std::memcpy(other_holder.bytes.data() + sizeof(uint64_t), &other_stale, sizeof(other_stale));
    vg::hal::Submission submission; assert(device->submit(compiled, arena, &submission, &error));
    assert(!submission.result.ok && submission.result.poison == vg::core::PoisonState::Poisoned);
    assert(submission.result.fault.task_index == 0);
    assert(submission.report.transition_serialized_fallback_count == 0);
    assert(event_count(submission.report, "schedule_program_order") == 2);
  }
  // Component A is executed first (0 -> 3), so its fault is observed before
  // B's task 1 fault.  Canonical schedule rank, not observation order, picks
  // task 1 as the public primary fault; B's task 2 is cancelled.
  {
    auto device = vg::reference::make_device_hal(); vg::core::Arena arena;
    auto& output = arena.allocate(16); auto& a_holder = arena.allocate(16); auto& a_target = arena.allocate(16);
    auto& b_holder = arena.allocate(16); auto& b_target = arena.allocate(16); auto& b_descendant = arena.allocate(16);
    write_pointer_ref(a_holder, {a_target.id, a_target.generation});
    write_pointer_ref(b_holder, {b_target.id, b_target.generation});
    vg::test_support::MultiNodePlanFixture fixture; vg::core::ExecutionPlan plan; std::string error;
    assert(vg::test_support::assemble_multi_node_plan(arena,
        {compute_store_value(output, 5), runtime_pointer_fault(b_holder, b_target), compute_store_value(b_descendant, 7), runtime_pointer_fault(a_holder, a_target)},
        {vg::test_support::compute_task(output.id, output.generation), vg::test_support::compute_task(b_holder.id, b_holder.generation),
         vg::test_support::compute_task(b_descendant.id, b_descendant.generation), vg::test_support::compute_task(a_holder.id, a_holder.generation)},
        {{0, 3}, {1, 2}}, &fixture, &plan, &error));
    vg::hal::CompiledPlan compiled; assert(device->compile(plan, &compiled, &error));
    uint32_t stale = a_target.generation + 1; std::memcpy(a_holder.bytes.data() + sizeof(uint64_t), &stale, sizeof(stale));
    stale = b_target.generation + 1; std::memcpy(b_holder.bytes.data() + sizeof(uint64_t), &stale, sizeof(stale));
    vg::hal::Submission submission; assert(device->submit(compiled, arena, &submission, &error));
    assert(!submission.result.ok && submission.result.fault.task_index == 1);
    assert(b_descendant.bytes[0] == 0);
    assert(compiled.report.transition_serialized_fallback_count == 2);
    assert(submission.report.transition_serialized_fallback_count == 1);
    assert(event_count(submission.report, "schedule_program_order") == 3);
  }
}

void independent_compute_raster_components_and_publication() {
  // This deliberately goes through NodeTable, TaskGraph, Envelope and the
  // public assembler fixture: no sealed schedule or package is hand-stamped.
  auto device = vg::reference::make_device_hal();
  vg::core::Arena arena;
  auto& source = arena.allocate(16);
  auto& target = arena.allocate(16);
  auto& compute_target = arena.allocate(16);
  std::memset(source.bytes.data(), 255, source.bytes.size());
  const auto source_view = view(source.id, 2, 2);
  const auto target_view = view(target.id, 2, 2);
  std::string error;
  vg::core::FacetRef source_ref, target_ref, vertices_ref;
  assert(device->facet_pool().acquire(arena, source_view, vg::core::FacetKind::Sample,
                                      &source_ref, &error));
  assert(device->facet_pool().acquire(arena, target_view, vg::core::FacetKind::Attachment,
                                      &target_ref, &error));
  const std::array<vg::reference::RasterVertex, 3> triangle{{
      {-1.f, -1.f, 0.f, 0.f, 0.f}, {3.f, -1.f, 0.f, 0.f, 0.f}, {-1.f, 3.f, 0.f, 0.f, 0.f}}};
  auto& vertices = arena.allocate(sizeof(triangle));
  std::memcpy(vertices.bytes.data(), triangle.data(), sizeof(triangle));
  auto vertex_view = view(vertices.id, static_cast<uint32_t>(sizeof(triangle) / 4), 1);
  assert(device->facet_pool().acquire(arena, vertex_view, vg::core::FacetKind::Address,
                                      &vertices_ref, &error));

  vg::core::TaskRecord compute = vg::test_support::compute_task(compute_target.id, compute_target.generation);
  vg::core::TaskRecord raster;
  raster.kind = vg::core::TaskKind::Raster;
  raster.raster_facets = {source_ref, target_ref};
  raster.vertex_buffer_ref = vertices_ref;
  raster.raster_filter = vg::core::FilterMode::Nearest;
  raster.raster_wrap = vg::core::WrapMode::Clamp;
  vg::ir::Module raster_module;
  raster_module.version = 1;
  raster_module.root_schema = "vg.test/v1";
  vg::ir::Instruction load;
  load.op = "load";
  load.allocation = source.id;
  load.generation = source.generation;
  load.representation_epoch = source.representation_epoch;
  load.size = 4;
  raster_module.instructions.push_back(load);
  raster_module.declared_effects.push_back({source.id, 0, 16, vg::ir::Access::Read,
                                            source.representation_epoch});
  vg::test_support::MultiNodePlanFixture fixture;
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions options;
  options.facet_pool = &device->facet_pool();
  if (!vg::test_support::assemble_multi_node_plan(
      arena, {compute_store_value(compute_target, 7), raster_module}, {compute, raster}, {},
      &fixture, &plan, &error, options)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    std::abort();
  }
  assert(plan.execution_schedule.components.size() == 2);
  assert(plan.execution_schedule.task_order.size() == 2);
  vg::hal::CompiledPlan compiled;
  assert(device->compile(plan, &compiled, &error));
  assert(compiled.report.transition_serialized_fallback_count == 0);
  vg::hal::Submission submission;
  assert(device->submit(compiled, arena, &submission, &error));
  assert(submission.result.ok);
  assert(submission.raster_results.size() == 1);
  assert(compute_target.bytes[0] == 7);
  assert(submission.raster_results[0].stored);
  assert(submission.published_tasks.size() == 2);
  for (size_t rank = 0; rank < submission.published_tasks.size(); ++rank) {
    const auto task_index = plan.execution_schedule.task_order[rank];
    assert(submission.published_tasks[rank].node_index == plan.task_graph.tasks()[task_index].node_index);
    assert(submission.published_tasks[rank].kind == plan.task_graph.tasks()[task_index].kind);
  }
  // A compiled mixed schedule remains reusable: all allocation and facet
  // lifetime holds from the completed first submit must have been discharged.
  assert(arena.lookup(vg::core::PointerRef{source.id, source.generation})->in_flight == 0);
  assert(arena.lookup(vg::core::PointerRef{target.id, target.generation})->in_flight == 0);
  assert(arena.lookup(vg::core::PointerRef{compute_target.id, compute_target.generation})->in_flight == 0);
  assert(device->facet_pool().in_flight(source_ref) == 0);
  assert(device->facet_pool().in_flight(target_ref) == 0);
  assert(device->facet_pool().in_flight(vertices_ref) == 0);
  vg::hal::Submission repeated;
  assert(device->submit(compiled, arena, &repeated, &error));
  assert(repeated.result.ok);
  assert(arena.lookup(vg::core::PointerRef{source.id, source.generation})->in_flight == 0);
  assert(arena.lookup(vg::core::PointerRef{target.id, target.generation})->in_flight == 0);
  assert(arena.lookup(vg::core::PointerRef{compute_target.id, compute_target.generation})->in_flight == 0);
}

void compute_to_raster_visibility() {
  auto device = vg::reference::make_device_hal();
  vg::core::Arena arena;
  auto& source = arena.allocate(16); auto& target = arena.allocate(16);
  std::memset(source.bytes.data(), 0, source.bytes.size());
  std::string error; vg::core::FacetRef sample, attachment, vertex;
  assert(device->facet_pool().acquire(arena, view(source.id, 2, 2), vg::core::FacetKind::Sample, &sample, &error));
  assert(device->facet_pool().acquire(arena, view(target.id, 2, 2), vg::core::FacetKind::Attachment, &attachment, &error));
  const std::array<vg::reference::RasterVertex, 3> triangle{{
      {-1.f, -1.f, 0.f, 0.f, 0.f}, {3.f, -1.f, 0.f, 2.f, 0.f}, {-1.f, 3.f, 0.f, 0.f, 2.f}}};
  auto& vertices = arena.allocate(sizeof(triangle)); std::memcpy(vertices.bytes.data(), triangle.data(), sizeof(triangle));
  assert(device->facet_pool().acquire(arena, view(vertices.id, sizeof(triangle) / 4, 1), vg::core::FacetKind::Address, &vertex, &error));
  vg::core::TaskRecord raster; raster.kind = vg::core::TaskKind::Raster;
  raster.raster_facets = {sample, attachment}; raster.vertex_buffer_ref = vertex;
  raster.raster_filter = vg::core::FilterMode::Nearest; raster.raster_wrap = vg::core::WrapMode::Clamp;
  vg::ir::Module raster_module; raster_module.version = 1; raster_module.root_schema = "vg.test/v1";
  raster_module.instructions.push_back({"load", source.id, 0, 4, 0, source.generation, source.representation_epoch, 0, ""});
  raster_module.declared_effects.push_back({source.id, 0, 16, vg::ir::Access::Read, source.representation_epoch});
  vg::test_support::MultiNodePlanFixture fixture; vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions options; options.facet_pool = &device->facet_pool();
  assert(vg::test_support::assemble_multi_node_plan(arena, {compute_store_value(source, 7), raster_module},
      {vg::test_support::compute_task(source.id, source.generation), raster}, {{0, 1}}, &fixture, &plan, &error, options));
  vg::hal::CompiledPlan compiled; assert(device->compile(plan, &compiled, &error));
  vg::hal::Submission submission; assert(device->submit(compiled, arena, &submission, &error));
  assert(submission.result.ok && source.bytes[0] == 7 && submission.raster_results.size() == 1);
  assert(submission.report.transition_serialized_fallback_count == 1);
  assert(event_count(submission.report, "schedule_program_order") == 2);
  assert(std::ranges::any_of(submission.raster_results[0].resolved_rgba, [](const auto& pixel) {
    return pixel[0] > 0.0f || pixel[1] > 0.0f || pixel[2] > 0.0f || pixel[3] > 0.0f;
  }));

  // The same sealed dependency has no executed transition when admission
  // exits on an unsatisfied Timeline. Reuse the output object to catch stale
  // execution evidence as well as copied compile-time costs.
  options.timeline_wait = 1000;
  assert(vg::test_support::assemble_multi_node_plan(arena, {compute_store_value(source, 7), raster_module},
      {vg::test_support::compute_task(source.id, source.generation), raster}, {{0, 1}}, &fixture, &plan, &error, options));
  assert(device->compile(plan, &compiled, &error));
  assert(compiled.report.transition_serialized_fallback_count == 1);
  assert(device->submit(compiled, arena, &submission, &error));
  assert(!submission.result.ok && submission.result.fault.code == "TIMELINE_WAIT_UNSATISFIED");
  assert(submission.report.transition_serialized_fallback_count == 0);
  assert(event_count(submission.report, "schedule_program_order") == 0);
  assert(submission.published_tasks.empty() && submission.raster_results.empty());
}

void representation_submission_executes_once() {
  auto device = vg::reference::make_device_hal();
  vg::core::Arena arena;
  auto& compute_output = arena.allocate(16); auto& source = arena.allocate(16);
  auto& target = arena.allocate(16); auto& transform_only = arena.allocate(16);
  auto& vertices = arena.allocate(64);
  std::memset(source.bytes.data(), 255, source.bytes.size());
  const std::array<vg::reference::RasterVertex, 3> triangle{{
      {-1.f, -1.f, 0.f, 0.f, 0.f}, {3.f, -1.f, 0.f, 2.f, 0.f}, {-1.f, 3.f, 0.f, 0.f, 2.f}}};
  std::memcpy(vertices.bytes.data(), triangle.data(), sizeof(triangle));
  std::string error; vg::core::FacetRef sample, attachment, vertex;
  assert(device->facet_pool().acquire(arena, view(source.id, 2, 2), vg::core::FacetKind::Sample, &sample, &error));
  assert(device->facet_pool().acquire(arena, view(target.id, 2, 2), vg::core::FacetKind::Attachment, &attachment, &error));
  assert(device->facet_pool().acquire(arena, view(vertices.id, 16, 1), vg::core::FacetKind::Address, &vertex, &error));
  vg::core::TaskRecord raster; raster.kind = vg::core::TaskKind::Raster;
  raster.raster_facets = {sample, attachment}; raster.vertex_buffer_ref = vertex;
  vg::ir::Module raster_module; raster_module.version = 1; raster_module.root_schema = "vg.test/v1";
  raster_module.instructions.push_back({"load", source.id, 0, 4, 0, source.generation, source.representation_epoch, 0, ""});
  raster_module.declared_effects.push_back({source.id, 0, 16, vg::ir::Access::Read, source.representation_epoch});
  // A Task consumer of this transform's old facet is correctly refused by
  // SR-5 before submit. The unrelated backing here is the legal case. MD-2's
  // contract unit owns the separate proof that multiple transitions can refer
  // to one operation while its execution owner remains unique; this test
  // proves Reference Stage 7 performs that owner exactly once.
  const std::vector<vg::core::RepresentationRequest> requests{{view(transform_only.id, 2, 2),
                                                                 vg::core::FacetKind::Storage}};
  vg::test_support::AssemblyOptions options; options.facet_pool = &device->facet_pool();
  options.representation_requests = &requests;
  vg::test_support::MultiNodePlanFixture fixture; vg::core::ExecutionPlan plan;
  assert(vg::test_support::assemble_multi_node_plan(arena,
      {compute_store_value(compute_output, 7), raster_module},
      {vg::test_support::compute_task(compute_output.id, compute_output.generation), raster}, {},
      &fixture, &plan, &error, options));
  vg::hal::CompiledPlan compiled; assert(device->compile(plan, &compiled, &error));
  assert(compiled.representation_operations.size() == 1);
  assert(compiled.representation_operation_execution_order == std::vector<uint32_t>({0}));
  size_t transition_references = 0;
  for (const auto& transition : compiled.transition_operations)
    for (uint32_t operation : transition.representation_operations)
      if (operation == 0) ++transition_references;
  assert(transition_references == 0 || transition_references == 1);
  assert(std::ranges::any_of(compiled.report.events, [](const auto& event) {
    return event.operation == "representation_transform" &&
           event.classification == vg::hal::LoweringClass::Direct;
  }));
  vg::hal::Submission submission; assert(device->submit(compiled, arena, &submission, &error));
  const auto* transformed = arena.lookup(vg::core::PointerRef{transform_only.id, transform_only.generation});
  assert(transformed != nullptr && transformed->representation_epoch == 1);
  assert(submission.representation_facets.size() == 1);
}

void raster_to_compute_visibility() {
  // First execute exactly the same raster fixture in an independent Arena.
  // Rather than assuming where software rasterization stores its first covered
  // pixel, find a real nonzero aligned backing word and use it as the consumer
  // atomic location in the mixed submission below.
  const auto run = [](bool include_compute, uint64_t atomic_offset, uint64_t* observed) {
    auto device = vg::reference::make_device_hal();
    vg::core::Arena arena;
    auto& source = arena.allocate(64); auto& target = arena.allocate(64);
    const auto target_ref = vg::core::PointerRef{target.id, target.generation};
    std::memset(source.bytes.data(), 255, source.bytes.size());
    std::string error; vg::core::FacetRef sample, attachment, vertices_ref;
    assert(device->facet_pool().acquire(arena, view(source.id, 4, 4), vg::core::FacetKind::Sample, &sample, &error));
    assert(device->facet_pool().acquire(arena, view(target.id, 4, 4), vg::core::FacetKind::Attachment, &attachment, &error));
    const std::array<vg::reference::RasterVertex, 6> triangle{{
        {-1,1,0,0,0},{1,1,0,1,0},{-1,-1,0,0,1},
        {1,1,0,1,0},{1,-1,0,1,1},{-1,-1,0,0,1}}};
    auto& vertices = arena.allocate(sizeof(triangle)); std::memcpy(vertices.bytes.data(), triangle.data(), sizeof(triangle));
    assert(device->facet_pool().acquire(arena, view(vertices.id, sizeof(triangle)/4, 1), vg::core::FacetKind::Address, &vertices_ref, &error));
    vg::core::TaskRecord raster; raster.kind = vg::core::TaskKind::Raster; raster.raster_facets = {sample, attachment}; raster.vertex_buffer_ref = vertices_ref;
    raster.raster_filter = vg::core::FilterMode::Nearest; raster.raster_wrap = vg::core::WrapMode::Clamp;
    vg::ir::Module raster_module; raster_module.version = 1; raster_module.root_schema = "vg.test/v1";
    vg::ir::Instruction load; load.op = "load"; load.allocation = source.id; load.generation = source.generation;
    load.representation_epoch = source.representation_epoch; load.size = 4; raster_module.instructions.push_back(load);
    raster_module.declared_effects.push_back({source.id, 0, 16, vg::ir::Access::Read, source.representation_epoch});
    vg::test_support::MultiNodePlanFixture fixture; vg::core::ExecutionPlan plan;
    vg::test_support::AssemblyOptions options; options.facet_pool = &device->facet_pool();
    std::vector<vg::ir::Module> modules{raster_module}; std::vector<vg::core::TaskRecord> tasks{raster};
    std::vector<std::pair<uint32_t, uint32_t>> dependencies;
    if (include_compute) { modules.push_back(atomic_add(target, atomic_offset)); tasks.push_back(vg::test_support::compute_task(target.id, target.generation)); dependencies.push_back({0, 1}); }
    assert(vg::test_support::assemble_multi_node_plan(arena, std::move(modules), std::move(tasks), dependencies, &fixture, &plan, &error, options));
    vg::hal::CompiledPlan compiled; assert(device->compile(plan, &compiled, &error));
    vg::hal::Submission submission; assert(device->submit(compiled, arena, &submission, &error));
    assert(submission.result.ok && submission.raster_results.size() == 1);
    assert(submission.raster_results[0].stored && submission.raster_results[0].contents_defined);
    assert(std::ranges::any_of(submission.result.trace, [&](const auto& effect) { return effect.allocation == target_ref.allocation && effect.access != vg::ir::Access::Read; }));
    const auto* final_target = arena.lookup(target_ref); assert(final_target != nullptr);
    if (observed != nullptr) std::memcpy(observed, final_target->bytes.data() + atomic_offset, sizeof(*observed));
    return final_target->bytes;
  };
  const auto oracle_bytes = run(false, 0, nullptr);
  uint64_t offset = UINT64_MAX, oracle{};
  for (uint64_t candidate = 0; candidate + sizeof(oracle) <= oracle_bytes.size(); candidate += sizeof(oracle)) {
    std::memcpy(&oracle, oracle_bytes.data() + candidate, sizeof(oracle));
    if (oracle != 0) { offset = candidate; break; }
  }
  assert(offset != UINT64_MAX);
  uint64_t final{};
  run(true, offset, &final);
  assert(final == oracle + 1);
}

int main() {
  auto continuation_device = vg::reference::make_device_hal();
  vg::test_support::check_mixed_continuation_admission(*continuation_device);
  runtime_failure_matrix();
  compute_to_raster_visibility();
  representation_submission_executes_once();
  independent_compute_raster_components_and_publication();
  raster_to_compute_visibility();
  return 0;
}
