# ADR-015: B4 Shared Compute Codegen Contract

Status: Accepted

## Context

B5 (Metal) and B6 (Vulkan) both need a target-neutral description of a
linear-address compute kernel: which allocations it touches, generated
device source, and a mapping back to the IR instructions that produced each
generated line. Without a single shared contract, each backend would invent
its own package shape and its own notion of which operations are safe to
lower, and the two vertical slices would drift out of byte-level agreement
with the reference oracle. The existing `atomic_add` codegen also predated
the reference executor's actual atomic contract: it emitted a 4-byte
`uint`/`atomic_fetch_add_explicit(atomic<uint>*...)`, while
`reference_executor.cpp` requires `size == sizeof(int64_t)` and performs a
genuine `memcpy`-based `int64_t` add. Backends implementing the 4-byte form
would produce lowerings that "succeed" while being byte-incompatible with
the oracle -- exactly the silent-degradation failure mode `docs/START.md`
§4's tenth invariant forbids.

## Decision

Define the B4 linear subset as exactly three operations:
`load`/`store` (4-byte size, 4-byte-aligned offset) and `atomic_add`
(8-byte size, 8-byte-aligned offset, matching the reference executor's
`int64_t` atomic contract exactly). `publish` and any other IR operation is
rejected by `build_linear_compute_package()`, not silently dropped.

`atomic_add` codegen is extended to 8 bytes on both targets:
- MSL: `atomic_fetch_add_explicit((device atomic<ulong>*)(...), (ulong)VALUEUL, memory_order_relaxed)`.
  Whether this compiles to a *native* 64-bit atomic is a runtime property of
  the selected Metal GPU family, not a compile-time guarantee -- see
  ADR-016 for the `HostAssisted` fallback this implies for Metal.
- GLSL: `#extension GL_EXT_shader_atomic_int64` +
  `atomicAdd(VgAllocationRef64(...).words64[...], uint64_t(operandUL))`
  against a buffer-device-address reference. NVIDIA/Vulkan 1.2+ hardware is
  assumed to support this natively; see ADR-017 for why Vulkan has no
  HostAssisted branch.

`ComputePackage` is finalized as
`{version, canonical_ir_hash, root_schema, bindings[], source_map[], metal_source, vulkan_glsl_source}`.
Every generated MSL/GLSL line traces back to a `ComputeSourceMapEntry
{instruction_index, generated_line, source}`, including the new atomic
branch.

Golden IR fixtures (`tests/fixtures/ir/{load_only,store_only,
atomic_add_only,mixed}.vgir.json`) and their generated-source snapshots
(`tests/fixtures/golden/<name>.{msl,glsl,sourcemap}.golden`) are introduced
as the single source of truth for expected codegen output. `tools/vg-golden-gen`
regenerates these snapshots for human review; it is never invoked from CI.
`compiler.compute-package-golden` (CTest) regenerates each fixture in-memory
and diffs it against the committed golden file. These same fixtures are
reused unmodified by M2a, M2b, and M3's conformance suites (ADR-013's
"don't duplicate semantic expectations" intent, now actually load-bearing
across three backends).

## Alternatives

- Keep 4-byte atomics and have backends interpret `atomic_add` loosely:
  rejected -- it is byte-incompatible with the reference oracle, which is
  the project's sole arbiter of correctness.
- Let each backend define its own linear-subset predicate: rejected --
  produces silent disagreement about which ops are "supported" between
  Metal, Vulkan, and the golden fixtures.
- Generate golden snapshots at CI time instead of committing them: rejected
  -- codegen output is meant to be human-reviewable on change, not
  regenerated invisibly.

## Consequences

Both vertical slices (M2a, M2b) can be written against one finalized
contract and one fixture set instead of guessing at each other's
expectations. A 64-bit atomic that a backend cannot lower natively must be
reported as `HostAssisted` or `Unsupported`, never silently truncated to
32 bits. Golden fixtures become a permanent review surface: any future
codegen change shows up as an explicit, reviewable diff.

## Evidence

`src/compiler/compute_package.cpp` implements the 8-byte atomic path on
both targets; `tests/unit/compute_package_test.cpp` and
`compiler.compute-package-golden` (CTest) pass locally on this machine.
`reference_executor.cpp`'s `atomic_add` handling was read to confirm the
byte contract this ADR pins down.

## Revisit trigger

Revisit if a target GPU family requires a materially different atomic
width or alignment than the current 4-byte load/store, 8-byte atomic_add
split, or if `publish` needs to enter the linear subset (currently out of
scope; publication lowering stays in B7/B8).
