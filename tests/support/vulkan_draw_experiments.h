#ifndef VG_TEST_SUPPORT_VULKAN_DRAW_EXPERIMENTS_H_
#define VG_TEST_SUPPORT_VULKAN_DRAW_EXPERIMENTS_H_

#include "backends/vulkan/vulkan_tier2.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vg::vulkan::draw_experiments {

// Exact Vulkan ABI mirrors. These stay test-only and intentionally contain no
// Vulkan handle or public API type.
struct DrawIndirectCommand {
  uint32_t vertex_count, instance_count, first_vertex, first_instance;
};
struct DrawIndexedIndirectCommand {
  uint32_t index_count, instance_count, first_index;
  int32_t vertex_offset;
  uint32_t first_instance;
};
static_assert(sizeof(DrawIndirectCommand) == 16);
static_assert(sizeof(DrawIndexedIndirectCommand) == 20);

inline constexpr uint32_t kMaxIndirectCommands = 65535;

struct DrawBatch {
  bool indexed{};
  uint32_t command_count{};
  uint32_t stride{};
  uint32_t byte_offset{};
};

struct Tier2DrawInput {
  std::vector<tier2::SelectionRecord> records;
  std::vector<tier2::AuthorizedBucket> authorized;
};

// Checks byte-level command consumption prerequisites only. It does not
// execute a draw; E1's formal pipeline is required for that.
bool validate_draw_batch(const DrawBatch &batch, std::string *error = nullptr);
bool validate_tier2_draw_input(const Tier2DrawInput &input,
                               tier2::ValidatedSelection *result,
                               std::string *error = nullptr);

// Deliberately fails until E1 offers a formal pipeline/descriptor consumer.
bool require_formal_e1_pipeline(std::string *error = nullptr);

} // namespace vg::vulkan::draw_experiments

#endif
