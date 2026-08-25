#include "api/vg_api_internal.h"
#include "api/vg_api_handle_registry.h"

#include <atomic>
#include <memory>

namespace vg_api {
namespace {
HandleRegistry<VgAddressDomain_T> g_domains;
HandleRegistry<VgArena_T> g_arenas;
std::atomic<uint64_t> g_next_arena_id{1};
}  // namespace

bool is_valid_address_domain(VgAddressDomain domain) { return g_domains.contains(domain); }
bool is_valid_arena(VgArena arena) { return g_arenas.contains(arena); }

VgResult VG_CALL create_address_domain(VgDevice device, const VgAddressDomainDesc* desc,
                                        VgAddressDomain* out_domain) {
  if (!is_valid_device(device)) {
    set_diagnostic("device handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (desc == nullptr || out_domain == nullptr) {
    set_diagnostic("address domain descriptor and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result =
      validate_header(desc->header, VG_STRUCTURE_ADDRESS_DOMAIN_DESC, sizeof(VgAddressDomainDesc));
  if (header_result != VG_SUCCESS) return header_result;

  auto wrapper = std::make_unique<VgAddressDomain_T>();
  wrapper->domain.kind = desc->kind;
  *out_domain = g_domains.insert(std::move(wrapper));
  return VG_SUCCESS;
}

void VG_CALL destroy_address_domain(VgAddressDomain domain) {
  if (!g_domains.contains(domain)) return;
  g_domains.erase(domain);
}

VgResult VG_CALL create_arena(VgDevice device, const VgArenaDesc* desc, VgArena* out_arena) {
  if (!is_valid_device(device)) {
    set_diagnostic("device handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (desc == nullptr || out_arena == nullptr) {
    set_diagnostic("arena descriptor and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result = validate_header(desc->header, VG_STRUCTURE_ARENA_DESC, sizeof(VgArenaDesc));
  if (header_result != VG_SUCCESS) return header_result;
  if (!is_valid_address_domain(desc->domain)) {
    set_diagnostic("address domain handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }

  auto wrapper = std::make_unique<VgArena_T>();
  wrapper->arena = vg::core::Arena(g_next_arena_id.fetch_add(1));
  *out_arena = g_arenas.insert(std::move(wrapper));
  return VG_SUCCESS;
}

void VG_CALL destroy_arena(VgArena arena) {
  if (!g_arenas.contains(arena)) return;
  g_arenas.erase(arena);
}

VgResult VG_CALL arena_allocate(VgArena arena, uint64_t size, VgAllocation* out_allocation) {
  if (!g_arenas.contains(arena)) {
    set_diagnostic("arena handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (out_allocation == nullptr) {
    set_diagnostic("output allocation handle is required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  vg::core::Allocation& allocation = arena->arena.allocate(size);
  *out_allocation = reinterpret_cast<VgAllocation>(&allocation);
  return VG_SUCCESS;
}

VgResult VG_CALL get_allocation_ref(VgAllocation allocation, uint64_t* out_id, uint32_t* out_generation) {
  if (allocation == nullptr || out_id == nullptr || out_generation == nullptr) {
    set_diagnostic("allocation handle and output pointers are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const auto* real = reinterpret_cast<const vg::core::Allocation*>(allocation);
  *out_id = real->id;
  *out_generation = real->generation;
  return VG_SUCCESS;
}

}  // namespace vg_api
