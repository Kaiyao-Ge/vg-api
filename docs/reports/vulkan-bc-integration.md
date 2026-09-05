# Vulkan B/C integration and acceptance

Date: 2026-09-04. Workspace: `/Users/gokyrie/projects/vg-api`.

B and C are complete production Stage 6/7 paths. B lowers the restricted
declared-edge pointer graph into a complete-`NodeRef` keyed `CachedObject`
compute package and revalidates the immutable package before dispatch. C
consumes Core-sealed discovery, graph epoch, access certificate, budget, and
lease facts without a backend topology walk or authority reconstruction.

The coordinator integrated the shared lowering, commit, CMake, public ABI, and
source-contract changes after the user-requested GPT-5.6 Terra / medium
subagents completed their exclusive tests and reports. The real Linux tests
cover pointer load/store through the C ABI, stale-edge rejection, discovery
reachability, exact/within-budget plans, lease-only plans, and over-budget or
stale-lease rejection.

Final evidence from the direct checkout:

- Linux full suite: 68/68.
- Vulkan subset on llvmpipe with Khronos validation: 29/29, zero validation
  errors and zero VUID diagnostics.
- Vulkan ASan/UBSan device set: all device tests pass, zero sanitizer or
  validation findings.
- Reference ASan/UBSan: 45/45.
- Metal ASan/UBSan: 79/79.
