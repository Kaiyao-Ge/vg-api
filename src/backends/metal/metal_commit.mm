#include "backends/metal/metal_device_internal.h"
#include "backends/reference/reference_executor.h"
#include "compiler/compute_task_ring.h"
#include "core/scene_root.h"
#include "vg_scene_root_layout.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <span>
#include <utility>

namespace vg::metal {

// TASK-D5 / ADR-039: publish only the envelope window. A quota split or
// leftover drain must not GPU-publish the parked suffix (gid == packed
// slot, so a full-graph dispatch would still write leftover).
bool publish_envelope_order(const core::TaskGraph& graph, const std::vector<uint32_t>& order,
                            std::vector<core::TaskRecord>* published, std::string* error) {
  if (published == nullptr) {
    if (error) *error = "envelope published-task output is required";
    return false;
  }
  published->clear();
  if (order.empty()) return true;
  core::PublicationRing ring(static_cast<uint32_t>(order.size()));
  const auto& tasks = graph.tasks();
  published->reserve(order.size());
  for (uint32_t index : order) {
    uint32_t slot = 0;
    std::string task_error;
    if (index >= tasks.size() || !ring.publish_task(tasks[index], &slot, &task_error) ||
        !ring.consume(slot, &task_error)) {
      if (error) *error = task_error.empty() ? "envelope task index is out of range" : task_error;
      return false;
    }
    published->push_back(tasks[index]);
  }
  return true;
}

struct DeviceHal::SubmitOps {
  enum class Flow { Fail, Finish, Continue };

  static bool take(Flow flow, bool* result) {
    if (flow == Flow::Continue) return false;
    *result = (flow == Flow::Finish);
    return true;
  }

  static std::map<uint64_t, std::pair<uint32_t, uint32_t>> generations(const ir::Module& module) {
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> map;
    for (const auto& instruction : module.instructions)
      map.emplace(instruction.allocation,
                  std::make_pair(instruction.generation, instruction.representation_epoch));
    return map;
  }

  static bool bind(DeviceHal& metal, core::Arena& arena, uint64_t allocation, uint32_t generation,
                   uint32_t epoch, id<MTLBuffer>* buffer, core::Allocation** touched,
                   hal::Submission* submission) {
    *touched = arena.lookup(core::RepresentationRef{allocation, generation, epoch});
    if (*touched == nullptr) {
      submission->result.ok = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = "stale generation, representation epoch, or out-of-bounds allocation reference";
      return false;
    }
    *buffer = metal.impl_->ensure_buffer(**touched);
    if (*buffer == nil) {
      submission->result.ok = false;
      submission->result.message = "Metal buffer allocation failed";
      return false;
    }
    return true;
  }

  static bool begin(const hal::CompiledPlan& compiled, core::Arena& arena, hal::Submission* submission,
                    std::string* error) {
    if (submission == nullptr) { if (error) *error = "submission output is required"; return false; }
    if (!compiled.report.supported) { if (error) *error = "compiled plan is unsupported"; return false; }
    if (!compiled.plan.validate(error)) return false;
    if (compiled.per_node_packages.size() != compiled.plan.resolved_nodes.size()) {
      if (error) *error = "compiled plan does not contain exactly one package per resolved NodeRef";
      return false;
    }
    for (const auto& node : compiled.plan.resolved_nodes) {
      const size_t matches = std::count_if(compiled.per_node_packages.begin(),
                                           compiled.per_node_packages.end(), [&](const auto& package) {
        return package.ref.index == node.ref.index && package.ref.generation == node.ref.generation;
      });
      if (matches != 1) {
        if (error) *error = "compiled plan contains a duplicate or missing NodeRef package";
        return false;
      }
      const auto package = std::ranges::find_if(compiled.per_node_packages, [&](const auto& candidate) {
        return candidate.ref.index == node.ref.index && candidate.ref.generation == node.ref.generation;
      });
      const auto task = std::ranges::find_if(compiled.plan.task_graph.tasks(), [&](const auto& candidate) {
        return candidate.node_index == node.ref.index && candidate.node_generation == node.ref.generation;
      });
      const bool raster = task != compiled.plan.task_graph.tasks().end() &&
                          task->kind == core::TaskKind::Raster;
      if (package == compiled.per_node_packages.end() ||
          (raster && (package->kind != hal::CompiledPlan::NodePackageKind::Raster ||
                      package->package.has_value())) ||
          (!raster && (package->kind != hal::CompiledPlan::NodePackageKind::CanonicalCompute ||
                       !package->package.has_value()))) {
        if (error) *error = "compiled NodeRef package kind disagrees with the sealed Task domain";
        return false;
      }
    }
    if (!compiled.plan.graph_epoch_matches(arena, error)) return false;
    submission->abi_version = hal::kDeviceHalAbiVersion;
    submission->report = compiled.report;
    submission->report.transition_encoder_boundary_count = 0;
    submission->report.transition_host_wait_count = 0;
    submission->report.transition_serialized_fallback_count = 0;
    if (!hal::run_discovery_stage(compiled.plan, arena, submission, error)) return false;
    if (!hal::apply_working_set_budget(compiled.plan, arena, submission, error)) return false;
    return true;
  }

  static bool stage5(DeviceHal& metal, const hal::CompiledPlan& compiled, core::Arena& arena,
                     hal::Submission* submission, std::string* error) {
    std::string representation_error;
    DispatchStats representation_stats;
    if (!hal::commit_representation_operations(
            compiled.plan, compiled.representation_operations, arena, metal.facet_pool(),
            [&](const core::RepresentationSemanticPlanItem& request, const hal::CompiledPlan::PhysicalRepresentationOperation&, core::FacetRef facet,
                hal::RepresentationTransformCost* cost, std::string* physical_error) {
              Impl::TransformCost transform_cost;
              const bool transformed = metal.impl_->transform_into_private_facet(arena, metal.facet_pool(), request.view,
                                                             request.target_kind, facet, &transform_cost,
                                                             physical_error);
              representation_stats.encoder_count += transform_cost.encoder_count;
              representation_stats.command_buffer_count += transform_cost.command_buffer_count;
              representation_stats.queue_wait_count += transform_cost.queue_wait_count;
              const auto operation_index = static_cast<uint32_t>(
                  &request - compiled.plan.representation_plan.data());
              if (std::ranges::any_of(compiled.transition_operations, [&](const auto& transition) {
                    return std::ranges::find(transition.representation_operations, operation_index) !=
                           transition.representation_operations.end();
                  })) {
                submission->report.transition_encoder_boundary_count += transform_cost.encoder_count;
                submission->report.transition_host_wait_count += transform_cost.queue_wait_count;
              }
              if (!transformed) return false;
              cost->new_backing_bytes = transform_cost.new_backing_bytes;
              cost->temporary_bytes = transform_cost.temporary_bytes;
              cost->heap_fragmentation_bytes = 0;
              cost->used_device_optimal = true;
              cost->distinct_backing = true;
              return true;
            },
            submission, &representation_error)) {
      apply_dispatch_stats(representation_stats, submission);
      if (error) *error = representation_error;
      return false;
    }
    apply_dispatch_stats(representation_stats, submission);
    uint32_t retired_textures = 0;
    uint64_t released_linear = 0;
    metal.impl_->reclaim_released_backing(arena, metal.facet_pool(), &retired_textures, &released_linear);
    if (retired_textures != 0) {
      submission->report.add("facet_texture_retire", hal::LoweringClass::Direct, retired_textures, 0,
                             "MTLTextures belonging to retired facet slots or superseded "
                             "RepresentationEpochs destroyed after the stage");
    }
    if (released_linear != 0) {
      submission->report.add("consume_input_backing_release", hal::LoweringClass::Direct, 1, released_linear,
                             "the superseded linear representation's device buffer was destroyed at once "
                             "rather than retained to command-buffer completion (06 §11, E005)");
    }
    return true;
  }

  // F2 (ADR-043 Decision #3): runs every Raster-kind TaskRecord in the
  // compiled plan's task graph through Impl::run_raster_pass() -- the same
  // facet-acquisition/pipeline/draw/readback code run_raster_triangles() uses
  // -- so the rasterizer is reachable through compile()/submit() by being
  // moved, not rewritten.
  //
  // MD-4 passes only task indices selected from the sealed component/wave
  // schedule. Keeping the physical draw helper here avoids introducing a
  // second raster execution path while allowing compute and raster steps to
  // share one scheduler.
  static bool raster(DeviceHal& metal, const hal::CompiledPlan& compiled, core::Arena& arena,
                     std::span<const uint32_t> task_indices, DispatchStats* stats,
                     hal::Submission* submission, bool* command_submitted,
                     std::string* out_message) {
    if (command_submitted != nullptr) *command_submitted = false;
    const auto& tasks = compiled.plan.task_graph.tasks();
    for (uint32_t task_index : task_indices) {
      if (task_index >= tasks.size()) {
        if (out_message) *out_message = "sealed execution schedule names an out-of-range raster Task";
        return false;
      }
      const core::TaskRecord& task = tasks[task_index];
      if (task.kind != core::TaskKind::Raster) {
        if (out_message) *out_message = "Metal raster schedule step received a non-raster Task";
        return false;
      }

      const core::NodeTable::Ref ref{task.node_index, task.node_generation};
      const auto resolved = std::ranges::find_if(compiled.plan.resolved_nodes, [ref](const auto& node) {
        return node.ref.index == ref.index && node.ref.generation == ref.generation;
      });
      if (resolved == compiled.plan.resolved_nodes.end()) {
        if (out_message) *out_message = "raster task NodeRef is missing from the immutable plan snapshot";
        return false;
      }
      const auto package = std::ranges::find_if(compiled.per_node_packages, [ref](const auto& item) {
        return item.ref.index == ref.index && item.ref.generation == ref.generation;
      });
      if (package == compiled.per_node_packages.end() ||
          package->kind != hal::CompiledPlan::NodePackageKind::Raster ||
          package->package.has_value()) {
        if (out_message) *out_message = "raster Task resolved a non-raster NodeRef package";
        return false;
      }
      const std::string& root_schema = resolved->user_raster_shader.has_value()
          ? resolved->user_raster_shader->root_schema : resolved->module->root_schema;
      const bool uses_scene_root = core::is_scene_root_raster_schema(root_schema);

      std::string task_error;
      core::RasterFacetPair facets = task.raster_facets;
      std::array<float, 4> tint = task.raster_tint;
      std::optional<core::ResolvedSceneRootRaster> scene_root;
      id<MTLBuffer> scene_root_buffer = nil;
      bool identity_buffer_created = false;
      if (uses_scene_root) {
        scene_root.emplace();
        if (!core::resolve_scene_root_raster(arena, task, &*scene_root, &task_error)) {
          if (out_message) *out_message = task_error;
          return false;
        }
        facets.source = scene_root->albedo;
        tint = scene_root->base_color;
        scene_root_buffer = metal.impl_->ensure_buffer(*scene_root->allocation);
      } else {
        // The built-in shader always reads slot 1 after F6. Bind an explicit
        // identity root for every pre-F6 task so its pixels and PSO key stay
        // unchanged rather than relying on an unbound Metal buffer.
        scene_root_buffer = metal.impl_->make_identity_scene_root_buffer(&identity_buffer_created);
      }
      if (scene_root_buffer == nil) {
        if (out_message) *out_message = "Metal SceneRoot buffer allocation failed";
        return false;
      }
      if (!uses_scene_root) {
        submission->report.add(identity_buffer_created ? "identity_scene_root_buffer_create"
                                                       : "identity_scene_root_buffer_reuse",
                               hal::LoweringClass::Direct, 1,
                               identity_buffer_created ? VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE : 0,
                               identity_buffer_created
                                   ? "one immutable device-local legacy SceneRoot buffer created"
                                   : "reused immutable device-local legacy SceneRoot buffer; no draw allocation");
      }
      // Address facets are just as much GPU-visible capabilities as sample
      // and attachment facets. Hold vertex (and, below, index) slots until
      // run_raster_pass has committed and waited for the command buffer.
      FacetUseGuard vertex_use(metal.facet_pool(), task.vertex_buffer_ref);
      if (!vertex_use.begin(arena, &task_error)) {
        if (out_message) *out_message = task_error;
        return false;
      }
      id<MTLBuffer> vertex_buffer = metal.impl_->ensure_facet_buffer(
          arena, metal.facet_pool(), task.vertex_buffer_ref, core::FacetKind::Address, &task_error);
      if (vertex_buffer == nil) {
        if (out_message) *out_message = task_error;
        return false;
      }

      // ensure_facet_buffer() already resolved the facet slot internally to
      // reach the allocation's device buffer; resolving it again here is
      // read-only and cheap, and is the only way to reach the backing
      // Allocation's byte length to derive a vertex count.
      const core::FacetSlot* vertex_slot = metal.impl_->resolve_facet(
          arena, metal.facet_pool(), task.vertex_buffer_ref, core::FacetKind::Address, &task_error);
      if (vertex_slot == nullptr) {
        if (out_message) *out_message = task_error;
        return false;
      }
      const core::Allocation* vertex_allocation = arena.lookup(
          core::PointerRef{vertex_slot->view.allocation, vertex_slot->view.allocation_generation});
      if (vertex_allocation == nullptr) {
        if (out_message) *out_message = "raster task vertex buffer allocation not found in arena";
        return false;
      }
      if (vertex_allocation->bytes.size() % sizeof(RasterVertex) != 0) {
        if (out_message)
          *out_message = "raster task vertex buffer byte size is not a multiple of sizeof(RasterVertex)";
        return false;
      }
      const uint32_t vertex_count =
          static_cast<uint32_t>(vertex_allocation->bytes.size() / sizeof(RasterVertex));
      // Metal clips in homogeneous coordinates, but F4's public contract is
      // normalized window depth. Validate before upload so an old F3 four-float
      // vertex stream cannot be silently reinterpreted as z/u/v data.
      for (uint32_t i = 0; i < vertex_count; ++i) {
        RasterVertex vertex{};
        std::memcpy(&vertex, vertex_allocation->bytes.data() + i * sizeof(RasterVertex), sizeof(vertex));
        if (!std::isfinite(vertex.z) || vertex.z < 0.0f || vertex.z > 1.0f) {
          if (out_message)
            *out_message = "raster task vertex z must be finite and normalized to [0,1] (F4 vertex ABI)";
          return false;
        }
        if (scene_root.has_value() &&
            !core::transform_scene_root_vertex(*scene_root, vertex.x, vertex.y, vertex.z,
                                                &vertex.x, &vertex.y, &vertex.z, &task_error)) {
          if (out_message) *out_message = task_error;
          return false;
        }
      }

      id<MTLBuffer> index_buffer = nil;
      MTLIndexType index_type = MTLIndexTypeUInt16;
      std::unique_ptr<FacetUseGuard> index_use;
      if (task.index_count != 0) {
        index_use = std::make_unique<FacetUseGuard>(metal.facet_pool(), task.index_buffer_ref);
        if (!index_use->begin(arena, &task_error)) { if (out_message) *out_message = task_error; return false; }
        const core::FacetSlot* index_slot = metal.impl_->resolve_facet(
            arena, metal.facet_pool(), task.index_buffer_ref, core::FacetKind::Address, &task_error);
        if (index_slot == nullptr) { if (out_message) *out_message = task_error; return false; }
        const size_t index_stride = index_slot->view.format == core::PixelFormat::R16Uint ? sizeof(uint16_t) :
                                    index_slot->view.format == core::PixelFormat::R32Uint ? sizeof(uint32_t) : 0;
        if (index_stride == 0 || task.index_count % 3 != 0 ||
            task.index_count > std::numeric_limits<size_t>::max() / index_stride) {
          if (out_message) *out_message = "raster task index buffer requires R16Uint/R32Uint and a triangle-list count";
          return false;
        }
        const core::Allocation* index_allocation = arena.lookup(
            core::PointerRef{index_slot->view.allocation, index_slot->view.allocation_generation});
        if (index_allocation == nullptr || index_allocation->bytes.size() < task.index_count * index_stride) {
          if (out_message) *out_message = "raster task index buffer is shorter than index_count";
          return false;
        }
        const uint8_t* indices = index_allocation->bytes.data();
        for (uint32_t i = 0; i < task.index_count; ++i) {
          uint32_t index = 0;
          if (index_stride == sizeof(uint16_t)) { uint16_t value{}; std::memcpy(&value, indices + i * index_stride, sizeof(value)); index = value; }
          else std::memcpy(&index, indices + i * index_stride, sizeof(index));
          if (index >= vertex_count) { if (out_message) *out_message = "raster task index references a vertex outside the vertex buffer"; return false; }
        }
        index_buffer = metal.impl_->ensure_facet_buffer(arena, metal.facet_pool(), task.index_buffer_ref,
                                                          core::FacetKind::Address, &task_error);
        if (index_buffer == nil) { if (out_message) *out_message = task_error; return false; }
        index_type = index_stride == sizeof(uint16_t) ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
      }

      id<MTLBuffer> tint_buffer = [metal.impl_->device newBufferWithLength:sizeof(float) * 4
                                                                  options:MTLResourceStorageModeShared];
      if (tint_buffer == nil) {
        if (out_message) *out_message = "Metal raster buffer allocation failed";
        return false;
      }
      std::memcpy([tint_buffer contents], tint.data(), sizeof(float) * 4);

      // F2 fixed defaults for the backend-private half of RasterDesc; the
      // rest (filter/wrap/tint) comes straight off the TaskRecord.
      RasterDesc desc;
      desc.attachment = hal::f2_default_raster_attachment_config<AttachmentFacetDesc>();
      desc.filter = task.raster_filter;
      desc.wrap = task.raster_wrap;
      desc.tint = tint;
      desc.depth_attachment_ref = task.depth_attachment_ref;
      desc.depth_test_enable = task.depth_test_enable;
      desc.depth_write_enable = task.depth_write_enable;
      desc.depth_compare_op = task.depth_compare_op;

      RasterResult result;
      const ir::UserRasterShaderContract* user_shader =
          resolved->user_raster_shader.has_value() ? &*resolved->user_raster_shader : nullptr;
      bool task_submitted = false;
      const bool raster_ok = metal.impl_->run_raster_pass(arena, metal.facet_pool(), facets, desc, vertex_buffer, scene_root_buffer,
                                        tint_buffer, vertex_count, index_buffer, index_type, task.index_count,
                                        &result, &task_error, user_shader, &task_submitted);
      if (stats != nullptr) {
        stats->encoder_count += result.report.encoder_count;
        stats->command_buffer_count += result.report.command_buffer_count;
        stats->barrier_count += result.report.barrier_count;
        stats->queue_wait_count += result.report.queue_wait_count;
      }
      if (!raster_ok) {
        if (command_submitted != nullptr) *command_submitted = task_submitted;
        if (out_message) *out_message = task_error;
        return false;
      }
      if (command_submitted != nullptr) *command_submitted = true;

      submission->raster_results.push_back(hal::RasterTaskResult{
          .task_index = static_cast<uint32_t>(task_index),
          .resolved_rgba = result.resolved_rgba,
          .resolved_depth = result.resolved_depth,
          .width = result.width,
          .height = result.height,
          .stored = result.stored,
          .contents_defined = result.contents_defined,
      });
      for (const auto& event : result.report.events)
        submission->report.events.push_back(event);
      for (const auto& effect : compiled.plan.task_effects[task_index]) {
        submission->result.trace.push_back(effect);
        submission->result.witness.record(
            effect, static_cast<uint32_t>(submission->result.witness.entries().size()));
      }
    }
    return true;
  }

  static Flow precheck_timeline(DeviceHal& metal, uint64_t wait_value, uint64_t signal_value,
                                hal::Submission* submission) {
    if (wait_value == 0 && signal_value == 0) return Flow::Continue;
    std::string timeline_error;
    if (!metal.impl_->ensure_timeline_event(&timeline_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = timeline_error;
      submission->result.fault.code = "TIMELINE_UNAVAILABLE";
      submission->result.fault.message = timeline_error;
      return Flow::Finish;
    }
    const uint64_t current = metal.impl_->timeline_event.signaledValue;
    if (wait_value != 0 && current < wait_value) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = "timeline wait point is unsatisfied";
      submission->result.fault.code = "TIMELINE_WAIT_UNSATISFIED";
      submission->result.fault.message = submission->result.message;
      return Flow::Finish;
    }
    if (signal_value != 0 && signal_value <= current) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = core::PoisonState::Poisoned;
      submission->result.message = "timeline signal must be strictly monotonic";
      submission->result.fault.code = "TIMELINE_SIGNAL_NOT_MONOTONIC";
      submission->result.fault.message = submission->result.message;
      return Flow::Finish;
    }
    return Flow::Continue;
  }

  static Flow sealed_effects(const hal::CompiledPlan& compiled, hal::Submission* submission) {
    if (compiled.plan.certificate.ranges.empty()) return Flow::Continue;
    for (const auto& effect : compiled.plan.instantiated_effects) {
      if (!compiled.plan.certificate.covers(effect)) {
        submission->result.ok = false;
        submission->result.poison = core::PoisonState::Poisoned;
        submission->result.message = "certificate does not cover a sealed per-Task effect";
        submission->result.missing_effects.push_back(effect);
        return Flow::Finish;
      }
    }
    return Flow::Continue;
  }

  static Flow execute_schedule(DeviceHal& metal, const hal::CompiledPlan& compiled,
                               core::Arena& arena, uint64_t signal_value,
                               hal::Submission* submission) {
    const auto& tasks = compiled.plan.task_graph.tasks();
    const auto& schedule = compiled.plan.execution_schedule;
    const size_t task_count = tasks.size();
    DispatchStats stats;
    std::vector<uint8_t> cancelled(task_count);
    struct Failure { uint32_t task{}; std::string message; core::FaultRecord fault; };
    std::vector<Failure> failures;
    std::vector<uint64_t> task_encoder_boundaries(task_count), task_host_waits(task_count);
    std::vector<uint8_t> started(task_count);
    std::vector<id<MTLComputePipelineState>> pipeline_ordinals;
    bool produced_output = false;

    submission->result = {};
    submission->result.ok = true;
    metal.impl_->last_node_aware_dispatches.clear();
    metal.impl_->last_node_aware_dispatches.reserve(task_count);

    const auto cancel_descendants = [&](uint32_t failed) {
      std::vector<uint32_t> work{failed};
      for (size_t cursor = 0; cursor < work.size(); ++cursor) {
        for (uint32_t successor : schedule.structural_successors[work[cursor]]) {
          if (cancelled[successor] == 0) {
            cancelled[successor] = 1;
            work.push_back(successor);
          }
        }
      }
    };

    const auto record_effects = [&](uint32_t task_index) {
      for (const auto& effect : compiled.plan.task_effects[task_index]) {
        submission->result.trace.push_back(effect);
        submission->result.witness.record(
            effect, static_cast<uint32_t>(submission->result.witness.entries().size()));
        if (effect.access != ir::Access::Read) produced_output = true;
      }
    };

    const auto run_compute = [&](uint32_t task_index, std::string* task_error) {
      const auto& task = tasks[task_index];
      const core::NodeTable::Ref ref{task.node_index, task.node_generation};
      const auto node = std::ranges::find_if(compiled.plan.resolved_nodes, [ref](const auto& candidate) {
        return candidate.ref.index == ref.index && candidate.ref.generation == ref.generation;
      });
      const auto node_package = std::ranges::find_if(compiled.per_node_packages, [ref](const auto& candidate) {
        return candidate.ref.index == ref.index && candidate.ref.generation == ref.generation;
      });
      if (node == compiled.plan.resolved_nodes.end() || !node->module.has_value() ||
          node_package == compiled.per_node_packages.end() ||
          node_package->kind != hal::CompiledPlan::NodePackageKind::CanonicalCompute ||
          !node_package->package.has_value()) {
        if (task_error) *task_error = "compute Task could not resolve its immutable NodeRef package";
        return false;
      }
      const auto& module = *node->module;
      const auto& package = *node_package->package;
      if (package.canonical_ir_hash != module.hash || package.root_schema != module.root_schema) {
        if (task_error) *task_error = "NodeRef package hash disagrees with its immutable module snapshot";
        return false;
      }
      if (node_package->host_assisted) {
        const auto host_start = std::chrono::steady_clock::now();
        auto result = reference::execute(
            module, arena,
            compiled.plan.certificate.ranges.empty() ? nullptr : &compiled.plan.certificate,
            nullptr, {}, &compiled.plan.task_effects[task_index]);
        submission->cpu_submit_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - host_start).count();
        submission->result.trace.insert(submission->result.trace.end(),
                                        result.trace.begin(), result.trace.end());
        for (const auto& entry : result.witness.entries())
          submission->result.witness.record(
              entry.effect,
              static_cast<uint32_t>(submission->result.witness.entries().size()));
        submission->result.missing_effects.insert(submission->result.missing_effects.end(),
                                                 result.missing_effects.begin(), result.missing_effects.end());
        if (!result.ok) {
          produced_output = produced_output ||
              result.poison == core::PoisonState::PartiallyProduced;
          submission->result.fault = result.fault;
          if (task_error) *task_error = result.message;
          return false;
        }
        for (const auto& effect : compiled.plan.task_effects[task_index])
          if (effect.access != ir::Access::Read) produced_output = true;
        return true;
      }
      id<MTLComputePipelineState> pipeline = nil;
      std::string pipeline_error;
      const bool pointer_graph = is_pointer_graph_module(module);
      if (!metal.impl_->ensure_node_pipeline({package.canonical_ir_hash, package.metal_source},
                                             &pipeline, &pipeline_error,
                                             pointer_graph ? "vg_pointer_graph_compute" : "vg_linear_compute")) {
        if (task_error) *task_error = "Metal per-Node pipeline lookup failed: " + pipeline_error;
        return false;
      }
      auto ordinal = std::ranges::find(pipeline_ordinals, pipeline);
      if (ordinal == pipeline_ordinals.end()) {
        pipeline_ordinals.push_back(pipeline);
        ordinal = std::prev(pipeline_ordinals.end());
      }
      std::vector<id<MTLBuffer>> buffers;
      std::map<uint64_t, core::Allocation*> bound_by_id;
      std::map<uint64_t, id<MTLBuffer>> buffer_by_allocation_id;
      const auto generation_by_allocation = generations(module);
      for (const auto& binding : package.bindings) {
        const auto generation = generation_by_allocation.find(binding.allocation);
        id<MTLBuffer> buffer = nil;
        core::Allocation* allocation = nullptr;
        if (generation == generation_by_allocation.end() ||
            !bind(metal, arena, binding.allocation, generation->second.first,
                  generation->second.second, &buffer, &allocation, submission)) {
          if (task_error && task_error->empty()) *task_error = submission->result.message;
          return false;
        }
        buffers.push_back(buffer);
        bound_by_id.emplace(allocation->id, allocation);
        buffer_by_allocation_id[allocation->id] = buffer;
      }
      bool command_submitted = false;
      if (!metal.impl_->dispatch_compute_task(
              pipeline, buffers, task, task_index,
              static_cast<uint32_t>(std::distance(pipeline_ordinals.begin(), ordinal)),
              &command_submitted, &stats, task_error)) {
        if (command_submitted && std::ranges::any_of(
                compiled.plan.task_effects[task_index], [](const auto& effect) {
                  return effect.access != ir::Access::Read;
                }))
          produced_output = true;
        return false;
      }
      for (const auto& effect : compiled.plan.task_effects[task_index]) {
        if (effect.access == ir::Access::Read) continue;
        const auto allocation = bound_by_id.find(effect.allocation);
        const auto buffer = buffer_by_allocation_id.find(effect.allocation);
        if (allocation != bound_by_id.end() && buffer != buffer_by_allocation_id.end())
          metal.impl_->commit_buffer_write(*allocation->second, buffer->second);
      }
      record_effects(task_index);
      return true;
    };

    for (const auto& component : schedule.components) {
      for (const auto& wave : component.waves) {
        for (uint32_t task_index : wave.tasks) {
          if (cancelled[task_index] != 0) continue;
          started[task_index] = 1;
          submission->result.fault = {};
          const uint64_t encoders_before = stats.encoder_count;
          const uint64_t waits_before = stats.queue_wait_count;
          std::string task_error;
          bool ok = false;
          if (tasks[task_index].kind == core::TaskKind::Compute) {
            ok = run_compute(task_index, &task_error);
          } else {
            const std::array<uint32_t, 1> raster_task{task_index};
            const size_t result_count = submission->raster_results.size();
            bool command_submitted = false;
            ok = raster(metal, compiled, arena, raster_task, &stats, submission,
                        &command_submitted, &task_error);
            if (!ok && command_submitted)
              produced_output = produced_output || std::ranges::any_of(
                  compiled.plan.task_effects[task_index], [](const auto& effect) {
                    return effect.access != ir::Access::Read;
                  });
            if (ok && submission->raster_results.size() == result_count + 1) {
              const auto& result = submission->raster_results.back();
              produced_output = produced_output || (result.stored && result.contents_defined);
            }
          }
          task_encoder_boundaries[task_index] = stats.encoder_count - encoders_before;
          task_host_waits[task_index] = stats.queue_wait_count - waits_before;
          if (!ok) {
            failures.push_back({task_index, std::move(task_error), submission->result.fault});
            cancel_descendants(task_index);
          }
        }
      }
    }

    apply_dispatch_stats(stats, submission);
    // CompiledPlan records planned lowering. Submission records only the
    // transitions actually reached, including successful representation
    // preludes, never waits for cancelled or pre-command failed producers.
    for (const auto& transition : compiled.transition_operations) {
      if (!transition.covers_execution_completion) continue;
      const auto& component = schedule.components[transition.component];
      for (uint32_t producer : component.waves[transition.before_wave].tasks) {
        submission->report.transition_encoder_boundary_count += task_encoder_boundaries[producer];
        submission->report.transition_host_wait_count += task_host_waits[producer];
      }
      if (std::ranges::any_of(component.waves[transition.after_wave].tasks,
                             [&](uint32_t consumer) { return started[consumer] != 0; }))
        ++submission->report.transition_serialized_fallback_count;
    }
    if (!failures.empty()) {
      std::vector<uint32_t> rank(task_count, UINT32_MAX);
      for (uint32_t index = 0; index < schedule.task_order.size(); ++index)
        rank[schedule.task_order[index]] = index;
      const auto primary = std::ranges::min_element(failures, [&](const auto& left, const auto& right) {
        return rank[left.task] < rank[right.task];
      });
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = produced_output ? core::PoisonState::PartiallyProduced
                                                  : core::PoisonState::Poisoned;
      submission->result.message = primary->message;
      submission->result.fault = primary->fault;
      if (submission->result.fault.code.empty()) submission->result.fault.code = "METAL_TASK_FAILED";
      submission->result.fault.message = primary->message;
      submission->result.fault.task_index = primary->task;
    } else {
      submission->result.ok = true;
      submission->result.poison = core::PoisonState::Valid;
      if (signal_value != 0) metal.impl_->timeline_event.signaledValue = signal_value;
    }
    submission->timeline_value =
        metal.impl_->timeline_event != nil ? metal.impl_->timeline_event.signaledValue : 0;
    return Flow::Finish;
  }

  static Flow publish_tasks(DeviceHal& metal, const hal::CompiledPlan& compiled,
                            const std::vector<uint32_t>& order,
                            DispatchStats* stats, hal::Submission* submission, std::string* error) {
    if (compiled.plan.task_graph.tasks().empty()) return Flow::Continue;
    const auto& tasks = compiled.plan.task_graph.tasks();
    const bool compute_only = std::ranges::all_of(order, [&](uint32_t task_index) {
      return task_index < tasks.size() && tasks[task_index].kind == core::TaskKind::Compute;
    });
    const bool host_split = order.size() != tasks.size() || submission->envelope_overflow.has_value();
    if (order.empty()) return Flow::Continue;
    if (host_split || !compute_only) {
      std::string publish_error;
      if (!publish_envelope_order(compiled.plan.task_graph, order, &submission->published_tasks, &publish_error)) {
        submission->result.ok = false;
        submission->result.message = publish_error;
        return Flow::Finish;
      }
      return Flow::Continue;
    }
    const auto count = static_cast<uint32_t>(tasks.size());
    id<MTLBuffer> state_buffer = [metal.impl_->device newBufferWithLength:std::max<size_t>(count * sizeof(uint32_t), 1)
                                                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> fields_buffer =
        [metal.impl_->device newBufferWithLength:std::max<size_t>(count * compiler::kTaskRingWordsPerRecord * sizeof(uint32_t), 1)
                                         options:MTLResourceStorageModeShared];
    id<MTLBuffer> inputs_buffer =
        [metal.impl_->device newBufferWithLength:std::max<size_t>(count * compiler::kTaskRingWordsPerRecord * sizeof(uint32_t), 1)
                                         options:MTLResourceStorageModeShared];
    if (state_buffer == nil || fields_buffer == nil || inputs_buffer == nil) {
      submission->result.ok = false;
      submission->result.message = "Metal task ring buffer allocation failed";
      return Flow::Finish;
    }
    std::memset([state_buffer contents], 0, count * sizeof(uint32_t));
    auto* inputs = static_cast<uint32_t*>([inputs_buffer contents]);
    for (uint32_t i = 0; i < count; ++i) {
      compiler::ComputeTaskRingRecord record;
      std::string codec_error;
      if (!compiler::make_compute_task_ring_record(tasks[i], &record, &codec_error) ||
          !compiler::pack_compute_task_ring_record(
              record,
              std::span<uint32_t>(inputs + i * compiler::kTaskRingWordsPerRecord,
                                  compiler::kTaskRingWordsPerRecord),
              &codec_error)) {
        submission->result.ok = false;
        submission->result.message = "Metal compute Task ring encode failed: " + codec_error;
        return Flow::Finish;
      }
    }
    std::string task_pipeline_error;
    if (!metal.impl_->ensure_task_ring_pipeline(&task_pipeline_error)) {
      submission->result.ok = false;
      submission->result.message = "Metal task ring pipeline compile failed: " + task_pipeline_error;
      return Flow::Finish;
    }
    std::string publish_error;
    if (!metal.impl_->dispatch_task_publish({.state = state_buffer, .fields = fields_buffer, .inputs = inputs_buffer},
                                            count, stats, &publish_error)) {
      submission->result.ok = false;
      submission->result.message = "Metal task ring dispatch failed: " + publish_error;
      return Flow::Finish;
    }
    const auto* states = static_cast<const uint32_t*>([state_buffer contents]);
    const auto* fields = static_cast<const uint32_t*>([fields_buffer contents]);
    submission->published_tasks.reserve(count);
    for (uint32_t index : order) {
      if (states[index] != static_cast<uint32_t>(core::PublicationState::Published)) {
        submission->result.ok = false;
        submission->result.message = "task ring slot did not reach Published state";
        return Flow::Finish;
      }
      compiler::ComputeTaskRingRecord record;
      std::string codec_error;
      if (!compiler::unpack_compute_task_ring_record(
              std::span<const uint32_t>(fields + index * compiler::kTaskRingWordsPerRecord,
                                        compiler::kTaskRingWordsPerRecord),
              &record, &codec_error)) {
        submission->result.ok = false;
        submission->result.message = "Metal compute Task ring decode failed: " + codec_error;
        return Flow::Finish;
      }
      submission->published_tasks.push_back(compiler::make_task_record(record));
    }
    return Flow::Continue;
  }

  static Flow publish(DeviceHal& metal, const hal::CompiledPlan& compiled,
                      const std::vector<uint32_t>& order,
                      hal::Submission* submission, std::string* error) {
    DispatchStats stats;
    const Flow flow = publish_tasks(metal, compiled, order, &stats, submission, error);
    submission->cpu_encode_ns += stats.cpu_encode_ns;
    submission->cpu_submit_ns += stats.cpu_submit_ns;
    submission->report.encoder_count += stats.encoder_count;
    submission->report.command_buffer_count += stats.command_buffer_count;
    submission->report.barrier_count += stats.barrier_count;
    submission->report.queue_wait_count += stats.queue_wait_count;
    if (flow == Flow::Finish) {
      submission->result.outputs_valid = false;
      submission->result.poison = core::PoisonState::Poisoned;
    }
    return flow;
  }
};

bool DeviceHal::submit_plan(const hal::CompiledPlan& compiled, core::Arena& arena, hal::Submission* submission,
                       std::string* error) {
  if (!hal::validate_stage7_compiled_plan(compiled, hal::BackendKind::Metal, error)) return false;
  if (!SubmitOps::begin(compiled, arena, submission, error)) return false;
  const uint64_t wait_value = compiled.plan.timeline_wait;
  const uint64_t signal_value = compiled.plan.timeline_signal;
  bool result = false;
  if (SubmitOps::take(SubmitOps::precheck_timeline(*this, wait_value, signal_value, submission), &result))
    return result;
  if (SubmitOps::take(SubmitOps::sealed_effects(compiled, submission), &result)) return result;
  // Freeze publication admission before a representation can change epochs,
  // retire facets or submit a physical transform. Publication reuses this order.
  std::vector<uint32_t> publish_order;
  if (!hal::apply_envelope_continuation(compiled.plan, &envelope_continuations(), submission,
                                        &publish_order, error)) return false;
  hal::SubmissionLifetimeHold lifetime_hold;
  if (!lifetime_hold.prepare(compiled.plan, arena, facet_pool(), error)) return false;
  if (!SubmitOps::stage5(*this, compiled, arena, submission, error)) return false;
  if (!lifetime_hold.acquire(submission->representation_facets, error)) return false;

  // Publication is the complete Envelope-filtered semantic graph and precedes
  // execution, as it does on Reference. A later logical Task fault therefore
  // does not make Raster records disappear from the observed submission.
  if (SubmitOps::take(SubmitOps::publish(*this, compiled, publish_order, submission, error), &result)) return result;
  if (SubmitOps::execute_schedule(*this, compiled, arena, signal_value, submission) == SubmitOps::Flow::Fail)
    return false;
  return true;
}

}  // namespace vg::metal
