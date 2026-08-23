#include "vg/vg.h"

#include "core/core.h"

#include <cstring>
#include <type_traits>

// 04-public-c-abi.md §3/§8: the facet capability token the runtime hands out
// is the same 32/32 pair the core pool mints, so it crosses the C ABI by value
// with no translation layer that could drift out of sync.
static_assert(std::is_standard_layout_v<vg::core::FacetRef>);
static_assert(sizeof(vg::core::FacetRef) == sizeof(VgFacetRef));
static_assert(alignof(vg::core::FacetRef) == alignof(VgFacetRef));
static_assert(offsetof(vg::core::FacetRef, index) == offsetof(VgFacetRef, index));
static_assert(offsetof(vg::core::FacetRef, generation) == offsetof(VgFacetRef, generation));

static_assert(static_cast<uint32_t>(vg::core::FacetKind::Address) == VG_FACET_KIND_ADDRESS);
static_assert(static_cast<uint32_t>(vg::core::FacetKind::Sample) == VG_FACET_KIND_SAMPLE);
static_assert(static_cast<uint32_t>(vg::core::FacetKind::Storage) == VG_FACET_KIND_STORAGE);
static_assert(static_cast<uint32_t>(vg::core::FacetKind::Attachment) == VG_FACET_KIND_ATTACHMENT);
static_assert(static_cast<uint32_t>(vg::core::FacetKind::Transfer) == VG_FACET_KIND_TRANSFER);

int main() {
  vg::core::Arena arena;
  auto& allocation = arena.allocate(64);
  vg::core::CanonicalView view;
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  view.width = 4;
  view.height = 4;

  vg::core::FacetPool pool;
  vg::core::FacetRef ref{};
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &ref)) return 1;

  // A pool-minted ref must survive a byte copy through the public token, so a
  // GPU-visible structure can carry it without the host re-deriving it.
  VgFacetRef public_ref{};
  std::memcpy(&public_ref, &ref, sizeof(public_ref));
  if (public_ref.index != ref.index || public_ref.generation != ref.generation) return 2;

  vg::core::FacetRef round_tripped{};
  std::memcpy(&round_tripped, &public_ref, sizeof(round_tripped));
  if (pool.lookup(arena, round_tripped) == nullptr) return 3;

  // A forged token (same index, wrong generation) must not resolve.
  VgFacetRef forged = public_ref;
  forged.generation += 1;
  vg::core::FacetRef forged_core{};
  std::memcpy(&forged_core, &forged, sizeof(forged_core));
  if (pool.lookup(arena, forged_core) != nullptr) return 4;
  return 0;
}
