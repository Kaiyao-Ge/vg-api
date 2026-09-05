# Vulkan D/F integration and acceptance

Date: 2026-09-04. Source workspace: `/Users/gokyrie/projects/vg-api`.

D's facet and representation paths and F's indirect GPU mechanisms are integrated and validated. Sample/Storage/checked-generation facets, representation transforms, ConsumeInput, pipeline classification, dispatch-indirect, cull/compact, indexed-address, and Tier2 bucket/fill all execute on Linux llvmpipe. Transfer, compute, draw-indirect, attachment, and host-read synchronization are explicit, and the validation layer reports no VUID.

These mechanisms now also feed the formal production Raster path: sealed schedules can mix Compute and Raster; direct/indexed triangle draws consume SceneRoot and optional D32 depth; Tier2 consumes Core-sealed complete `NodeRef` authority and GPU-authors indirect draw commands. Results update content epochs and publish completed tasks in canonical order.

Evidence from the integrated checkout:

- Linux Vulkan: 68/68 full CTest pass.
- Vulkan subset: 29/29 with `VK_LAYER_KHRONOS_validation`, zero `Validation Error` and zero `VUID`.
- CPU Reference regression: 45/45.
- Metal ASan/UBSan full regression: 79/79.

The Vulkan device is llvmpipe. The result is correctness and contract evidence; it makes no discrete-GPU performance claim.
