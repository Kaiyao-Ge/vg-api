#include "backends/device_hal.h"

namespace vg::hal {
namespace {

void report_working_set(Submission* submission, uint64_t requested, bool committed) {
  // No OS residency counter is available in this helper (M1 unified memory
  // does not expose a public working-set counter we can treat as truth).
  // Every byte figure is therefore a request/commit/proxy, and the reason
  // must say so -- 06 §10: do not write a missing counter as "no migration".
  submission->report.add("working_set_requested", LoweringClass::Direct, 1, requested,
                         "this-submit requested bytes (proxy: not an OS residency counter)");
  submission->report.add("working_set_committed", LoweringClass::Direct, 1, committed ? requested : 0,
                         committed ? "this-submit committed bytes (proxy: not an OS residency counter)"
                                   : "not committed; working-set budget exceeded (proxy: not an OS "
                                     "residency counter)");
  submission->report.add("working_set_proxy", LoweringClass::Direct, 1, requested,
                         "proxy: allocation-size stand-in; unified memory is not infinite");
  // Sparse is optional research (09 E011). Reporting Unsupported is the
  // honest contract: Metal sparse heap/texture is not implemented, and
  // Vulkan sparse binding is explicit map/unmap -- not automatic fault.
  submission->report.add("working_set_sparse", LoweringClass::Unsupported, 1, 0,
                         "sparse residency is Unsupported; not an automatic page fault");
}

}  // namespace

bool apply_working_set_budget(const core::ExecutionPlan& plan, core::Arena& arena, Submission* submission,
                              std::string* error) {
  if (submission == nullptr) {
    if (error) *error = "submission output is required";
    return false;
  }
  (void)arena;
  if (!plan.assembled || !plan.capability_requirements_derived) {
    if (error) *error = "Stage 7 working-set operation requires a core-assembled execution plan";
    return false;
  }
  if (!plan.working_set_budget.has_value() && !plan.working_set_lease.has_value()) return true;
  if (!plan.access_plan_derived ||
      (plan.working_set_budget.has_value() && !plan.working_set_budget_checked)) {
    if (error) *error = "assembled execution plan is missing sealed working-set facts";
    return false;
  }
  report_working_set(submission, plan.working_set_requested_bytes, true);
  return true;
}

}  // namespace vg::hal
