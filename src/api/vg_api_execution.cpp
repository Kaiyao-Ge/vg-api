#include "api/vg_api_internal.h"
#include "api/vg_api_handle_registry.h"

#include "core/execution_plan.h"

#include <memory>
#include <string>

namespace vg_api {
namespace {
HandleRegistry<VgExecutionEnvelope_T> g_envelopes;
HandleRegistry<VgTimeline_T> g_timelines;
HandleRegistry<VgSubmission_T> g_submissions;

VgResult classify_plan_error(const std::string& error) {
  if (error.find("unknown or stale node") != std::string::npos ||
      error.find("stale") != std::string::npos)
    return VG_ERROR_STALE_HANDLE;
  if (error.find("Unsupported") != std::string::npos ||
      error.find("unsupported") != std::string::npos)
    return VG_ERROR_UNSUPPORTED;
  return VG_ERROR_INVALID_ARGUMENT;
}
}  // namespace

bool is_valid_execution_envelope(VgExecutionEnvelope envelope) { return g_envelopes.contains(envelope); }
bool is_valid_timeline(VgTimeline timeline) { return g_timelines.contains(timeline); }
bool is_valid_submission(VgSubmission submission) { return g_submissions.contains(submission); }

VgResult VG_CALL create_execution_envelope(VgDevice device, const VgExecutionEnvelopeDesc* desc,
                                            VgExecutionEnvelope* out_envelope) {
  if (!is_valid_device(device)) {
    set_diagnostic("device handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (desc == nullptr || out_envelope == nullptr) {
    set_diagnostic("execution envelope descriptor and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result =
      validate_header(desc->header, VG_STRUCTURE_EXECUTION_ENVELOPE_DESC, sizeof(VgExecutionEnvelopeDesc));
  if (header_result != VG_SUCCESS) return header_result;
  if (!is_valid_arena(desc->arena)) {
    set_diagnostic("arena handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (desc->allowed_nodes == nullptr && desc->allowed_node_count != 0) {
    set_diagnostic("allowed node list is required when allowed_node_count is non-zero");
    return VG_ERROR_INVALID_ARGUMENT;
  }

  auto wrapper = std::make_unique<VgExecutionEnvelope_T>();
  wrapper->owner_device = device;
  wrapper->arena = desc->arena;
  wrapper->envelope.allowed_nodes.reserve(desc->allowed_node_count);
  for (uint32_t i = 0; i < desc->allowed_node_count; ++i) {
    const vg::core::NodeTable::Ref ref{desc->allowed_nodes[i].index, desc->allowed_nodes[i].generation};
    if (!device->nodes.contains(ref)) {
      set_diagnostic("execution envelope references an unknown or stale node");
      return VG_ERROR_INVALID_ARGUMENT;
    }
    wrapper->envelope.allowed_nodes.push_back(ref);
  }
  if (desc->access_certificate != nullptr) {
    wrapper->envelope.has_certificate_mode = true;
    wrapper->envelope.certificate_mode =
        static_cast<vg::core::AccessCertificateMode>(desc->access_certificate->mode);
    const VgAccessCertificateDesc& certificate_desc = *desc->access_certificate;
    if (certificate_desc.ranges == nullptr && certificate_desc.range_count != 0) {
      set_diagnostic("access certificate range list is required when range_count is non-zero");
      return VG_ERROR_INVALID_ARGUMENT;
    }
    wrapper->envelope.certificate_touched.reserve(certificate_desc.range_count);
    for (uint32_t i = 0; i < certificate_desc.range_count; ++i) {
      if (certificate_desc.ranges[i].allocation == nullptr) {
        set_diagnostic("access certificate range allocation handle is required");
        return VG_ERROR_INVALID_ARGUMENT;
      }
      const auto* allocation =
          reinterpret_cast<const vg::core::Allocation*>(certificate_desc.ranges[i].allocation);
      // VgAllocation carries no handle registry of its own (see the
      // VgAllocation exception noted in vg_api_internal.h) -- it's a raw
      // pointer into desc->arena's core::Arena::allocations_ map, so a stale
      // caller-held VgAllocation from an already-destroyed arena would
      // otherwise be dereferenced here. desc->arena is already validated
      // live above; confirm the pointer itself still resolves to one of that
      // arena's live map entries before ever reading through it.
      if (!desc->arena->arena.is_live_allocation(allocation)) {
        set_diagnostic("access certificate range allocation handle is stale or invalid");
        return VG_ERROR_STALE_HANDLE;
      }
      wrapper->envelope.certificate_touched.push_back({allocation->id, allocation->generation});
    }
  }
  wrapper->envelope.has_task_quota = desc->has_task_quota != 0;
  wrapper->envelope.task_quota = desc->task_quota;
  wrapper->envelope.timeline_wait = desc->timeline_wait_value;
  wrapper->envelope.timeline_signal = desc->timeline_signal_value;

  *out_envelope = g_envelopes.insert(std::move(wrapper));
  return VG_SUCCESS;
}

void VG_CALL destroy_execution_envelope(VgExecutionEnvelope envelope) {
  if (!g_envelopes.contains(envelope)) return;
  g_envelopes.erase(envelope);
}

VgResult VG_CALL create_timeline(VgDevice device, VgTimeline* out_timeline) {
  if (!is_valid_device(device)) {
    set_diagnostic("device handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (out_timeline == nullptr) {
    set_diagnostic("output timeline handle is required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  auto wrapper = std::make_unique<VgTimeline_T>();
  wrapper->timeline = &device->timeline;
  *out_timeline = g_timelines.insert(std::move(wrapper));
  return VG_SUCCESS;
}

void VG_CALL destroy_timeline(VgTimeline timeline) {
  if (!g_timelines.contains(timeline)) return;
  g_timelines.erase(timeline);
}

VgResult VG_CALL wait_timeline(VgTimeline timeline, uint64_t value) {
  if (!g_timelines.contains(timeline)) {
    set_diagnostic("timeline handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (value == 0) {
    set_diagnostic("timeline wait point must be non-zero");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  std::string error;
  if (!timeline->timeline->validate_wait(value, &error)) {
    set_diagnostic(error.c_str());
    return VG_ERROR_TIMEOUT;
  }
  return VG_SUCCESS;
}

VgResult VG_CALL submit(VgDevice device, const VgSubmitDesc* submit_desc, VgSubmission* out_submission) {
  if (!is_valid_device(device)) {
    set_diagnostic("device handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (submit_desc == nullptr || out_submission == nullptr) {
    set_diagnostic("submit descriptor and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result = validate_header(submit_desc->header, VG_STRUCTURE_SUBMIT_DESC, sizeof(VgSubmitDesc));
  if (header_result != VG_SUCCESS) return header_result;
  if (!is_valid_task_graph(submit_desc->graph)) {
    set_diagnostic("task graph handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (!is_valid_execution_envelope(submit_desc->envelope)) {
    set_diagnostic("execution envelope handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }

  VgTaskGraph_T* graph_wrapper = submit_desc->graph;
  VgExecutionEnvelope_T* envelope_wrapper = submit_desc->envelope;
  if (graph_wrapper->owner_device != device || envelope_wrapper->owner_device != device) {
    set_diagnostic("task graph and execution envelope must belong to the submitting device");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  if (!is_valid_arena(envelope_wrapper->arena)) {
    set_diagnostic("execution envelope's arena handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  VgArena_T* arena_wrapper = envelope_wrapper->arena;
  if (!graph_wrapper->graph.published()) {
    std::string publish_error;
    if (!graph_wrapper->graph.publish(&publish_error)) {
      set_diagnostic(publish_error.c_str());
      return VG_ERROR_INVALID_STATE;
    }
  }

  // TOCTOU narrowing (ADR-044 Concurrency): re-validate immediately before
  // each handle's last dereference in this function. This does not close
  // the race (a concurrent destroy can still land in the gap after these
  // checks) -- it only shrinks the window between the early validation
  // above and the point where a dangling pointer would actually be used.
  if (!is_valid_task_graph(graph_wrapper)) {
    set_diagnostic("task graph handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (!is_valid_execution_envelope(envelope_wrapper)) {
    set_diagnostic("execution envelope handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }

  // Stage 0--5 have one construction path.  The C ABI decodes handles and
  // publishes the graph above; core owns Node snapshots, envelope authority,
  // effect/certificate facts, and the immutable plan handed to DeviceHAL.
  vg::core::ExecutionPlan plan;
  std::string error;
  vg::core::ExecutionPlanAssemblerInputs assembler_inputs{
      &graph_wrapper->graph, &device->nodes, &envelope_wrapper->envelope,
      &arena_wrapper->arena, nullptr, nullptr, nullptr, arena_wrapper->arena.topology_epoch()};
  assembler_inputs.facet_pool = &device->hal->facet_pool();
  if (!vg::core::ExecutionPlanAssembler::assemble(assembler_inputs, &plan, &error)) {
    set_diagnostic(error.c_str());
    return classify_plan_error(error);
  }

  vg::hal::CompiledPlan compiled;
  if (!device->hal->compile(plan, &compiled, &error)) {
    set_diagnostic(error.c_str());
    return classify_plan_error(error);
  }

  // Last touch of device and arena_wrapper in this function -- re-validate
  // both immediately before device->hal->submit() below (see comment above).
  if (!is_valid_device(device)) {
    set_diagnostic("device handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (!is_valid_arena(arena_wrapper)) {
    set_diagnostic("execution envelope's arena handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  auto wrapper = std::make_unique<VgSubmission_T>();
  if (!device->hal->submit(compiled, arena_wrapper->arena, &wrapper->submission, &error)) {
    set_diagnostic(error.c_str());
    return VG_ERROR_INVALID_STATE;
  }
  wrapper->lowering_json = wrapper->submission.report.canonical_json();
  wrapper->execution_result_json = wrapper->submission.result.canonical_json();
  *out_submission = g_submissions.insert(std::move(wrapper));
  return VG_SUCCESS;
}

void VG_CALL destroy_submission(VgSubmission submission) {
  if (!g_submissions.contains(submission)) return;
  g_submissions.erase(submission);
}

VgResult VG_CALL get_submission_lowering_report(VgSubmission submission, const char** out_json) {
  if (!g_submissions.contains(submission)) {
    set_diagnostic("submission handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (out_json == nullptr) {
    set_diagnostic("output json pointer is required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  *out_json = submission->lowering_json.c_str();
  return VG_SUCCESS;
}

// v1.2 (ADR-045): submit() previously discarded hal::Submission::result
// entirely -- VG_SUCCESS from submit() only ever meant "the submission
// mechanism accepted the plan," never "the execution actually succeeded."
// This surfaces the real outcome (ok/poison/message/fault/witness/
// missing_effects/outputs_valid) the same way getSubmissionLoweringReport
// already surfaces the lowering report: as a canonical JSON string valid
// until the submission is destroyed.
VgResult VG_CALL get_submission_execution_result(VgSubmission submission, const char** out_json) {
  if (!g_submissions.contains(submission)) {
    set_diagnostic("submission handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (out_json == nullptr) {
    set_diagnostic("output json pointer is required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  *out_json = submission->execution_result_json.c_str();
  return VG_SUCCESS;
}

}  // namespace vg_api
