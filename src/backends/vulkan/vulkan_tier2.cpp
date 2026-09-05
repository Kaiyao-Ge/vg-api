#include "backends/vulkan/vulkan_tier2.h"

#include <algorithm>

namespace vg::vulkan::tier2 {
namespace {

bool same_ref(core::NodeTable::Ref left, core::NodeTable::Ref right) {
  return left.index == right.index && left.generation == right.generation;
}

} // namespace

bool validate_pre_authorized_selection(
    const std::vector<SelectionRecord> &records,
    const std::vector<AuthorizedBucket> &authorized, ValidatedSelection *result,
    std::string *error) {
  if (result != nullptr)
    *result = {};
  if (result == nullptr || records.empty() || records.size() > kMaxTasks ||
      authorized.size() < 2 || authorized.size() > kMaxAuthorizedBuckets) {
    if (error)
      *error = "Tier2 physical selection requires 1..65535 records and 2..16 "
               "authorized buckets";
    return false;
  }
  for (size_t i = 0; i < authorized.size(); ++i) {
    const auto &bucket = authorized[i];
    if (bucket.node.generation == 0) {
      if (error)
        *error =
            "Tier2 physical selection received an invalid authorized NodeRef";
      return false;
    }
    if (bucket.physical_bucket >= authorized.size() ||
        bucket.compute_package_slot >= authorized.size()) {
      if (error)
        *error = "Tier2 physical selection requires compact in-range bucket "
                 "and package slots";
      return false;
    }
    for (size_t prior = 0; prior < i; ++prior) {
      if (same_ref(bucket.node, authorized[prior].node) ||
          bucket.physical_bucket == authorized[prior].physical_bucket ||
          bucket.compute_package_slot ==
              authorized[prior].compute_package_slot) {
        if (error)
          *error = "Tier2 physical selection requires unique authorized "
                   "NodeRefs, buckets, and package slots";
        return false;
      }
    }
  }
  result->selected_buckets.reserve(records.size());
  for (const auto &record : records) {
    const auto it =
        std::ranges::find_if(authorized, [&](const AuthorizedBucket &bucket) {
          return same_ref(record.node, bucket.node);
        });
    if (record.node.generation == 0 || it == authorized.end()) {
      if (error)
        *error = "Tier2 physical selection refused a NodeRef that did not "
                 "match a complete authorized NodeRef";
      *result = {};
      return false;
    }
    result->selected_buckets.push_back(it->physical_bucket);
  }
  result->authorized_bucket_count = static_cast<uint32_t>(authorized.size());
  return true;
}

} // namespace vg::vulkan::tier2
