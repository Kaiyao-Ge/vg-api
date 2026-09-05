# Vulkan D: facet, representation, and ConsumeInput evidence

Status: complete and integrated; all registered physical modes execute on Linux Vulkan.

This slice uses the existing `AdapterHarness` only for physical Vulkan
observations. Representation and ConsumeInput are constructed through the real
`CodeObject -> NodeTable -> TaskGraph -> ExecutionEnvelope ->
ExecutionPlanAssembler -> compile -> submit` route. No sealed plan is written
by a test.

`vg_vulkan_facet_representation_test` has five real-device modes, registered
as the frozen CTests:

- `vertical-slice.vulkan.facets`: CheckedNative SampleFacet is compared with
  the Reference archive at two UVs; it confirms the in-shader checked-profile
  specialization and zero violations, repeats to observe a cache hit, writes
  Image and LinearBuffer StorageFacets, and refuses ReferenceStrict rather
  than changing profiles. A stale token is separately a host/pool rejection;
  it is not presented as shader-guard evidence.
- `vertical-slice.vulkan.representation`: an assembled representation request
  must publish a later epoch and retained SampleFacet, keep the old host and
  Vulkan linear backing, and sample the retained VkImage against Reference.
- `vertical-slice.vulkan.consume-input`: the same assembled path with a
  complete proof releases host bytes and the old Vulkan buffer while retaining
  and sampling the new VkImage. The observer reports zero old linear bytes and
  nonzero retained facet-image bytes. Incomplete proof, external in-flight
  hold, and same-allocation compute requests fail closed without advancing the
  source epoch or consuming its bytes; the explicit held allocation is then
  released to prove failed work does not strand the hold.
- `vertical-slice.vulkan.facet-raster`: invokes the existing narrow physical
  dynamic-rendering harness only: clear/readback plus one legacy xyuv triangle
  with constant UV/tint. The latter's first pixel is predicted by the existing
  Reference SampleFacet oracle. It does not open Raster E1, depth, SceneRoot,
  or a production capability.
- `vertical-slice.vulkan.pipeline-classification`: invokes the existing narrow
  VkPipeline cache/classification probe. It requires real enabled
  `dynamicRendering` and a graphics-capable selected queue, but does not turn
  that observation into the production Raster capability.

`cpu-fixture <repo_root>` is deliberately unregistered as a Vulkan pass. It
executes the existing Reference sampling archive on distinct RGBA texels,
transforms and retires the facet (asserting `FacetStatus::Retired`), and runs
the real Core assembler's incomplete-proof and live-source-facet negatives.
It also calls the in-flight Core transform refusal and verifies its explicit
hold is released. This fixture emits a `cpu-fixture:` success line and makes
no device claim.

## Integrated validation record

All five registered modes execute on llvmpipe through the Vulkan adapter. The
formal production path additionally covers mixed Compute/Raster scheduling,
SceneRoot, indexed drawing, D32 depth, content publication, and Tier2 indirect
draws. The final checkout passes 68/68 Linux tests and all 29 Vulkan tests under
`VK_LAYER_KHRONOS_validation`, with zero validation errors or VUID diagnostics.
The Vulkan ASan/UBSan device tests have zero sanitizer findings. Reference
ASan/UBSan passes 45/45 and Metal ASan/UBSan passes 79/79.
