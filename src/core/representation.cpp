#include "core/representation.h"

#include "core/arena.h"
#include "core/facet.h"

#include <algorithm>

namespace vg::core {

const char* ConsumeProof::first_unmet() const {
  if (!envelope_complete) return "old envelope has not completed";
  if (!no_external_references) return "an external reference to the old representation still exists";
  if (!no_replay_required) return "the old representation may still be replayed";
  if (!failure_semantics_accepted) return "destructive-failure semantics were not accepted";
  return nullptr;
}

bool RepresentationEpoch::contains(RepresentationRef reference) const {
  return std::ranges::any_of(representations_, [&](RepresentationRef candidate) {
    return candidate.allocation == reference.allocation &&
           candidate.allocation_generation == reference.allocation_generation &&
           candidate.representation_epoch == reference.representation_epoch;
  });
}

bool RepresentationEpoch::contains(FacetRef ref) const {
  return std::ranges::any_of(facets_, [&](FacetRef candidate) {
    return candidate.index == ref.index && candidate.generation == ref.generation;
  });
}

bool RepresentationEpoch::stale(const Arena& arena) const {
  return std::ranges::any_of(representations_, [&](RepresentationRef reference) {
    return arena.lookup(reference) == nullptr;
  });
}

bool RepresentationEpochBuilder::add_representation(RepresentationRef reference, std::string* error) {
  if (sealed_) { if (error) *error = "representation epoch is sealed"; return false; }
  if (reference.allocation_generation == 0) {
    if (error) *error = "representation reference generation must be non-zero";
    return false;
  }
  if (std::ranges::any_of(representations_, [&](RepresentationRef candidate) {
        return candidate.allocation == reference.allocation &&
               candidate.allocation_generation == reference.allocation_generation &&
               candidate.representation_epoch == reference.representation_epoch;
      })) return true;
  representations_.push_back(reference);
  return true;
}

bool RepresentationEpochBuilder::add_representation(const Arena& arena, uint64_t allocation,
                                                    uint32_t generation, std::string* error) {
  const auto* record = arena.lookup(PointerRef{allocation, generation});
  if (record == nullptr) {
    if (error) *error = "representation reference is not active in arena";
    return false;
  }
  if (arena_ == nullptr) arena_ = &arena;
  return add_representation({allocation, generation, record->representation_epoch}, error);
}

bool RepresentationEpochBuilder::add_facet(const Arena& arena, const FacetPool& pool, FacetRef ref,
                                           std::string* error) {
  FacetStatus status = FacetStatus::Ok;
  const FacetSlot* slot = pool.lookup(arena, ref, &status);
  if (slot == nullptr) { if (error) *error = to_string(status); return false; }
  if (!add_representation(arena, slot->view.allocation, slot->view.allocation_generation, error)) return false;
  if (!std::ranges::any_of(facets_, [&](FacetRef candidate) {
        return candidate.index == ref.index && candidate.generation == ref.generation;
      })) facets_.push_back(ref);
  return true;
}

bool RepresentationEpochBuilder::seal(RepresentationEpoch* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "representation epoch output is required"; return false; }
  if (sealed_) { if (error) *error = "representation epoch builder is already sealed"; return false; }
  out->value_ = arena_ != nullptr ? arena_->representation_clock() : next_epoch_;
  out->representations_ = representations_;
  out->facets_ = facets_;
  out->sealed_ = true;
  sealed_ = true;
  return true;
}

}  // namespace vg::core
