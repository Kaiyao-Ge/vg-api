#ifndef VG_CORE_ARENA_H_
#define VG_CORE_ARENA_H_

#include "core/resource_types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace vg::core {

struct ConsumeProof;

struct Allocation {
  uint64_t id{};
  uint32_t generation{1};
  uint64_t size{};
  ObjectState state{ObjectState::Active};
  uint32_t representation_epoch{0};
  // Byte-content revision. Unlike representation_epoch this does not stale
  // facets: it tells backend mirrors when canonical allocation bytes changed.
  uint64_t content_epoch{1};
  uint32_t in_flight{};
  // How many representation versions of this allocation are still live. Starts
  // at 1 (the allocation's initial representation); transform() adds one and
  // release_representation() removes one. This is what a non-zero
  // Arena::max_in_flight_representations() budget is checked against (E016:
  // "禁止无界创建版本").
  uint32_t live_representations{1};
  std::vector<uint8_t> bytes;
};

class Arena {
 public:
  explicit Arena(uint64_t id = 1) : id_(id) {}
  Allocation& allocate(uint64_t size);
  bool retire(const PointerRef& ref);
  Allocation* lookup(const PointerRef& ref);
  [[nodiscard]] const Allocation* lookup(const PointerRef& ref) const;
  Allocation* lookup(const RepresentationRef& ref);
  [[nodiscard]] const Allocation* lookup(const RepresentationRef& ref) const;
  bool acquire(uint64_t id, uint32_t generation);
  bool release(uint64_t id, uint32_t generation);
  bool transform(uint64_t id, uint32_t generation, uint32_t* new_epoch, std::string* error = nullptr);
  bool transform(uint64_t id, uint32_t generation, uint32_t expected_epoch, uint32_t* new_epoch, std::string* error = nullptr);
  // ConsumeInput (02 Sec.4.2). Destructive: the allocation is retired, its
  // generation bumped so no old token can ever resolve again, and its backing
  // bytes actually released -- there is no rollback to the pre-consume
  // representation afterwards, which is exactly what `proof` attests the
  // caller accepts. Refuses an incomplete proof before touching any state.
  bool consume(uint64_t id, uint32_t generation, uint32_t expected_epoch, const ConsumeProof& proof,
               std::string* error = nullptr);
  // ConsumeInput applied to a representation transform (06 Sec.11: the proof
  // buys "reuse heap range、in-place compute transform 或立即释放旧 backing").
  // The object survives -- its identity, generation and freshly published
  // epoch stay live, so facets acquired against the new representation keep
  // resolving -- but the superseded backing is released at once instead of
  // being retained until the relevant command buffer completes. That released
  // byte count is the watermark reduction E005 measures, and it is the whole
  // difference from the default retain-until-completion behaviour.
  //
  // Distinct from consume() above, which retires the object entirely: these
  // are two different destructive operations and collapsing them would make a
  // transform's target facet stale the moment its own transform succeeded.
  bool consume_representation(uint64_t id, uint32_t generation, uint32_t expected_epoch,
                              const ConsumeProof& proof, uint64_t* released_bytes = nullptr,
                              std::string* error = nullptr);
  // E016 backpressure. 0 (the default) means unbounded; a non-zero budget makes
  // transform() refuse -- predictably, with an explicit error -- once an
  // allocation already has that many live representations, instead of letting
  // versions accumulate until the process thrashes.
  void set_max_in_flight_representations(uint32_t budget) { max_in_flight_representations_ = budget; }
  [[nodiscard]] uint32_t max_in_flight_representations() const { return max_in_flight_representations_; }
  // Drops one live representation version of `id` without retiring the
  // allocation, i.e. the producer observed that an older version's readers are
  // done. Never drops the last one: an Active allocation always has at least
  // its current representation.
  bool release_representation(uint64_t id, uint32_t generation, std::string* error = nullptr);
  [[nodiscard]] const std::unordered_map<uint64_t, Allocation>& allocations() const { return allocations_; }
  // Pointer-identity liveness check for a raw Allocation* obtained earlier
  // (e.g. via arena_allocate's reinterpret_cast out-param) and now of unknown
  // provenance to the caller. allocate()/retire()/consume() etc. never erase
  // an allocations_ entry -- only destroying the whole Arena invalidates a
  // pointer into this map -- so this exists to let a caller that still has
  // this Arena alive confirm `ptr` actually points at one of its own live
  // map entries *before* dereferencing it, rather than trusting an untrusted
  // pointer's own bytes. Never dereferences `ptr` itself; only compares its
  // address against `&kv.second` for entries this Arena owns.
  [[nodiscard]] bool is_live_allocation(const Allocation* ptr) const {
    for (const auto& kv : allocations_) {
      if (&kv.second == ptr) return true;
    }
    return false;
  }
  bool copy_into(Allocation* allocation, uint64_t offset, const void* source, uint64_t size,
                 std::string* error = nullptr);
  bool copy_out(const Allocation* allocation, uint64_t offset, void* destination, uint64_t size,
                std::string* error = nullptr) const;
  void mark_content_modified(Allocation& allocation) { ++allocation.content_epoch; }
  bool import_allocation(const RepresentationRef& ref, uint64_t size, ObjectState state,
                         const std::vector<uint8_t>& bytes, std::string* error = nullptr);
  [[nodiscard]] uint64_t id() const { return id_; }
  [[nodiscard]] uint64_t topology_epoch() const { return topology_epoch_; }
  // Arena-wide monotone counter of representation transitions, sibling to
  // topology_epoch(): that one ticks when the pointer-bearing topology changes,
  // this one ticks when any allocation's backing/facet interpretation does
  // (02 Sec.4.1). RepresentationEpochBuilder::seal() stamps it, the same way
  // GraphEpochBuilder::seal() stamps topology_epoch().
  [[nodiscard]] uint64_t representation_clock() const { return representation_clock_; }

 private:
  uint64_t id_;
  uint64_t next_id_{1};
  uint64_t topology_epoch_{};
  uint64_t representation_clock_{};
  uint32_t max_in_flight_representations_{};
  std::unordered_map<uint64_t, Allocation> allocations_;
};

}  // namespace vg::core

#endif
