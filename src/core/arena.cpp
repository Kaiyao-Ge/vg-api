#include "core/arena.h"

#include "core/representation.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace vg::core {

bool Arena::copy_into(Allocation* allocation, uint64_t offset, const void* source, uint64_t size,
                      std::string* error) {
  if (!is_live_allocation(allocation)) { if (error) *error = "allocation is not owned by arena"; return false; }
  if (size != 0 && source == nullptr) { if (error) *error = "source is required for non-zero write"; return false; }
  const uint64_t byte_count = allocation->bytes.size();
  if (offset > byte_count || size > byte_count - offset) { if (error) *error = "allocation write range is out of bounds"; return false; }
  if (size != 0) std::memcpy(allocation->bytes.data() + offset, source, static_cast<size_t>(size));
  if (size != 0) mark_content_modified(*allocation);
  return true;
}

bool Arena::copy_out(const Allocation* allocation, uint64_t offset, void* destination, uint64_t size,
                     std::string* error) const {
  if (!is_live_allocation(allocation)) { if (error) *error = "allocation is not owned by arena"; return false; }
  if (size != 0 && destination == nullptr) { if (error) *error = "destination is required for non-zero read"; return false; }
  const uint64_t byte_count = allocation->bytes.size();
  if (offset > byte_count || size > byte_count - offset) { if (error) *error = "allocation read range is out of bounds"; return false; }
  if (size != 0) std::memcpy(destination, allocation->bytes.data() + offset, static_cast<size_t>(size));
  return true;
}

Allocation& Arena::allocate(uint64_t size) {
  Allocation allocation;
  allocation.id = next_id_++;
  allocation.size = size;
  allocation.bytes.resize(static_cast<size_t>(size));
  ++topology_epoch_;
  auto [it, inserted] = allocations_.emplace(allocation.id, std::move(allocation));
  (void)inserted;
  return it->second;
}

bool Arena::import_allocation(const RepresentationRef& ref, uint64_t size, ObjectState state,
                              const std::vector<uint8_t>& bytes, std::string* error) {
  if (ref.allocation == 0 || ref.allocation_generation == 0 || size == 0 || bytes.size() != size ||
      allocations_.contains(ref.allocation)) {
    if (error) *error = "invalid or duplicate capture allocation";
    return false;
  }
  Allocation allocation;
  allocation.id = ref.allocation;
  allocation.generation = ref.allocation_generation;
  allocation.size = size;
  allocation.representation_epoch = ref.representation_epoch;
  allocation.state = state;
  allocation.bytes = bytes;
  allocations_.emplace(ref.allocation, std::move(allocation));
  next_id_ = std::max(next_id_, ref.allocation + 1);
  ++topology_epoch_;
  return true;
}

bool Arena::retire(const PointerRef& ref) {
  auto it = allocations_.find(ref.allocation);
  if (it == allocations_.end() || it->second.generation != ref.generation ||
      it->second.state != ObjectState::Active || it->second.in_flight != 0)
    return false;
  it->second.state = ObjectState::Retired;
  ++it->second.generation;
  ++topology_epoch_;
  return true;
}

Allocation* Arena::lookup(const PointerRef& ref) {
  auto it = allocations_.find(ref.allocation);
  if (it == allocations_.end() || it->second.state != ObjectState::Active ||
      it->second.generation != ref.generation)
    return nullptr;
  return &it->second;
}

const Allocation* Arena::lookup(const PointerRef& ref) const {
  auto it = allocations_.find(ref.allocation);
  if (it == allocations_.end() || it->second.state != ObjectState::Active ||
      it->second.generation != ref.generation)
    return nullptr;
  return &it->second;
}

Allocation* Arena::lookup(const RepresentationRef& ref) {
  auto* allocation = lookup(PointerRef{ref.allocation, ref.allocation_generation});
  return allocation != nullptr && allocation->representation_epoch == ref.representation_epoch ? allocation
                                                                                              : nullptr;
}

const Allocation* Arena::lookup(const RepresentationRef& ref) const {
  const auto* allocation = lookup(PointerRef{ref.allocation, ref.allocation_generation});
  return allocation != nullptr && allocation->representation_epoch == ref.representation_epoch ? allocation
                                                                                              : nullptr;
}

bool Arena::acquire(uint64_t id, uint32_t generation) {
  auto* allocation = lookup(PointerRef{id, generation});
  if (allocation == nullptr) return false;
  ++allocation->in_flight;
  return true;
}

bool Arena::release(uint64_t id, uint32_t generation) {
  auto* allocation = lookup(PointerRef{id, generation});
  if (allocation == nullptr || allocation->in_flight == 0) return false;
  --allocation->in_flight;
  return true;
}

bool Arena::transform(uint64_t id, uint32_t generation, uint32_t* new_epoch, std::string* error) {
  auto* allocation = lookup(PointerRef{id, generation});
  if (allocation == nullptr) { if (error) *error = "stale allocation for representation transform"; return false; }
  if (allocation->in_flight != 0) { if (error) *error = "representation epoch is referenced in flight"; return false; }
  if (max_in_flight_representations_ != 0 &&
      allocation->live_representations >= max_in_flight_representations_) {
    if (error) *error = "in-flight representation budget exceeded";
    return false;
  }
  ++allocation->representation_epoch;
  ++allocation->live_representations;
  ++representation_clock_;
  if (new_epoch != nullptr) *new_epoch = allocation->representation_epoch;
  return true;
}

bool Arena::transform(uint64_t id, uint32_t generation, uint32_t expected_epoch, uint32_t* new_epoch, std::string* error) {
  if (lookup(RepresentationRef{id, generation, expected_epoch}) == nullptr) { if (error) *error = "representation epoch is stale"; return false; }
  return transform(id, generation, new_epoch, error);
}

bool Arena::release_representation(uint64_t id, uint32_t generation, std::string* error) {
  auto* allocation = lookup(PointerRef{id, generation});
  if (allocation == nullptr) { if (error) *error = "stale allocation for representation release"; return false; }
  if (allocation->live_representations <= 1) {
    if (error) *error = "an active allocation always retains its current representation";
    return false;
  }
  --allocation->live_representations;
  ++representation_clock_;
  return true;
}

bool Arena::consume_representation(uint64_t id, uint32_t generation, uint32_t expected_epoch,
                                   const ConsumeProof& proof, uint64_t* released_bytes,
                                   std::string* error) {
  if (const char* unmet = proof.first_unmet()) {
    if (error) *error = std::string("ConsumeInput proof incomplete: ") + unmet;
    return false;
  }
  auto* allocation = lookup(RepresentationRef{id, generation, expected_epoch});
  if (allocation == nullptr) {
    if (error) *error = "stale allocation or representation epoch for consume";
    return false;
  }
  if (allocation->in_flight != 0) { if (error) *error = "consume requires exclusive ownership"; return false; }
  const uint64_t released = allocation->bytes.size();
  allocation->bytes.clear();
  allocation->bytes.shrink_to_fit();
  allocation->live_representations = 1;
  ++representation_clock_;
  if (released_bytes != nullptr) *released_bytes = released;
  return true;
}

bool Arena::consume(uint64_t id, uint32_t generation, uint32_t expected_epoch, const ConsumeProof& proof,
                    std::string* error) {
  if (const char* unmet = proof.first_unmet()) {
    if (error) *error = std::string("ConsumeInput proof incomplete: ") + unmet;
    return false;
  }
  auto* allocation = lookup(RepresentationRef{id, generation, expected_epoch});
  if (allocation == nullptr) { if (error) *error = "stale allocation or representation epoch for consume"; return false; }
  if (allocation->in_flight != 0) { if (error) *error = "consume requires exclusive ownership"; return false; }
  allocation->state = ObjectState::Retired;
  ++allocation->generation;
  allocation->live_representations = 0;
  // The point of ConsumeInput is that the old backing stops costing memory, so
  // the bytes are actually returned rather than left behind under a retired
  // handle -- that is the watermark E005 measures.
  allocation->bytes.clear();
  allocation->bytes.shrink_to_fit();
  ++topology_epoch_;
  ++representation_clock_;
  return true;
}

}  // namespace vg::core
