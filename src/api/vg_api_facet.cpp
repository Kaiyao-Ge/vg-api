#include "api/vg_api_internal.h"

#include <string>

namespace vg_api {

// v1.3 (F2/ADR-046, F3.5/ADR-048): raster reaches the public C-ABI.
// acquireFacet is the public entry point onto core::FacetPool::acquire,
// letting a caller obtain the VgFacetRef a VG_TASK_KIND_RASTER
// VgTaskRecord's raster_facets/vertex_buffer_ref/index_buffer_ref fields
// require. Unlike createArena/createTaskGraphBuilder etc., a facet is not a
// registry-tracked handle -- it is the same index+generation capability
// token shape as VgFacetRef/VgNodeRef, owned by the device's FacetPool
// (device_hal.h), so this entry point has no is_valid_facet() of its own and
// no dedicated handle wrapper struct.
VgResult VG_CALL acquire_facet(VgDevice device, VgArena arena, const VgCanonicalViewDesc* view,
                                uint32_t facet_kind, VgFacetRef* out_facet) {
  if (!is_valid_device(device)) {
    set_diagnostic("device handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (!is_valid_arena(arena)) {
    set_diagnostic("arena handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (view == nullptr || out_facet == nullptr) {
    set_diagnostic("canonical view descriptor and output facet ref are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result =
      validate_header(view->header, VG_STRUCTURE_CANONICAL_VIEW_DESC, sizeof(VgCanonicalViewDesc));
  if (header_result != VG_SUCCESS) return header_result;

  // The public format/dimension/swizzle/facet_kind fields are raw uint32_t
  // ordinals that match core::PixelFormat/ViewDimension/Swizzle/FacetKind's
  // ordinals 1:1 (see the v1.3 additions comment in vg.h), so no translation
  // table is needed -- but an out-of-range ordinal must be rejected here
  // rather than cast into an enum value that doesn't exist.
  if (view->format > static_cast<uint32_t>(vg::core::PixelFormat::R32Uint) ||
      view->dimension > static_cast<uint32_t>(vg::core::ViewDimension::Texture2DArray) ||
      view->swizzle_red > static_cast<uint32_t>(vg::core::Swizzle::One) ||
      view->swizzle_green > static_cast<uint32_t>(vg::core::Swizzle::One) ||
      view->swizzle_blue > static_cast<uint32_t>(vg::core::Swizzle::One) ||
      view->swizzle_alpha > static_cast<uint32_t>(vg::core::Swizzle::One)) {
    set_diagnostic("canonical view descriptor has an out-of-range enum field");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  if (facet_kind > static_cast<uint32_t>(vg::core::FacetKind::Transfer)) {
    set_diagnostic("facet kind is out of range");
    return VG_ERROR_INVALID_ARGUMENT;
  }

  vg::core::CanonicalView view_internal;
  view_internal.allocation = view->allocation;
  view_internal.allocation_generation = view->allocation_generation;
  view_internal.format = static_cast<vg::core::PixelFormat>(view->format);
  view_internal.dimension = static_cast<vg::core::ViewDimension>(view->dimension);
  view_internal.width = view->width;
  view_internal.height = view->height;
  view_internal.array_layers = view->array_layers;
  view_internal.mip_levels = view->mip_levels;
  view_internal.swizzle.red = static_cast<vg::core::Swizzle>(view->swizzle_red);
  view_internal.swizzle.green = static_cast<vg::core::Swizzle>(view->swizzle_green);
  view_internal.swizzle.blue = static_cast<vg::core::Swizzle>(view->swizzle_blue);
  view_internal.swizzle.alpha = static_cast<vg::core::Swizzle>(view->swizzle_alpha);
  const auto kind = static_cast<vg::core::FacetKind>(facet_kind);

  vg::core::FacetRef facet_internal{};
  std::string error;
  if (!device->hal->facet_pool().acquire(arena->arena, view_internal, kind, &facet_internal, &error)) {
    set_diagnostic(error.c_str());
    return VG_ERROR_INVALID_ARGUMENT;
  }
  out_facet->index = facet_internal.index;
  out_facet->generation = facet_internal.generation;
  return VG_SUCCESS;
}

}  // namespace vg_api
