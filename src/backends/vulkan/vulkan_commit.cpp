#include "backends/vulkan/vulkan_device_internal.h"
#include "compiler/compute_task_ring.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace vg::vulkan::detail {

bool same_compute_bindings(const std::vector<vg::compiler::ComputeBinding>& left,
                           const std::vector<vg::compiler::ComputeBinding>& right) {
  return left.size() == right.size() &&
         std::ranges::equal(left, right, [](const auto& lhs, const auto& rhs) {
           return lhs.allocation == rhs.allocation && lhs.binding == rhs.binding;
         });
}

bool same_vulkan_compute_package(const vg::compiler::ComputePackage& actual,
                                 const vg::compiler::ComputePackage& expected) {
  return actual.canonical_ir_hash == expected.canonical_ir_hash &&
         actual.root_schema == expected.root_schema &&
         actual.vulkan_glsl_source == expected.vulkan_glsl_source &&
         same_compute_bindings(actual.bindings, expected.bindings);
}

bool node_ref_equal(vg::core::NodeTable::Ref left, vg::core::NodeTable::Ref right) {
  return left.index == right.index && left.generation == right.generation;
}

bool DeviceState::submit(const vg::hal::CompiledPlan& compiled, vg::core::Arena& arena,
                       vg::hal::Submission* submission, std::string* error) {
  if (!vg::hal::validate_stage7_compiled_plan(compiled, vg::hal::BackendKind::Vulkan, error)) return false;
  if (!submission) { set_error(error, "submission output is null"); return false; }
  if (!compiled.report.supported) { set_error(error, "compiled plan is unsupported"); return false; }
  if (!compiled.plan.validate(error)) return false;
  for (const auto& transition : compiled.transition_operations) {
    const uint64_t expected_barriers = transition.covers_execution_completion ? 1 : 0;
    if (transition.state != vg::hal::CompiledPlan::TransitionLoweringState::Lowered ||
        transition.barrier_count != expected_barriers ||
        transition.serialized_fallback != transition.covers_execution_completion ||
        transition.fence_count != 0 || transition.encoder_boundary_count != 0 ||
        transition.host_wait_count != 0) {
      set_error(error, "Vulkan compiled wave transition does not match its physical lowering");
      return false;
    }
  }
  if (compiled.per_node_packages.size() != compiled.plan.resolved_nodes.size()) {
    set_error(error, "compiled plan must contain exactly one package for every immutable NodeRef");
    return false;
  }
  for (const auto& node : compiled.plan.resolved_nodes) {
    if (!node.module.has_value()) {
      set_error(error, "compiled Vulkan plan contains a non-compute Node snapshot");
      return false;
    }
    const auto package_it = std::ranges::find_if(compiled.per_node_packages, [&](const auto& candidate) {
      return node_ref_equal(candidate.ref, node.ref);
    });
    if (package_it == compiled.per_node_packages.end() ||
        std::ranges::count_if(compiled.per_node_packages, [&](const auto& candidate) {
          return node_ref_equal(candidate.ref, node.ref);
        }) != 1) {
      set_error(error, "compiled plan has a missing or duplicate immutable NodeRef package");
      return false;
    }
    if (package_it->kind != vg::hal::CompiledPlan::NodePackageKind::CanonicalCompute ||
        !package_it->package.has_value()) {
      set_error(error, "compiled Vulkan NodeRef package has the wrong domain kind");
      return false;
    }
    const auto expected = vg::compiler::build_linear_compute_package(*node.module);
    if (!expected.ok || !same_vulkan_compute_package(*package_it->package, expected.package)) {
      set_error(error, "compiled NodeRef package contents disagree with the immutable module snapshot");
      return false;
    }
  }
  if (!compiled.plan.graph_epoch_matches(arena, error)) return false;

  *submission = {};
  submission->abi_version = vg::hal::kDeviceHalAbiVersion;
  submission->report = compiled.report;
  // Compilation costs remain historical facts. Planned execution costs are
  // replaced below only after the corresponding commands were really encoded.
  std::erase_if(submission->report.events, [](const auto& event) {
    return event.operation == "task_effect_barrier" || event.operation == "representation_transform" ||
           event.operation == "image_layout_transition" || event.operation == "consume_input" ||
           event.operation == "timeline";
  });
  submission->report.transition_barrier_count = 0;
  submission->report.transition_serialized_fallback_count = 0;
  if (!vg::hal::run_discovery_stage(compiled.plan, arena, submission, error)) return false;
  if (!vg::hal::apply_working_set_budget(compiled.plan, arena, submission, error)) return false;

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
    if (vkGetSemaphoreCounterValue(device_, timeline_semaphore_, &current) != VK_SUCCESS) {
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
  // The resulting order filters publication only; all scheduled Tasks still run.
  std::vector<uint32_t> publish_order;
  if (!vg::hal::apply_envelope_continuation(compiled.plan, &envelope_continuations(),
                                            submission, &publish_order, error))
    return false;

  vg::hal::SubmissionLifetimeHold lifetime_hold;
  if (!lifetime_hold.prepare(compiled.plan, arena, facet_pool(), error)) return false;

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
    const bool representation_committed = vg::hal::commit_representation_operations(
            compiled.plan, compiled.representation_operations, arena, facet_pool(),
            [&](const vg::core::RepresentationSemanticPlanItem& request, const vg::hal::CompiledPlan::PhysicalRepresentationOperation&, vg::core::FacetRef facet,
                vg::hal::RepresentationTransformCost* cost, std::string* physical_error) {
              return transform_representation(arena, request, facet, cost, &representation_counts,
                                              physical_error);
            },
            submission, &representation_error);
    submission->report.barrier_count = representation_counts.barrier_count;
    submission->report.command_buffer_count = representation_counts.command_buffer_count;
    submission->report.encoder_count = representation_counts.command_buffer_count;
    submission->report.queue_wait_count = representation_counts.queue_wait_count;
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
          "image_layout_transition", vg::hal::LoweringClass::Direct, representation_counts.barrier_count, 0,
          "vkCmdPipelineBarrier2 image barriers issued by the transfer passes above "
          "(UNDEFINED->TRANSFER_DST, then TRANSFER_DST->the target facet's read layout), reported apart "
          "from the representation transforms themselves (07 §7)");
    }
    // Only facets the pool has already retired are touched here: the images
    // are keyed by FacetRef generation and RepresentationEpoch, so a superseded
    // epoch's VkImage/VkImageView can be destroyed without disturbing the
    // allocation's current backing (07 §14). The new image is that backing now,
    // so nothing in this pass destroys it.
    const uint32_t retired_images = retire_stale_facet_images(arena, facet_pool());
    if (retired_images != 0) {
      submission->report.add("facet_image_retire", vg::hal::LoweringClass::Direct, retired_images, 0,
                             "VkImage/VkImageView/VkDeviceMemory belonging to retired facet slots or "
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
      for (const auto& request : compiled.plan.representation_plan) {
        if (!request.consume_input) continue;
        const vg::core::Allocation* allocation =
            arena.lookup(core::PointerRef{request.view.allocation, request.view.allocation_generation});
        if (allocation == nullptr || !allocation->bytes.empty()) continue;
        const auto it = allocation_map_.find(request.view.allocation);
        if (it == allocation_map_.end() || it->second.byte_size == 0) continue;
        const uint64_t released = static_cast<uint64_t>(it->second.byte_size);
        if (it->second.mapped != nullptr) vkUnmapMemory(device_, it->second.memory);
        if (it->second.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, it->second.buffer, nullptr);
        if (it->second.memory != VK_NULL_HANDLE) vkFreeMemory(device_, it->second.memory, nullptr);
        allocation_map_.erase(it);
        submission->report.add("consume_input_backing_release", vg::hal::LoweringClass::Direct, 1, released,
                               "the superseded linear representation's device buffer was destroyed at once "
                               "rather than retained to command-buffer completion (07 §14, E005)");
      }
    }
  }

  if (!lifetime_hold.acquire(submission->representation_facets, error)) return false;

  const auto& tasks = compiled.plan.task_graph.tasks();
  std::vector<CanonicalTaskDispatch> task_dispatches;
  task_dispatches.reserve(tasks.size());
  const auto& schedule = compiled.plan.execution_schedule;
  for (uint32_t component_index = 0; component_index < schedule.components.size(); ++component_index) {
    const auto& component = schedule.components[component_index];
    for (uint32_t wave_index = 0; wave_index < component.waves.size(); ++wave_index) {
      const auto& wave = component.waves[wave_index];
      bool first_in_wave = true;
      for (uint32_t task_index : wave.tasks) {
        CanonicalTaskDispatch dispatch;
        dispatch.task_index = task_index;
        if (first_in_wave)
          for (const auto& transition : compiled.transition_operations)
            if (transition.component == component_index && transition.after_wave == wave_index &&
                transition.barrier_count != 0)
              dispatch.transitions_before.push_back(transition.semantic_transition);
        task_dispatches.push_back(std::move(dispatch));
        first_in_wave = false;
      }
    }
  }
  std::vector<vg::core::Allocation*> touched;
  for (auto& dispatch : task_dispatches) {
    const uint32_t task_index = dispatch.task_index;
    const auto& task = tasks[task_index];
    const vg::core::NodeTable::Ref task_ref{task.node_index, task.node_generation};
    const auto node_it = std::ranges::find_if(compiled.plan.resolved_nodes, [&](const auto& node) {
      return node_ref_equal(node.ref, task_ref);
    });
    const auto package_it = std::ranges::find_if(compiled.per_node_packages, [&](const auto& package) {
      return node_ref_equal(package.ref, task_ref);
    });
    if (node_it == compiled.plan.resolved_nodes.end() ||
        package_it == compiled.per_node_packages.end() ||
        !node_it->module.has_value() || !package_it->package.has_value()) {
      set_error(error, "sealed Task references an unavailable immutable NodeRef package");
      return false;
    }
    const auto& module = *node_it->module;
    const auto& package = *package_it->package;
    const std::string cache_key = compute_pipeline_cache_key(*node_it, package);
    const auto pipeline_it = compute_pipeline_cache_.find(cache_key);
    if (pipeline_it == compute_pipeline_cache_.end() ||
        pipeline_it->second.binding_count != package.bindings.size()) {
      set_error(error, "Stage 7 cannot find the Stage-6 Vulkan pipeline for a sealed NodeRef package");
      return false;
    }

    std::map<uint64_t, std::pair<uint32_t, uint32_t>> generation_by_allocation;
    for (const auto& instruction : module.instructions)
      generation_by_allocation.emplace(
          instruction.allocation,
          std::make_pair(instruction.generation, instruction.representation_epoch));

    dispatch.x = task.x;
    dispatch.y = task.y;
    dispatch.z = task.z;
    dispatch.pipeline = &pipeline_it->second;
    dispatch.addresses.reserve(package.bindings.size());
    for (const auto& binding : package.bindings) {
      const auto generation = generation_by_allocation.find(binding.allocation);
      vg::core::Allocation* allocation = generation == generation_by_allocation.end()
          ? nullptr
          : arena.lookup(core::RepresentationRef{binding.allocation,
                                                 generation->second.first,
                                                 generation->second.second});
      if (allocation == nullptr) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::Poisoned;
        submission->result.message =
            "stale generation, representation epoch, or out-of-bounds allocation reference";
        submission->result.fault.task_index = task_index;
        submission->result.fault.code = "STALE_ALLOCATION";
        submission->result.fault.message = submission->result.message;
        return true;
      }
      AllocationRecord* record = nullptr;
      std::string buffer_error;
      if (!ensure_buffer(*allocation, &record, &buffer_error)) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::Poisoned;
        submission->result.message = "Vulkan buffer allocation failed: " + buffer_error;
        submission->result.fault.task_index = task_index;
        submission->result.fault.code = "BACKEND_ALLOCATION_FAILED";
        submission->result.fault.message = submission->result.message;
        return true;
      }
      dispatch.addresses.push_back(record->device_address);
      if (std::ranges::find(touched, allocation) == touched.end()) touched.push_back(allocation);
    }

  }

  std::string dispatch_error;
  const auto dispatch_start = std::chrono::steady_clock::now();
  TaskDispatchCounts dispatch_counts;
  const bool dispatched = dispatch_task_graph(task_dispatches, wait_value, signal_value,
                                               &dispatch_counts, &dispatch_error);
  submission->cpu_submit_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dispatch_start).count();
  for (size_t i = 0; i < dispatch_counts.dispatch_count; ++i) {
    const auto& dispatch = task_dispatches[i];
    const auto& task = tasks[dispatch.task_index];
    submission->report.add(
        "vulkan_task_dispatch", vg::hal::LoweringClass::Direct, 1, 0,
        "task=" + std::to_string(dispatch.task_index) + " NodeRef{" +
            std::to_string(task.node_index) + "," + std::to_string(task.node_generation) +
            "} groups=" + std::to_string(dispatch.x) + "x" +
            std::to_string(dispatch.y) + "x" + std::to_string(dispatch.z));
  }
  submission->report.command_buffer_count = representation_counts.command_buffer_count + dispatch_counts.command_buffer_count;
  submission->report.encoder_count = representation_counts.command_buffer_count + dispatch_counts.command_buffer_count;
  submission->report.barrier_count =
      representation_counts.barrier_count + dispatch_counts.barrier_count;
  submission->report.queue_wait_count = representation_counts.queue_wait_count + dispatch_counts.queue_wait_count;
  for (uint32_t transition : dispatch_counts.encoded_transitions) {
    ++submission->report.transition_barrier_count;
    ++submission->report.transition_serialized_fallback_count;
    submission->report.add("task_effect_barrier", vg::hal::LoweringClass::Serialized, 1, 0,
                           "encoded sealed wave transition=" + std::to_string(transition) +
                           "; conservative global compute visibility via vkCmdPipelineBarrier2");
  }
  if ((wait_value != 0 || signal_value != 0) && dispatch_counts.queue_wait_count != 0)
    submission->report.add("timeline", vg::hal::LoweringClass::Direct, 1, 0,
                           "submitted Vulkan timeline semaphore wait/signal");
  if (!dispatched) {
    submission->result.ok = false;
    submission->result.outputs_valid = false;
    submission->result.poison = dispatch_counts.queue_wait_count != 0
        ? vg::core::PoisonState::PartiallyProduced : vg::core::PoisonState::Poisoned;
    submission->result.message = "Vulkan dispatch failed: " + dispatch_error;
    submission->result.fault.code = "BACKEND_DISPATCH_FAILED";
    submission->result.fault.message = submission->result.message;
    return true;
  }
  // No separate host-side mirror: timeline_value always reflects what the
  // GPU actually reached, read back fresh rather than passed through from
  // the plan (which is merely the requested value).
  if (timeline_semaphore_ != VK_NULL_HANDLE) {
    uint64_t current = 0;
    vkGetSemaphoreCounterValue(device_, timeline_semaphore_, &current);
    submission->timeline_value = current;
  }

  for (auto* allocation : touched) {
    auto it = allocation_map_.find(allocation->id);
    if (it != allocation_map_.end() && !allocation->bytes.empty())
      std::memcpy(allocation->bytes.data(), it->second.mapped, allocation->bytes.size());
  }

  uint32_t witness_index = 0;
  for (uint32_t task_index : compiled.plan.execution_schedule.task_order)
    for (const auto& effect : compiled.plan.task_effects[task_index]) {
      submission->result.trace.push_back(effect);
      submission->result.witness.record(effect, witness_index++);
    }
  submission->result.ok = true;
  submission->result.outputs_valid = true;
  submission->result.poison = vg::core::PoisonState::Valid;

  if (!compiled.plan.task_graph.tasks().empty()) {
    // TASK-D5 / ADR-039 (compile-review-only): envelope continuation is a
    // host split of the assembler-sealed canonical schedule (HostAssisted). This
    // file does not implement overflow-buffer / next-submit, and must not
    // pretend a DelegatedEnvelope or firmware enlarge exists. Envelope
    // refusal uses "envelope task quota exceeded" / leftover deferred --
    // never the publication-ring string "publication ring quota overflow".
    // Pack -> dispatch the GPU publication kernel -> read back -> verify every
    // slot reached Published -> unpack, walking
    // slots in that sealed dependency/effect order so
    // submission->published_tasks is sequence-identical to
    // reference::execute_task_graph()'s oracle output (same ordering
    // convention as Metal's submit()).
    const auto& tasks = compiled.plan.task_graph.tasks();
    const uint32_t count = static_cast<uint32_t>(tasks.size());

    std::string task_pipeline_error;
    if (!ensure_task_ring_pipeline(&task_pipeline_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::PartiallyProduced;
      submission->result.message = "Vulkan task ring pipeline compile failed: " + task_pipeline_error;
      submission->result.fault.code = "TASK_PUBLICATION_FAILED";
      submission->result.fault.message = submission->result.message;
      return true;
    }

    TaskRingBuffers ring_buffers{};
    std::string ring_error;
    if (!create_task_ring_buffers(count, &ring_buffers, &ring_error)) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::PartiallyProduced;
      submission->result.message = "Vulkan task ring buffer allocation failed: " + ring_error;
      submission->result.fault.code = "TASK_PUBLICATION_FAILED";
      submission->result.fault.message = submission->result.message;
      return true;
    }
    uint32_t* inputs = static_cast<uint32_t*>(ring_buffers.inputs_mapped);
    for (uint32_t i = 0; i < count; ++i) {
      vg::compiler::ComputeTaskRingRecord record;
      std::string codec_error;
      if (!vg::compiler::make_compute_task_ring_record(tasks[i], &record, &codec_error) ||
          !vg::compiler::pack_compute_task_ring_record(
              record,
              std::span<uint32_t>(inputs + i * vg::compiler::kTaskRingWordsPerRecord,
                                  vg::compiler::kTaskRingWordsPerRecord),
              &codec_error)) {
        submission->result.ok = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message = "Vulkan compute Task ring encode failed: " + codec_error;
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
    const bool published = dispatch_task_ring_publication(ring_buffers, &publication_counts, &publication_error);
    submission->report.command_buffer_count += publication_counts.command_buffer_count;
    submission->report.encoder_count += publication_counts.command_buffer_count;
    submission->report.barrier_count += publication_counts.barrier_count;
    submission->report.queue_wait_count += publication_counts.queue_wait_count;
    if (publication_counts.dispatch_count != 0)
      submission->report.add("task_publication_dispatch", vg::hal::LoweringClass::Direct,
                             publication_counts.dispatch_count, 0,
                             "compute-only GPU ring published " + std::to_string(count) +
                             " records; Envelope-filtered canonical observation has " +
                             std::to_string(publish_order.size()) + " Tasks");
    if (publication_counts.barrier_count != 0)
      submission->report.add(
          "task_publication_barrier", vg::hal::LoweringClass::Direct, publication_counts.barrier_count, 0,
          "vkCmdPipelineBarrier2 makes the publication shader writes visible to host readback");
    if (!published) {
      submission->result.ok = false;
      submission->result.outputs_valid = false;
      submission->result.poison = vg::core::PoisonState::PartiallyProduced;
      submission->result.message = "Vulkan task ring publication failed: " + publication_error;
      submission->result.fault.code = "TASK_PUBLICATION_FAILED";
      submission->result.fault.message = submission->result.message;
      destroy_task_ring_buffers(&ring_buffers);
      return true;
    }
    submission->cpu_submit_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - publication_start).count();

    const uint32_t* states = static_cast<const uint32_t*>(ring_buffers.state_mapped);
    const uint32_t* fields = static_cast<const uint32_t*>(ring_buffers.fields_mapped);
    submission->published_tasks.reserve(count);
    for (uint32_t index : publish_order) {
      if (states[index] != static_cast<uint32_t>(vg::core::PublicationState::Published)) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message = "task ring slot did not reach Published state";
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        destroy_task_ring_buffers(&ring_buffers);
        return true;
      }
      vg::compiler::ComputeTaskRingRecord record;
      std::string codec_error;
      if (!vg::compiler::unpack_compute_task_ring_record(
              std::span<const uint32_t>(fields + index * vg::compiler::kTaskRingWordsPerRecord,
                                        vg::compiler::kTaskRingWordsPerRecord),
              &record, &codec_error)) {
        submission->result.ok = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message = "Vulkan compute Task ring decode failed: " + codec_error;
        submission->result.outputs_valid = false;
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        destroy_task_ring_buffers(&ring_buffers);
        return true;
      }
      const auto* expected_words = inputs + index * vg::compiler::kTaskRingWordsPerRecord;
      const auto* published_words = fields + index * vg::compiler::kTaskRingWordsPerRecord;
      if (!std::equal(expected_words, expected_words + vg::compiler::kTaskRingWordsPerRecord, published_words)) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = vg::core::PoisonState::PartiallyProduced;
        submission->result.message = "Vulkan publication record disagrees with sealed Task";
        submission->result.fault.code = "TASK_PUBLICATION_FAILED";
        submission->result.fault.message = submission->result.message;
        destroy_task_ring_buffers(&ring_buffers);
        return true;
      }
      // The ring verifies physical publication; only Core owns Task identity.
      submission->published_tasks.push_back(tasks[index]);
    }
    destroy_task_ring_buffers(&ring_buffers);
  }
  return true;
#endif
}

}  // namespace vg::vulkan::detail
