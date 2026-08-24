#include "backends/device_hal.h"

#include <algorithm>
#include <cstddef>

namespace vg::hal {
namespace {

bool pending_is_deferred(const ExecutionPlan& plan) {
  return plan.pending_overflow.has_value() &&
         plan.pending_overflow->disposition == core::EnvelopeOverflowDisposition::Deferred;
}

bool pending_is_rejected(const ExecutionPlan& plan) {
  return plan.pending_overflow.has_value() &&
         plan.pending_overflow->disposition == core::EnvelopeOverflowDisposition::Rejected;
}

void report_host_split(Submission* submission, uint64_t count, const char* reason) {
  submission->report.add("envelope_continuation", LoweringClass::HostAssisted, count, 0, reason);
}

bool leftover_matches_graph(const ExecutionPlan& plan, const std::vector<uint32_t>& leftover,
                            std::string* error) {
  const auto task_count = static_cast<uint32_t>(plan.task_graph.tasks().size());
  if (!std::ranges::all_of(leftover, [task_count](uint32_t index) { return index < task_count; })) {
    if (error) *error = "envelope continuation leftover does not match this graph";
    return false;
  }
  return true;
}

}  // namespace

bool apply_envelope_continuation(const ExecutionPlan& plan, core::EnvelopeContinuationTable* table,
                                 Submission* submission, std::vector<uint32_t>* publish_order,
                                 std::string* error) {
  if (submission == nullptr) {
    if (error) *error = "submission output is required";
    return false;
  }
  if (publish_order == nullptr) {
    if (error) *error = "envelope publish order output is required";
    return false;
  }
  publish_order->clear();
  submission->envelope_overflow.reset();

  if (plan.pending_overflow.has_value() && !plan.pending_overflow->valid(error)) return false;

  if (pending_is_rejected(plan)) {
    if (error) *error = "envelope leftover was rejected";
    return false;
  }

  if (pending_is_deferred(plan)) {
    if (table == nullptr) {
      if (error) *error = "envelope continuation table is required";
      return false;
    }
    std::vector<uint32_t> leftover;
    if (!table->lookup(plan.pending_overflow->continuation_token, &leftover, error)) return false;
    if (leftover.size() != plan.pending_overflow->overflow_task_count) {
      if (error) *error = "envelope continuation leftover does not match";
      return false;
    }
    if (!leftover_matches_graph(plan, leftover, error)) return false;
    if (!table->take(plan.pending_overflow->continuation_token, &leftover, error)) return false;
    *publish_order = std::move(leftover);
    report_host_split(submission, publish_order->size(),
                      "host-assisted leftover drain; no DelegatedEnvelope");
    return true;
  }

  std::vector<uint32_t> order;
  if (!plan.task_graph.tasks().empty()) {
    if (!plan.task_graph.deterministic_order(&order, error)) return false;
  }

  if (!plan.envelope_task_quota.has_value()) {
    *publish_order = std::move(order);
    return true;
  }

  const uint32_t quota = *plan.envelope_task_quota;
  if (order.size() <= quota) {
    *publish_order = std::move(order);
    return true;
  }

  if (table == nullptr) {
    if (error) *error = "envelope continuation table is required";
    return false;
  }
  publish_order->assign(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(quota));
  std::vector<uint32_t> leftover(order.begin() + static_cast<std::ptrdiff_t>(quota), order.end());
  core::EnvelopeOverflow overflow;
  overflow.disposition = core::EnvelopeOverflowDisposition::Deferred;
  overflow.overflow_task_count = static_cast<uint32_t>(leftover.size());
  overflow.continuation_token = table->mint(std::move(leftover));
  if (!overflow.valid(error)) return false;
  submission->envelope_overflow = overflow;
  report_host_split(submission, overflow.overflow_task_count,
                    "envelope task quota exceeded; leftover deferred");
  return true;
}

}  // namespace vg::hal
