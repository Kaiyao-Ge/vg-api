#include "core/facet.h"

#include "core/arena.h"

#include <algorithm>

namespace vg::core {

uint32_t bytes_per_texel(PixelFormat format) {
  switch (format) {
    case PixelFormat::RGBA8Unorm: return 4;
    case PixelFormat::R32Float: return 4;
    case PixelFormat::Depth32Float: return 4;
    case PixelFormat::R16Uint: return 2;
    case PixelFormat::R32Uint: return 4;
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

bool validate_facet_target(const Arena& arena, const CanonicalView& view, FacetKind kind,
                           std::string* error) {
  if (!view.valid(error)) return false;
  if (view.format == PixelFormat::Depth32Float && kind != FacetKind::Attachment) {
    if (error) *error = "Depth32Float canonical views may only acquire Attachment facets";
    return false;
  }
  if ((view.format == PixelFormat::R16Uint || view.format == PixelFormat::R32Uint) &&
      kind != FacetKind::Address) {
    if (error) *error = "R16Uint/R32Uint canonical views may only acquire Address facets";
    return false;
  }
  const auto* allocation = arena.lookup(PointerRef{view.allocation, view.allocation_generation});
  if (allocation == nullptr) { if (error) *error = "canonical view allocation is not active in arena"; return false; }
  if (allocation->size < view.byte_size()) {
    if (error) *error = "canonical view describes more texels than its allocation backs";
    return false;
  }
  return true;
}

bool FacetPool::acquire(const Arena& arena, const CanonicalView& view, FacetKind kind, FacetRef* out,
                        std::string* error) {
  if (out == nullptr) { if (error) *error = "facet ref output is required"; return false; }
  if (!validate_facet_target(arena, view, kind, error)) return false;
  const auto* allocation = arena.lookup(PointerRef{view.allocation, view.allocation_generation});
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

}  // namespace vg::core
