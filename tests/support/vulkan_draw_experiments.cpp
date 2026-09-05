#include "vulkan_draw_experiments.h"

#include <limits>

namespace vg::vulkan::draw_experiments {

bool validate_draw_batch(const DrawBatch &batch, std::string *error) {
  const uint32_t command_size = batch.indexed
                                    ? sizeof(DrawIndexedIndirectCommand)
                                    : sizeof(DrawIndirectCommand);
  const uint64_t bytes = static_cast<uint64_t>(batch.command_count) *
                         static_cast<uint64_t>(batch.stride);
  if (batch.command_count == 0 || batch.command_count > kMaxIndirectCommands ||
      batch.stride != command_size || (batch.byte_offset % 4) != 0 ||
      bytes >
          static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1u ||
      static_cast<uint64_t>(batch.byte_offset) + bytes >
          static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1u) {
    if (error)
      *error = "draw indirect batch requires non-zero bounded count, exact "
               "stride, aligned offset, and a byte range within the uint32 "
               "address domain";
    return false;
  }
  return true;
}

bool validate_tier2_draw_input(const Tier2DrawInput &input,
                               tier2::ValidatedSelection *result,
                               std::string *error) {
  return tier2::validate_pre_authorized_selection(
      input.records, input.authorized, result, error);
}

bool require_formal_e1_pipeline(std::string *error) {
  if (error)
    *error = "Vulkan F draw experiment requires E1's callable formal Raster "
             "pipeline and descriptor contract; no legacy raster helper or CPU "
             "fallback is permitted";
  return false;
}

} // namespace vg::vulkan::draw_experiments
