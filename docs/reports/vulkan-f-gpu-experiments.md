# Vulkan F narrow GPU experiments

These BUILD_TESTING-only probes supplement the production Stage7 path. `indirect` has a GPU writer author tightly packed `VkDispatchIndirectCommand` records and consumes them with `vkCmdDispatchIndirect`. `cull-compact` runs the compiler-owned BDA shader and compares the unordered compacted IDs with the Reference oracle. `indexed-address` executes the indexed compute package through a GPU BDA address table. `tier2` exercises GPU bucket/count/fill and consumes every authorized bucket indirectly.

The probes expose real command, barrier, wait, and temporary-byte accounting. Tier2 is classified `EmulatedDevicePass` because host code validates the bounded sealed authority before GPU selection; GPU still writes the match data and commands without host count readback or re-encoding.

All four modes are registered in the Linux Vulkan configuration and pass on llvmpipe under `VK_LAYER_KHRONOS_validation` with no VUID. Formal plan-driven Raster and Tier2 acceptance are covered separately by the G4 Vulkan device tests.
