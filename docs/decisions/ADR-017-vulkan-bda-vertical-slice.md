# ADR-017: Vulkan Buffer-Device-Address Vertical Slice

Status: Accepted

## Context

ADR-014 established the Vulkan adapter foundation but left `compile()`
hardcoded to fail (no SPIR-V path existed) and `VkDeviceCreateInfo` never
populated `ppEnabledExtensionNames` -- meaning even a feature-struct probe
that reported BDA/int64-atomic support as available could not actually
enable those features on the logical device. B6 needs a real, equal-footing
counterpart to the Metal vertical slice (ADR-016): the same B4 golden
fixtures (ADR-015), lowered through GLSL -> SPIR-V -> `VkPipeline`, bound
via buffer device address, dispatched, and read back.

This machine has no Linux/NVIDIA hardware, so all Vulkan work in this ADR
is compile-review-only; actual execution requires a Linux/NVIDIA server.
That constraint is carried forward explicitly rather than glossed over.

## Decision

**SPIR-V compilation is a runtime subprocess call to `glslc`, not a
build-time CMake custom command.** GLSL is generated per-`ir::Module` at
compile() time (it is not a static build asset), so it must be compiled at
runtime. `CMakeLists.txt` does `find_program(VG_GLSLC_EXECUTABLE NAMES glslc)`
at configure time under `VG_ENABLE_VULKAN`, failing with an actionable
`FATAL_ERROR` if not found, and passes the resolved path via
`VG_GLSLC_PATH` compile definition. At runtime, `compile_glsl_to_spirv()`
uses `posix_spawn` with three pipes (stdin/stdout/stderr): GLSL source is
written to `glslc`'s stdin, SPIR-V binary is read back from stdout, and
`stderr` is captured into the `LoweringReport` diagnostic on failure. No
temporary files are written to disk.

**Two findings during implementation reversed part of the original plan's
framing, and are recorded here as a deliberate deviation with
justification:**

1. `shaderBufferInt64Atomics` (`VkPhysicalDeviceVulkan12Features`) and
   `shaderInt64` (base `VkPhysicalDeviceFeatures`) are **core-Vulkan-1.2-promoted**.
   They require no extension strings in `ppEnabledExtensionNames` at all --
   only the corresponding feature-struct bit set to `VK_TRUE` and chained
   into `VkDeviceCreateInfo::pNext`. The original plan's framing (treating
   the missing `ppEnabledExtensionNames` population as *the* blocking bug
   requiring an explicit extension list) was too broad: for this vertical
   slice's actual feature set, `device_info.enabledExtensionCount = 0;
   device_info.ppEnabledExtensionNames = nullptr;` is correct and
   intentional, with a comment recording why.
2. `VkPhysicalDeviceVulkan12Features`/`VkPhysicalDeviceVulkan13Features` are
   only spec-legal to chain when the **physical device's own reported
   `properties.apiVersion`** supports that version -- not the instance's
   requested `VkApplicationInfo.apiVersion`. `make_device_hal()` enforces a
   hard `device_supports_1_2` gate (clean failure with an actionable error
   if the physical device reports below 1.2) and only chains
   `VkPhysicalDeviceVulkan13Features` (for both feature querying and device
   creation) when `device_supports_1_3` is independently true.

`EffectDag`/sync2 capability is simplified to require full core-1.3 support
on the physical device (`device_supports_1_3 && features13.synchronization2`),
**dropping the previously-planned `VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME`
extension fallback path**. This vertical slice's dispatch uses classic
`vkQueueSubmit` + `VkFence`, not `vkQueueSubmit2`; sync2 is advertised as a
capability bit but not functionally exercised by this slice, so the
extension-fallback complexity is not justified yet. The corresponding
`has_extension()` helper was removed as dead code.

`TaskPublication`'s previous unconditional
`capability_bits |= TaskPublication` is removed entirely (not replaced with
a conditional) -- B6 does not yet wire real task-graph submission through
this backend, and claiming the bit unconditionally contradicted ADR-014's
own stated honesty principle.

BDA is a hard requirement for this backend: `if (!bda) { set_error(...);
return nullptr; }`. Allocation uses manual `VkBuffer`/`VkDeviceMemory`
(no VMA) with `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` +
`VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`, host-visible-coherent memory only
(no staging buffer, symmetric with Metal's Shared storage mode choice in
ADR-016). Kernel binding uses a push constant carrying the BDA address
directly (no descriptor set), matching the "linear fast path should not
carry descriptor indirection" invariant.

Unlike Metal (ADR-016), **Vulkan has no HostAssisted branch.** The target
NVIDIA/Linux hardware is assumed to support 64-bit atomics natively via
core-1.2 promotion; if `ensure_pipeline()` fails, `compile()` reports a
genuine `Unsupported` with the `glslc`/pipeline-creation diagnostic. There
is no CPU-side fallback path for Vulkan in this slice.

A new CTest, `vertical-slice.vulkan`
(`tests/vertical_slice/vulkan_bda_vertical_slice_test.cpp`, gated on
`VG_ENABLE_VULKAN`), mirrors `vertical-slice.metal` exactly against the
same four golden fixtures.

## Alternatives

- Keep the originally-planned explicit extension-string list for BDA/int64
  atomics: rejected once WebSearch confirmed both are core-1.2-promoted;
  keeping unnecessary extension strings would only add a failure mode on
  drivers that don't recognize the (redundant) strings.
- Add a Vulkan HostAssisted fallback symmetric with Metal: rejected --
  target hardware for this backend is specifically expected to have native
  64-bit atomics; a silent-seeming HostAssisted path here would mask real
  driver/hardware gaps that should surface as `Unsupported` instead.
- Keep the sync2 extension-fallback path: rejected for this slice since
  `vkQueueSubmit2` is not used yet; revisit when it is (see Revisit
  trigger).
- Use VMA for allocation: rejected as a non-goal per the approved plan --
  manual allocation keeps ownership and failure modes directly observable.

## Consequences

Vulkan now has a complete, spec-legal, review-passed compile/submit path
symmetric with Metal's, built entirely against core-Vulkan-1.2/1.3
features with zero required extension strings for this feature set. The
codebase carries an explicit, load-bearing distinction: Metal may degrade
to `HostAssisted`, Vulkan may not -- this must not be "fixed" into
symmetry without revisiting the hardware assumption behind it.

**This entire ADR's implementation is compile-review-only as of this
writing.** `VG_ENABLE_VULKAN` cannot be configured on this project's only
development machine (macOS; `CMakeLists.txt` hard-fails with
`FATAL_ERROR` on non-Linux). No claim of "verified" or "tested" applies to
any Vulkan-specific code path here -- only "written and reviewed, pending
build/execution on a Linux/NVIDIA server."

## Evidence

Code review only, performed on macOS. `src/backends/vulkan/vulkan_device_hal.cpp`
was fully rewritten and read back in full; `vulkan_probe.cpp` was confirmed
independent (uses raw Vulkan calls directly, unaffected by this rewrite).
`CMakeLists.txt`'s `find_program(VG_GLSLC_EXECUTABLE)` gate and the new
`vertical-slice.vulkan` CTest target were added and reviewed but not built.
No Linux/NVIDIA execution evidence exists yet.

## Revisit trigger

Revisit when this backend is first built/run on a Linux/NVIDIA server:
confirm the core-1.2/1.3 promoted-feature assumption holds on real
driver/hardware combinations, and confirm the dropped sync2
extension-fallback path is still unnecessary. Revisit the "no
HostAssisted" decision only if target hardware assumptions change (e.g. a
future NVIDIA/Linux target that lacks native 64-bit atomics). Revisit when
`vkQueueSubmit2` is actually adopted, at which point the sync2
capability bit needs to gate real behavior, not just be advertised.
