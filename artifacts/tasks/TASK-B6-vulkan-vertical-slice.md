# TASK-B6: Vulkan Buffer-Device-Address Vertical Slice

Status: complete (linear subset only; compile-review-only on this machine, unverified on Linux/NVIDIA)

Normative docs: `docs/START.md`; `docs/vg-project/07-backend-linux-nvidia-vulkan.md`

## Goal

Provide the Vulkan counterpart to B5 against the same B4 golden fixtures:
GLSL -> SPIR-V -> `VkPipeline`, buffer-device-address binding, dispatch,
fence-wait, and byte-exact readback comparison against the reference
oracle.

## Invariants

- SPIR-V is compiled at runtime via a `glslc` subprocess (posix_spawn,
  pipe-based, no temp files); `CMakeLists.txt` locates `glslc` at configure
  time and fails with an actionable error if absent.
- `shaderBufferInt64Atomics`/`shaderInt64`/BDA are enabled via core-1.2
  feature structs with **no extension strings** required; feature structs
  are only chained when the physical device's own reported `apiVersion`
  supports that Vulkan version.
- BDA is a hard requirement: device creation fails cleanly if unavailable.
- `TaskPublication` capability bit is never set by this backend (B6 does
  not wire real task-graph submission).
- No `HostAssisted` fallback: a pipeline that fails to compile is reported
  as genuine `Unsupported`, since target NVIDIA/Linux hardware is assumed
  to support 64-bit atomics natively. This is a deliberate asymmetry with
  Metal (TASK-B5) and must not be "fixed" into symmetry without revisiting
  the hardware assumption.
- Kernel binding uses a push constant carrying the BDA address; no
  descriptor set.

## Files

- `src/backends/vulkan/vulkan_device_hal.h`, `.cpp`
- `tests/vertical_slice/vulkan_bda_vertical_slice_test.cpp` (CTest `vertical-slice.vulkan`, gated `VG_ENABLE_VULKAN`)
- `CMakeLists.txt`, `CMakePresets.json` (`dev-vulkan` testPresets entry)
- `docs/decisions/ADR-017-vulkan-bda-vertical-slice.md`

## Validation

**Compile-review-only on this project's macOS development machine.**
`VG_ENABLE_VULKAN` cannot be configured on non-Linux
(`CMakeLists.txt` hard-fails with `FATAL_ERROR`). No build or execution
evidence exists yet; this must not be represented as "verified" or
"tested" until run on a Linux/NVIDIA server.

## Known limits

Host-visible-coherent memory only (no staging-buffer/device-local path).
No VMA. No descriptor indexing, sparse residency, or Tier-1 indirect
dispatch. Sync2 capability bit is advertised but not functionally
exercised (dispatch uses classic `vkQueueSubmit`+`VkFence`, not
`vkQueueSubmit2`); the previously-planned extension-fallback path for
sync2 was dropped as unnecessary while unused. Real validation-layer
wiring is deferred.
