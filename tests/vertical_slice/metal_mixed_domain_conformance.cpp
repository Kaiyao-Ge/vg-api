#include "backends/metal/metal_device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "compiler/compiler.h"
#include "assembled_plan_fixture.h"
#include "mixed_continuation_contract.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Vertex { float x, y, z, u, v; };

vg::core::CanonicalView view(const vg::core::Allocation& allocation,
                             uint32_t width, uint32_t height) {
  vg::core::CanonicalView result;
  result.allocation = allocation.id;
  result.allocation_generation = allocation.generation;
  result.format = vg::core::PixelFormat::RGBA8Unorm;
  result.dimension = vg::core::ViewDimension::Texture2D;
  result.width = width;
  result.height = height;
  return result;
}

vg::ir::Module store(const vg::core::Allocation& allocation, int64_t value) {
  auto result = vg::compiler::compile_c_like("@node @effects store(1,0,4,1)");
  assert(result.ok);
  auto module = std::move(result.module);
  module.instructions[0].allocation = allocation.id;
  module.instructions[0].generation = allocation.generation;
  module.instructions[0].representation_epoch = allocation.representation_epoch;
  module.instructions[0].value = value;
  module.declared_effects[0].allocation = allocation.id;
  module.declared_effects[0].representation_epoch = allocation.representation_epoch;
  return module;
}

vg::ir::Module atomic_add(const vg::core::Allocation& allocation, uint64_t offset) {
  auto result = vg::compiler::compile_c_like("@node @effects atomic_add(1,0,8,1)");
  assert(result.ok);
  auto module = std::move(result.module);
  module.instructions[0].allocation = allocation.id;
  module.instructions[0].generation = allocation.generation;
  module.instructions[0].representation_epoch = allocation.representation_epoch;
  module.instructions[0].offset = offset;
  module.declared_effects[0].allocation = allocation.id;
  module.declared_effects[0].offset = offset;
  module.declared_effects[0].representation_epoch = allocation.representation_epoch;
  return module;
}

vg::ir::Module load(const vg::core::Allocation& allocation) {
  auto result = vg::compiler::compile_c_like("@node @effects load(1,0,4)");
  assert(result.ok);
  auto module = std::move(result.module);
  module.instructions[0].allocation = allocation.id;
  module.instructions[0].generation = allocation.generation;
  module.instructions[0].representation_epoch = allocation.representation_epoch;
  module.declared_effects[0].allocation = allocation.id;
  module.declared_effects[0].representation_epoch = allocation.representation_epoch;
  return module;
}

vg::ir::Module raster_program(const vg::core::Allocation& source) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/v1";
  module.instructions.push_back(
      {"load", source.id, 0, 4, 0, source.generation,
       source.representation_epoch, 0, ""});
  module.declared_effects.push_back(
      {source.id, 0, source.bytes.size(), vg::ir::Access::Read,
       source.representation_epoch});
  return module;
}

vg::core::TaskRecord raster_task(vg::core::FacetRef sample,
                                 vg::core::FacetRef attachment,
                                 vg::core::FacetRef vertices) {
  vg::core::TaskRecord task;
  task.kind = vg::core::TaskKind::Raster;
  task.raster_facets = {sample, attachment};
  task.vertex_buffer_ref = vertices;
  task.raster_filter = vg::core::FilterMode::Nearest;
  task.raster_wrap = vg::core::WrapMode::Clamp;
  return task;
}

void restricted_user_raster_guard(vg::hal::DeviceHal& device) {
  vg::core::Arena arena;
  auto& compute_output = arena.allocate(16);
  auto& source = arena.allocate(16);
  auto& target = arena.allocate(16);
  const std::array<Vertex, 3> triangle{{
      {-1.f, -1.f, 0.f, 0.f, 0.f},
      {3.f, -1.f, 0.f, 2.f, 0.f},
      {-1.f, 3.f, 0.f, 0.f, 2.f},
  }};
  auto& vertices = arena.allocate(sizeof(triangle));
  std::memcpy(vertices.bytes.data(), triangle.data(), sizeof(triangle));

  std::string error;
  vg::core::FacetRef sample, attachment, address;
  assert(device.facet_pool().acquire(arena, view(source, 2, 2),
                                     vg::core::FacetKind::Sample, &sample, &error));
  assert(device.facet_pool().acquire(arena, view(target, 2, 2),
                                     vg::core::FacetKind::Attachment, &attachment, &error));
  assert(device.facet_pool().acquire(arena, view(vertices, sizeof(triangle) / 4, 1),
                                     vg::core::FacetKind::Address, &address, &error));

  vg::test_support::MultiNodePlanFixture fixture;
  auto compute_module = store(compute_output, 7);
  const std::string canonical = vg::ir::serialize_module(compute_module);
  compute_module.canonical_json = canonical;
  compute_module.hash = vg::ir::sha256_hex(canonical);
  auto compute_object = std::make_shared<vg::core::CodeObject>();
  compute_object->module = std::move(compute_module);
  auto raster_object = std::make_shared<vg::core::CodeObject>();
  raster_object->user_raster_shader = vg::ir::UserRasterShaderContract{
      "vg.test.raster/v1", "vg_user_raster_vertex", "vg_user_raster_fragment",
      vg::ir::kRasterVertexAbiXyzuvPackedV1,
      "restricted source is deliberately opaque to Core"};
  const auto compute_node = fixture.nodes.create(compute_object, "mixed-compute");
  const auto raster_node = fixture.nodes.create(raster_object, "mixed-user-raster");
  fixture.code_objects = {compute_object, raster_object};
  fixture.node_refs = {compute_node, raster_node};

  auto compute = vg::test_support::compute_task(compute_output.id,
                                                 compute_output.generation);
  compute.node_index = compute_node.index;
  compute.node_generation = compute_node.generation;
  auto raster = raster_task(sample, attachment, address);
  raster.node_index = raster_node.index;
  raster.node_generation = raster_node.generation;
  vg::core::TaskGraphBuilder builder;
  assert(builder.append(compute, &error));
  assert(builder.append(raster, &error));
  assert(builder.add_dependency(0, 1));
  assert(builder.seal(&fixture.graph, &error));
  assert(fixture.graph.publish());
  fixture.envelope.allowed_nodes = fixture.node_refs;
  vg::core::ExecutionPlanAssemblerInputs inputs{
      &fixture.graph, &fixture.nodes, &fixture.envelope, &arena};
  inputs.facet_pool = &device.facet_pool();
  vg::core::ExecutionPlan plan;
  assert(vg::core::ExecutionPlanAssembler::assemble(inputs, &plan, &error));
  assert(plan.resolved_nodes.size() == 2);
  assert(plan.resolved_nodes[0].ref.index != plan.resolved_nodes[1].ref.index);

  const auto output_before = compute_output.bytes;
  const auto target_before = target.bytes;
  vg::hal::CompiledPlan compiled;
  assert(!device.compile(plan, &compiled, &error));
  assert(!compiled.report.supported);
  assert(std::ranges::any_of(compiled.report.events, [](const auto& event) {
    return event.operation == "mixed_domain_user_raster_shader" &&
           event.classification == vg::hal::LoweringClass::Unsupported;
  }));
  assert(compute_output.bytes == output_before && target.bytes == target_before);
  assert(compute_output.in_flight == 0 && source.in_flight == 0 &&
         target.in_flight == 0 && vertices.in_flight == 0);
  assert(device.facet_pool().in_flight(sample) == 0);
  assert(device.facet_pool().in_flight(attachment) == 0);
  assert(device.facet_pool().in_flight(address) == 0);
}

std::vector<std::array<float, 4>> compute_to_raster(vg::hal::DeviceHal& device,
                                                    bool metal_checks) {
  vg::core::Arena arena;
  auto& source = arena.allocate(16);
  auto& target = arena.allocate(16);
  const std::array<Vertex, 3> triangle{{
      {-1.f, -1.f, 0.f, 0.f, 0.f},
      {3.f, -1.f, 0.f, 2.f, 0.f},
      {-1.f, 3.f, 0.f, 0.f, 2.f},
  }};
  auto& vertices = arena.allocate(sizeof(triangle));
  std::memcpy(vertices.bytes.data(), triangle.data(), sizeof(triangle));
  std::string error;
  vg::core::FacetRef sample, attachment, address;
  assert(device.facet_pool().acquire(arena, view(source, 2, 2),
                                     vg::core::FacetKind::Sample, &sample, &error));
  assert(device.facet_pool().acquire(arena, view(target, 2, 2),
                                     vg::core::FacetKind::Attachment, &attachment, &error));
  assert(device.facet_pool().acquire(arena, view(vertices, sizeof(triangle) / 4, 1),
                                     vg::core::FacetKind::Address, &address, &error));
  vg::test_support::MultiNodePlanFixture fixture;
  vg::core::ExecutionPlan plan;
  vg::test_support::AssemblyOptions options;
  options.facet_pool = &device.facet_pool();
  assert(vg::test_support::assemble_multi_node_plan(
      arena, {store(source, 255), raster_program(source)},
      {vg::test_support::compute_task(source.id, source.generation),
       raster_task(sample, attachment, address)}, {{0, 1}},
      &fixture, &plan, &error, options));
  vg::hal::CompiledPlan compiled;
  assert(device.compile(plan, &compiled, &error));
  if (metal_checks) {
    assert(compiled.report.supported);
    assert(compiled.transition_operations.size() == 1);
    const auto& transition = compiled.transition_operations[0];
    assert(transition.state ==
           vg::hal::CompiledPlan::TransitionLoweringState::Lowered);
    assert(transition.covers_execution_completion);
    assert(transition.host_wait_count == 1 &&
           transition.encoder_boundary_count == 1 &&
           transition.serialized_fallback);
    assert(compiled.report.transition_host_wait_count == 1);
    assert(compiled.report.transition_encoder_boundary_count == 1);
    assert(compiled.report.transition_serialized_fallback_count == 1);
  }
  vg::hal::Submission submission;
  assert(device.submit(compiled, arena, &submission, &error));
  assert(submission.result.ok && submission.raster_results.size() == 1);
  assert(source.bytes[0] == 255);
  assert(submission.published_tasks.size() == 2);
  for (size_t rank = 0; rank < submission.published_tasks.size(); ++rank) {
    const uint32_t task = plan.execution_schedule.task_order[rank];
    assert(submission.published_tasks[rank].kind == plan.task_graph.tasks()[task].kind);
    assert(submission.published_tasks[rank].node_index == plan.task_graph.tasks()[task].node_index);
    assert(submission.published_tasks[rank].node_generation == plan.task_graph.tasks()[task].node_generation);
  }
  assert(source.in_flight == 0 && target.in_flight == 0 && vertices.in_flight == 0);
  assert(device.facet_pool().in_flight(sample) == 0);
  assert(device.facet_pool().in_flight(attachment) == 0);
  assert(device.facet_pool().in_flight(address) == 0);

  vg::hal::Submission repeated;
  assert(device.submit(compiled, arena, &repeated, &error));
  assert(repeated.result.ok && repeated.raster_results.size() == 1);
  assert(source.in_flight == 0 && target.in_flight == 0 && vertices.in_flight == 0);
  assert(device.facet_pool().in_flight(sample) == 0);
  assert(device.facet_pool().in_flight(attachment) == 0);
  assert(device.facet_pool().in_flight(address) == 0);

  if (metal_checks) {
    auto tampered = compiled;
    tampered.transition_operations[0].host_wait_count = 0;
    vg::hal::Submission rejected;
    const auto source_before = source.bytes;
    const auto target_before = target.bytes;
    assert(!device.submit(tampered, arena, &rejected, &error));
    assert(source.bytes == source_before && target.bytes == target_before);
    assert(rejected.published_tasks.empty() && rejected.raster_results.empty());
    assert(source.in_flight == 0 && target.in_flight == 0 && vertices.in_flight == 0);

    fixture.envelope.timeline_wait = 1000;
    vg::core::ExecutionPlanAssemblerInputs blocked_inputs{
        &fixture.graph, &fixture.nodes, &fixture.envelope, &arena};
    blocked_inputs.facet_pool = &device.facet_pool();
    vg::core::ExecutionPlan blocked_plan;
    assert(vg::core::ExecutionPlanAssembler::assemble(blocked_inputs, &blocked_plan, &error));
    vg::hal::CompiledPlan blocked;
    assert(device.compile(blocked_plan, &blocked, &error));
    assert(blocked.report.transition_host_wait_count == 1);
    vg::hal::Submission waiting;
    assert(device.submit(blocked, arena, &waiting, &error));
    assert(!waiting.result.ok && waiting.result.fault.code == "TIMELINE_WAIT_UNSATISFIED");
    assert(waiting.report.transition_host_wait_count == 0);
    assert(waiting.report.transition_encoder_boundary_count == 0);
    assert(waiting.report.transition_serialized_fallback_count == 0);
    assert(waiting.report.command_buffer_count == 0 && waiting.report.queue_wait_count == 0);
    assert(waiting.published_tasks.empty() && waiting.raster_results.empty());
    assert(source.bytes == source_before && target.bytes == target_before);
  }
  return submission.raster_results[0].resolved_rgba;
}

void independent_and_representation(vg::hal::DeviceHal& device) {
  vg::core::Arena arena;
  auto& compute_output = arena.allocate(16);
  auto& source = arena.allocate(16);
  auto& target = arena.allocate(16);
  auto& transform_only = arena.allocate(16);
  std::memset(source.bytes.data(), 255, source.bytes.size());
  const std::array<Vertex, 3> triangle{{
      {-1.f, -1.f, 0.f, 0.f, 0.f}, {3.f, -1.f, 0.f, 2.f, 0.f},
      {-1.f, 3.f, 0.f, 0.f, 2.f}}};
  auto& vertices = arena.allocate(sizeof(triangle));
  std::memcpy(vertices.bytes.data(), triangle.data(), sizeof(triangle));
  std::string error;
  vg::core::FacetRef sample, attachment, address;
  assert(device.facet_pool().acquire(arena, view(source, 2, 2),
                                     vg::core::FacetKind::Sample, &sample, &error));
  assert(device.facet_pool().acquire(arena, view(target, 2, 2),
                                     vg::core::FacetKind::Attachment, &attachment, &error));
  assert(device.facet_pool().acquire(arena, view(vertices, sizeof(triangle) / 4, 1),
                                     vg::core::FacetKind::Address, &address, &error));
  const std::vector<vg::core::RepresentationRequest> requests{{
      view(transform_only, 2, 2), vg::core::FacetKind::Storage}};
  vg::test_support::AssemblyOptions options;
  options.facet_pool = &device.facet_pool();
  options.representation_requests = &requests;
  options.timeline_signal = 7;
  vg::test_support::MultiNodePlanFixture fixture;
  vg::core::ExecutionPlan plan;
  assert(vg::test_support::assemble_multi_node_plan(
      arena, {store(compute_output, 7), raster_program(source)},
      {vg::test_support::compute_task(compute_output.id, compute_output.generation),
       raster_task(sample, attachment, address)}, {},
      &fixture, &plan, &error, options));
  assert(plan.execution_schedule.components.size() == 2);
  vg::hal::CompiledPlan compiled;
  assert(device.compile(plan, &compiled, &error));
  assert(compiled.representation_operation_execution_order ==
         std::vector<uint32_t>({0}));
  assert(std::ranges::any_of(compiled.report.events, [](const auto& event) {
    return event.operation == "execution_schedule" &&
           event.classification == vg::hal::LoweringClass::Serialized;
  }));
  vg::hal::Submission submission;
  assert(device.submit(compiled, arena, &submission, &error));
  assert(submission.result.ok && compute_output.bytes[0] == 7);
  assert(submission.raster_results.size() == 1);
  assert(submission.published_tasks.size() == 2);
  assert(transform_only.representation_epoch == 1);
  assert(submission.representation_facets.size() == 1);
  assert(submission.timeline_value == 7);
  assert(submission.report.encoder_count == 3);
  assert(submission.report.command_buffer_count == 3);
  assert(submission.report.queue_wait_count == 3);
  assert(compute_output.in_flight == 0 && source.in_flight == 0 &&
         target.in_flight == 0 && vertices.in_flight == 0 &&
         transform_only.in_flight == 0);
  assert(device.facet_pool().in_flight(sample) == 0);
  assert(device.facet_pool().in_flight(attachment) == 0);
  assert(device.facet_pool().in_flight(address) == 0);
  assert(device.facet_pool().in_flight(submission.representation_facets[0]) == 0);
}

void fault_reachability_and_timeline(vg::hal::DeviceHal& device) {
  // One invalid Raster task cancels only its descendant. The independent
  // Compute task completes, making the aggregate result PartiallyProduced;
  // the submission-wide timeline signal must not advance.
  {
    vg::core::Arena arena;
    auto& independent = arena.allocate(16);
    auto& descendant = arena.allocate(16);
    auto& source = arena.allocate(16);
    auto& target = arena.allocate(16);
    std::memset(source.bytes.data(), 255, source.bytes.size());
    const std::array<Vertex, 3> invalid{{
        {-1.f, -1.f, 2.f, 0.f, 0.f}, {3.f, -1.f, 2.f, 2.f, 0.f},
        {-1.f, 3.f, 2.f, 0.f, 2.f}}};
    auto& vertices = arena.allocate(sizeof(invalid));
    std::memcpy(vertices.bytes.data(), invalid.data(), sizeof(invalid));
    std::string error;
    vg::core::FacetRef sample, attachment, address;
    assert(device.facet_pool().acquire(arena, view(source, 2, 2),
                                       vg::core::FacetKind::Sample, &sample, &error));
    assert(device.facet_pool().acquire(arena, view(target, 2, 2),
                                       vg::core::FacetKind::Attachment, &attachment, &error));
    assert(device.facet_pool().acquire(arena, view(vertices, sizeof(invalid) / 4, 1),
                                       vg::core::FacetKind::Address, &address, &error));
    vg::test_support::AssemblyOptions options;
    options.facet_pool = &device.facet_pool();
    options.timeline_wait = 7;
    options.timeline_signal = 8;
    vg::test_support::MultiNodePlanFixture fixture;
    vg::core::ExecutionPlan plan;
    assert(vg::test_support::assemble_multi_node_plan(
        arena, {store(independent, 7), raster_program(source), store(descendant, 9)},
        {vg::test_support::compute_task(independent.id, independent.generation),
         raster_task(sample, attachment, address),
         vg::test_support::compute_task(descendant.id, descendant.generation)},
        {{1, 2}}, &fixture, &plan, &error, options));
    vg::hal::CompiledPlan compiled;
    assert(device.compile(plan, &compiled, &error));
    assert(compiled.report.transition_host_wait_count == 1);
    vg::hal::Submission submission;
    assert(device.submit(compiled, arena, &submission, &error));
    assert(!submission.result.ok);
    assert(submission.result.poison == vg::core::PoisonState::PartiallyProduced);
    assert(submission.result.fault.task_index == 1);
    assert(independent.bytes[0] == 7 && descendant.bytes[0] == 0);
    assert(submission.published_tasks.size() == 3);
    assert(submission.timeline_value == 7);
    // The failed Raster producer never encoded a command, and its consumer
    // was cancelled. The independent Compute command is not this transition.
    assert(submission.report.transition_host_wait_count == 0);
    assert(submission.report.transition_encoder_boundary_count == 0);
    assert(submission.report.transition_serialized_fallback_count == 0);
    assert(independent.in_flight == 0 && descendant.in_flight == 0 &&
           source.in_flight == 0 && target.in_flight == 0 && vertices.in_flight == 0);
    assert(device.facet_pool().in_flight(sample) == 0);
    assert(device.facet_pool().in_flight(attachment) == 0);
    assert(device.facet_pool().in_flight(address) == 0);
  }

  // Component {0,3} is visited before {1,2}, so task 3 fails first. Canonical
  // schedule rank nevertheless chooses task 1 as primary. Neither successful
  // task wrote output, and both descendants are cancelled, so the result is
  // Poisoned rather than inferred partial from attempted traces.
  {
    vg::core::Arena arena;
    auto& read_only = arena.allocate(16);
    auto& descendant = arena.allocate(16);
    auto& source_a = arena.allocate(16); auto& target_a = arena.allocate(16);
    auto& source_b = arena.allocate(16); auto& target_b = arena.allocate(16);
    auto& vertices_a = arena.allocate(sizeof(Vertex) * 3);
    auto& vertices_b = arena.allocate(sizeof(Vertex) * 3);
    std::memset(source_a.bytes.data(), 255, source_a.bytes.size());
    std::memset(source_b.bytes.data(), 255, source_b.bytes.size());
    const std::array<Vertex, 3> invalid{{
        {-1.f, -1.f, 2.f, 0.f, 0.f}, {3.f, -1.f, 2.f, 2.f, 0.f},
        {-1.f, 3.f, 2.f, 0.f, 2.f}}};
    std::memcpy(vertices_a.bytes.data(), invalid.data(), sizeof(invalid));
    std::memcpy(vertices_b.bytes.data(), invalid.data(), sizeof(invalid));
    std::string error;
    vg::core::FacetRef sample_a, attachment_a, address_a;
    vg::core::FacetRef sample_b, attachment_b, address_b;
    assert(device.facet_pool().acquire(arena, view(source_a, 2, 2), vg::core::FacetKind::Sample, &sample_a, &error));
    assert(device.facet_pool().acquire(arena, view(target_a, 2, 2), vg::core::FacetKind::Attachment, &attachment_a, &error));
    assert(device.facet_pool().acquire(arena, view(vertices_a, sizeof(invalid) / 4, 1), vg::core::FacetKind::Address, &address_a, &error));
    assert(device.facet_pool().acquire(arena, view(source_b, 2, 2), vg::core::FacetKind::Sample, &sample_b, &error));
    assert(device.facet_pool().acquire(arena, view(target_b, 2, 2), vg::core::FacetKind::Attachment, &attachment_b, &error));
    assert(device.facet_pool().acquire(arena, view(vertices_b, sizeof(invalid) / 4, 1), vg::core::FacetKind::Address, &address_b, &error));
    vg::test_support::AssemblyOptions options;
    options.facet_pool = &device.facet_pool();
    options.timeline_wait = 7;
    options.timeline_signal = 9;
    vg::test_support::MultiNodePlanFixture fixture;
    vg::core::ExecutionPlan plan;
    assert(vg::test_support::assemble_multi_node_plan(
        arena,
        {load(read_only), raster_program(source_a), store(descendant, 9), raster_program(source_b)},
        {vg::test_support::compute_task(read_only.id, read_only.generation),
         raster_task(sample_a, attachment_a, address_a),
         vg::test_support::compute_task(descendant.id, descendant.generation),
         raster_task(sample_b, attachment_b, address_b)},
        {{0, 3}, {1, 2}}, &fixture, &plan, &error, options));
    vg::hal::CompiledPlan compiled;
    assert(device.compile(plan, &compiled, &error));
    vg::hal::Submission submission;
    assert(device.submit(compiled, arena, &submission, &error));
    assert(!submission.result.ok);
    assert(submission.result.poison == vg::core::PoisonState::Poisoned);
    assert(submission.result.fault.task_index == 1);
    assert(descendant.bytes[0] == 0);
    assert(submission.published_tasks.size() == 4);
    assert(submission.timeline_value == 7);
  }
}

void host_assisted_schedule(vg::hal::DeviceHal& device, bool independent_raster) {
  vg::core::Arena arena;
  auto& first = arena.allocate(16);
  auto& independent = arena.allocate(16);
  auto& last = arena.allocate(16);
  auto& source = arena.allocate(16);
  auto& target = arena.allocate(16);
  std::memset(source.bytes.data(), 255, source.bytes.size());
  const std::array<Vertex, 3> triangle{{
      {-1.f, -1.f, 0.f, 0.f, 0.f}, {3.f, -1.f, 0.f, 2.f, 0.f},
      {-1.f, 3.f, 0.f, 0.f, 2.f}}};
  auto& vertices = arena.allocate(sizeof(triangle));
  std::memcpy(vertices.bytes.data(), triangle.data(), sizeof(triangle));
  std::vector<vg::ir::Module> modules{
      atomic_add(first, 0), atomic_add(independent, 0), atomic_add(last, 0)};
  std::vector<vg::core::TaskRecord> tasks{
      vg::test_support::compute_task(first.id, first.generation),
      vg::test_support::compute_task(independent.id, independent.generation),
      vg::test_support::compute_task(last.id, last.generation)};
  std::string error;
  vg::core::FacetRef sample, attachment, address;
  if (independent_raster) {
    assert(device.facet_pool().acquire(arena, view(source, 2, 2),
                                       vg::core::FacetKind::Sample, &sample, &error));
    assert(device.facet_pool().acquire(arena, view(target, 2, 2),
                                       vg::core::FacetKind::Attachment, &attachment, &error));
    assert(device.facet_pool().acquire(arena, view(vertices, sizeof(triangle) / 4, 1),
                                       vg::core::FacetKind::Address, &address, &error));
    modules.push_back(raster_program(source));
    tasks.push_back(raster_task(sample, attachment, address));
  }
  vg::test_support::AssemblyOptions options;
  options.facet_pool = &device.facet_pool();
  vg::test_support::MultiNodePlanFixture fixture;
  vg::core::ExecutionPlan plan;
  assert(vg::test_support::assemble_multi_node_plan(
      arena, std::move(modules), std::move(tasks), {{0, 2}},
      &fixture, &plan, &error, options));
  vg::hal::CompiledPlan compiled;
  assert(device.compile(plan, &compiled, &error));
  assert(std::ranges::count_if(compiled.per_node_packages, [](const auto& package) {
    return package.host_assisted;
  }) == 3);
  // The only transition has HostAssisted Compute producers and consumers.
  // An unrelated native Raster component must not invent a device wait here.
  assert(std::ranges::count_if(compiled.transition_operations, [](const auto& transition) {
    return transition.covers_execution_completion;
  }) == 1);
  assert(compiled.report.transition_host_wait_count == 0);
  assert(compiled.report.transition_encoder_boundary_count == 0);
  assert(compiled.report.transition_serialized_fallback_count == 1);
  vg::hal::Submission submission;
  assert(device.submit(compiled, arena, &submission, &error));
  assert(submission.result.ok);
  assert(first.bytes[0] == 1 && independent.bytes[0] == 1 && last.bytes[0] == 1);
  assert(submission.published_tasks.size() == (independent_raster ? 4 : 3));
  assert(submission.raster_results.size() == (independent_raster ? 1 : 0));
  // Both routes use the same component/wave scheduler: {0,2} then {1},
  // not the obsolete all-host fast path's canonical {0,1,2} execution.
  assert(submission.result.trace.size() >= 3);
  assert(submission.result.trace[0].allocation == first.id);
  assert(submission.result.trace[1].allocation == last.id);
  assert(submission.result.trace[2].allocation == independent.id);
  assert(submission.report.transition_host_wait_count == 0);
  assert(submission.report.transition_encoder_boundary_count == 0);
  assert(submission.report.transition_serialized_fallback_count == 1);
  // Mixed publication is host-side plus one Raster command. All-compute
  // publication has one ring command; the host computations add none.
  assert(submission.report.encoder_count == 1);
  assert(submission.report.command_buffer_count == 1);
  assert(submission.report.queue_wait_count == 1);
  assert(first.in_flight == 0 && independent.in_flight == 0 && last.in_flight == 0);
  if (independent_raster) {
    assert(source.in_flight == 0 && target.in_flight == 0 && vertices.in_flight == 0);
    assert(device.facet_pool().in_flight(sample) == 0);
    assert(device.facet_pool().in_flight(attachment) == 0);
    assert(device.facet_pool().in_flight(address) == 0);
  }
}

std::vector<uint8_t> raster_then_compute(vg::hal::DeviceHal& device,
                                         bool include_compute,
                                         uint64_t atomic_offset) {
  vg::core::Arena arena;
  auto& source = arena.allocate(64);
  auto& target = arena.allocate(64);
  std::memset(source.bytes.data(), 255, source.bytes.size());
  const std::array<Vertex, 6> quad{{
      {-1,1,0,0,0},{1,1,0,1,0},{-1,-1,0,0,1},
      {1,1,0,1,0},{1,-1,0,1,1},{-1,-1,0,0,1}}};
  auto& vertices = arena.allocate(sizeof(quad));
  std::memcpy(vertices.bytes.data(), quad.data(), sizeof(quad));
  std::string error;
  vg::core::FacetRef sample, attachment, address;
  assert(device.facet_pool().acquire(arena, view(source, 4, 4),
                                     vg::core::FacetKind::Sample, &sample, &error));
  assert(device.facet_pool().acquire(arena, view(target, 4, 4),
                                     vg::core::FacetKind::Attachment, &attachment, &error));
  assert(device.facet_pool().acquire(arena, view(vertices, sizeof(quad) / 4, 1),
                                     vg::core::FacetKind::Address, &address, &error));
  std::vector<vg::ir::Module> modules{raster_program(source)};
  std::vector<vg::core::TaskRecord> tasks{raster_task(sample, attachment, address)};
  std::vector<std::pair<uint32_t, uint32_t>> dependencies;
  if (include_compute) {
    modules.push_back(atomic_add(target, atomic_offset));
    tasks.push_back(vg::test_support::compute_task(target.id, target.generation));
    dependencies.push_back({0, 1});
  }
  vg::test_support::AssemblyOptions options;
  options.facet_pool = &device.facet_pool();
  vg::test_support::MultiNodePlanFixture fixture;
  vg::core::ExecutionPlan plan;
  assert(vg::test_support::assemble_multi_node_plan(
      arena, std::move(modules), std::move(tasks), dependencies,
      &fixture, &plan, &error, options));
  vg::hal::CompiledPlan compiled;
  if (!device.compile(plan, &compiled, &error)) {
    std::fprintf(stderr, "raster_then_compute compile failed: %s\n", error.c_str());
    std::abort();
  }
  if (include_compute && device.capabilities().backend == vg::hal::BackendKind::Metal) {
    const bool host_assisted = std::ranges::any_of(compiled.per_node_packages, [](const auto& package) {
      return package.host_assisted;
    });
    // The canonical atomic_add probe currently requires 64-bit integer
    // atomics, which Metal cannot lower natively on the tested device. Keep
    // this R->C coverage honest: it validates the sealed cross-domain order
    // through the explicitly reported host-assisted compute package, not a
    // native render-to-compute fence claim.
    assert(host_assisted);
    assert(std::ranges::any_of(compiled.report.events, [](const auto& event) {
      return event.operation == "metal_pipeline" &&
             event.classification == vg::hal::LoweringClass::HostAssisted;
    }));
  }
  vg::hal::Submission submission;
  assert(device.submit(compiled, arena, &submission, &error));
  assert(submission.result.ok && submission.raster_results.size() == 1);
  return target.bytes;
}

void compare_pixels(const std::vector<std::array<float, 4>>& reference,
                    const std::vector<std::array<float, 4>>& metal) {
  assert(reference.size() == metal.size());
  for (size_t pixel = 0; pixel < reference.size(); ++pixel)
    for (size_t channel = 0; channel < 4; ++channel)
      assert(std::fabs(reference[pixel][channel] - metal[pixel][channel]) <= 1.0f / 255.0f);
}

}  // namespace

int main() {
  auto metal = vg::metal::make_device_hal();
  if (!metal) return 77;
  auto continuation_device = vg::metal::make_device_hal();
  assert(continuation_device);
  vg::test_support::check_mixed_continuation_admission(*continuation_device);
  auto reference = vg::reference::make_device_hal();
  restricted_user_raster_guard(*metal);
  compare_pixels(compute_to_raster(*reference, false),
                 compute_to_raster(*metal, true));
  independent_and_representation(*metal);
  fault_reachability_and_timeline(*metal);
  host_assisted_schedule(*metal, false);
  host_assisted_schedule(*metal, true);

  auto reference_raster = raster_then_compute(*reference, false, 0);
  uint64_t offset = UINT64_MAX;
  for (uint64_t candidate = 0; candidate + sizeof(uint64_t) <= reference_raster.size();
       candidate += sizeof(uint64_t)) {
    uint64_t word = 0;
    std::memcpy(&word, reference_raster.data() + candidate, sizeof(word));
    if (word != 0) { offset = candidate; break; }
  }
  assert(offset != UINT64_MAX);
  const auto reference_mixed = raster_then_compute(*reference, true, offset);
  const auto metal_mixed = raster_then_compute(*metal, true, offset);
  uint64_t reference_word = 0, metal_word = 0;
  std::memcpy(&reference_word, reference_mixed.data() + offset, sizeof(reference_word));
  std::memcpy(&metal_word, metal_mixed.data() + offset, sizeof(metal_word));
  assert(reference_word == metal_word);
  return 0;
}
