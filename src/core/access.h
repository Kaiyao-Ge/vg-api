#ifndef VG_CORE_ACCESS_H_
#define VG_CORE_ACCESS_H_

#include "core/pointer_graph.h"
#include "ir/ir.h"

#include <functional>
#include <string>
#include <vector>

namespace vg::core {

struct Certificate {
  std::vector<ir::Effect> ranges;
  [[nodiscard]] bool covers(const ir::Effect& effect) const;
};

struct WitnessEntry {
  ir::Effect effect;
  uint32_t instruction_index{};
};

struct WitnessDiff {
  std::vector<ir::Effect> missing;
  std::vector<ir::Effect> unused;
};

class AccessWitness {
 public:
  void record(ir::Effect effect, uint32_t instruction_index);
  [[nodiscard]] const std::vector<WitnessEntry>& entries() const { return entries_; }
  [[nodiscard]] WitnessDiff diff(const Certificate& certificate) const;

 private:
  std::vector<WitnessEntry> entries_;
};

// The five E004 adapter memory-visibility policies (09-experiment-catalog.md).
// CertifiedPinned/Universe/DiscoverThenLease have real implementations on
// reference and Metal (see build_access_certificate); SoftwarePaged and
// FaultManaged are pre-sanctioned Unsupported results (docs/START.md §4) —
// build_access_certificate refuses to fabricate a certificate for either.
enum class AccessCertificateMode { CertifiedPinned, Universe, DiscoverThenLease, SoftwarePaged, FaultManaged };

struct AccessCertificate {
  AccessCertificateMode mode{AccessCertificateMode::CertifiedPinned};
  GraphEpoch epoch;
  uint64_t discovery_host_ns{};
  uint64_t discovery_gpu_ns{};
  uint64_t scanned_bytes{};
  uint64_t result_bytes{};
  uint64_t working_set_bytes{};
};

// Builds an AccessCertificate for `mode` over `arena`. `touched` is the set
// of allocations the compiled module statically references, used as the
// CertifiedPinned working set; Universe and DiscoverThenLease instead scan
// every Active allocation in `arena` (DiscoverThenLease additionally times
// the scan as a real host-side rescan and reports it via discovery_host_ns).
// Returns false for SoftwarePaged/FaultManaged — callers must classify those
// modes Unsupported themselves rather than treat a false return as a bug.
bool build_access_certificate(const Arena& arena, AccessCertificateMode mode,
                              const std::vector<PointerRef>& touched,
                              AccessCertificate* out, std::string* error = nullptr);

// TASK-D2 / ADR-036: seed-topology discovery (02 §7.2). Distinct from
// build_access_certificate's B-era DiscoverThenLease, which still scans
// every Active allocation. This walk reads 12-byte PointerRef slots
// packed the same way load_ref does ({u64 allocation, u32 generation},
// not sizeof(PointerRef) which may pad) and follows only refs that are
// well-formed and resolve to Active allocations. Result = seeds +
// reachable, which can be strictly smaller than Universe on the same
// Arena. This is a semantic reachable set / proxy, not an OS page
// migration (06 §10).
//
// topology_epoch is frozen at the start of the walk; a change mid-walk
// refuses rather than certifying a mixed-epoch set.
struct DiscoveryResult {
  std::vector<PointerRef> reachable;
  uint64_t frozen_topology_epoch{};
  uint64_t scanned_bytes{};
  uint64_t result_bytes{};
  uint64_t discovery_host_ns{};
};

// `after_visit` is invoked after each newly reached allocation is
// recorded and before the next hop. Production callers leave it empty.
// Tests use it to bump topology_epoch mid-walk so the freeze check in
// 02 §7.2 is observable.
bool discover_reachable(const Arena& arena, const std::vector<PointerRef>& seeds,
                        DiscoveryResult* out, std::string* error = nullptr,
                        const std::function<void()>& after_visit = {});

// Seals a DiscoverThenLease AccessCertificate over `discovery.reachable`
// only -- not the B-era full-arena scan. Callers that still want that
// scan must keep using build_access_certificate.
bool build_discovered_certificate(const Arena& arena, const DiscoveryResult& discovery,
                                  AccessCertificate* out, std::string* error = nullptr);

// 02 §10: certificate (proof) covers witness (observation). An extra
// forged allocation in `witness` is a refuse, not a silent enlarge.
bool certificate_covers_discovery_witness(const AccessCertificate& certificate,
                                          const std::vector<PointerRef>& witness,
                                          std::string* error = nullptr);

// Conservative composition of effect certificates (12 §8 open question 3 /
// Task children and indirect calls). Union of ranges is a sound
// over-approximation: the result covers every input range. It does not try
// to stay tight; overlapping ranges are kept rather than merged into a
// smaller cover. Empty `parts` is refused -- there is no implicit Universe.
bool compose_certificates(const std::vector<Certificate>& parts, Certificate* out,
                          std::string* error = nullptr);

// Conservative composition of AccessCertificates: union of GraphEpoch
// references on one Arena. Mixed topology epochs are refused. If the union
// names every Active allocation while some input did not, `exploded` is
// true -- that is the honest "composition can become Universe" outcome,
// not a silent success. `exploded` may be null.
bool compose_access_certificates(const Arena& arena, const std::vector<AccessCertificate>& parts,
                                 AccessCertificate* out, bool* exploded = nullptr,
                                 std::string* error = nullptr);

// Phase D shared contracts (ADR-035). Independent of AccessCertificate
// (sound over-approximation) and of Allocation eviction: this is the
// residency hold and overflow bookkeeping D2/D3/D5 fill in. Default
// construction is "not applied" so existing callers stay unchanged.

// Unset (has_limit == false) is distinct from a set limit of 0. A set
// limit that requested bytes exceed is a predictable refusal, not a clamp.
struct WorkingSetBudget {
  bool has_limit{};
  uint64_t byte_limit{};

  static WorkingSetBudget unlimited() { return {}; }
  static WorkingSetBudget limited(uint64_t bytes) {
    WorkingSetBudget budget;
    budget.has_limit = true;
    budget.byte_limit = bytes;
    return budget;
  }
  bool allows(uint64_t bytes, std::string* error = nullptr) const;
};

// This submission's residency hold. A lease cannot name an allocation
// absent from the caller-supplied proven set (certificate or discovery
// witness). `complete` is the caller's claim that the named set is the
// whole hold -- it is not inferred from Arena state.
struct WorkingSetLease {
  std::vector<PointerRef> allocations;
  uint64_t byte_limit{};
  bool complete{};

  [[nodiscard]] bool covers(PointerRef ref) const;
  bool add(PointerRef ref, const std::vector<PointerRef>& proven, std::string* error = nullptr);
  bool valid(const std::vector<PointerRef>& proven, std::string* error = nullptr) const;
};

bool validate_certificate(const Certificate& certificate,
                          const std::vector<ir::Effect>& effects,
                          std::string* error = nullptr);

}  // namespace vg::core

#endif
