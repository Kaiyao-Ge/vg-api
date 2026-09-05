#ifndef VG_BACKENDS_VULKAN_TIER2_H_
#define VG_BACKENDS_VULKAN_TIER2_H_

// Vulkan Tier2 physical selection component. This remains below the Core
// semantic boundary: Stage 6 supplies records authorized by the sealed
// ExecutionPlan selection fact, and Stage 7 consumes them without inferring
// intent from a multi-Node graph.
#include "core/core.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vg::vulkan::tier2 {

inline constexpr uint32_t kMaxAuthorizedBuckets = 16;
inline constexpr uint32_t kMaxTasks = 65535;

struct AuthorizedBucket {
  core::NodeTable::Ref node;
  uint32_t physical_bucket{};
  uint32_t compute_package_slot{};
};

struct SelectionRecord {
  core::NodeTable::Ref node;
  // These fields are private physical inputs. Stage 7 derives them from the
  // immutable selected Raster TaskRecord and its sealed facet references; the
  // GPU writes the Vulkan indirect command and the host never reads it back.
  uint32_t vertex_or_index_count{};
  uint32_t instance_count{1};
  uint32_t first_vertex_or_index{};
  int32_t vertex_offset{};
  uint32_t first_instance{};
  bool indexed_draw{};
};

struct ValidatedSelection {
  std::vector<uint32_t> selected_buckets;
  uint32_t authorized_bucket_count{};
};

// Validates the physical handoff only. `node` carries index and generation;
// `physical_bucket` is backend-private and cannot become a public authority.
bool validate_pre_authorized_selection(
    const std::vector<SelectionRecord> &records,
    const std::vector<AuthorizedBucket> &authorized, ValidatedSelection *result,
    std::string *error = nullptr);

} // namespace vg::vulkan::tier2

#endif
