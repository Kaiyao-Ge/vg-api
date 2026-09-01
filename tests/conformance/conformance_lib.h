#ifndef VG_TESTS_CONFORMANCE_LIB_H_
#define VG_TESTS_CONFORMANCE_LIB_H_

#include "backends/device_hal.h"

#include <string>

// Shared B1 conformance suite, reused by all three backend-specific
// device_hal_conformance_*.cpp mains so semantic expectations are defined
// exactly once (ADR-013's stated intent) rather than duplicated per backend.
namespace vg::conformance {

struct ConformanceExpectation {
  // Capability-gated, not a hard true/false assertion: true asserts the
  // backend declares Capability::TaskPublication; false means "don't assert
  // either way" -- a backend that honestly hasn't implemented a capability
  // yet simply omits the bit rather than lying about it, and that omission
  // is not itself a conformance failure.
  bool expect_task_publication = false;
  bool expect_timeline = false;
};

// Runs the full suite against `device`: ABI/timeline validation contract
// checks (backed by the shared, backend-agnostic core::ExecutionPlan::validate
// logic), capability-gated snapshot assertions per `expectation`, and the one
// invariant that holds across all three backends -- for every M1 golden IR
// fixture, if this backend's LoweringReport claims success, its output bytes
// must match the reference oracle exactly for that same fixture. An honest
// "unsupported" is not a conformance failure; silently wrong bytes are.
// `repo_root` locates tests/fixtures/ir/*.vgir.json. Returns true iff every
// check passed; failures are logged to stderr with the fixture/check name.
bool run(vg::hal::DeviceHal& device, const std::string& backend_name,
        const ConformanceExpectation& expectation, const std::string& repo_root);

}  // namespace vg::conformance

#endif
