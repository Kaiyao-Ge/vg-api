#include "backends/device_hal.h"

namespace vg::hal {
namespace {

uint64_t sum_active_bytes(const core::Arena& arena) {
  uint64_t total = 0;
  for (const auto& [id, allocation] : arena.allocations()) {
    (void)id;
    if (allocation.state != core::ObjectState::Active) continue;
    total += allocation.size;
  }
  return total;
}

bool sum_leased_bytes(const core::WorkingSetLease& lease, const core::Arena& arena, uint64_t* out,
                      std::string* error) {
  uint64_t total = 0;
  for (const core::PointerRef& ref : lease.allocations) {
    const core::Allocation* allocation = arena.lookup(ref.allocation, ref.generation);
    if (allocation == nullptr) {
      if (error) *error = "working-set lease names a missing or stale allocation";
      return false;
    }
    total += allocation->size;
  }
  *out = total;
  return true;
}

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

bool apply_working_set_budget(const ExecutionPlan& plan, core::Arena& arena, Submission* submission,
                              std::string* error) {
  if (submission == nullptr) {
    if (error) *error = "submission output is required";
    return false;
  }
  if (!plan.working_set_budget.has_value() && !plan.working_set_lease.has_value()) return true;

  uint64_t requested = 0;
  if (plan.working_set_lease.has_value()) {
    if (!sum_leased_bytes(*plan.working_set_lease, arena, &requested, error)) return false;
  } else {
    requested = sum_active_bytes(arena);
  }

  std::string budget_error;
  const bool allowed =
      !plan.working_set_budget.has_value() || plan.working_set_budget->allows(requested, &budget_error);
  report_working_set(submission, requested, allowed);
  if (allowed) return true;
  if (error) *error = budget_error;
  return false;
}

}  // namespace vg::hal
