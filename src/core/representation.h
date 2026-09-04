#ifndef VG_CORE_REPRESENTATION_H_
#define VG_CORE_REPRESENTATION_H_

#include "core/resource_types.h"

#include <string>
#include <vector>

namespace vg::core {

class Arena;
class FacetPool;

// The four obligations 02-principles-and-semantics.md Sec.4.2 requires a
// caller to discharge before ConsumeInput may run: the old envelope has
// completed, nothing outside holds a reference, the old version will never be
// replayed, and the caller accepts that a fault leaves no rollback. Passed as
// an argument to Arena::consume rather than inferred, because none of the four
// is observable from arena state alone -- consume() refuses unless all four
// hold, which is 10-validation-and-benchmarks.md Sec.3's "ConsumeInput proof"
// conformance row and Sec.4's "illegal consume" negative case.
struct ConsumeProof {
  bool envelope_complete{};
  bool no_external_references{};
  bool no_replay_required{};
  bool failure_semantics_accepted{};

  [[nodiscard]] bool complete() const {
    return envelope_complete && no_external_references && no_replay_required &&
           failure_semantics_accepted;
  }
  // Names the first unmet obligation so a rejection says which proof failed
  // rather than only that one did; nullptr when complete().
  [[nodiscard]] const char* first_unmet() const;
};

// One frozen interpretation of a set of allocations' backing/metadata/facets
// (02 Sec.4.1). Sibling to GraphEpoch, not a generalization of it: GraphEpoch
// freezes which pointers a topology may dereference, this freezes how the bytes
// behind those allocations are to be read. A representation transform produces
// a new one rather than editing this one, which is 02 Sec.8's "transform 不是
// 纯 barrier" stated as a data structure.
class RepresentationEpoch {
 public:
  [[nodiscard]] uint64_t value() const { return value_; }
  [[nodiscard]] bool sealed() const { return sealed_; }
  [[nodiscard]] const std::vector<RepresentationRef>& representations() const { return representations_; }
  [[nodiscard]] const std::vector<FacetRef>& facets() const { return facets_; }
  [[nodiscard]] bool contains(RepresentationRef reference) const;
  [[nodiscard]] bool contains(FacetRef ref) const;
  // 02 Sec.10's "facet generation vs epoch = stale token" at epoch granularity:
  // true once any frozen representation no longer matches `arena`, meaning
  // every facet this epoch authorized has to be rebuilt rather than reused.
  [[nodiscard]] bool stale(const Arena& arena) const;

 private:
  friend class RepresentationEpochBuilder;
  uint64_t value_{1};
  std::vector<RepresentationRef> representations_;
  std::vector<FacetRef> facets_;
  bool sealed_{};
};

// Mirrors GraphEpochBuilder's shape (add references, seal once, stamp the
// arena's clock) so the two epoch kinds stay recognizably the same mechanism
// applied to different state.
class RepresentationEpochBuilder {
 public:
  explicit RepresentationEpochBuilder(uint64_t next_epoch = 1) : next_epoch_(next_epoch) {}
  RepresentationEpochBuilder(const Arena* arena, uint64_t next_epoch = 1)
      : next_epoch_(next_epoch), arena_(arena) {}
  bool add_representation(RepresentationRef reference, std::string* error = nullptr);
  // Snapshots the allocation's *current* representation_epoch, so a caller
  // cannot freeze a version the arena is not actually at.
  bool add_representation(const Arena& arena, uint64_t allocation, uint32_t generation,
                          std::string* error = nullptr);
  // Adds the facet and the representation it was acquired against together --
  // a facet whose slot is already stale is refused, since freezing it would
  // authorize a token that is dead on arrival.
  bool add_facet(const Arena& arena, const FacetPool& pool, FacetRef ref, std::string* error = nullptr);
  bool seal(RepresentationEpoch* out, std::string* error = nullptr);
  [[nodiscard]] bool sealed() const { return sealed_; }

 private:
  uint64_t next_epoch_;
  const Arena* arena_{};
  std::vector<RepresentationRef> representations_;
  std::vector<FacetRef> facets_;
  bool sealed_{};
};

}  // namespace vg::core

#endif
