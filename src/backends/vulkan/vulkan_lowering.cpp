#include "backends/vulkan/vulkan_device_internal.h"
#include <algorithm>

namespace vg::vulkan::detail {

#if defined(VG_HAS_VULKAN)
void lower_wave_transitions(vg::hal::CompiledPlan* compiled) {
  for (auto& transition : compiled->transition_operations) {
    transition.state = vg::hal::CompiledPlan::TransitionLoweringState::Lowered;
    if (!transition.covers_execution_completion) continue;
    transition.barrier_count = 1;
    transition.serialized_fallback = true;
    ++compiled->report.transition_barrier_count;
    ++compiled->report.transition_serialized_fallback_count;
    compiled->report.add("task_effect_barrier", vg::hal::LoweringClass::Serialized, 1, 0,
                         "sealed wave transition=" + std::to_string(transition.semantic_transition) +
                         "; conservative global compute memory visibility via vkCmdPipelineBarrier2");
  }
}
#endif

bool DeviceState::can_lower_representation_requests(const vg::core::ExecutionPlan& plan,
                                                  std::string* reason) const {
  if (plan.representation_plan.empty()) return true;
#if !defined(VG_HAS_VULKAN)
  if (reason != nullptr)
    *reason = "Stage 5 representation transform is Unsupported: the Vulkan adapter is unavailable in "
              "this build, so no facet image can be created";
  return false;
#else
  if (!capabilities_.supports(vg::hal::Capability::RepresentationTransform)) {
    if (reason != nullptr)
      *reason = "Stage 5 representation transform is Unsupported on this device: the adapter did not "
                "claim Capability::RepresentationTransform, which requires synchronization2 for the "
                "layout barriers and an optimal-tiled image path for the target facet";
    return false;
  }
  for (size_t index = 0; index < plan.representation_plan.size(); ++index) {
    const auto& request = plan.representation_plan[index];
    const std::string label = "representation request " + std::to_string(index);
    const FormatSupport& support = format_support(request.view.format);
    if (!support.transfer_dst || !support.transfer_src) {
      if (reason != nullptr)
        *reason = label + " is Unsupported: this device's optimal tiling does not advertise transfer "
                          "for the view's format, so its linear backing cannot be copied into an image";
      return false;
    }
    // The target facet decides which format feature has to be present. A
    // missing one is reported rather than worked around by substituting a
    // format the caller did not ask for (06 §6.2).
    if (request.target_kind == vg::core::FacetKind::Sample && !support.sampled_image) {
      if (reason != nullptr)
        *reason = label + " is Unsupported: the view's format is not a sampled-image format on this device";
      return false;
    }
    if (request.target_kind == vg::core::FacetKind::Storage && !support.storage_image) {
      if (reason != nullptr)
        *reason = label + " is Unsupported: the view's format is not a storage-image format on this device";
      return false;
    }
    if (request.target_kind == vg::core::FacetKind::Attachment && !support.color_attachment) {
      if (reason != nullptr)
        *reason =
            label + " is Unsupported: the view's format is not a color-attachment format on this device";
      return false;
    }
    if (!request.view.swizzle.identity() && request.target_kind != vg::core::FacetKind::Sample) {
      if (reason != nullptr)
        *reason = label + " is Unsupported: a non-identity swizzle applies to a SampleFacet only, and a "
                          "Storage or Attachment target would silently ignore the channel mapping asked for";
      return false;
    }
    if (!request.consume_input) continue;
    // A ConsumeInput releases the allocation's linear backing at once
    // (core::Arena::consume_representation), and Stage 5 runs before this
    // submission's compute dispatch (03 §7). A plan that both consumes an
    // allocation's linear representation and then computes over that same
    // allocation is therefore asking for two incompatible things, and the
    // honest answer is to say so here rather than to dispatch over a buffer
    // whose bytes were just handed back.
    for (const auto& effects : plan.task_effects) for (const auto& effect : effects) {
      if (effect.allocation != request.view.allocation) continue;
      if (reason != nullptr)
        *reason = label + " is Unsupported: it asks for ConsumeInput on allocation " +
                  std::to_string(request.view.allocation) +
                  ", whose linear representation this plan's compute module also reads or writes; the "
                  "consume releases that backing before the dispatch could run";
      return false;
    }
  }
  return true;
#endif
}

bool DeviceState::compile(const vg::core::ExecutionPlan& plan,
                        vg::hal::CompiledPlan* compiled,
                        std::string* error) {
  if (!compiled) { set_error(error, "compiled plan output is null"); return false; }
  *compiled = {};
  if (!plan.validate(error)) return false;
  // Keep Vulkan's advertised capabilities tied to command sequences this
  // adapter actually records. These checks deliberately precede the shared
  // Stage-6 preflight so discovery/working-set policy and multi-Node lowering
  // cannot inherit a promise merely because synchronization2 is
  // available.  In every case return a complete, Vulkan-owned Unsupported
  // report rather than accepting the plan and silently using the linear path.
  const auto reject_plan_requirement = [&](const char* operation, const std::string& message) {
    compiled->abi_version = vg::hal::kDeviceHalAbiVersion;
    compiled->plan = plan;
    compiled->report = {};
    compiled->report.backend = vg::hal::BackendKind::Vulkan;
    compiled->report.supported = false;
    compiled->report.diagnostic = message;
    compiled->report.add(operation, vg::hal::LoweringClass::Unsupported, 1, 0, message);
    set_error(error, message.c_str());
    return false;
  };
  for (const auto& task : plan.task_graph.tasks()) {
    if (task.kind == vg::core::TaskKind::Raster)
      return reject_plan_requirement(
          "raster_task", "Vulkan Unsupported: NodeRef{" + std::to_string(task.node_index) + "," +
              std::to_string(task.node_generation) + "} domain=Raster; complete plan rejected before Commit");
  }
  if (!plan.discovery_seeds.empty())
    return reject_plan_requirement("discovery",
                                   "discovery is Unsupported on Vulkan: no host-assisted reachable-set walk is "
                                   "implemented by this adapter");
  if (plan.working_set_budget.has_value() || plan.working_set_lease.has_value())
    return reject_plan_requirement("working_set_sparse",
                                   "working-set residency is Unsupported on Vulkan: sparse binding is not an "
                                   "automatic fault-managed working-set implementation");
  if (!vg::hal::preflight_stage6(plan, capabilities(), vg::hal::BackendKind::Vulkan, compiled, error)) return false;
  // F2 (ADR-046) wired TaskGraph-driven rasterization through compile()/
  // submit() for the reference and Metal backends only; this backend's own
  // raster machinery (ensure_raster_pipeline/run_raster_facet below) is
  // separate, pre-existing, and permanently compile-review-only (ADR-043
  // §7). Without this check a Raster-kind task would fall through this
  // file's compute-only task-graph publication path. The shared checked codec
  // would also refuse it, but Stage 6 retains this capability-specific
  // Unsupported diagnostic for every raster shape, including
  // F5 indexed draws (START.md §4, invariant 10: "任何无法在当前
  // 硬件表达的语义必须返回 Unsupported...不允许静默伪装").
  std::string representation_reason;
  if (!can_lower_representation_requests(plan, &representation_reason)) {
    compiled->abi_version = vg::hal::kDeviceHalAbiVersion;
    compiled->plan = plan;
    compiled->report = {};
    compiled->report.backend = vg::hal::BackendKind::Vulkan;
    compiled->report.supported = false;
    compiled->report.diagnostic = representation_reason;
    compiled->report.add("representation_transform", vg::hal::LoweringClass::Unsupported,
                         plan.representation_plan.size(), 0, representation_reason);
    set_error(error, compiled->report.diagnostic.c_str());
    return false;
  }
  compiled->abi_version = vg::hal::kDeviceHalAbiVersion;
  compiled->plan = plan;
  compiled->report = {};
  compiled->report.backend = vg::hal::BackendKind::Vulkan;
  compiled->per_node_packages.reserve(plan.resolved_nodes.size());
  for (const auto& node : plan.resolved_nodes) {
    if (!node.module.has_value())
      return reject_plan_requirement(
          "per_node_lowering",
          "Vulkan compute-only lowering requires a canonical module for NodeRef{" +
              std::to_string(node.ref.index) + "," + std::to_string(node.ref.generation) + "}");
    if (std::ranges::any_of(node.module->instructions, [](const auto& instruction) {
          return instruction.op == "load_ref" || instruction.op == "load_via" ||
                 instruction.op == "store_via";
        }))
      return reject_plan_requirement(
          "node_compute_package",
          "Vulkan pointer-graph Node lowering is Unsupported by the current linear BDA package for NodeRef{" +
              std::to_string(node.ref.index) + "," + std::to_string(node.ref.generation) + "}");
    const auto package = vg::compiler::build_linear_compute_package(*node.module);
    if (!package.ok)
      return reject_plan_requirement(
          "node_compute_package",
          "Vulkan per-Node package compilation failed for NodeRef{" +
              std::to_string(node.ref.index) + "," + std::to_string(node.ref.generation) + "}: " +
              package.message);
    compiled->per_node_packages.push_back(
        {node.ref, vg::hal::CompiledPlan::NodePackageKind::CanonicalCompute,
         package.package, false});
    compiled->report.add(
        "node_compute_package", vg::hal::LoweringClass::Direct, 1,
        package.package.bindings.size(),
        "full NodeRef{" + std::to_string(node.ref.index) + "," +
            std::to_string(node.ref.generation) + "}-keyed GLSL package generated by B4");
  }

  // Accepted Stage 5 work, described before it runs so a caller can see what
  // submit() has committed to (03 §7's stage 6 output is the LoweringReport,
  // not a promise made after the fact). Three separate events per request
  // because 07 §7 requires a layout transition and a representation transform
  // to be reported apart, and 02 §4.2/06 §11 make a ConsumeInput a distinct
  // decision from the transform that made it possible.
  if (!plan.representation_plan.empty()) {
    for (const auto& request : plan.representation_plan) {
      compiled->representation_operations.push_back({vg::hal::CompiledPlan::RepresentationOperation::CopyToImage,
                                                     request.transform_order, "Vulkan buffer-to-image copy"});
      compiled->report.add("representation_transform", vg::hal::LoweringClass::DevicePass, 1,
                           request.view.byte_size(),
                           "linear->optimal transfer pass: an optimal-tiled VkImage plus one "
                           "vkCmdCopyBufferToImage per subresource out of the allocation's existing "
                           "BDA buffer, publishing a new RepresentationEpoch (02 §8, 07 §13)");
      compiled->report.add("image_layout_transition", vg::hal::LoweringClass::Direct, 2, 0,
                           "two vkCmdPipelineBarrier2 image barriers (UNDEFINED->TRANSFER_DST, then "
                           "TRANSFER_DST->the target facet's read layout), reported apart from the "
                           "transform itself (07 §7)");
      if (request.consume_input) {
        compiled->report.add("consume_input", vg::hal::LoweringClass::Direct, 1, 0,
                             "the superseded linear backing is released at once instead of being "
                             "retained until command-buffer completion, and this backend additionally "
                             "destroys that allocation's VkBuffer (07 §14)");
      }
    }
  }

#if !defined(VG_HAS_VULKAN)
  compiled->report.supported = false;
  compiled->report.diagnostic = "Vulkan adapter is unavailable in this build";
  compiled->report.add("vulkan_pipeline", vg::hal::LoweringClass::Unsupported, 1, 0, compiled->report.diagnostic);
  set_error(error, compiled->report.diagnostic.c_str());
  return false;
#else
  if ((plan.timeline_wait != 0 || plan.timeline_signal != 0) &&
      !capabilities_.supports(vg::hal::Capability::Timeline)) {
    compiled->report.supported = false;
    compiled->report.diagnostic = "timeline wait/signal requested but device does not support timeline semaphores";
    compiled->report.add("timeline", vg::hal::LoweringClass::Unsupported, 1, 0, compiled->report.diagnostic);
    set_error(error, compiled->report.diagnostic.c_str());
    return false;
  }
  for (size_t index = 0; index < compiled->per_node_packages.size(); ++index) {
    const auto& node = plan.resolved_nodes[index];
    const auto& package = *compiled->per_node_packages[index].package;
    const std::string cache_key = compute_pipeline_cache_key(node, package);
    const ComputePipelineRecord* pipeline = nullptr;
    bool cache_hit = false;
    std::string pipeline_error;
    if (!ensure_pipeline(cache_key, package.vulkan_glsl_source,
                         static_cast<uint32_t>(package.bindings.size()),
                         &pipeline, &cache_hit, &pipeline_error)) {
      compiled->report.supported = false;
      compiled->report.diagnostic =
          "Vulkan pipeline compilation failed for NodeRef{" +
          std::to_string(node.ref.index) + "," + std::to_string(node.ref.generation) + "}: " +
          pipeline_error;
      compiled->report.add("vulkan_pipeline", vg::hal::LoweringClass::Unsupported, 1, 0,
                           compiled->report.diagnostic);
      set_error(error, compiled->report.diagnostic.c_str());
      return false;
    }
    (void)pipeline;
    compiled->report.add(
        "vulkan_pipeline",
        cache_hit ? vg::hal::LoweringClass::CachedObject : vg::hal::LoweringClass::Direct,
        1, 0,
        "NodeRef{" + std::to_string(node.ref.index) + "," +
            std::to_string(node.ref.generation) + "} SPIR-V pipeline " +
            (cache_hit ? "reused from the backend-owned package cache"
                       : "compiled via glslc and cached by immutable package/entry identity"));
  }
  compiled->report.supported = true;
  if (!plan.task_graph.tasks().empty()) {
    const bool publication_pipeline_cache_hit = task_ring_pipeline_ != VK_NULL_HANDLE;
    std::string publication_pipeline_error;
    if (!ensure_task_ring_pipeline(&publication_pipeline_error)) {
      compiled->report.supported = false;
      compiled->report.diagnostic =
          "Vulkan task publication pipeline compilation failed: " + publication_pipeline_error;
      compiled->report.add("task_publication", vg::hal::LoweringClass::Unsupported, 1, 0,
                           compiled->report.diagnostic);
      set_error(error, compiled->report.diagnostic.c_str());
      return false;
    }
    compiled->report.add(
        "task_publication",
        publication_pipeline_cache_hit ? vg::hal::LoweringClass::CachedObject
                                       : vg::hal::LoweringClass::Direct,
        1, 0,
        "Vulkan task ring GPU publication pass; canonical Nodes execute separately per Task");
  }
  lower_wave_transitions(compiled);
  if (plan.timeline_wait != 0 || plan.timeline_signal != 0) {
    compiled->report.add("timeline", vg::hal::LoweringClass::Direct, 1, 0,
                         "VkSemaphore(TIMELINE) wait/signal surrounds the canonical task-graph submission");
  }
  return true;
#endif
}

}  // namespace vg::vulkan::detail
