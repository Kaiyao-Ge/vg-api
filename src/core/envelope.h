#ifndef VG_CORE_ENVELOPE_H_
#define VG_CORE_ENVELOPE_H_

#include "core/access.h"
#include "core/node.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vg::core {

// Wait-then-signal pair for one submission. Adjacent uint64_t wait/signal
// parameters are otherwise interchangeable at every dispatch call site.
struct TimelineGate {
  uint64_t wait{};
  uint64_t signal{};
};

class Timeline {
 public:
  explicit Timeline(uint64_t value = 0) : value_(value) {}
  bool signal(uint64_t value, std::string* error = nullptr);
  [[nodiscard]] bool wait(uint64_t value) const { return value <= value_; }
  bool validate_wait(uint64_t value, std::string* error = nullptr) const;
  [[nodiscard]] uint64_t value() const { return value_; }

 private:
  uint64_t value_{};
};

// Work this submit could not fit. Rejected cannot be reported as
// continued. Deferred leftover requires a non-zero continuation token
// for the next submit -- not a silent quota increase (ADR-010's
// set_quota is build-time only).
enum class EnvelopeOverflowDisposition { None, Rejected, Deferred };

struct EnvelopeOverflow {
  uint32_t overflow_task_count{};
  EnvelopeOverflowDisposition disposition{EnvelopeOverflowDisposition::None};
  uint64_t continuation_token{};

  bool valid(std::string* error = nullptr) const;
  // True only for a valid Deferred leftover. A Rejected record never
  // answers true, even if a caller stuffed a token in.
  [[nodiscard]] bool continued() const;
};

// Per-device leftover buffer for E017 / ADR-039. Tokens are minted here
// so a second submit can present ExecutionPlan::pending_overflow without
// turning leftover into an implicit global queue. Token 0 is never issued.
class EnvelopeContinuationTable {
 public:
  // Stores leftover deterministic-order indices and returns a non-zero
  // token. Empty leftover is not a Deferred record -- mint returns 0 and
  // stores nothing.
  uint64_t mint(std::vector<uint32_t> leftover_order);
  [[nodiscard]] bool contains(uint64_t token) const;
  bool lookup(uint64_t token, std::vector<uint32_t>* leftover, std::string* error = nullptr) const;
  // Removes the leftover so a later submit cannot drain it twice.
  bool take(uint64_t token, std::vector<uint32_t>* leftover, std::string* error = nullptr);

 private:
  uint64_t next_token_{1};
  std::unordered_map<uint64_t, std::vector<uint32_t>> leftover_;
};

// Combines what 04-public-c-abi.md Sec.17 calls the envelope's
// "authorization + certificate + epoch + quota + timeline": which Node
// classes may run, an access certificate mode, a per-submit task quota and
// the timeline wait/signal values one submit() call authorizes. ADR-044
// disclosed narrowing: certificate_touched is a whole-allocation PointerRef
// set derived from the caller's VgAccessRange array (offset/size/access_mask
// are recorded on the C-side VgAccessRange for a future range-granular
// certificate but not yet enforced at that granularity here); one timeline
// per device, so timeline_wait/timeline_signal are the only ones submit()
// honors.
class ExecutionEnvelope {
 public:
  // Complete device-scoped identity.  An index alone could authorize a
  // recycled generation.
  std::vector<NodeTable::Ref> allowed_nodes;
  std::vector<PointerRef> certificate_touched;
  bool has_certificate_mode{};
  AccessCertificateMode certificate_mode{AccessCertificateMode::CertifiedPinned};
  bool has_task_quota{};
  uint32_t task_quota{};
  uint64_t timeline_wait{};
  uint64_t timeline_signal{};
};

}  // namespace vg::core

#endif
