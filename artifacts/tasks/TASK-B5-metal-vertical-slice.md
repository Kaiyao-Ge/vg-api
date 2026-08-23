# TASK-B5: Metal Single-Kernel Vertical Slice

Status: complete (linear subset only; verified on real hardware)

Normative docs: `docs/START.md`; `docs/vg-project/06-backend-macos-metal.md`

## Goal

Exercise the B4 linear subset end to end on Metal: shared buffer ->
canonical store/atomic -> command buffer -> completion -> readback,
compared byte-for-byte against the reference oracle.

## Invariants

- `command_queue` is actually used for dispatch (previously created but
  idle).
- `compile()`'s `report.supported` reflects a real MSL pipeline-compile
  attempt, not a hardcoded value.
- A kernel containing `atomic_add` that fails native 64-bit compilation
  falls back to `LoweringClass::HostAssisted` (CPU execution via
  `vg::reference::execute()`), never a silent 32-bit downcast. A kernel
  with no atomic that fails to compile is `Unsupported`.
- Allocation lifetime is keyed by `core::Allocation.generation`; no
  parallel epoch counter.

## Files

- `src/backends/metal/metal_device_hal.h`, `.mm`
- `tests/vertical_slice/metal_vertical_slice_test.cpp` (CTest `vertical-slice.metal`, gated `VG_ENABLE_METAL`)
- `CMakeLists.txt`
- `docs/decisions/ADR-016-metal-vertical-slice.md`

## Validation

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`vertical-slice.metal` passes with byte-exact agreement against the
reference oracle for all four B4 golden fixtures. On this machine,
`load_only`/`store_only` classify `Direct`; `atomic_add_only`/`mixed`
classify `HostAssisted`.

## Known limits

Synchronous `waitUntilCompleted` only; async completion handlers are
deferred. No Tier-1 indirect dispatch. Capability classification is
empirical (attempt-and-observe), not derived from a static GPU-family
table, so results may vary across Metal GPU families/OS versions.
