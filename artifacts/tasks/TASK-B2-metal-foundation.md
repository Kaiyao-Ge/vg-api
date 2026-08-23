# TASK-B2: Metal device and resource foundation

Status: complete (foundation slice)

Normative docs: `docs/START.md`; `docs/vg-project/03-system-architecture.md`; `docs/vg-project/06-backend-macos-metal.md`

## Outcome

The macOS build now has a private Objective-C++ Metal DeviceHAL foundation. It
creates an immutable runtime capability snapshot, probes a shared or private
buffer without exposing an Objective-C object to core, and emits an explicit
`Unsupported` lowering report until the canonical compute codegen work (B4) is
available.

## Invariants

- Metal objects remain inside the backend target.
- Capability bits are set from runtime queries, not an Apple GPU model guess.
- Missing GPU-address, timeline, or indirect capabilities are represented as
  absent bits; no fallback is silently labelled native.
- B2 does not alter PortableCore semantics or the public C ABI.

## Files

- `src/backends/metal/metal_device_hal.h`
- `src/backends/metal/metal_device_hal.mm`
- `CMakeLists.txt`

## Validation

The Linux/non-Metal build must continue to configure, compile, and pass CTest.
On macOS, configure with `-DVG_ENABLE_METAL=ON`; the backend target is
`vg_backend_metal`. A full executable submit remains intentionally deferred to
B4/B5.
