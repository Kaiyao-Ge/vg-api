#include "vulkan_draw_experiments.h"

#include <iostream>

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  const std::string mode = argv[1];
  std::string error;
  if (mode == "cpu-oracle") {
    using vg::core::NodeTable;
    using namespace vg::vulkan;
    using namespace vg::vulkan::draw_experiments;
    if (!validate_draw_batch({false, 3, 16, 0}, &error) ||
        !validate_draw_batch({true, 2, 20, 4}, &error) ||
        !validate_draw_batch({false, 1, 16, 0xfffffff0u}, &error))
      return 1;
    tier2::ValidatedSelection selected;
    Tier2DrawInput tail{{{{4, 7}}, {{9, 2}}, {{4, 7}}},
                        {{{4, 7}, 0, 0}, {{9, 2}, 1, 1}}};
    if (!validate_tier2_draw_input(tail, &selected, &error) ||
        selected.selected_buckets != std::vector<uint32_t>({0, 1, 0}))
      return 1;
    if (validate_draw_batch({false, 0, 16, 0}, &error) ||
        validate_draw_batch({false, 1, 20, 0}, &error) ||
        validate_draw_batch({true, 1, 20, 2}, &error) ||
        validate_draw_batch({true, 1, 20, 0xfffffff0u}, &error) ||
        validate_draw_batch({true, kMaxIndirectCommands + 1, 20, 0}, &error) ||
        validate_tier2_draw_input({{{{4, 8}}}, tail.authorized}, &selected,
                                  &error) ||
        validate_tier2_draw_input(
            {tail.records, {{{4, 7}, 0, 0}, {{4, 7}, 1, 1}}}, &selected,
            &error))
      return 1;
    std::cout << "draw-experiments-cpu: ok\n";
    return 0;
  }
  if (mode == "gpu")
    return vg::vulkan::draw_experiments::require_formal_e1_pipeline(&error)
               ? 0
               : (std::cerr << error << "\n", 1);
  return 2;
}
