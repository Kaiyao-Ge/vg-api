#include "api/vg_api_internal.h"
#include "api/vg_api_handle_registry.h"

#include "ir/ir.h"

#include <memory>
#include <string>

namespace vg::core {

// Defined here rather than core.cpp: this is the one place core-layer state
// legitimately reaches into backends::ExecutionPlan (see core.h). Only the
// fields an envelope actually authorizes are spliced in -- apply_to never
// builds an AccessCertificate itself, since the backend already derives one
// automatically from plan.module when requested_certificate_mode is set and
// discovery_seeds is empty (reference_device_hal.cpp's automatic-discovery
// path), which is what certificate_touched/certificate_mode feed into.
void ExecutionEnvelope::apply_to(hal::ExecutionPlan& plan) const {
  plan.authorized_node_classes = allowed_node_classes;
  if (has_certificate_mode) {
    plan.requested_certificate_mode = certificate_mode;
  }
  if (has_task_quota) {
    plan.envelope_task_quota = task_quota;
  }
  plan.timeline_wait = timeline_wait;
  plan.timeline_signal = timeline_signal;
}

}  // namespace vg::core

namespace vg_api {
namespace {
HandleRegistry<VgExecutionEnvelope_T> g_envelopes;
HandleRegistry<VgTimeline_T> g_timelines;
HandleRegistry<VgSubmission_T> g_submissions;
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
  wrapper->arena = desc->arena;
  wrapper->envelope.allowed_node_classes.reserve(desc->allowed_node_count);
  // Only .index is copied; .generation is intentionally dropped here, unlike
  // task_graph_append()'s VgTaskRecord.node handling (vg_api_taskgraph.cpp),
  // which validates {index,generation} against builder->code_object->nodes.
  // This function takes a VgDevice, not a VgCodeObject, so no single
  // core::NodeTable is in scope to validate against -- an envelope is
  // device-scoped and can be applied across submits of different task
  // graphs/code objects. allowed_node_classes is therefore a plain
  // node-class allow-list matched by value at submit time against whichever
  // graph's already-NodeTable-validated TaskRecord.node_index values are
  // present (see select_tier2_nodes/validate_tier2_select), not a live
  // per-CodeObject node identity. See ADR-044's ExecutionEnvelope section.
  for (uint32_t i = 0; i < desc->allowed_node_count; ++i) {
    wrapper->envelope.allowed_node_classes.push_back(desc->allowed_nodes[i].index);
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
  if (!is_valid_arena(envelope_wrapper->arena)) {
    set_diagnostic("execution envelope's arena handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  VgArena_T* arena_wrapper = envelope_wrapper->arena;
  VgCodeObject_T* code_object = graph_wrapper->code_object;
  if (code_object == nullptr) {
    set_diagnostic("task graph has no associated code object");
    return VG_ERROR_INVALID_STATE;
  }
  if (!is_valid_code_object(code_object)) {
    set_diagnostic("task graph's code object handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }

  if (!graph_wrapper->graph.published()) {
    std::string publish_error;
    if (!graph_wrapper->graph.publish(&publish_error)) {
      set_diagnostic(publish_error.c_str());
      return VG_ERROR_INVALID_STATE;
    }
  }

  vg::hal::ExecutionPlan plan;
  // F3 (ADR-043 Decision #4): a "vg.msl.raster/v1" CodeObject carries a
  // restricted-import hand-written MSL raster shader, not the linear IR --
  // parse the envelope instead of ir::parse_module and leave plan.module at
  // its default (ExecutionPlan::validate() skips ir::verify(module) when
  // user_raster_shader is set). Every other format_tag keeps the pre-F3
  // ir::parse_module path unchanged.
  if (code_object->code.format_tag == "vg.msl.raster/v1") {
    try {
      const std::string text(code_object->code.bytes.begin(), code_object->code.bytes.end());
      plan.user_raster_shader = vg::ir::parse_msl_raster_envelope(text);
    } catch (const std::exception& e) {
      set_diagnostic(e.what());
      return VG_ERROR_INVALID_ARGUMENT;
    }
  } else {
    vg::ir::Module module;
    try {
      const std::string text(code_object->code.bytes.begin(), code_object->code.bytes.end());
      module = vg::ir::parse_module(text);
    } catch (const std::exception& e) {
      set_diagnostic(e.what());
      return VG_ERROR_INVALID_ARGUMENT;
    }
    plan.module = std::move(module);
  }

  plan.capabilities = device->hal->capabilities();
  plan.published = true;
  // TOCTOU narrowing (ADR-044 Concurrency): re-validate immediately before
  // each handle's last dereference in this function. This does not close
  // the race (a concurrent destroy can still land in the gap after these
  // checks) -- it only shrinks the window between the early validation
  // above and the point where a dangling pointer would actually be used.
  if (!is_valid_task_graph(graph_wrapper)) {
    set_diagnostic("task graph handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  plan.task_graph = graph_wrapper->graph;
  plan.graph_epoch = arena_wrapper->arena.topology_epoch();
  if (!is_valid_execution_envelope(envelope_wrapper)) {
    set_diagnostic("execution envelope handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  envelope_wrapper->envelope.apply_to(plan);

  vg::hal::CompiledPlan compiled;
  std::string error;
  if (!device->hal->compile(plan, &compiled, &error)) {
    set_diagnostic(error.c_str());
    return VG_ERROR_INVALID_ARGUMENT;
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
