#include "backends/device_hal.h"

#include <string>

namespace vg::hal {

// Host-assisted discovery (02 §7.2, ADR-036) is performed by the core
// assembler before lowering. This Stage 7 hook only consumes and reports the
// frozen witness/certificate; it never reads Arena bytes to walk a graph.

bool run_discovery_stage(const ExecutionPlan& plan, core::Arena& arena, Submission* submission,
                         std::string* error) {
  if (submission == nullptr) {
    if (error) *error = "submission output is required";
    return false;
  }
  (void)arena;
  // An assembled plan has already frozen topology, walked the seed graph,
  // built its certificate and selected its lease in core Stage 0--5.  HAL
  // only records that physical/host operation; it never derives a second
  // witness from mutable Arena bytes.
  if (!plan.assembled || !plan.capability_requirements_derived) {
    if (error) *error = "Stage 7 discovery requires a core-assembled execution plan";
    return false;
  }
  if (!plan.requested_certificate_mode.has_value()) return true;
  if (!plan.access_plan_derived || !plan.access_certificate.has_value()) {
    if (error) *error = "assembled execution plan is missing sealed access-planning facts";
    return false;
  }
  submission->access_certificate = plan.access_certificate;
  const auto& certificate = *plan.access_certificate;
  if (plan.discovery_result.has_value()) {
    submission->report.add(
        "discovery", LoweringClass::HostAssisted, certificate.epoch.references().size(),
        certificate.result_bytes,
        "core-sealed host walk of seed topology; HAL consumed immutable discovery witness; "
        "discovery_host_ns=" + std::to_string(certificate.discovery_host_ns) +
            " scanned_bytes=" + std::to_string(certificate.scanned_bytes));
  } else {
    const auto classification = certificate.mode == core::AccessCertificateMode::DiscoverThenLease
                                    ? LoweringClass::HostAssisted : LoweringClass::Direct;
    submission->report.add("access_certificate", classification, certificate.epoch.references().size(),
                           certificate.result_bytes,
                           "core-sealed access certificate consumed without backend authority derivation");
  }
  return true;
}

}  // namespace vg::hal
