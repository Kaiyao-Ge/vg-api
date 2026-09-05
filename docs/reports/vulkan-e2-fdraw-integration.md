# Vulkan E2 and F draw integration

Date: 2026-09-04. The work was implemented directly in `/Users/gokyrie/projects/vg-api` and integrated by the coordinator.

Vulkan now has a distinct `vg.glsl.raster/v1` import contract and executes it through the formal Raster pipeline. Metal `vg.msl.raster/v1` input rejects before side effects. Direct and indexed draw command layouts are fixed to Vulkan's 16-byte and 20-byte records with bounded count, stride, alignment, and range checks.

The production Tier2 path consumes Core-sealed complete `NodeRef` authority, runs GPU bucket and fill passes, and issues direct or indexed indirect draws without host command readback/re-encoding. Test-only indirect, cull/compact, indexed-address, and Tier2 mechanism probes remain registered as additional physical coverage.

Linux llvmpipe passes the real user GLSL, built-in Raster, indexed SceneRoot+D32, direct/indexed Tier2, mixed-domain, and physical experiment tests under Khronos validation with no VUID.
