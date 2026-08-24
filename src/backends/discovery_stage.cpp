#include "backends/device_hal.h"

#include <string>

namespace vg::hal {

// Host-assisted discovery (02 §7.2, ADR-036). Never DevicePass: the walk
// reads 12-byte PointerRefs out of allocation bytes on the host and the
// reachable set comes back to the host before any later submit. A
// GPU-compact-and-continue path is out of scope. The result is a semantic
// reachable set / proxy, not an OS page-migration claim (06 §10).

bool run_discovery_stage(const ExecutionPlan& plan, core::Arena& arena, Submission* submission,
                         std::string* error) {
  if (plan.discovery_seeds.empty()) return true;
  if (submission == nullptr) {
    if (error) *error = "submission output is required";
    return false;
  }
  if (plan.requested_certificate_mode == core::AccessCertificateMode::SoftwarePaged ||
      plan.requested_certificate_mode == core::AccessCertificateMode::FaultManaged) {
    submission->report.add("discovery", LoweringClass::Unsupported, 1, 0,
                           "SoftwarePaged/FaultManaged stay Unsupported; discovery will not "
                           "approximate them");
    if (error)
      *error = "this access certificate mode has no implementation; callers must classify it "
               "Unsupported";
    return false;
  }

  core::DiscoveryResult discovery;
  if (!core::discover_reachable(arena, plan.discovery_seeds, &discovery, error)) return false;

  core::AccessCertificate certificate;
  if (!core::build_discovered_certificate(arena, discovery, &certificate, error)) return false;
  if (!core::certificate_covers_discovery_witness(certificate, discovery.reachable, error)) {
    return false;
  }

  // Fill a lease from the discovered set via add(ref, discovered, ...).
  // A name absent from the proven set is already a refuse (ADR-035).
  core::WorkingSetLease lease;
  for (const auto& ref : discovery.reachable) {
    if (!lease.add(ref, discovery.reachable, error)) return false;
    const core::Allocation* allocation = arena.lookup(ref);
    if (allocation != nullptr) lease.byte_limit += allocation->size;
  }
  lease.complete = true;

  if (plan.working_set_lease.has_value()) {
    // A caller-supplied lease is a witness claim. An extra name is a
    // refuse, not a silent enlarge of the discovered set (02 §10).
    if (!plan.working_set_lease->valid(discovery.reachable, error)) return false;
    if (!core::certificate_covers_discovery_witness(
            certificate, plan.working_set_lease->allocations, error))
      return false;
  }

  submission->access_certificate = certificate;
  submission->report.add(
      "discovery", LoweringClass::HostAssisted, certificate.epoch.references().size(),
      certificate.result_bytes,
      "host walk of seed topology (HostAssisted, not DevicePass); semantic reachable set / "
      "proxy, not OS page migration; discovery_host_ns=" +
          std::to_string(certificate.discovery_host_ns) +
          " scanned_bytes=" + std::to_string(certificate.scanned_bytes));
  return true;
}

}  // namespace vg::hal
