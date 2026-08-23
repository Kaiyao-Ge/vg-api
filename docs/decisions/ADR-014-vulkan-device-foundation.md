# ADR-014: Vulkan Device Foundation

Status: Accepted

## Context

Phase B needs a real Vulkan adapter boundary while canonical IR code generation
and command submission are still being implemented. The adapter must discover
features at runtime and keep Vulkan object ownership out of PortableCore.

## Decision

Add an internal `vg::vulkan::DeviceHal` that owns the Vulkan instance, selected
physical device, logical device, and compute queue. It publishes only the
versioned `CapabilitySnapshot` and `DeviceHal` methods. BDA, timeline semaphore,
and synchronization2 bits are enabled only when reported by the selected device.
The current compile and submit methods return `Unsupported` with a
`LoweringReport` diagnostic until the SPIR-V/codegen slice is available.

## Alternatives

- Treat Vulkan as an alias of the reference backend: rejected because it hides
  capability and lowering evidence.
- Expose `VkDevice` through core/public headers: rejected by layer boundaries.
- Claim support from GPU model names: rejected because extensions/features vary.

## Consequences

The adapter can be probed and capability-gated independently of B4/B6. A device
without required features remains observable but cannot compile a linear-address
plan. Real allocation, SPIR-V, synchronization, and submit paths remain open.

## Evidence

The implementation queries physical-device properties, queue families, and
Vulkan 1.2/1.3 feature structs at runtime; unsupported paths are classified in
`LoweringReport`.

## Revisit trigger

Replace the unsupported compile/submit path once B4 produces validated SPIR-V
and B6 adds Vulkan command submission and resource allocation.

**Triggered.** B4 (ADR-015) and B6 (ADR-017) now provide a validated SPIR-V
codegen contract and a real compile/submit path with buffer-device-address
resource binding. The unconditional `TaskPublication` capability bit this
ADR's foundation slice set is also corrected in ADR-017 (the bit is no
longer set at all, since B6 does not yet wire real task-graph submission).
ADR-017's implementation remains compile-review-only pending Linux/NVIDIA
execution.
