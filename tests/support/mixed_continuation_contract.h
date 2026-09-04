#pragma once

#include "assembled_plan_fixture.h"
#include "backends/device_hal.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>

namespace vg::test_support {

// Shared Reference/Metal conformance, not a direct-adapter harness: every plan
// is assembled from real Nodes, TaskGraph, Envelope and representation inputs.
// The caller supplies a fresh device so the Timeline begins at zero.
inline void check_mixed_continuation_admission(hal::DeviceHal& device) {
  core::Arena arena;
  auto& output = arena.allocate(16);
  auto& source = arena.allocate(16);
  auto& target = arena.allocate(16);
  auto& transform_only = arena.allocate(16);
  const std::array<std::array<float, 5>, 3> triangle{{
      {-1, -1, 0, 0, 0}, {3, -1, 0, 2, 0}, {-1, 3, 0, 0, 2}}};
  auto& vertices = arena.allocate(sizeof(triangle));
  std::memcpy(vertices.bytes.data(), triangle.data(), sizeof(triangle));
  std::fill(source.bytes.begin(), source.bytes.end(), 255);
  const auto view = [](const core::Allocation& allocation, uint32_t width, uint32_t height) {
    core::CanonicalView result;
    result.allocation = allocation.id;
    result.allocation_generation = allocation.generation;
    result.format = core::PixelFormat::RGBA8Unorm;
    result.dimension = core::ViewDimension::Texture2D;
    result.width = width;
    result.height = height;
    return result;
  };
  std::string error;
  core::FacetRef sample, attachment, address, old_representation;
  assert(device.facet_pool().acquire(arena, view(source, 2, 2), core::FacetKind::Sample, &sample, &error));
  assert(device.facet_pool().acquire(arena, view(target, 2, 2), core::FacetKind::Attachment, &attachment, &error));
  assert(device.facet_pool().acquire(arena, view(vertices, sizeof(triangle) / 4, 1),
                                     core::FacetKind::Address, &address, &error));
  assert(device.facet_pool().acquire(arena, view(transform_only, 2, 2),
                                     core::FacetKind::Address, &old_representation, &error));
  ir::Module compute;
  compute.version = 1;
  compute.root_schema = "vg.test/v1";
  compute.instructions.push_back({"store", output.id, 0, 4, 7, output.generation,
                                   output.representation_epoch, 0, ""});
  compute.declared_effects.push_back({output.id, 0, 4, ir::Access::Write, output.representation_epoch});
  ir::Module raster_program;
  raster_program.version = 1;
  raster_program.root_schema = "vg.test/v1";
  raster_program.instructions.push_back({"load", source.id, 0, 4, 0, source.generation,
                                          source.representation_epoch, 0, ""});
  raster_program.declared_effects.push_back({source.id, 0, 16, ir::Access::Read, source.representation_epoch});
  core::TaskRecord raster;
  raster.kind = core::TaskKind::Raster;
  raster.raster_facets = {sample, attachment};
  raster.vertex_buffer_ref = address;
  const std::vector<core::TaskRecord> tasks{compute_task(output.id, output.generation), raster};
  const std::vector<core::RepresentationRequest> requests{{view(transform_only, 2, 2), core::FacetKind::Storage}};
  const auto assemble = [&](const AssemblyOptions& options, bool reverse, core::ExecutionPlan* plan) {
    MultiNodePlanFixture fixture;
    assert(assemble_multi_node_plan(arena, {compute, raster_program}, tasks,
        reverse ? std::vector<std::pair<uint32_t, uint32_t>>{{1, 0}}
                : std::vector<std::pair<uint32_t, uint32_t>>{{0, 1}},
        &fixture, plan, &error, options));
  };
  const auto submit = [&](const AssemblyOptions& options, bool reverse) {
    core::ExecutionPlan plan;
    assemble(options, reverse, &plan);
    hal::CompiledPlan compiled;
    assert(device.compile(plan, &compiled, &error));
    hal::Submission result;
    assert(device.submit(compiled, arena, &result, &error));
    assert(result.result.ok);
    return result;
  };
  AssemblyOptions options;
  options.facet_pool = &device.facet_pool();
  options.task_quota = 1;
  options.timeline_signal = 1;
  const auto first = submit(options, false);
  assert(first.timeline_value == 1 && first.published_tasks.size() == 1);
  assert(first.envelope_overflow.has_value());
  const auto valid = *first.envelope_overflow;
  options.timeline_signal = 0;
  const auto alternate = submit(options, true);
  assert(alternate.envelope_overflow.has_value());

  const auto reject = [&](const core::EnvelopeOverflow& pending, const char* diagnostic,
                          uint64_t signal, uint64_t wait = 0) {
    AssemblyOptions refused;
    refused.facet_pool = &device.facet_pool();
    refused.pending_overflow = &pending;
    refused.representation_requests = &requests;
    refused.timeline_signal = signal;
    refused.timeline_wait = wait;
    std::fill(output.bytes.begin(), output.bytes.end(), 0x55);
    std::fill(target.bytes.begin(), target.bytes.end(), 0x55);
    arena.mark_content_modified(output);
    arena.mark_content_modified(target);
    const std::array<core::Allocation*, 5> allocations{&output, &source, &target, &transform_only, &vertices};
    std::vector<std::vector<uint8_t>> bytes;
    std::vector<uint32_t> epochs;
    for (const auto* allocation : allocations) {
      bytes.push_back(allocation->bytes);
      epochs.push_back(allocation->representation_epoch);
    }
    std::vector<uint32_t> generations_before, generations_after;
    device.facet_pool().snapshot_generations(&generations_before);
    core::ExecutionPlan plan;
    assemble(refused, false, &plan);
    hal::CompiledPlan compiled;
    assert(device.compile(plan, &compiled, &error));
    assert(compiled.representation_operations.size() == 1);
    hal::Submission result;
    const bool submitted = device.submit(compiled, arena, &result, &error);
    if (wait != 0) {
      assert(submitted && !result.result.ok);
      assert(result.result.fault.code == "TIMELINE_WAIT_UNSATISFIED");
    } else {
      assert(!submitted);
      assert(error == diagnostic);
    }
    for (size_t i = 0; i < allocations.size(); ++i) {
      assert(allocations[i]->bytes == bytes[i]);
      assert(allocations[i]->representation_epoch == epochs[i]);
      assert(allocations[i]->in_flight == 0);
    }
    device.facet_pool().snapshot_generations(&generations_after);
    assert(generations_before == generations_after);
    for (auto ref : {sample, attachment, address, old_representation}) {
      assert(device.facet_pool().lookup(arena, ref) != nullptr);
      assert(device.facet_pool().in_flight(ref) == 0);
    }
    assert(result.published_tasks.empty() && result.raster_results.empty());
    assert(result.representation_facets.empty());
    assert(result.retired_facet_count == 0 && result.consumed_allocation_count == 0);
    assert(result.result.poison != core::PoisonState::PartiallyProduced);
    assert(result.report.command_buffer_count == 0 && result.report.encoder_count == 0);
    assert(result.report.barrier_count == 0 && result.report.queue_wait_count == 0);
    assert(result.report.transition_host_wait_count == 0 && result.report.transition_encoder_boundary_count == 0);
    assert(result.report.transition_serialized_fallback_count == 0);
  };
  auto rejected = valid;
  rejected.disposition = core::EnvelopeOverflowDisposition::Rejected;
  rejected.continuation_token = 0;
  auto unknown = valid;
  unknown.continuation_token += 100;
  // Timeline refusal must not consume a valid continuation or transform data.
  reject(valid, "", 1001, 1000);
  reject(rejected, "envelope leftover was rejected", 2);
  reject(unknown, "envelope continuation token does not match", 2);
  reject(*alternate.envelope_overflow, "envelope continuation leftover is not the canonical schedule suffix", 2);
  assert(device.envelope_continuations().contains(valid.continuation_token));
  assert(device.envelope_continuations().contains(alternate.envelope_overflow->continuation_token));
  options.task_quota.reset();
  options.pending_overflow = &valid;
  options.timeline_signal = 2;
  const auto resumed = submit(options, false);
  assert(resumed.timeline_value == 2 && resumed.published_tasks.size() == 1);
  assert(resumed.published_tasks[0].kind == core::TaskKind::Raster);
  assert(!resumed.envelope_overflow.has_value());
  assert(!device.envelope_continuations().contains(valid.continuation_token));
  std::vector<uint8_t> expected_output(output.bytes.size(), 0x55);
  std::fill_n(expected_output.begin(), 4, 7);
  assert(output.bytes == expected_output);  // Quota filters publication, not execution.
  reject(valid, "envelope continuation token does not match", 3);
  options.pending_overflow = nullptr;
  options.timeline_signal = 3;
  options.representation_requests = &requests;
  const auto recovered = submit(options, false);
  assert(recovered.timeline_value == 3 && recovered.published_tasks.size() == 2);
  assert(output.bytes == expected_output && recovered.raster_results.size() == 1);
  assert(transform_only.representation_epoch == 1 && recovered.representation_facets.size() == 1);
  assert(output.in_flight == 0 && transform_only.in_flight == 0);
  assert(device.facet_pool().in_flight(recovered.representation_facets[0]) == 0);
}

}  // namespace vg::test_support
