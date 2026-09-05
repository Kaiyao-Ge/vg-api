#include "backends/vulkan/vulkan_device_internal.h"
#include "compiler/compute_task_ring.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace vg::vulkan::detail {

bool same_compute_bindings(
    const std::vector<vg::compiler::ComputeBinding> &left,
    const std::vector<vg::compiler::ComputeBinding> &right) {
  return left.size() == right.size() &&
         std::ranges::equal(left, right, [](const auto &lhs, const auto &rhs) {
           return lhs.allocation == rhs.allocation &&
                  lhs.binding == rhs.binding;
         });
}

bool same_vulkan_compute_package(const vg::compiler::ComputePackage &actual,
                                 const vg::compiler::ComputePackage &expected) {
  return actual.canonical_ir_hash == expected.canonical_ir_hash &&
         actual.root_schema == expected.root_schema &&
         actual.vulkan_glsl_source == expected.vulkan_glsl_source &&
         same_compute_bindings(actual.bindings, expected.bindings);
}

bool node_ref_equal(vg::core::NodeTable::Ref left,
                    vg::core::NodeTable::Ref right) {
  return left.index == right.index && left.generation == right.generation;
}

bool DeviceState::submit(const vg::hal::CompiledPlan &compiled,
                         vg::core::Arena &arena,
                         vg::hal::Submission *submission, std::string *error) {
  if (!vg::hal::validate_stage7_compiled_plan(
          compiled, vg::hal::BackendKind::Vulkan, error))
    return false;
  if (!submission) {
    set_error(error, "submission output is null");
    return false;
  }
  if (!compiled.report.supported) {
    set_error(error, "compiled plan is unsupported");
    return false;
  }
  if (!compiled.plan.validate(error))
    return false;
  for (const auto &transition : compiled.transition_operations) {
    const uint64_t expected_barriers =
        transition.covers_execution_completion ? 1 : 0;
    if (transition.state !=
            vg::hal::CompiledPlan::TransitionLoweringState::Lowered ||
        transition.barrier_count != expected_barriers ||
        transition.serialized_fallback !=
            transition.covers_execution_completion ||
        transition.fence_count != 0 || transition.encoder_boundary_count != 0 ||
        transition.host_wait_count != 0) {
      set_error(error, "Vulkan compiled wave transition does not match its "
                       "physical lowering");
      return false;
    }
  }
  if (compiled.per_node_packages.size() !=
      compiled.plan.resolved_nodes.size()) {
    set_error(error, "compiled plan must contain exactly one package for every "
                     "immutable NodeRef");
    return false;
  }
  for (const auto &node : compiled.plan.resolved_nodes) {
    const auto package_it = std::ranges::find_if(
        compiled.per_node_packages, [&](const auto &candidate) {
          return node_ref_equal(candidate.ref, node.ref);
        });
    if (package_it == compiled.per_node_packages.end() ||
        std::ranges::count_if(compiled.per_node_packages,
                              [&](const auto &candidate) {
                                return node_ref_equal(candidate.ref, node.ref);
                              }) != 1) {
      set_error(
          error,
          "compiled plan has a missing or duplicate immutable NodeRef package");
      return false;
    }
    if (node.execution_domain == vg::core::TaskKind::Raster) {
      if ((!node.module.has_value() && !node.user_raster_shader.has_value()) ||
          package_it->kind != vg::hal::CompiledPlan::NodePackageKind::Raster ||
          package_it->package.has_value()) {
        set_error(
            error,
            "compiled Vulkan Raster NodeRef package has the wrong domain kind");
        return false;
      }
      continue;
    }
    if (node.execution_domain != vg::core::TaskKind::Compute ||
        !node.module.has_value() ||
        package_it->kind !=
            vg::hal::CompiledPlan::NodePackageKind::CanonicalCompute ||
        !package_it->package.has_value()) {
      set_error(
          error,
          "compiled Vulkan Compute NodeRef package has the wrong domain kind");
      return false;
    }
    const bool pointer_graph = std::ranges::any_of(
        node.module->instructions, [](const auto &instruction) {
          return instruction.op == "load_ref" || instruction.op == "load_via" ||
                 instruction.op == "store_via";
        });
    const auto expected =
        pointer_graph
            ? vg::compiler::build_pointer_graph_compute_package(*node.module)
            : vg::compiler::build_linear_compute_package(*node.module);
    if (!expected.ok ||
        !same_vulkan_compute_package(*package_it->package, expected.package)) {
      set_error(error, "compiled NodeRef package contents disagree with the "
                       "immutable module snapshot");
      return false;
    }
  }
  if (!compiled.plan.graph_epoch_matches(arena, error))
    return false;

  *submission = {};
  submission->abi_version = vg::hal::kDeviceHalAbiVersion;
  submission->report = compiled.report;
  // Compilation costs remain historical facts. Planned execution costs are
  // replaced below only after the corresponding commands were really encoded.
  std::erase_if(submission->report.events, [](const auto &event) {
    return event.operation == "task_effect_barrier" ||
           event.operation == "representation_transform" ||
           event.operation == "image_layout_transition" ||
           event.operation == "consume_input" || event.operation == "timeline";
  });
  submission->report.transition_barrier_count = 0;
  submission->report.transition_serialized_fallback_count = 0;
  if (!vg::hal::run_discovery_stage(compiled.plan, arena, submission, error))
    return false;
  if (!vg::hal::apply_working_set_budget(compiled.plan, arena, submission,
                                         error))
    return false;

#if !defined(VG_HAS_VULKAN)
  set_error(error, "Vulkan adapter is unavailable in this build");
  return false;
#else
  const uint64_t wait_value = compiled.plan.timeline_wait;
  const uint64_t signal_value = compiled.plan.timeline_signal;
  // Pre-checked host-side, mirroring reference::execute()'s Timeline::
  // validate_wait/signal and Metal's identical pre-check: fail fast on an
  // unsatisfied wait or a non-monotonic signal rather than letting
  // vkQueueSubmit block forever on a wait value nothing will ever reach.
  if (wait_value != 0 || signal_value != 0) {
    std::string timeline_error;
    if (!ensure_timeline_semaphore(&timeline_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::Poisoned;
      submission->result.message = timeline_error;
      submission->result.fault.code = "TIMELINE_UNAVAILABLE";
      submission->result.fault.message = timeline_error;
      return true;
    }
    uint64_t current = 0;
    if (vkGetSemaphoreCounterValue(device_, timeline_semaphore_, &current) !=
        VK_SUCCESS) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::Poisoned;
      submission->result.message = "vkGetSemaphoreCounterValue failed";
      submission->result.fault.code = "TIMELINE_UNAVAILABLE";
      submission->result.fault.message = submission->result.message;
      return true;
    }
    if (wait_value != 0 && current < wait_value) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::Poisoned;
      submission->result.message = "timeline wait point is unsatisfied";
      submission->result.fault.code = "TIMELINE_WAIT_UNSATISFIED";
      submission->result.fault.message = submission->result.message;
      return true;
    }
    if (signal_value != 0 && signal_value <= current) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::Poisoned;
      submission->result.message = "timeline signal must be strictly monotonic";
      submission->result.fault.code = "TIMELINE_SIGNAL_NOT_MONOTONIC";
      submission->result.fault.message = submission->result.message;
      return true;
    }
  }

  // Validate and consume the shared continuation exactly once, before any
  // representation commit, GPU execution, output writeback, or Timeline signal.
  // The resulting order filters publication only; all scheduled Tasks still
  // run.
  std::vector<uint32_t> publish_order;
  if (!vg::hal::apply_envelope_continuation(compiled.plan,
                                            &envelope_continuations(),
                                            submission, &publish_order, error))
    return false;

  vg::hal::SubmissionLifetimeHold lifetime_hold;
  if (!lifetime_hold.prepare(compiled.plan, arena, facet_pool(), error))
    return false;

  const auto &tasks = compiled.plan.task_graph.tasks();
  struct ScheduledStep {
    uint32_t task_index{};
    bool raster{};
    std::vector<uint32_t> transitions_before;
  };
  std::vector<ScheduledStep> steps;
  steps.reserve(tasks.size());
  const auto &schedule = compiled.plan.execution_schedule;
  for (uint32_t component_index = 0;
       component_index < schedule.components.size(); ++component_index) {
    const auto &component = schedule.components[component_index];
    for (uint32_t wave_index = 0; wave_index < component.waves.size();
         ++wave_index) {
      const auto &wave = component.waves[wave_index];
      bool first_in_wave = true;
      for (uint32_t task_index : wave.tasks) {
        if (task_index >= tasks.size()) {
          set_error(error, "sealed Vulkan schedule names an out-of-range Task");
          return false;
        }
        const auto &task = tasks[task_index];
        const auto node_it = std::ranges::find_if(
            compiled.plan.resolved_nodes, [&](const auto &node) {
              return node_ref_equal(node.ref,
                                    {task.node_index, task.node_generation});
            });
        if (node_it == compiled.plan.resolved_nodes.end()) {
          set_error(
              error,
              "sealed Vulkan Task references an unavailable immutable NodeRef");
          return false;
        }
        ScheduledStep step;
        step.task_index = task_index;
        step.raster = node_it->execution_domain == vg::core::TaskKind::Raster;
        if (first_in_wave) {
          for (const auto &transition : compiled.transition_operations)
            if (transition.component == component_index &&
                transition.after_wave == wave_index &&
                transition.barrier_count != 0)
              step.transitions_before.push_back(transition.semantic_transition);
        }
        steps.push_back(std::move(step));
        first_in_wave = false;
      }
    }
  }

  if (steps.empty()) {
    set_error(error, "Vulkan sealed schedule contains no executable Tasks");
    return false;
  }

  struct Tier2ViewOwner {
    DeviceState &state;
    GpuDrawCommandView view{};
    ~Tier2ViewOwner() { destroy_plan_tier2_draw_commands(state, &view); }
  } tier2_view{*this};
  PlanIndirectStats tier2_counts{};
  std::vector<uint32_t> tier2_command_by_task(
      tasks.size(), std::numeric_limits<uint32_t>::max());
  bool tier2_active = false;
  std::vector<tier2::SelectionRecord> tier2_selection_records;
  std::optional<bool> tier2_indexed_draw;
  if (compiled.plan.tier2_selection_requested) {
    tier2_selection_records.reserve(steps.size());
    for (const auto &step : steps) {
      if (!step.raster)
        continue;
      const auto &task = tasks[step.task_index];
      if (task.kind != vg::core::TaskKind::Raster) {
        set_error(error,
                  "sealed Tier2 selection cannot consume a non-Raster Task");
        return false;
      }
      const vg::core::NodeTable::Ref task_ref{task.node_index,
                                              task.node_generation};
      if (std::ranges::find_if(
              compiled.plan.tier2_selection_nodes, [&](const auto ref) {
                return node_ref_equal(ref, task_ref);
              }) == compiled.plan.tier2_selection_nodes.end()) {
        set_error(error, "scheduled Raster Task NodeRef is outside the sealed "
                         "Tier2 authorization set");
        return false;
      }
      const bool task_indexed = task.index_count != 0;
      if (tier2_indexed_draw.has_value() &&
          *tier2_indexed_draw != task_indexed) {
        set_error(error, "sealed Tier2 Raster Tasks require a compatible "
                         "indirect command ABI");
        return false;
      }
      tier2_indexed_draw = task_indexed;
      uint32_t draw_count = task.index_count;
      if (!task_indexed) {
        const auto *vertex_slot =
            resolve_facet(arena, facet_pool(), task.vertex_buffer_ref,
                          vg::core::FacetKind::Address, error);
        const auto *vertices =
            vertex_slot == nullptr
                ? nullptr
                : arena.lookup(vg::core::PointerRef{
                      vertex_slot->view.allocation,
                      vertex_slot->view.allocation_generation});
        constexpr uint32_t kRasterVertexBytes = 5 * sizeof(float);
        if (vertices == nullptr ||
            vertices->bytes.size() % kRasterVertexBytes != 0 ||
            vertices->bytes.size() / kRasterVertexBytes >
                std::numeric_limits<uint32_t>::max()) {
          set_error(error,
                    "sealed Tier2 Raster Task has an invalid vertex facet");
          return false;
        }
        draw_count =
            static_cast<uint32_t>(vertices->bytes.size() / kRasterVertexBytes);
      }
      tier2_command_by_task[step.task_index] =
          static_cast<uint32_t>(tier2_selection_records.size());
      tier2_selection_records.push_back(
          {task_ref, draw_count, 1, 0, 0, 0, task_indexed});
    }
    if (tier2_selection_records.empty() || !tier2_indexed_draw.has_value()) {
      set_error(error, "sealed Tier2 selection has no scheduled Raster Tasks");
      return false;
    }
  }

  // Stage 5 runs before Stage 6/7's dispatch (03 §7), not alongside it. That
  // ordering is load-bearing for more than tidiness: a transform publishes a
  // new RepresentationEpoch, and the binding loop below resolves through
  // core::Arena::lookup(id, generation, epoch), which demands an exact epoch
  // match. A plan that transforms an allocation and then computes over it must
  // therefore author its instructions at the post-transform epoch -- the
  // caller's contract, not something this backend may quietly patch up.
  // compile() already refused the one combination that cannot be authored at
  // all (a ConsumeInput of an allocation the same module reads or writes).
  RepresentationStageCounts representation_counts{};
  if (!compiled.plan.representation_plan.empty()) {
    std::string representation_error;
    // The shared helper owns the parts that are core's, not the backend's:
    // acquiring the facet out of the device-owned pool, calling
    // core::Arena::transform_representation, deciding whether a ConsumeInput
    // is admissible and calling consume_representation, retiring stale facets,
    // and writing the RepresentationEvent into the submission (02 §4.2, 06
    // §11). This backend supplies only the physical step below, which is why
    // ConsumeInput is never inferred here.
    const bool representation_committed =
        vg::hal::commit_representation_operations(
            compiled.plan, compiled.representation_operations, arena,
            facet_pool(),
            [&](const vg::core::RepresentationSemanticPlanItem &request,
                const vg::hal::CompiledPlan::PhysicalRepresentationOperation &,
                vg::core::FacetRef facet,
                vg::hal::RepresentationTransformCost *cost,
                std::string *physical_error) {
              return transform_representation(arena, request, facet, cost,
                                              &representation_counts,
                                              physical_error);
            },
            submission, &representation_error);
    submission->report.barrier_count = representation_counts.barrier_count;
    submission->report.command_buffer_count =
        representation_counts.command_buffer_count;
    submission->report.encoder_count =
        representation_counts.command_buffer_count;
    submission->report.queue_wait_count =
        representation_counts.queue_wait_count;
    if (!representation_committed) {
      // A physical failure mid-stage is a hard submit() failure rather than a
      // poisoned result: the transform either happened or it did not, and the
      // helper has already recorded an Unsupported event describing which
      // request stopped.
      set_error(error, representation_error.c_str());
      return false;
    }
    if (representation_counts.barrier_count != 0) {
      submission->report.add(
          "image_layout_transition", vg::hal::LoweringClass::Direct,
          representation_counts.barrier_count, 0,
          "vkCmdPipelineBarrier2 image barriers issued by the transfer passes "
          "above "
          "(UNDEFINED->TRANSFER_DST, then TRANSFER_DST->the target facet's "
          "read layout), reported apart "
          "from the representation transforms themselves (07 §7)");
    }
    // Only facets the pool has already retired are touched here: the images
    // are keyed by FacetRef generation and RepresentationEpoch, so a superseded
    // epoch's VkImage/VkImageView can be destroyed without disturbing the
    // allocation's current backing (07 §14). The new image is that backing now,
    // so nothing in this pass destroys it.
    const uint32_t retired_images =
        retire_stale_facet_images(arena, facet_pool());
    if (retired_images != 0) {
      submission->report.add(
          "facet_image_retire", vg::hal::LoweringClass::Direct, retired_images,
          0,
          "VkImage/VkImageView/VkDeviceMemory belonging to retired facet slots "
          "or "
          "superseded RepresentationEpochs destroyed after the stage");
    }
    // ConsumeInput follow-through. Whether a consume happened is core's
    // decision, taken inside the helper (which refuses one whose transform
    // produced no distinct backing) -- this backend does not infer it and does
    // not re-derive it from the request. What it does is observe the outcome:
    // core::Arena::consume_representation() clears the allocation's bytes, so
    // an emptied allocation whose device mirror is still sized is exactly the
    // case where this backend is holding onto a superseded linear
    // representation. Leaving that VkBuffer alive would mean the peak-memory
    // saving E005 measures never materializes on the device side at all. Safe
    // here only because the stage's command buffers were waited on above.
    if (submission->consumed_allocation_count != 0) {
      for (const auto &request : compiled.plan.representation_plan) {
        if (!request.consume_input)
          continue;
        const vg::core::Allocation *allocation = arena.lookup(core::PointerRef{
            request.view.allocation, request.view.allocation_generation});
        if (allocation == nullptr || !allocation->bytes.empty())
          continue;
        const auto it = allocation_map_.find(request.view.allocation);
        if (it == allocation_map_.end() || it->second.byte_size == 0)
          continue;
        const uint64_t released = static_cast<uint64_t>(it->second.byte_size);
        if (it->second.mapped != nullptr)
          vkUnmapMemory(device_, it->second.memory);
        if (it->second.buffer != VK_NULL_HANDLE)
          vkDestroyBuffer(device_, it->second.buffer, nullptr);
        if (it->second.memory != VK_NULL_HANDLE)
          vkFreeMemory(device_, it->second.memory, nullptr);
        allocation_map_.erase(it);
        submission->report.add(
            "consume_input_backing_release", vg::hal::LoweringClass::Direct, 1,
            released,
            "the superseded linear representation's device buffer was "
            "destroyed at once "
            "rather than retained to command-buffer completion (07 §14, E005)");
      }
    }
  }

  if (!lifetime_hold.acquire(submission->representation_facets, error))
    return false;

  std::vector<vg::core::Allocation *> touched;
  std::vector<CanonicalTaskDispatch> compute_batch;
  compute_batch.reserve(steps.size());
  TaskDispatchCounts dispatch_counts{};
  PlanRasterStepStats raster_counts{};

  auto prepare_compute_dispatch = [&](const ScheduledStep &step) -> bool {
    const uint32_t task_index = step.task_index;
    const auto &task = tasks[task_index];
    const vg::core::NodeTable::Ref task_ref{task.node_index,
                                            task.node_generation};
    const auto node_it = std::ranges::find_if(
        compiled.plan.resolved_nodes,
        [&](const auto &node) { return node_ref_equal(node.ref, task_ref); });
    const auto package_it = std::ranges::find_if(
        compiled.per_node_packages, [&](const auto &package) {
          return node_ref_equal(package.ref, task_ref);
        });
    if (node_it == compiled.plan.resolved_nodes.end() ||
        node_it->execution_domain != vg::core::TaskKind::Compute ||
        package_it == compiled.per_node_packages.end() ||
        !node_it->module.has_value() || !package_it->package.has_value()) {
      set_error(
          error,
          "sealed Task references an unavailable immutable NodeRef package");
      return false;
    }
    const auto &module = *node_it->module;
    const auto &package = *package_it->package;
    const std::string cache_key = compute_pipeline_cache_key(*node_it, package);
    const auto pipeline_it = compute_pipeline_cache_.find(cache_key);
    if (pipeline_it == compute_pipeline_cache_.end() ||
        pipeline_it->second.binding_count != package.bindings.size()) {
      set_error(error, "Stage 7 cannot find the Stage-6 Vulkan pipeline for a "
                       "sealed NodeRef package");
      return false;
    }

    std::map<uint64_t, std::pair<uint32_t, uint32_t>> generation_by_allocation;
    for (const auto &instruction : module.instructions)
      generation_by_allocation.emplace(
          instruction.allocation,
          std::make_pair(instruction.generation,
                         instruction.representation_epoch));

    CanonicalTaskDispatch dispatch;
    dispatch.task_index = task_index;
    dispatch.transitions_before = step.transitions_before;
    dispatch.x = task.x;
    dispatch.y = task.y;
    dispatch.z = task.z;
    dispatch.pipeline = &pipeline_it->second;
    dispatch.addresses.reserve(package.bindings.size());
    for (const auto &binding : package.bindings) {
      const auto generation = generation_by_allocation.find(binding.allocation);
      vg::core::Allocation *allocation =
          generation == generation_by_allocation.end()
              ? nullptr
              : arena.lookup(core::RepresentationRef{
                    binding.allocation, generation->second.first,
                    generation->second.second});
      if (allocation == nullptr) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::Poisoned;
        submission->result.message = "stale generation, representation epoch, "
                                     "or out-of-bounds allocation reference";
        submission->result.fault.task_index = task_index;
        submission->result.fault.code = "STALE_ALLOCATION";
        submission->result.fault.message = submission->result.message;
        return false;
      }
      AllocationRecord *record = nullptr;
      std::string buffer_error;
      if (!ensure_buffer(*allocation, &record, &buffer_error)) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::Poisoned;
        submission->result.message =
            "Vulkan buffer allocation failed: " + buffer_error;
        submission->result.fault.task_index = task_index;
        submission->result.fault.code = "BACKEND_ALLOCATION_FAILED";
        submission->result.fault.message = submission->result.message;
        return false;
      }
      dispatch.addresses.push_back(record->device_address);
      if (std::ranges::find(touched, allocation) == touched.end())
        touched.push_back(allocation);
    }
    compute_batch.push_back(std::move(dispatch));
    return true;
  };

  auto synchronize_mapped_allocations =
      [&](const std::vector<CanonicalTaskDispatch> &completed) {
        for (auto *allocation : touched) {
          const auto it = allocation_map_.find(allocation->id);
          if (it == allocation_map_.end() || allocation->bytes.empty())
            continue;
          std::memcpy(allocation->bytes.data(), it->second.mapped,
                      allocation->bytes.size());
        }
        std::vector<vg::core::Allocation *> modified;
        for (const auto &dispatch : completed) {
          for (const auto &effect :
               compiled.plan.task_effects[dispatch.task_index]) {
            const bool writes_content =
                effect.access == vg::ir::Access::Write ||
                effect.access == vg::ir::Access::Atomic ||
                effect.access == vg::ir::Access::Publish;
            if (!writes_content)
              continue;
            const auto allocation_it =
                std::ranges::find_if(touched, [&](const auto *allocation) {
                  return allocation->id == effect.allocation;
                });
            if (allocation_it != touched.end() &&
                std::ranges::find(modified, *allocation_it) == modified.end()) {
              arena.mark_content_modified(**allocation_it);
              modified.push_back(*allocation_it);
            }
          }
        }
      };
  auto merge_dispatch_counts = [](TaskDispatchCounts *total,
                                  const TaskDispatchCounts &batch) {
    total->dispatch_count += batch.dispatch_count;
    total->barrier_count += batch.barrier_count;
    total->command_buffer_count += batch.command_buffer_count;
    total->queue_wait_count += batch.queue_wait_count;
    total->encoded_transitions.insert(total->encoded_transitions.end(),
                                      batch.encoded_transitions.begin(),
                                      batch.encoded_transitions.end());
  };
  auto submit_timeline_marker =
      [&](uint64_t marker_wait, uint64_t marker_signal,
          TaskDispatchCounts *counts, std::string *marker_error) -> bool {
    if (marker_wait == 0 && marker_signal == 0)
      return true;
    if (!ensure_command_pool(device_, compute_queue_family_, &command_pool_,
                             marker_error))
      return false;
    VkCommandBuffer command_buffer{VK_NULL_HANDLE};
    if (!allocate_command_buffer(device_, command_pool_, &command_buffer,
                                 marker_error))
      return false;
    VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS ||
        vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
      vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
      set_error(marker_error, "vkBeginCommandBuffer/vkEndCommandBuffer failed "
                              "for Vulkan timeline marker");
      return false;
    }
    VkTimelineSemaphoreSubmitInfo timeline_info{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    if (marker_wait != 0) {
      timeline_info.waitSemaphoreValueCount = 1;
      timeline_info.pWaitSemaphoreValues = &marker_wait;
    }
    if (marker_signal != 0) {
      timeline_info.signalSemaphoreValueCount = 1;
      timeline_info.pSignalSemaphoreValues = &marker_signal;
    }
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const uint32_t wait_count = marker_wait != 0 ? 1 : 0;
    const uint32_t signal_count = marker_signal != 0 ? 1 : 0;
    if (counts != nullptr)
      ++counts->command_buffer_count;
    return submit_and_wait(
        device_, compute_queue_, command_pool_, command_buffer, &timeline_info,
        wait_count, wait_count != 0 ? &timeline_semaphore_ : nullptr,
        wait_count != 0 ? &wait_stage : nullptr, signal_count,
        signal_count != 0 ? &timeline_semaphore_ : nullptr, marker_error,
        counts != nullptr ? &counts->queue_wait_count : nullptr);
  };

  bool executed_work = false;
  auto ensure_tier2_commands = [&]() -> bool {
    if (!compiled.plan.tier2_selection_requested || tier2_active)
      return true;
    hal::LoweringReport tier2_report;
    std::string tier2_error;
    if (!submit_plan_tier2_indirect(
            *this, compiled.plan, compiled.per_node_packages,
            tier2_selection_records, *tier2_indexed_draw, &tier2_view.view,
            &tier2_counts, &tier2_report, &tier2_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison =
          executed_work || representation_counts.command_buffer_count != 0
              ? vg::core::PoisonState::PartiallyProduced
              : vg::core::PoisonState::Poisoned;
      submission->result.message =
          "Vulkan Tier2 command production failed: " + tier2_error;
      submission->result.fault.code = "BACKEND_TIER2_FAILED";
      submission->result.fault.message = submission->result.message;
      return false;
    }
    tier2_active = true;
    submission->report.add(
        "tier2_bucket_fill_draw_commands",
        vg::hal::LoweringClass::EmulatedDevicePass,
        tier2_counts.indirect_draw_count, tier2_counts.temporary_bytes,
        "GPU bucket/count/fill produced sealed Raster indirect commands; "
        "host did not read back or re-encode them");
    return true;
  };
  auto flush_compute_batch = [&](uint64_t batch_wait) -> bool {
    if (compute_batch.empty())
      return true;
    TaskDispatchCounts batch_counts{};
    std::string dispatch_error;
    if (!dispatch_task_graph(compute_batch, batch_wait, 0, &batch_counts,
                             &dispatch_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison =
          executed_work || batch_counts.queue_wait_count != 0
              ? vg::core::PoisonState::PartiallyProduced
              : vg::core::PoisonState::Poisoned;
      submission->result.message = "Vulkan dispatch failed: " + dispatch_error;
      submission->result.fault.code = "BACKEND_DISPATCH_FAILED";
      submission->result.fault.message = submission->result.message;
      return false;
    }
    for (const auto &dispatch : compute_batch) {
      const auto &task = tasks[dispatch.task_index];
      submission->report.add(
          "vulkan_task_dispatch", vg::hal::LoweringClass::Direct, 1, 0,
          "task=" + std::to_string(dispatch.task_index) + " NodeRef{" +
              std::to_string(task.node_index) + "," +
              std::to_string(task.node_generation) +
              "} groups=" + std::to_string(dispatch.x) + "x" +
              std::to_string(dispatch.y) + "x" + std::to_string(dispatch.z));
    }
    merge_dispatch_counts(&dispatch_counts, batch_counts);
    executed_work = true;
    synchronize_mapped_allocations(compute_batch);
    compute_batch.clear();
    return true;
  };

  const auto dispatch_start = std::chrono::steady_clock::now();
  bool first_submission = true;
  if (steps.front().raster && wait_value != 0) {
    TaskDispatchCounts marker_counts{};
    std::string marker_error;
    if (!submit_timeline_marker(wait_value, 0, &marker_counts, &marker_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::Poisoned;
      submission->result.message =
          "Vulkan timeline wait marker failed: " + marker_error;
      submission->result.fault.code = "BACKEND_DISPATCH_FAILED";
      submission->result.fault.message = submission->result.message;
      return true;
    }
    merge_dispatch_counts(&dispatch_counts, marker_counts);
    first_submission = false;
  }
  for (const auto &step : steps) {
    if (!step.raster) {
      if (!prepare_compute_dispatch(step)) {
        if (!submission->result.fault.code.empty())
          return true;
        return false;
      }
      continue;
    }
    const bool submitted_compute = !compute_batch.empty();
    if (!flush_compute_batch(first_submission ? wait_value : 0))
      return true;
    if (submitted_compute)
      first_submission = false;
    const bool submitted_tier2 =
        compiled.plan.tier2_selection_requested && !tier2_active;
    if (!ensure_tier2_commands())
      return true;
    if (submitted_tier2)
      first_submission = false;
    PlanRasterStepStats step_counts{};
    std::string raster_error;
    const uint32_t tier2_command = tier2_command_by_task[step.task_index];
    const GpuDrawCommandView *indirect_view =
        tier2_active && tier2_command != std::numeric_limits<uint32_t>::max()
            ? &tier2_view.view
            : nullptr;
    if (!submit_plan_raster_step(*this, compiled, step.task_index,
                                 step.transitions_before, arena, submission,
                                 &step_counts, &raster_error, indirect_view,
                                 tier2_command)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = executed_work
                                      ? vg::core::PoisonState::PartiallyProduced
                                      : vg::core::PoisonState::Poisoned;
      submission->result.message = "Vulkan Raster step failed: " + raster_error;
      submission->result.fault.task_index = step.task_index;
      submission->result.fault.code = "BACKEND_RASTER_FAILED";
      submission->result.fault.message = submission->result.message;
      return true;
    }
    executed_work = true;
    first_submission = false;
    raster_counts.draw_count += step_counts.draw_count;
    raster_counts.command_buffer_count += step_counts.command_buffer_count;
    raster_counts.encoder_count += step_counts.encoder_count;
    raster_counts.barrier_count += step_counts.barrier_count;
    raster_counts.queue_wait_count += step_counts.queue_wait_count;
    submission->report.add("vulkan_raster_draw", vg::hal::LoweringClass::Direct,
                           step_counts.draw_count, 0,
                           "task=" + std::to_string(step.task_index) +
                               " submitted through the sealed Raster step");
    for (uint32_t transition : step.transitions_before) {
      ++submission->report.transition_barrier_count;
      ++submission->report.transition_serialized_fallback_count;
      submission->report.add(
          "task_effect_barrier", vg::hal::LoweringClass::Serialized, 1, 0,
          "encoded sealed wave transition=" + std::to_string(transition) +
              "; vkCmdPipelineBarrier2 plus completed mapped backing handoff");
    }
  }
  if (!flush_compute_batch(first_submission ? wait_value : 0))
    return true;
  if (signal_value != 0) {
    TaskDispatchCounts marker_counts{};
    std::string marker_error;
    if (!submit_timeline_marker(0, signal_value, &marker_counts,
                                &marker_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = executed_work
                                      ? vg::core::PoisonState::PartiallyProduced
                                      : vg::core::PoisonState::Poisoned;
      submission->result.message =
          "Vulkan timeline signal marker failed: " + marker_error;
      submission->result.fault.code = "BACKEND_DISPATCH_FAILED";
      submission->result.fault.message = submission->result.message;
      return true;
    }
    merge_dispatch_counts(&dispatch_counts, marker_counts);
  }
  submission->cpu_submit_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - dispatch_start)
          .count();
  submission->report.command_buffer_count =
      representation_counts.command_buffer_count +
      dispatch_counts.command_buffer_count +
      raster_counts.command_buffer_count + tier2_counts.command_buffer_count;
  submission->report.encoder_count =
      representation_counts.command_buffer_count +
      dispatch_counts.command_buffer_count + raster_counts.encoder_count +
      tier2_counts.encoder_count;
  submission->report.barrier_count =
      representation_counts.barrier_count + dispatch_counts.barrier_count +
      raster_counts.barrier_count + tier2_counts.barrier_count;
  submission->report.queue_wait_count = representation_counts.queue_wait_count +
                                        dispatch_counts.queue_wait_count +
                                        raster_counts.queue_wait_count +
                                        tier2_counts.queue_wait_count;
  for (uint32_t transition : dispatch_counts.encoded_transitions) {
    ++submission->report.transition_barrier_count;
    ++submission->report.transition_serialized_fallback_count;
    submission->report.add(
        "task_effect_barrier", vg::hal::LoweringClass::Serialized, 1, 0,
        "encoded sealed wave transition=" + std::to_string(transition) +
            "; conservative global compute visibility via "
            "vkCmdPipelineBarrier2");
  }
  if ((wait_value != 0 || signal_value != 0) &&
      dispatch_counts.queue_wait_count != 0)
    submission->report.add("timeline", vg::hal::LoweringClass::Direct, 1, 0,
                           "submitted Vulkan timeline semaphore wait/signal");
  // No separate host-side mirror: timeline_value always reflects what the
  // GPU actually reached, read back fresh rather than passed through from
  // the plan (which is merely the requested value).
  if (timeline_semaphore_ != VK_NULL_HANDLE) {
    uint64_t current = 0;
    vkGetSemaphoreCounterValue(device_, timeline_semaphore_, &current);
    submission->timeline_value = current;
  }

  for (auto *allocation : touched) {
    auto it = allocation_map_.find(allocation->id);
    if (it != allocation_map_.end() && !allocation->bytes.empty())
      std::memcpy(allocation->bytes.data(), it->second.mapped,
                  allocation->bytes.size());
  }

  uint32_t witness_index = 0;
  for (uint32_t task_index : compiled.plan.execution_schedule.task_order)
    for (const auto &effect : compiled.plan.task_effects[task_index]) {
      submission->result.trace.push_back(effect);
      submission->result.witness.record(effect, witness_index++);
    }
  submission->result.ok = true;
  submission->result.outputs_valid = true;
  submission->result.poison = vg::core::PoisonState::Valid;

  if (!compiled.plan.task_graph.tasks().empty()) {
    // TASK-D5 / ADR-039 (compile-review-only): envelope continuation is a
    // host split of the assembler-sealed canonical schedule (HostAssisted).
    // This file does not implement overflow-buffer / next-submit, and must not
    // pretend a DelegatedEnvelope or firmware enlarge exists. Envelope
    // refusal uses "envelope task quota exceeded" / leftover deferred --
    // never the publication-ring string "publication ring quota overflow".
    // Pack -> dispatch the GPU publication kernel -> read back -> verify every
    // slot reached Published -> unpack, walking
    // slots in that sealed dependency/effect order so
    // submission->published_tasks is sequence-identical to
    // reference::execute_task_graph()'s oracle output (same ordering
    // convention as Metal's submit()).
    const auto &tasks = compiled.plan.task_graph.tasks();
    const uint32_t count = static_cast<uint32_t>(tasks.size());
    std::vector<uint32_t> compute_tasks;
    std::vector<uint32_t> ring_slot_by_task(
        count, std::numeric_limits<uint32_t>::max());
    for (uint32_t index = 0; index < count; ++index) {
      if (tasks[index].kind != vg::core::TaskKind::Compute)
        continue;
      ring_slot_by_task[index] = static_cast<uint32_t>(compute_tasks.size());
      compute_tasks.push_back(index);
    }

    TaskRingBuffers ring_buffers{};
    const uint32_t *states = nullptr;
    const uint32_t *fields = nullptr;
    uint32_t *inputs = nullptr;
    if (!compute_tasks.empty()) {
      std::string task_pipeline_error;
      if (!ensure_task_ring_pipeline(&task_pipeline_error)) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message =
            "Vulkan task ring pipeline compile failed: " + task_pipeline_error;
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        return true;
      }
      std::string ring_error;
      if (!create_task_ring_buffers(static_cast<uint32_t>(compute_tasks.size()),
                                    &ring_buffers, &ring_error)) {
        submission->result.ok = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message =
            "Vulkan task ring buffer allocation failed: " + ring_error;
        submission->result.outputs_valid = false;
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        return true;
      }
      inputs = static_cast<uint32_t *>(ring_buffers.inputs_mapped);
      for (uint32_t slot = 0; slot < compute_tasks.size(); ++slot) {
        vg::compiler::ComputeTaskRingRecord record;
        std::string codec_error;
        if (!vg::compiler::make_compute_task_ring_record(
                tasks[compute_tasks[slot]], &record, &codec_error) ||
            !vg::compiler::pack_compute_task_ring_record(
                record,
                std::span<uint32_t>(
                    inputs + slot * vg::compiler::kTaskRingWordsPerRecord,
                    vg::compiler::kTaskRingWordsPerRecord),
                &codec_error)) {
          submission->result.ok = false;
          submission->result.poison = vg::core::PoisonState::PartiallyProduced;
          submission->result.message =
              "Vulkan compute Task ring encode failed: " + codec_error;
          submission->result.outputs_valid = false;
          submission->result.fault.code = "TASK_PUBLICATION_FAILED";
          submission->result.fault.message = submission->result.message;
          destroy_task_ring_buffers(&ring_buffers);
          return true;
        }
      }
      std::string publication_error;
      const auto publication_start = std::chrono::steady_clock::now();
      TaskDispatchCounts publication_counts;
      const bool published = dispatch_task_ring_publication(
          ring_buffers, &publication_counts, &publication_error);
      submission->report.command_buffer_count +=
          publication_counts.command_buffer_count;
      submission->report.encoder_count +=
          publication_counts.command_buffer_count;
      submission->report.barrier_count += publication_counts.barrier_count;
      submission->report.queue_wait_count +=
          publication_counts.queue_wait_count;
      if (publication_counts.dispatch_count != 0)
        submission->report.add(
            "task_publication_dispatch", vg::hal::LoweringClass::Direct,
            publication_counts.dispatch_count, 0,
            "compute-only GPU ring published " +
                std::to_string(compute_tasks.size()) +
                " records; Envelope-filtered canonical observation has " +
                std::to_string(publish_order.size()) + " Tasks");
      if (publication_counts.barrier_count != 0)
        submission->report.add("task_publication_barrier",
                               vg::hal::LoweringClass::Direct,
                               publication_counts.barrier_count, 0,
                               "vkCmdPipelineBarrier2 makes the publication "
                               "shader writes visible to host readback");
      if (!published) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message =
            "Vulkan task ring publication failed: " + publication_error;
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        destroy_task_ring_buffers(&ring_buffers);
        return true;
      }
      submission->cpu_submit_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - publication_start)
              .count();
      states = static_cast<const uint32_t *>(ring_buffers.state_mapped);
      fields = static_cast<const uint32_t *>(ring_buffers.fields_mapped);
    }

    submission->published_tasks.reserve(count);
    uint32_t raster_publications = 0;
    for (uint32_t index : publish_order) {
      if (tasks[index].kind == vg::core::TaskKind::Raster) {
        ++raster_publications;
        submission->published_tasks.push_back(tasks[index]);
        continue;
      }
      const uint32_t slot = ring_slot_by_task[index];
      if (slot == std::numeric_limits<uint32_t>::max() ||
          states[slot] !=
              static_cast<uint32_t>(vg::core::PublicationState::Published)) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message =
            "task ring slot did not reach Published state";
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        destroy_task_ring_buffers(&ring_buffers);
        return true;
      }
      vg::compiler::ComputeTaskRingRecord record;
      std::string codec_error;
      if (!vg::compiler::unpack_compute_task_ring_record(
              std::span<const uint32_t>(
                  fields + slot * vg::compiler::kTaskRingWordsPerRecord,
                  vg::compiler::kTaskRingWordsPerRecord),
              &record, &codec_error) ||
          !std::equal(inputs + slot * vg::compiler::kTaskRingWordsPerRecord,
                      inputs +
                          (slot + 1) * vg::compiler::kTaskRingWordsPerRecord,
                      fields + slot * vg::compiler::kTaskRingWordsPerRecord)) {
        submission->result.ok = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message =
            "Vulkan compute publication record disagrees with sealed Task";
        submission->result.outputs_valid = false;
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        destroy_task_ring_buffers(&ring_buffers);
        return true;
      }
      submission->published_tasks.push_back(tasks[index]);
    }
    if (raster_publications != 0)
      submission->report.add("raster_task_publication",
                             vg::hal::LoweringClass::HostAssisted,
                             raster_publications, 0,
                             "completed formal Raster tasks published in "
                             "sealed order after their draw/readback");
    destroy_task_ring_buffers(&ring_buffers);
  }
  return true;
#endif
}

} // namespace vg::vulkan::detail
