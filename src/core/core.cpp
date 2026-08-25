#include "core/core.h"

#include "ir/json.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <utility>

namespace vg::core {

bool GraphEpoch::contains(PointerRef reference) const {
  return std::ranges::any_of(references_, [&](PointerRef candidate) {
    return candidate.allocation == reference.allocation && candidate.generation == reference.generation;
  });
}

bool GraphEpochBuilder::add_reference(PointerRef reference, std::string* error) {
  if (sealed_) { if (error) *error = "graph epoch is sealed"; return false; }
  if (reference.generation == 0) { if (error) *error = "graph reference generation must be non-zero"; return false; }
  if (std::ranges::any_of(references_, [&](PointerRef candidate) {
        return candidate.allocation == reference.allocation && candidate.generation == reference.generation;
      })) return true;
  references_.push_back(reference);
  return true;
}

bool GraphEpochBuilder::add_reference(const Arena& arena, PointerRef reference, std::string* error) {
  if (arena.lookup(reference) == nullptr) {
    if (error) *error = "graph reference is not active in arena";
    return false;
  }
  if (arena_ == nullptr) arena_ = &arena;
  return add_reference(reference, error);
}

bool GraphEpochBuilder::seal(GraphEpoch* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "graph epoch output is required"; return false; }
  if (sealed_) { if (error) *error = "graph epoch builder is already sealed"; return false; }
  out->value_ = arena_ != nullptr ? arena_->topology_epoch() : next_epoch_;
  out->references_ = references_;
  out->sealed_ = true;
  sealed_ = true;
  return true;
}

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

namespace {
bool pointer_ref_equal(PointerRef a, PointerRef b) { return a.allocation == b.allocation && a.generation == b.generation; }
}

bool PointerGraph::reachable(PointerRef from, uint64_t field_offset, PointerRef to) const {
  return std::ranges::any_of(edges_, [&](const Edge& edge) {
    return pointer_ref_equal(edge.from, from) && edge.field_offset == field_offset && pointer_ref_equal(edge.to, to);
  });
}

bool PointerGraph::reachable(ReachQuery query) const {
  std::vector<PointerRef> worklist{query.from};
  std::vector<PointerRef> seen{query.from};
  while (!worklist.empty()) {
    PointerRef current = worklist.back();
    worklist.pop_back();
    for (const auto& edge : edges_) {
      if (!pointer_ref_equal(edge.from, current)) continue;
      if (pointer_ref_equal(edge.to, query.to)) return true;
      if (std::ranges::any_of(seen, [&](PointerRef candidate) { return pointer_ref_equal(candidate, edge.to); })) continue;
      seen.push_back(edge.to);
      worklist.push_back(edge.to);
    }
  }
  return false;
}

bool PointerGraphBuilder::add_edge(PointerRef from, uint64_t field_offset, PointerRef to, std::string* error) {
  if (built_) { if (error) *error = "pointer graph builder is already built"; return false; }
  if (from.generation == 0 || to.generation == 0) { if (error) *error = "pointer edge generation must be non-zero"; return false; }
  edges_.push_back({from, field_offset, to});
  return true;
}

bool PointerGraphBuilder::build(PointerGraph* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "pointer graph output is required"; return false; }
  if (built_) { if (error) *error = "pointer graph builder is already built"; return false; }
  out->edges_ = edges_;
  built_ = true;
  return true;
}

PublicationRing::PublicationRing(uint32_t capacity) : slots_(capacity) {}

int32_t PublicationRing::reserve() {
  if (slots_.empty()) return -1;
  const uint32_t start = next_slot_.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; i < slots_.size(); ++i) {
    const uint32_t index = (start + i) % static_cast<uint32_t>(slots_.size());
    auto expected = PublicationState::Empty;
    if (slots_[index].state.compare_exchange_strong(expected, PublicationState::Writing,
                                                     std::memory_order_acquire,
                                                     std::memory_order_relaxed)) return static_cast<int32_t>(index);
  }
  return -1;
}

bool PublicationRing::write(uint32_t slot, const TaskRecord& task, std::string* error) {
  if (slot >= slots_.size()) { if (error) *error = "publication slot is out of range"; return false; }
  if (slots_[slot].state.load(std::memory_order_relaxed) != PublicationState::Writing) { if (error) *error = "publication slot is not writable"; return false; }
  if (task.node_generation == 0 || task.root_generation == 0) { if (error) *error = "task generation must be non-zero"; return false; }
  slots_[slot].task = task;
  return true;
}

bool PublicationRing::publish(uint32_t slot, std::string* error) {
  if (slot >= slots_.size()) { if (error) *error = "publication slot is out of range"; return false; }
  auto expected = PublicationState::Writing;
  if (!slots_[slot].state.compare_exchange_strong(expected, PublicationState::Published,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)) { if (error) *error = "publication slot is not in writing state"; return false; }
  return true;
}

bool PublicationRing::acquire(uint32_t slot, TaskRecord* out, std::string* error) const {
  if (slot >= slots_.size() || out == nullptr) { if (error) *error = "publication acquire arguments are invalid"; return false; }
  if (slots_[slot].state.load(std::memory_order_acquire) != PublicationState::Published) { if (error) *error = "publication slot is not published"; return false; }
  *out = slots_[slot].task;
  return true;
}

bool PublicationRing::consume(uint32_t slot, std::string* error) {
  if (slot >= slots_.size()) { if (error) *error = "publication slot is out of range"; return false; }
  auto expected = PublicationState::Published;
  if (!slots_[slot].state.compare_exchange_strong(expected, PublicationState::Consumed,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed)) { if (error) *error = "publication slot is not consumable"; return false; }
  return true;
}

bool PublicationRing::abort(uint32_t slot, std::string* error) {
  if (slot >= slots_.size()) { if (error) *error = "publication slot is out of range"; return false; }
  auto expected = PublicationState::Writing;
  if (!slots_[slot].state.compare_exchange_strong(expected, PublicationState::Empty,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
    if (error) *error = "publication slot is not abortable";
    return false;
  }
  return true;
}

bool PublicationRing::publish_task(const TaskRecord& task, uint32_t* slot, std::string* error) {
  const int32_t reserved = reserve();
  if (reserved < 0) { if (error) *error = "publication ring quota overflow"; return false; }
  const auto index = static_cast<uint32_t>(reserved);
  if (!write(index, task, error)) { abort(index); return false; }
  if (!publish(index, error)) { abort(index); return false; }
  if (slot != nullptr) *slot = index;
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

uint32_t bytes_per_texel(PixelFormat format) {
  switch (format) {
    case PixelFormat::RGBA8Unorm: return 4;
    case PixelFormat::R32Float: return 4;
  }
  return 0;
}

uint32_t CanonicalView::mip_width(uint32_t level) const {
  return std::max<uint32_t>(1, width >> level);
}

uint32_t CanonicalView::mip_height(uint32_t level) const {
  return std::max<uint32_t>(1, height >> level);
}

uint64_t CanonicalView::bytes_per_row(uint32_t level) const {
  return static_cast<uint64_t>(mip_width(level)) * bytes_per_texel(format);
}

uint64_t CanonicalView::subresource_byte_size(uint32_t level) const {
  return bytes_per_row(level) * mip_height(level);
}

uint64_t CanonicalView::subresource_byte_offset(SubresourceIndex index) const {
  uint64_t layer_bytes = 0;
  for (uint32_t candidate = 0; candidate < mip_levels; ++candidate) {
    layer_bytes += subresource_byte_size(candidate);
  }
  uint64_t offset = layer_bytes * index.array_layer;
  for (uint32_t candidate = 0; candidate < index.mip_level; ++candidate) {
    offset += subresource_byte_size(candidate);
  }
  return offset;
}

uint64_t CanonicalView::byte_size() const {
  return subresource_byte_offset({array_layers, 0});
}

bool CanonicalView::valid(std::string* error) const {
  const auto fail = [error](const char* reason) {
    if (error) *error = reason;
    return false;
  };
  if (width == 0 || height == 0) return fail("canonical view extent must be non-zero");
  if (array_layers == 0) return fail("canonical view must name at least one array layer");
  if (mip_levels == 0) return fail("canonical view must name at least one mip level");
  if (dimension == ViewDimension::Texture2D && array_layers != 1) {
    return fail("Texture2D canonical view cannot name multiple array layers");
  }
  // A level whose predecessor already reached 1x1 would alias it rather than
  // describe new texels, so a chain longer than the extent supports is a
  // malformed contract, not something to clamp.
  uint32_t deepest = 1;
  while (mip_width(deepest - 1) > 1 || mip_height(deepest - 1) > 1) ++deepest;
  if (mip_levels > deepest) return fail("canonical view mip chain is longer than its extent supports");
  return true;
}

const char* to_string(FacetStatus status) {
  switch (status) {
    case FacetStatus::Ok: return "ok";
    case FacetStatus::UnknownIndex: return "facet index out of range";
    case FacetStatus::Retired: return "facet slot retired";
    case FacetStatus::GenerationMismatch: return "facet generation mismatch";
    case FacetStatus::EpochStale: return "facet representation epoch stale";
    case FacetStatus::AllocationLost: return "facet backing allocation is no longer active";
  }
  return "unknown facet status";
}

bool FacetPool::acquire(const Arena& arena, const CanonicalView& view, FacetKind kind, FacetRef* out,
                        std::string* error) {
  if (out == nullptr) { if (error) *error = "facet ref output is required"; return false; }
  if (!view.valid(error)) return false;
  const auto* allocation = arena.lookup(PointerRef{view.allocation, view.allocation_generation});
  if (allocation == nullptr) { if (error) *error = "canonical view allocation is not active in arena"; return false; }
  if (allocation->size < view.byte_size()) {
    if (error) *error = "canonical view describes more texels than its allocation backs";
    return false;
  }
  FacetSlot slot;
  slot.active = true;
  slot.kind = kind;
  slot.view = view;
  slot.representation_epoch = allocation->representation_epoch;
  if (!free_list_.empty()) {
    const uint32_t index = free_list_.back();
    free_list_.pop_back();
    slot.generation = slots_[index].generation;
    slots_[index] = slot;
    *out = {index, slot.generation};
    return true;
  }
  slots_.push_back(slot);
  *out = {static_cast<uint32_t>(slots_.size() - 1), slot.generation};
  return true;
}

const FacetSlot* FacetPool::lookup(const Arena& arena, FacetRef ref, FacetStatus* status) const {
  const auto fail = [status](FacetStatus reason) -> const FacetSlot* {
    if (status) *status = reason;
    return nullptr;
  };
  if (ref.index >= slots_.size()) return fail(FacetStatus::UnknownIndex);
  const FacetSlot& slot = slots_[ref.index];
  if (!slot.active) return fail(FacetStatus::Retired);
  if (slot.generation != ref.generation) return fail(FacetStatus::GenerationMismatch);
  const auto* allocation = arena.lookup(PointerRef{slot.view.allocation, slot.view.allocation_generation});
  if (allocation == nullptr) return fail(FacetStatus::AllocationLost);
  if (allocation->representation_epoch != slot.representation_epoch) return fail(FacetStatus::EpochStale);
  if (status) *status = FacetStatus::Ok;
  return &slot;
}

bool FacetPool::retire(FacetRef ref, std::string* error) {
  if (ref.index >= slots_.size() || !slots_[ref.index].active || slots_[ref.index].generation != ref.generation) {
    if (error) *error = "stale or already-retired facet ref";
    return false;
  }
  retire_slot(ref.index);
  return true;
}

size_t FacetPool::retire_stale(const Arena& arena) {
  size_t retired = 0;
  for (uint32_t index = 0; index < slots_.size(); ++index) {
    FacetSlot& slot = slots_[index];
    if (!slot.active) continue;
    const auto* allocation = arena.lookup(PointerRef{slot.view.allocation, slot.view.allocation_generation});
    if (allocation != nullptr && allocation->representation_epoch == slot.representation_epoch) continue;
    retire_slot(index);
    ++retired;
  }
  return retired;
}

bool FacetPool::begin_gpu_use(const Arena& arena, FacetRef ref, std::string* error) {
  FacetStatus status = FacetStatus::Ok;
  if (lookup(arena, ref, &status) == nullptr) {
    if (error) *error = to_string(status);
    return false;
  }
  FacetSlot& slot = slots_[ref.index];
  slot.in_flight_generation = ref.generation;
  ++slot.in_flight;
  return true;
}

bool FacetPool::end_gpu_use(FacetRef ref, std::string* error) {
  if (ref.index >= slots_.size()) {
    if (error) *error = to_string(FacetStatus::UnknownIndex);
    return false;
  }
  FacetSlot& slot = slots_[ref.index];
  // Deliberately matched against in_flight_generation, not `generation`: the
  // slot may already have been retired while this use was outstanding, and
  // that use still has to be released.
  if (slot.in_flight == 0 || slot.in_flight_generation != ref.generation) {
    if (error) *error = "facet ref has no outstanding GPU use";
    return false;
  }
  if (--slot.in_flight == 0 && !slot.active) free_list_.push_back(ref.index);
  return true;
}

uint32_t FacetPool::in_flight(FacetRef ref) const {
  if (ref.index >= slots_.size()) return 0;
  const FacetSlot& slot = slots_[ref.index];
  return slot.in_flight_generation == ref.generation ? slot.in_flight : 0;
}

void FacetPool::snapshot_generations(std::vector<uint32_t>* out) const {
  if (out == nullptr) return;
  out->clear();
  out->reserve(slots_.size());
  for (const FacetSlot& slot : slots_) out->push_back(slot.active ? slot.generation : 0);
}

bool FacetPool::generation_valid(FacetRef ref) const {
  if (ref.index >= slots_.size()) return false;
  const FacetSlot& slot = slots_[ref.index];
  return slot.active && slot.generation == ref.generation;
}

bool FacetPool::references(const RepresentationRef& ref) const {
  return std::ranges::any_of(slots_, [&](const FacetSlot& slot) {
    return slot.view.allocation == ref.allocation &&
           slot.view.allocation_generation == ref.allocation_generation &&
           slot.representation_epoch == ref.representation_epoch &&
           (slot.active || slot.in_flight > 0);
  });
}

void FacetPool::retire_slot(uint32_t index) {
  FacetSlot& slot = slots_[index];
  slot.active = false;
  ++slot.generation;
  // An in-flight slot's index is withheld until end_gpu_use() drops the last
  // use; the token is already dead, but the backend resource behind it is not
  // reassignable yet.
  if (slot.in_flight == 0) free_list_.push_back(index);
}

bool TaskGraphBuilder::append(const TaskRecord& task, std::string* error) {
  if (sealed_) { if (error) *error = "task graph builder is sealed"; return false; }
  if (task.node_generation == 0 || task.root_generation == 0) { if (error) *error = "task generation must be non-zero"; return false; }
  if (tasks_.size() >= max_tasks_) { if (error) *error = "task graph quota overflow"; return false; }
  if (task.payload_size > max_payload_bytes_ - payload_bytes_) { if (error) *error = "task payload quota overflow"; return false; }
  tasks_.push_back(task);
  effects_.emplace_back();
  payload_bytes_ += task.payload_size;
  return true;
}

bool TaskGraphBuilder::add_dependency(uint32_t before, uint32_t after, std::string* error) {
  if (sealed_) { if (error) *error = "task graph builder is sealed"; return false; }
  if (before >= tasks_.size() || after >= tasks_.size() || before == after) { if (error) *error = "invalid task dependency"; return false; }
  dependencies_.push_back({before, after});
  return true;
}

bool TaskGraphBuilder::add_effect(uint32_t task, const ir::Effect& effect, std::string* error) {
  if (sealed_) { if (error) *error = "task graph builder is sealed"; return false; }
  if (task >= effects_.size()) { if (error) *error = "effect task index is out of range"; return false; }
  if (effect.allocation == 0 || effect.size == 0) {
    if (error) *error = "task effect identity and size must be non-zero";
    return false;
  }
  if (effect.offset > UINT64_MAX - effect.size) { if (error) *error = "task effect range overflows"; return false; }
  effects_[task].push_back(effect);
  return true;
}

bool TaskGraphBuilder::set_effects(uint32_t task, const std::vector<ir::Effect>& effects, std::string* error) {
  if (sealed_) { if (error) *error = "task graph builder is sealed"; return false; }
  if (task >= effects_.size()) { if (error) *error = "effect task index is out of range"; return false; }
  effects_[task].clear();
  return std::ranges::all_of(effects, [&](const ir::Effect& effect) { return add_effect(task, effect, error); });
}

bool TaskGraphBuilder::set_quota(uint32_t max_tasks, uint64_t max_payload_bytes, std::string* error) {
  if (sealed_) { if (error) *error = "task graph builder is sealed"; return false; }
  if (max_tasks < tasks_.size() || max_payload_bytes < payload_bytes_) {
    if (error) *error = "task graph quota is below current usage";
    return false;
  }
  max_tasks_ = max_tasks;
  max_payload_bytes_ = max_payload_bytes;
  return true;
}

bool TaskGraphBuilder::append_published(PublicationRing& ring, uint32_t slot, std::string* error) {
  TaskRecord task;
  if (!ring.acquire(slot, &task, error)) return false;
  if (!append(task, error)) return false;
  return ring.consume(slot, error);
}

bool TaskGraphBuilder::seal(TaskGraph* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "sealed graph output is required"; return false; }
  if (sealed_) { if (error) *error = "task graph builder is already sealed"; return false; }
  EffectGraph graph;
  for (const auto& edge : dependencies_) if (!graph.add_edge(edge.first, edge.second, EffectEdgeKind::Explicit, 0, error)) return false;
  for (uint32_t before = 0; before < effects_.size(); ++before) {
    for (uint32_t after = before + 1; after < effects_.size(); ++after) {
      bool hazard = false;
      for (const auto& lhs : effects_[before]) for (const auto& rhs : effects_[after])
        hazard = hazard || EffectGraph::conflicts(lhs, rhs);
      if (hazard && !graph.add_edge(before, after, EffectEdgeKind::InferredConflict, 0, error)) return false;
    }
  }
  if (!graph.valid()) { if (error) *error = "task graph dependency cycle"; return false; }
  if (!graph.validate_happens_before(effects_, error)) return false;
  out->tasks_ = tasks_;
  out->dependencies_.clear();
  for (const auto& edge : graph.edges()) out->dependencies_.push_back({edge.before, edge.after});
  out->effect_graph_ = std::move(graph);
  out->published_ = false;
  sealed_ = true;
  return true;
}

bool TaskGraph::publish(std::string* error) {
  if (!sealed_) { if (error) *error = "task graph must be sealed before publication"; return false; }
  if (published_) { if (error) *error = "task graph is already published"; return false; }
  published_ = true;
  return true;
}

bool TaskGraph::validate_execution(std::string* error) const {
  if (!sealed_) { if (error) *error = "task graph must be sealed before execution"; return false; }
  if (!published_) { if (error) *error = "task graph must be published before execution"; return false; }
  if (!std::ranges::all_of(tasks_, [](const TaskRecord& task) {
        return task.node_generation != 0 && task.root_generation != 0;
      })) {
    if (error) *error = "task generation is stale";
    return false;
  }
  return true;
}

bool TaskGraph::deterministic_order(std::vector<uint32_t>* out, std::string* error) const {
  if (out == nullptr) { if (error) *error = "deterministic order output is required"; return false; }
  const auto count = static_cast<uint32_t>(tasks_.size());
  std::vector<std::vector<uint32_t>> adjacency(count);
  std::vector<uint32_t> in_degree(count, 0);
  for (const auto& dependency : dependencies_) {
    adjacency[dependency.first].push_back(dependency.second);
    ++in_degree[dependency.second];
  }
  std::vector<uint32_t> ready;
  for (uint32_t i = 0; i < count; ++i) if (in_degree[i] == 0) ready.push_back(i);
  out->clear();
  out->reserve(count);
  while (!ready.empty()) {
    std::ranges::sort(ready);
    const uint32_t node = ready.front();
    ready.erase(ready.begin());
    out->push_back(node);
    for (uint32_t next : adjacency[node]) if (--in_degree[next] == 0) ready.push_back(next);
  }
  if (out->size() != count) {
    if (error) *error = "task graph dependency cycle detected during execution ordering";
    return false;
  }
  return true;
}

bool EffectGraph::add_edge(uint32_t before, uint32_t after, std::string* error) {
  return add_edge(before, after, EffectEdgeKind::Explicit, 0, error);
}

bool EffectGraph::add_edge(uint32_t before, uint32_t after, EffectEdgeKind kind,
                           uint64_t timeline_value, std::string* error) {
  if (before == after) { if (error) *error = "effect graph self-cycle"; return false; }
  edges_.push_back({before, after, kind, timeline_value});
  return true;
}

bool EffectGraph::add_timeline_edge(uint32_t before, uint32_t after, uint64_t required_value,
                                    uint64_t signaled_value, std::string* error) {
  if (required_value == 0) { if (error) *error = "timeline dependency value must be non-zero"; return false; }
  if (signaled_value < required_value) { if (error) *error = "timeline wait point is unsatisfied"; return false; }
  return add_edge(before, after, EffectEdgeKind::Timeline, required_value, error);
}

bool EffectGraph::conflicts(const ir::Effect& before, const ir::Effect& after) {
  if (before.allocation != after.allocation || before.representation_epoch != after.representation_epoch) return false;
  if (before.size == 0 || after.size == 0 || before.offset > UINT64_MAX - before.size || after.offset > UINT64_MAX - after.size) return false;
  const bool overlap = before.offset < after.offset + after.size && after.offset < before.offset + before.size;
  if (!overlap) return false;
  return before.access != ir::Access::Read || after.access != ir::Access::Read;
}

bool EffectGraph::validate_happens_before(const std::vector<std::vector<ir::Effect>>& effects,
                                          std::string* error) const {
  const auto count = static_cast<uint32_t>(effects.size());
  std::vector<std::vector<uint32_t>> adjacency(count);
  for (const auto& edge : edges_) {
    if (edge.before >= count || edge.after >= count) {
      if (error) *error = "effect edge references an unknown task";
      return false;
    }
    adjacency[edge.before].push_back(edge.after);
  }
  for (uint32_t before = 0; before < count; ++before) {
    for (uint32_t after = before + 1; after < count; ++after) {
      bool conflict = false;
      for (const auto& lhs : effects[before]) for (const auto& rhs : effects[after])
        conflict = conflict || conflicts(lhs, rhs);
      if (!conflict) continue;
      std::vector<uint8_t> seen(count);
      std::vector<uint32_t> work{before};
      seen[before] = 1;
      for (size_t i = 0; i < work.size(); ++i) for (uint32_t next : adjacency[work[i]])
        if (!seen[next]) { seen[next] = 1; work.push_back(next); }
      if (!seen[after]) {
        if (error) *error = "conflicting task effects have no happens-before edge";
        return false;
      }
    }
  }
  return true;
}

bool EffectGraph::valid() const {
  std::unordered_map<uint32_t, std::vector<uint32_t>> adjacency;
  for (const auto& edge : edges_) adjacency[edge.before].push_back(edge.after);
  std::unordered_map<uint32_t, uint8_t> mark;
  std::function<bool(uint32_t)> visit = [&](uint32_t node) {
    if (mark[node] == 1) return false;
    if (mark[node] == 2) return true;
    mark[node] = 1;
    if (!std::ranges::all_of(adjacency[node], [&](uint32_t next) { return visit(next); })) return false;
    mark[node] = 2;
    return true;
  };
  return std::ranges::all_of(adjacency, [&](const auto& pair) { return visit(pair.first); });
}

uint32_t EffectGraphBuilder::add_node(std::vector<ir::Effect> effects, std::string* error) {
  if (sealed_) { if (error) *error = "effect graph builder is sealed"; return UINT32_MAX; }
  effects_.push_back(std::move(effects));
  return static_cast<uint32_t>(effects_.size() - 1);
}

bool EffectGraphBuilder::add_dependency(uint32_t before, uint32_t after, std::string* error) {
  if (sealed_) { if (error) *error = "effect graph builder is sealed"; return false; }
  if (before >= effects_.size() || after >= effects_.size() || before == after) {
    if (error) *error = "invalid effect graph dependency";
    return false;
  }
  dependencies_.push_back({before, after});
  return true;
}

bool EffectGraphBuilder::seal(EffectGraph* out, uint32_t* node_count, std::string* error) {
  if (out == nullptr) { if (error) *error = "sealed effect graph output is required"; return false; }
  if (sealed_) { if (error) *error = "effect graph builder is already sealed"; return false; }
  EffectGraph graph;
  for (const auto& edge : dependencies_) if (!graph.add_edge(edge.first, edge.second, EffectEdgeKind::Explicit, 0, error)) return false;
  for (uint32_t before = 0; before < effects_.size(); ++before) {
    for (uint32_t after = before + 1; after < effects_.size(); ++after) {
      bool hazard = false;
      for (const auto& lhs : effects_[before]) for (const auto& rhs : effects_[after])
        hazard = hazard || EffectGraph::conflicts(lhs, rhs);
      if (hazard && !graph.add_edge(before, after, EffectEdgeKind::InferredConflict, 0, error)) return false;
    }
  }
  if (!graph.valid()) { if (error) *error = "effect graph dependency cycle"; return false; }
  if (!graph.validate_happens_before(effects_, error)) return false;
  if (node_count != nullptr) *node_count = static_cast<uint32_t>(effects_.size());
  *out = std::move(graph);
  sealed_ = true;
  return true;
}

EffectGraphShape classify_effect_graph_shape(const EffectGraph& graph, uint32_t node_count) {
  if (node_count <= 1) return EffectGraphShape::LinearChain;
  // Timeline/Publication edges are cross-cutting metadata, not structural
  // ordering the shape classifier reasons about -- only Explicit and
  // InferredConflict edges shape encoder/fence lowering (ADR-027).
  std::vector<uint32_t> out_degree(node_count, 0), in_degree(node_count, 0);
  uint32_t structural_edges = 0;
  for (const auto& edge : graph.edges()) {
    if (edge.kind != EffectEdgeKind::Explicit && edge.kind != EffectEdgeKind::InferredConflict) continue;
    if (edge.before >= node_count || edge.after >= node_count) return EffectGraphShape::Unsupported;
    ++out_degree[edge.before];
    ++in_degree[edge.after];
    ++structural_edges;
  }

  if (structural_edges == 0) return EffectGraphShape::IndependentBranches;

  // Linear chain: exactly node_count - 1 edges, every node has in/out
  // degree <= 1, forming a single path (checked by walking from the sole
  // in-degree-0 node and requiring every step to have exactly one option).
  if (structural_edges == node_count - 1) {
    bool is_chain = true;
    uint32_t sources = 0, start = 0;
    for (uint32_t i = 0; i < node_count; ++i) {
      if (in_degree[i] > 1 || out_degree[i] > 1) { is_chain = false; break; }
      if (in_degree[i] == 0) { ++sources; start = i; }
    }
    if (is_chain && sources == 1) {
      std::vector<std::vector<uint32_t>> adjacency(node_count);
      for (const auto& edge : graph.edges())
        if (edge.kind == EffectEdgeKind::Explicit || edge.kind == EffectEdgeKind::InferredConflict)
          adjacency[edge.before].push_back(edge.after);
      uint32_t node = start;
      uint32_t visited = 1;
      while (!adjacency[node].empty()) { node = adjacency[node].front(); ++visited; }
      if (visited == node_count) return EffectGraphShape::LinearChain;
    }
  }

  // Fork-join: exactly one source node fanning out to every other node
  // except a single join node, and exactly one join node fanning in from
  // every other node except the source, with no edges among the "middle"
  // nodes themselves.
  uint32_t source = UINT32_MAX, join = UINT32_MAX;
  for (uint32_t i = 0; i < node_count; ++i) {
    if (out_degree[i] == node_count - 1 && in_degree[i] == 0) source = i;
    if (in_degree[i] == node_count - 1 && out_degree[i] == 0) join = i;
  }
  if (source != UINT32_MAX && join != UINT32_MAX && source != join &&
      structural_edges == 2 * (node_count - 1)) {
    return EffectGraphShape::ForkJoin;
  }

  return EffectGraphShape::Unsupported;
}

bool effect_graph_deterministic_order(const EffectGraph& graph, uint32_t node_count,
                                      std::vector<uint32_t>* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "effect graph deterministic order output is required"; return false; }
  std::vector<std::vector<uint32_t>> adjacency(node_count);
  std::vector<uint32_t> in_degree(node_count, 0);
  for (const auto& edge : graph.edges()) {
    if (edge.kind != EffectEdgeKind::Explicit && edge.kind != EffectEdgeKind::InferredConflict) continue;
    if (edge.before >= node_count || edge.after >= node_count) {
      if (error) *error = "effect graph edge references an unknown node";
      return false;
    }
    adjacency[edge.before].push_back(edge.after);
    ++in_degree[edge.after];
  }
  std::vector<uint32_t> ready;
  for (uint32_t i = 0; i < node_count; ++i) if (in_degree[i] == 0) ready.push_back(i);
  out->clear();
  out->reserve(node_count);
  while (!ready.empty()) {
    std::ranges::sort(ready);
    const uint32_t node = ready.front();
    ready.erase(ready.begin());
    out->push_back(node);
    for (uint32_t next : adjacency[node]) if (--in_degree[next] == 0) ready.push_back(next);
  }
  if (out->size() != node_count) {
    if (error) *error = "effect graph dependency cycle detected during execution ordering";
    return false;
  }
  return true;
}

EffectGraphForkJoin describe_fork_join(const EffectGraph& graph, uint32_t node_count) {
  EffectGraphForkJoin result;
  std::vector<uint32_t> out_degree(node_count, 0), in_degree(node_count, 0);
  for (const auto& edge : graph.edges()) {
    if (edge.kind != EffectEdgeKind::Explicit && edge.kind != EffectEdgeKind::InferredConflict) continue;
    if (edge.before >= node_count || edge.after >= node_count) continue;
    ++out_degree[edge.before];
    ++in_degree[edge.after];
  }
  for (uint32_t i = 0; i < node_count; ++i) {
    if (out_degree[i] == node_count - 1 && in_degree[i] == 0) result.source = i;
    if (in_degree[i] == node_count - 1 && out_degree[i] == 0) result.join = i;
  }
  for (uint32_t i = 0; i < node_count; ++i) if (i != result.source && i != result.join) result.middle.push_back(i);
  return result;
}

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

bool Certificate::covers(const ir::Effect& effect) const {
  return std::ranges::any_of(ranges, [&](const ir::Effect& range) { return ir::effect_covers(range, effect); });
}

void AccessWitness::record(ir::Effect effect, uint32_t instruction_index) {
  entries_.push_back({effect, instruction_index});
}

WitnessDiff AccessWitness::diff(const Certificate& certificate) const {
  WitnessDiff result;
  for (const auto& entry : entries_) if (!certificate.covers(entry.effect)) result.missing.push_back(entry.effect);
  for (const auto& range : certificate.ranges) {
    const bool observed = std::ranges::any_of(entries_, [&](const WitnessEntry& entry) {
      return ir::effect_covers(range, entry.effect);
    });
    if (!observed) result.unused.push_back(range);
  }
  return result;
}

namespace {
// ir::effect_json (ir.cpp) is anonymous-namespace-local to that translation
// unit, so it is not reusable here -- this mirrors its exact key set
// ("access","allocation","offset","representation_epoch","size") and access
// name mapping so canonical_json() output is consistent with the IR module
// serializer's effect representation.
json::Value effect_result_json(const ir::Effect& effect) {
  const char* access_name = "unknown";
  switch (effect.access) {
    case ir::Access::None: access_name = "none"; break;
    case ir::Access::Read: access_name = "read"; break;
    case ir::Access::Write: access_name = "write"; break;
    case ir::Access::Atomic: access_name = "atomic"; break;
    case ir::Access::Publish: access_name = "publish"; break;
  }
  return json::Value(json::Value::Object{
      {"access", json::Value(std::string(access_name))},
      {"allocation", json::Value(static_cast<int64_t>(effect.allocation))},
      {"offset", json::Value(static_cast<int64_t>(effect.offset))},
      {"representation_epoch", json::Value(static_cast<int64_t>(effect.representation_epoch))},
      {"size", json::Value(static_cast<int64_t>(effect.size))}});
}

json::Value effect_array_json(const std::vector<ir::Effect>& effects) {
  json::Value::Array serialized;
  serialized.reserve(effects.size());
  for (const auto& effect : effects) serialized.emplace_back(effect_result_json(effect));
  return json::Value(std::move(serialized));
}
}  // namespace

std::string ExecutionResult::canonical_json() const {
  json::Value::Array witness_entries;
  witness_entries.reserve(witness.entries().size());
  for (const auto& entry : witness.entries()) {
    witness_entries.emplace_back(json::Value(json::Value::Object{
        {"effect", effect_result_json(entry.effect)},
        {"instruction_index", json::Value(static_cast<int64_t>(entry.instruction_index))}}));
  }
  return json::canonical(json::Value(json::Value::Object{
      {"fault", json::Value(json::Value::Object{
                    {"code", json::Value(fault.code)},
                    {"effect", effect_result_json(fault.effect)},
                    {"instruction_index", json::Value(static_cast<int64_t>(fault.instruction_index))},
                    {"message", json::Value(fault.message)},
                    {"task_index", json::Value(static_cast<int64_t>(fault.task_index))}})},
      {"message", json::Value(message)},
      {"missing_effects", effect_array_json(missing_effects)},
      {"ok", json::Value(static_cast<int64_t>(ok ? 1 : 0))},
      {"outputs_valid", json::Value(static_cast<int64_t>(outputs_valid ? 1 : 0))},
      {"poison", json::Value(static_cast<int64_t>(poison))},
      {"trace", effect_array_json(trace)},
      {"witness", json::Value(std::move(witness_entries))}}));
}

bool validate_certificate(const Certificate& certificate, const std::vector<ir::Effect>& effects, std::string* error) {
  if (!std::ranges::all_of(effects, [&](const ir::Effect& effect) { return certificate.covers(effect); })) {
    if (error) *error = "certificate does not cover inferred effect";
    return false;
  }
  return true;
}

bool build_access_certificate(const Arena& arena, AccessCertificateMode mode,
                              const std::vector<PointerRef>& touched,
                              AccessCertificate* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "access certificate output is required"; return false; }
  if (mode == AccessCertificateMode::SoftwarePaged || mode == AccessCertificateMode::FaultManaged) {
    if (error) *error = "this access certificate mode has no implementation; callers must classify it Unsupported";
    return false;
  }
  out->mode = mode;
  const auto started = std::chrono::steady_clock::now();
  GraphEpochBuilder builder(&arena);
  uint64_t scanned_bytes = 0;
  if (mode == AccessCertificateMode::CertifiedPinned) {
    for (const auto& reference : touched) {
      const Allocation* allocation = arena.lookup(reference);
      if (allocation == nullptr) { if (error) *error = "touched allocation is not active in arena"; return false; }
      if (!builder.add_reference(reference, error)) return false;
      scanned_bytes += allocation->size;
    }
  } else {
    // Universe and DiscoverThenLease both scan every live allocation in the
    // arena; under this project's unified-memory Metal/reference model there
    // is no GPU-resident subset distinct from the arena itself, so
    // DiscoverThenLease's "discovery" is a real, honestly-timed host rescan
    // that happens to find the same set Universe would — an accurate result,
    // not a gap.
    for (const auto& [id, allocation] : arena.allocations()) {
      if (allocation.state != ObjectState::Active) continue;
      if (!builder.add_reference(PointerRef{allocation.id, allocation.generation}, error)) return false;
      scanned_bytes += allocation.size;
    }
  }
  GraphEpoch epoch;
  if (!builder.seal(&epoch, error)) return false;
  out->epoch = epoch;
  out->scanned_bytes = scanned_bytes;
  out->result_bytes = scanned_bytes;
  out->working_set_bytes = scanned_bytes;
  if (mode == AccessCertificateMode::DiscoverThenLease) {
    const auto elapsed = std::chrono::steady_clock::now() - started;
    out->discovery_host_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  }
  return true;
}

namespace {
// load_ref's wire width is 12 bytes (u64 + u32). sizeof(PointerRef) may
// pad to 16; discovery must not invent a wider slot (ADR-028).
constexpr size_t kPointerRefWireBytes = sizeof(uint64_t) + sizeof(uint32_t);

bool decode_pointer_ref(const std::vector<uint8_t>& bytes, size_t offset, PointerRef* out) {
  if (out == nullptr || offset + kPointerRefWireBytes > bytes.size()) return false;
  PointerRef ref{};
  std::memcpy(&ref.allocation, bytes.data() + offset, sizeof(ref.allocation));
  std::memcpy(&ref.generation, bytes.data() + offset + sizeof(ref.allocation), sizeof(ref.generation));
  *out = ref;
  return true;
}

bool discovery_ref_seen(const std::vector<PointerRef>& seen, PointerRef ref) {
  return std::ranges::any_of(seen, [&](PointerRef candidate) {
    return candidate.allocation == ref.allocation && candidate.generation == ref.generation;
  });
}
}  // namespace

bool discover_reachable(const Arena& arena, const std::vector<PointerRef>& seeds, DiscoveryResult* out,
                        std::string* error, const std::function<void()>& after_visit) {
  if (out == nullptr) {
    if (error) *error = "discovery result output is required";
    return false;
  }
  *out = {};
  const auto started = std::chrono::steady_clock::now();
  const uint64_t frozen = arena.topology_epoch();
  out->frozen_topology_epoch = frozen;

  std::vector<PointerRef> worklist;
  for (const auto& seed : seeds) {
    if (seed.allocation == 0 || seed.generation == 0) {
      if (error) *error = "discovery seed is not a well-formed pointer ref";
      return false;
    }
    if (arena.lookup(seed) == nullptr) {
      if (error) *error = "discovery seed is not an active allocation";
      return false;
    }
    if (discovery_ref_seen(out->reachable, seed)) continue;
    out->reachable.push_back(seed);
    worklist.push_back(seed);
  }
  if (arena.topology_epoch() != frozen) {
    if (error) *error = "topology epoch changed during discovery";
    return false;
  }

  while (!worklist.empty()) {
    if (arena.topology_epoch() != frozen) {
      if (error) *error = "topology epoch changed during discovery";
      return false;
    }
    const PointerRef current = worklist.back();
    worklist.pop_back();
    const Allocation* allocation = arena.lookup(current);
    if (allocation == nullptr) {
      if (error) *error = "discovered allocation is no longer active";
      return false;
    }
    out->scanned_bytes += allocation->size;
    if (after_visit) after_visit();
    if (arena.topology_epoch() != frozen) {
      if (error) *error = "topology epoch changed during discovery";
      return false;
    }
    for (size_t offset = 0; offset + kPointerRefWireBytes <= allocation->bytes.size();
         offset += kPointerRefWireBytes) {
      PointerRef child{};
      if (!decode_pointer_ref(allocation->bytes, offset, &child)) continue;
      // Walk only well-formed refs that resolve to Active. A zero generation
      // or a stale id is a break in the chain, not a business store we
      // chase (02 §7.2: discovery Node has no side effects and does not
      // invent edges).
      if (child.allocation == 0 || child.generation == 0) continue;
      if (arena.lookup(child) == nullptr) continue;
      if (discovery_ref_seen(out->reachable, child)) continue;
      out->reachable.push_back(child);
      worklist.push_back(child);
    }
  }

  for (const auto& ref : out->reachable) {
    const Allocation* allocation = arena.lookup(ref);
    if (allocation != nullptr) out->result_bytes += allocation->size;
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  out->discovery_host_ns =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  return true;
}

bool build_discovered_certificate(const Arena& arena, const DiscoveryResult& discovery,
                                  AccessCertificate* out, std::string* error) {
  if (out == nullptr) {
    if (error) *error = "access certificate output is required";
    return false;
  }
  if (arena.topology_epoch() != discovery.frozen_topology_epoch) {
    if (error) *error = "topology epoch changed during discovery";
    return false;
  }
  GraphEpochBuilder builder(&arena);
  for (const auto& ref : discovery.reachable) {
    if (!builder.add_reference(arena, ref, error)) return false;
  }
  GraphEpoch epoch;
  if (!builder.seal(&epoch, error)) return false;
  out->mode = AccessCertificateMode::DiscoverThenLease;
  out->epoch = epoch;
  out->discovery_host_ns = discovery.discovery_host_ns;
  out->discovery_gpu_ns = 0;
  out->scanned_bytes = discovery.scanned_bytes;
  out->result_bytes = discovery.result_bytes;
  out->working_set_bytes = discovery.result_bytes;
  return true;
}

bool certificate_covers_discovery_witness(const AccessCertificate& certificate,
                                          const std::vector<PointerRef>& witness, std::string* error) {
  if (!std::ranges::all_of(witness, [&](const PointerRef& ref) { return certificate.epoch.contains(ref); })) {
    if (error) *error = "discovery witness is not covered by the certificate";
    return false;
  }
  return true;
}

bool compose_certificates(const std::vector<Certificate>& parts, Certificate* out, std::string* error) {
  if (out == nullptr) {
    if (error) *error = "composed certificate output is required";
    return false;
  }
  if (parts.empty()) {
    if (error) *error = "certificate composition requires at least one child certificate";
    return false;
  }
  Certificate composed;
  for (const auto& part : parts) {
    composed.ranges.insert(composed.ranges.end(), part.ranges.begin(), part.ranges.end());
  }
  for (const auto& part : parts) {
    for (const auto& range : part.ranges) {
      if (!composed.covers(range)) {
        if (error) *error = "composed certificate does not cover a child range";
        return false;
      }
    }
  }
  *out = std::move(composed);
  return true;
}

namespace {
uint64_t active_allocation_count(const Arena& arena) {
  uint64_t count = 0;
  for (const auto& [id, allocation] : arena.allocations()) {
    (void)id;
    if (allocation.state == ObjectState::Active) ++count;
  }
  return count;
}
}  // namespace

bool compose_access_certificates(const Arena& arena, const std::vector<AccessCertificate>& parts,
                                 AccessCertificate* out, bool* exploded, std::string* error) {
  if (out == nullptr) {
    if (error) *error = "composed access certificate output is required";
    return false;
  }
  if (exploded != nullptr) *exploded = false;
  if (parts.empty()) {
    if (error) *error = "access certificate composition requires at least one child certificate";
    return false;
  }
  const uint64_t epoch_value = parts.front().epoch.value();
  GraphEpochBuilder builder(&arena);
  bool any_universe = false;
  bool any_discover = false;
  bool any_strictly_smaller = false;
  const uint64_t universe = active_allocation_count(arena);
  for (const auto& part : parts) {
    if (part.epoch.value() != epoch_value) {
      if (error) *error = "cannot compose access certificates from different graph epochs";
      return false;
    }
    if (part.mode == AccessCertificateMode::SoftwarePaged || part.mode == AccessCertificateMode::FaultManaged) {
      if (error) *error = "cannot compose an unimplemented access certificate mode";
      return false;
    }
    if (part.mode == AccessCertificateMode::Universe) any_universe = true;
    if (part.mode == AccessCertificateMode::DiscoverThenLease) any_discover = true;
    if (static_cast<uint64_t>(part.epoch.references().size()) < universe) any_strictly_smaller = true;
    for (const auto& ref : part.epoch.references()) {
      if (!builder.add_reference(arena, ref, error)) return false;
    }
  }
  GraphEpoch epoch;
  if (!builder.seal(&epoch, error)) return false;
  AccessCertificate composed;
  composed.epoch = epoch;
  if (any_universe) composed.mode = AccessCertificateMode::Universe;
  else if (any_discover) composed.mode = AccessCertificateMode::DiscoverThenLease;
  else composed.mode = AccessCertificateMode::CertifiedPinned;
  uint64_t bytes = 0;
  for (const auto& ref : epoch.references()) {
    const Allocation* allocation = arena.lookup(ref);
    if (allocation != nullptr) bytes += allocation->size;
  }
  composed.scanned_bytes = bytes;
  composed.result_bytes = bytes;
  composed.working_set_bytes = bytes;
  const bool became_universe = static_cast<uint64_t>(epoch.references().size()) == universe;
  if (exploded != nullptr) *exploded = became_universe && any_strictly_smaller;
  for (const auto& part : parts) {
    if (!certificate_covers_discovery_witness(composed, part.epoch.references(), error)) return false;
  }
  *out = std::move(composed);
  return true;
}

bool WorkingSetBudget::allows(uint64_t bytes, std::string* error) const {
  if (!has_limit) return true;
  if (bytes <= byte_limit) return true;
  if (error) *error = "working-set budget exceeded";
  return false;
}

bool WorkingSetLease::covers(PointerRef ref) const {
  return std::ranges::any_of(allocations, [&](PointerRef candidate) {
    return candidate.allocation == ref.allocation && candidate.generation == ref.generation;
  });
}

bool WorkingSetLease::add(PointerRef ref, const std::vector<PointerRef>& proven, std::string* error) {
  const bool proven_hold = std::ranges::any_of(proven, [&](PointerRef candidate) {
    return candidate.allocation == ref.allocation && candidate.generation == ref.generation;
  });
  if (!proven_hold) {
    if (error) *error = "lease cannot cover an unproven allocation";
    return false;
  }
  if (covers(ref)) return true;
  allocations.push_back(ref);
  return true;
}

bool WorkingSetLease::valid(const std::vector<PointerRef>& proven, std::string* error) const {
  for (const PointerRef& ref : allocations) {
    const bool proven_hold = std::ranges::any_of(proven, [&](PointerRef candidate) {
      return candidate.allocation == ref.allocation && candidate.generation == ref.generation;
    });
    if (proven_hold) continue;
    if (error) *error = "lease cannot cover an unproven allocation";
    return false;
  }
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

NodeTable::Ref NodeTable::create(const std::string& entry_name) {
  NodeEntry entry;
  entry.entry_name = entry_name;
  entry.generation = 1;
  entry.live = true;
  entries_.push_back(std::move(entry));
  Ref ref;
  ref.index = static_cast<uint32_t>(entries_.size() - 1);
  ref.generation = entries_.back().generation;
  return ref;
}

bool NodeTable::destroy(Ref ref) {
  NodeEntry* entry = lookup(ref);
  if (!entry) return false;
  entry->live = false;
  entry->generation += 1;
  return true;
}

NodeEntry* NodeTable::lookup(Ref ref) {
  if (ref.index >= entries_.size()) return nullptr;
  NodeEntry& entry = entries_[ref.index];
  if (!entry.live || entry.generation != ref.generation) return nullptr;
  return &entry;
}

const NodeEntry* NodeTable::lookup(Ref ref) const {
  if (ref.index >= entries_.size()) return nullptr;
  const NodeEntry& entry = entries_[ref.index];
  if (!entry.live || entry.generation != ref.generation) return nullptr;
  return &entry;
}

}  // namespace vg::core
