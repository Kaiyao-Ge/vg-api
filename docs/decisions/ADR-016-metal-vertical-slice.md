# ADR-016: Metal Single-Kernel Vertical Slice

Status: Accepted

## Context

ADR-014's sibling foundation work left Metal's `command_queue` created but
never used, `compile()` hardcoded to `report.supported = false`, and
`submit()` as a pure stub. B4 (ADR-015) now defines a real linear-subset
codegen contract; B5 needs to actually exercise it end to end -- shared
buffer, canonical store/atomic, command buffer, completion, and readback
compared byte-for-byte against the reference oracle -- rather than continue
reporting a blanket `Unsupported`.

The open question this ADR resolves is what happens when a kernel touches
`atomic_add` (now 8 bytes per ADR-015) on Metal hardware/OS combinations
that cannot compile a native 64-bit `atomic<ulong>` MSL kernel.

## Decision

`DeviceHal::Impl` gains a private allocation map
(`allocation_id -> MetalAllocationRecord{buffer, generation, byte_size}`)
using `MTLResourceStorageModeShared`, keyed by `core::Allocation.generation`
for invalidation (no separate epoch counter). `ensure_pipeline()` attempts
a real MSL compile via `MTLCompileOptions` + `newComputePipelineStateWithFunction:error:`,
caching against `cached_ir_hash_` to avoid recompiling identical IR.
`dispatch_and_wait()` uses the previously-idle `command_queue`: command
buffer -> compute command encoder -> `setComputePipelineState` ->
`setBuffer` -> `dispatchThreads` -> `endEncoding` -> `commit` ->
`waitUntilCompleted` (synchronous; async completion handlers are out of
scope for this slice).

Capability/lowering classification is empirical, not guessed from GPU
family name: `ensure_pipeline()` is actually attempted.
- Compile succeeds -> `LoweringClass::Direct`.
- Compile fails *and* the kernel contains an `atomic_add` -> the kernel is
  re-lowered through `vg::reference::execute()` on the CPU and classified
  `LoweringClass::HostAssisted`, with a diagnostic reason. This is the
  fallback ADR-015 calls for: never silently truncate a 64-bit atomic to
  32 bits, and never claim a HostAssisted path is native.
- Compile fails and no atomic is present -> `LoweringClass::Unsupported`,
  reported honestly with the compiler diagnostic.

`compile()` and `submit()` are rewritten to use this real pipeline instead
of the previous hardcoded stub. `submit()` uploads via `ensure_buffer()`,
dispatches, and reads results back with a `memcpy` from
`MTLBuffer.contents()` into `core::Allocation.bytes`.

A new CTest, `vertical-slice.metal` (`tests/vertical_slice/metal_vertical_slice_test.cpp`,
gated on `VG_ENABLE_METAL`), runs all four B4 golden fixtures through the
real Metal backend and asserts byte-exact agreement with the reference
oracle.

## Alternatives

- Guess HostAssisted-vs-Direct from `MTLGPUFamily` enums: rejected --
  brittle across OS/driver revisions and violates ADR-014's "don't infer
  capability from a model name" precedent.
- Silently downcast a failed 64-bit atomic to 32-bit: rejected outright by
  `docs/START.md` §4's tenth invariant and by ADR-015's byte contract.
- Async completion handlers instead of `waitUntilCompleted`: deferred:
  this vertical slice is about correctness of the compile/dispatch/readback
  path, not overlap; out of scope until a later performance-focused slice.

## Consequences

Metal now performs a real, hardware-verified round trip for the B4 linear
subset. On this development machine (Apple Silicon, real hardware, not
simulated): `load_only`/`store_only` classify as `Direct`;
`atomic_add_only`/`mixed` classify as `HostAssisted` (this Metal
GPU/toolchain combination does not compile a native 64-bit atomic kernel
for this pattern). Both classifications are correctly represented in
`LoweringReport` and neither is presented as more capable than it is.

## Evidence

Verified locally on real Apple Silicon hardware via `dev-metal` preset:
`compiler.compute-package(-golden)`, `vertical-slice.metal`, and
(after M3, ADR-018) `conformance.device-hal.metal` all pass, with
byte-exact agreement against the reference oracle across all four golden
fixtures.

## Revisit trigger

Revisit if a target Metal GPU family/OS combination is confirmed to
compile native 64-bit MSL atomics, to check whether the empirical
capability probe correctly reclassifies those kernels as `Direct`; or if
async completion handling becomes necessary for a later throughput-focused
milestone.
