#include "core/envelope.h"

#include <utility>

namespace vg::core {

bool Timeline::signal(uint64_t value, std::string* error) {
  if (value <= value_) { if (error) *error = "timeline signal must be strictly monotonic"; return false; }
  value_ = value;
  return true;
}

bool Timeline::validate_wait(uint64_t value, std::string* error) const {
  if (value == 0) { if (error) *error = "timeline wait point must be non-zero"; return false; }
  if (value > value_) { if (error) *error = "timeline wait point is unsatisfied"; return false; }
  return true;
}

bool EnvelopeOverflow::valid(std::string* error) const {
  switch (disposition) {
    case EnvelopeOverflowDisposition::None:
      if (overflow_task_count != 0 || continuation_token != 0) {
        if (error) *error = "an unused overflow record cannot carry leftover work or a continuation token";
        return false;
      }
      return true;
    case EnvelopeOverflowDisposition::Rejected:
      if (continuation_token != 0) {
        if (error) *error = "a rejected overflow cannot be marked continued";
        return false;
      }
      return true;
    case EnvelopeOverflowDisposition::Deferred:
      if (overflow_task_count == 0 || continuation_token == 0) {
        if (error) *error = "a deferred overflow requires leftover work and a continuation token";
        return false;
      }
      return true;
  }
  if (error) *error = "unknown overflow disposition";
  return false;
}

bool EnvelopeOverflow::continued() const {
  return disposition == EnvelopeOverflowDisposition::Deferred && continuation_token != 0 &&
         overflow_task_count != 0;
}

uint64_t EnvelopeContinuationTable::mint(std::vector<uint32_t> leftover_order) {
  if (leftover_order.empty()) return 0;
  uint64_t token = next_token_++;
  if (token == 0) token = next_token_++;
  leftover_.emplace(token, std::move(leftover_order));
  return token;
}

bool EnvelopeContinuationTable::contains(uint64_t token) const {
  return token != 0 && leftover_.contains(token);
}

bool EnvelopeContinuationTable::lookup(uint64_t token, std::vector<uint32_t>* leftover,
                                       std::string* error) const {
  if (token == 0) {
    if (error) *error = "envelope continuation token does not match";
    return false;
  }
  const auto found = leftover_.find(token);
  if (found == leftover_.end()) {
    if (error) *error = "envelope continuation token does not match";
    return false;
  }
  if (leftover) *leftover = found->second;
  return true;
}

bool EnvelopeContinuationTable::take(uint64_t token, std::vector<uint32_t>* leftover,
                                     std::string* error) {
  if (!lookup(token, leftover, error)) return false;
  leftover_.erase(token);
  return true;
}

}  // namespace vg::core
