# TASK-B3: Vulkan Device Foundation

Status: complete (foundation slice; real SPIR-V/submit deferred)

Normative docs: `docs/START.md`; `docs/vg-project/02-principles-and-semantics.md`; `docs/vg-project/03-system-architecture.md`; `docs/vg-project/07-backend-linux-nvidia-vulkan.md`; `docs/vg-project/10-validation-and-benchmarks.md`

## Goal

Establish the Vulkan adapter foundation: runtime instance/device/queue discovery,
immutable capability snapshot, and private backend ownership. This slice does not
claim SPIR-V lowering or GPU submission; those remain B4/B6.

## Invariants

- Vulkan handles stay inside the adapter and never enter `vg_core` or public ABI.
- Capabilities are queried from the physical device, not inferred from a model name.
- Missing BDA/timeline/synchronization2 capabilities are represented explicitly.
- Unsupported codegen/submission returns a structured diagnostic and LoweringReport.

## Files

- `src/backends/vulkan/vulkan_device_hal.h`
- `src/backends/vulkan/vulkan_device_hal.cpp`
- `docs/decisions/ADR-014-vulkan-device-foundation.md`

## Validation

- Compile the adapter source without Vulkan enabled (stub/unavailable path).
- On Linux with Vulkan SDK, build and run the adapter capability smoke test.
- Existing reference CTest remains the semantic oracle.

## Known limits

SPIR-V generation, Vulkan buffer allocation/address mapping, synchronization2
lowering, and real submission are deliberately deferred to B4/B6/B7.
