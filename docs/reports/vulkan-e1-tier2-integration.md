# Vulkan E1 / F Tier2 integration result

Date: 2026-09-04. Workspace: `/Users/gokyrie/projects/vg-api`.

Built-in Raster and sealed Tier2 selection are both production `ExecutionPlan -> DeviceHal::compile -> DeviceHal::submit` paths. Raster supports direct/indexed triangle-list draws, SceneRoot, RGBA8 sampling/attachments, D32 depth, mixed Compute/Raster schedules, readback, content epochs, lifetime holds, lowering events, and canonical publication. Tier2 consumes an explicit Core-sealed authorization set and GPU-authors the indirect commands without host readback or re-encoding.

The Linux llvmpipe build passes all 68 tests, including 29 Vulkan tests under Khronos validation with zero VUID. The CPU Reference regression passes 45/45, and the Metal ASan/UBSan full regression passes 79/79. Dedicated real-device tests cover user GLSL import and MSL rejection, indexed SceneRoot+D32 Raster, and repeated/skewed direct and indexed Tier2.
