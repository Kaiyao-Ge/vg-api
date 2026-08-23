# ADR-029: Root Pointer vs. Bindless Binding Cost -- `IndexedComputeBinding`, `build_indexed_compute_package`

Status: Accepted

## Context

E007 ("root pointer vs. bindless binding cost") is one of the five Phase B
gate experiments ADR-024 committed to real Metal/reference implementations
for. It measures a specific, narrow question: for a module that touches N
distinct allocations, what is the binding-count cost of an
argument-buffer-style single-table indirection compared to
`build_linear_compute_package`'s N separate `buffer(N)` slots? This is the
seventh milestone (TASK-B16) of the eight-milestone plan approved this
session, deliberately ordered after E002/TASK-B15 to reuse its IR/codegen
toolchain conventions (sibling `build_*_compute_package` functions, the
`VgAllocationRef`/BDA push-constant pattern on Vulkan).

## Decision

**One new sibling compiler function, `build_indexed_compute_package`, not an
extension of `build_linear_compute_package` or a new opcode.** The IR
contract is deliberately unchanged from the plain load/store subset --
`supported_indexed_instruction()` accepts only `load`/`store` (rejects
`atomic_add` and any pointer-graph opcode), 4-byte size, 4-byte alignment,
identical to the existing linear granularity. What differs is purely the
*binding shape* the compiler emits for the same IR: every distinct
allocation the module references collapses into one argument-buffer-style
table binding (`IndexedComputeBinding{table_binding=0, stride=sizeof(uint64_t),
count=referenced_allocations.size()}`), in first-seen stable order (a linear
`std::find` scan, not sorted -- order has no semantic weight here, only
stability across recompiles of the same module matters). This isolates the
experiment's one variable (binding count) from every other axis that could
otherwise confound it.

**Metal: a real device-pointer table, classified `Direct`, not
`CachedObject`.** This is the opposite lowering choice from ADR-028's E002,
for a reason specific to this experiment's shape rather than a
reconsideration of ADR-028's reasoning. E002's `load_via`/`store_via` targets
are resolved statically host-side before `compile()` ever runs -- a real
GPU-side dereference would buy that milestone nothing. E007 is the inverse
case: the entire point is to measure the cost of a table the GPU actually
indexes through, so a `CachedObject` (host-resolved, statically bound)
lowering would not measure anything -- it would just be
`build_linear_compute_package` with extra steps. The generated kernel
(`vg_indexed_compute`) takes one `constant uint64_t* vg_table [[buffer(0)]]`
and dereferences each instruction's target via
`(device uint*)vg_table[K]`, where `K` is the instruction's precomputed
table index. The table is populated host-side with each object buffer's real
`[buffer gpuAddress]` (`Impl::dispatch_indexed_and_wait`), so the value the
kernel dereferences is a live device pointer, not a cached/resolved handle --
this is a genuine `Direct` lowering, reported as such via the
`"compute_package"` event.

**A real runtime capability probe (`Impl::probe_gpu_addresses()`) gates this
path, rather than trusting the existing (and already known-stale)
`CapabilitySnapshot::gpu_addresses` bit.** A prior milestone's investigation
had already found `make_hal_snapshot()`'s `gpu_addresses` field hardcoded to
`false`, never wired to any real check, and never fixed (out of scope for
whatever milestone found it). Reusing that stale bit here would have made
E007 permanently, silently `Unsupported` regardless of the actual hardware,
which is worse than the honest alternative: `probe_gpu_addresses()`
allocates a throwaway 16-byte `MTLBuffer` and checks
`[buffer respondsToSelector:@selector(gpuAddress)]` directly, caching the
(correct, live) result in two plain `bool` member fields rather than fixing
the pre-existing snapshot wiring. Fixing `make_hal_snapshot()` itself was
considered and rejected for this milestone -- it is a pre-existing
discrepancy unrelated to E007's own scope, and touching shared snapshot
construction risks affecting every other capability check that reads it,
for a benefit (one correct field) this milestone can get equally well with
a narrowly-scoped, local probe. `compile()` reports `Unsupported` with an
explicit diagnostic ("indexed binding requires MTLBuffer.gpuAddress,
unavailable on this OS/device") when the probe fails, rather than silently
falling back to a different lowering -- matching this project's
honest-degradation invariant.

**The residency cost is reported honestly, not hidden by the binding-count
win.** Every object buffer the table references must still be declared via
`[encoder useResource:buffer usage:...]` even though only the table buffer
itself occupies a `setBuffer:atIndex:` slot -- Metal's residency tracking is
a distinct cost axis from binding-table occupancy, and collapsing N bindings
into 1 does not eliminate the N `useResource:` calls
(`Impl::dispatch_indexed_and_wait` issues exactly one per referenced
allocation). The compiled report's `bytes` field intentionally reports `1`
(the one table binding) rather than folding in the `useResource:` count, so
the reported binding-count contrast (`N` vs. `1`) stays legible as
specifically the binding-slot metric this experiment is named for; the
residency cost is a real, separate cost this ADR records but does not fold
into that one number.

**Reference backend needs zero code changes.** `reference_device_hal.cpp`'s
`compile()` unconditionally calls `build_linear_compute_package` and never
reads `plan.request_indexed_binding`; `submit()` calls `execute()` directly
against `ir::Module` semantics with no `compute_package.has_value()` guard
at all. Since `build_indexed_compute_package`'s IR contract (load/store-only,
4-byte-aligned) is a strict subset of what the linear path already handles,
an indexed-binding-eligible module already compiles and executes correctly
on the reference backend today via the ordinary linear path -- there is
nothing indexed-specific for the reference interpreter to distinguish,
because binding-table shape is a Metal/Vulkan-specific device concept with
no reference-executor analogue.

**Vulkan stays compile-review-only (ADR-024), documentation-only, zero
functional change -- and this milestone records the largest scope deferral
in the whole Phase B gate plan.** A comment block was inserted before
`DeviceHal::compile(...)` in `vulkan_device_hal.cpp` documenting the
hypothetical mapping: reusing E002/ADR-028's existing `VgAllocationRef`
`buffer_reference` convention, but as a `layout(push_constant) uniform
VgIndexedPushConstants { uint64_t table[count]; }` array indexed per
instruction, mirroring `build_indexed_compute_package`'s own
`vulkan_glsl_source` output exactly. This is explicitly *not* the
traditional bindless baseline this experiment's name evokes: real descriptor
indexing (`VK_EXT_descriptor_indexing`, `nonuniformEXT` sampler/buffer
arrays) is not implemented, because this backend has no descriptor-set/pool
infrastructure at all today, and building that entire subsystem to serve a
compile-review-only requirement would be disproportionate to what ADR-024
actually asks for. This is recorded as `Unsupported`/"out of scope for this
milestone" rather than silently omitted.

## Alternatives

- Extend `build_linear_compute_package`/`supported_instruction()` to also
  emit an indexed binding shape: rejected -- same reasoning as ADR-028's
  equivalent alternative; a disjoint binding-shape concern is a sibling
  function, not a parameter threaded through the existing one.
- Lower E007 to `CachedObject` on Metal (matching ADR-028's E002 choice):
  rejected -- would not measure anything distinct from the existing linear
  path; the entire point of this experiment is the GPU-side table
  dereference cost, which requires a genuine `Direct` lowering.
- Fix `make_hal_snapshot()`'s hardcoded `gpu_addresses = false` bit instead
  of adding a separate runtme probe: rejected for this milestone's scope --
  a pre-existing discrepancy unrelated to E007, and touching shared snapshot
  construction risks affecting every other capability check that reads it;
  a narrowly-scoped local probe gets the one correct answer this milestone
  needs without that risk.
- Fold the `useResource:` residency-declaration count into the same `bytes`
  field the binding-count contrast uses: rejected -- would make the reported
  number a mix of two distinct cost axes (binding-slot occupancy vs.
  residency declarations), obscuring the specific N-vs-1 binding contrast
  this experiment is named for. The residency cost is real and recorded in
  this ADR, but is not the number this experiment's `bytes` metric reports.
- Implement real Vulkan descriptor indexing (`VK_EXT_descriptor_indexing`)
  as the bindless comparison baseline: rejected -- this backend has zero
  descriptor-set/pool code today; building that whole subsystem to satisfy
  a compile-review-only requirement (ADR-024) is disproportionate. This is
  the largest single scope deferral in the entire Phase B gate plan and is
  recorded as such rather than downplayed.

## Consequences

`hal::LoweringClass::Direct` gets a second, structurally different producer
alongside the ordinary linear path: both report `Direct`, but one binds N
separate slots and the other binds one table the GPU indexes through -- the
binding-count metric this experiment reports is exactly the distinguishing
signal between them. The `probe_gpu_addresses()` pattern (a narrowly-scoped
local capability check bypassing a known-stale shared snapshot field) is now
a precedent this codebase can reuse if another milestone discovers a similar
discrepancy, rather than a one-off inline hack. The reference backend's
zero-changes-needed finding confirms that binding-shape variance is
purely a device-HAL-layer concern with no reference-executor analogue --
useful precedent for scoping future device-specific-only experiments.

## Evidence

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`vertical-slice.metal.indexed-binding` builds a two-allocation module (one
`load`, one `store`), asserts `compile()`'s `"compute_package"` report event
carries `LoweringClass::Direct` with `bytes == 1` (the single table binding,
in contrast to the linear path's two separate `buffer(N)` slots for the same
module), then asserts the real GPU-executed `store` reproduces
`compute_package.cpp`'s byte-broadcast store pattern in the target
allocation after `submit()` -- a genuine hardware round trip through the
device-pointer table, not a compile-only check. This machine's Metal device
was confirmed via this same test to genuinely support
`MTLBuffer.gpuAddress` -- the test passed through the real dispatch path,
not the honest-`Unsupported`-skip fallback that would fire on a device
without that capability. `cmake --build build/dev-metal --target
vg_backend_metal`: clean, no errors. Full `cmake --build build/dev-metal`:
clean. `ctest --output-on-failure` under `dev-metal`: 26/26 tests passed --
the 25 pre-existing tests (including `vertical-slice.metal.pointer-graph`
and `compiler.compute-package-golden`) unchanged, plus the new
`vertical-slice.metal.indexed-binding`. Vulkan documentation edit to
`vulkan_device_hal.cpp` verified by inspection only: `vg_backend_vulkan` is
not a build target in `build/dev-metal` (Vulkan-SDK-gated CMake
conditional), so the edit carries zero compile risk on this machine and no
ctest exercises it. Reference-backend investigation (this milestone):
confirmed by direct inspection of `reference_device_hal.cpp` that
`compile()`/`submit()` never read `plan.request_indexed_binding` or branch
on `indexed_compute_package` at all -- no code change was made or needed.

## Revisit trigger

Revisit if a future milestone needs the residency-declaration cost
(`useResource:` calls) reported as its own first-class metric rather than
only documented in this ADR's prose -- today it is deliberately excluded
from the `bytes` field to keep the binding-count contrast legible. Revisit
if real Vulkan descriptor indexing becomes a concrete requirement (not just
compile-review-only evidence) -- this milestone's deferral is the largest in
the Phase B gate plan specifically because no descriptor-set/pool
infrastructure exists on this backend at all; building it would be new,
substantial scope, not a small follow-up. Revisit if
`make_hal_snapshot()`'s hardcoded `gpu_addresses = false` bit is ever fixed
for its own sake (unrelated to this milestone) -- at that point
`probe_gpu_addresses()`'s local cache could potentially be replaced by
reading the corrected snapshot field directly, though the local probe would
remain correct either way. Revisit if Vulkan hardware ever becomes reachable
from this environment (permanent constraint today, per ADR-024) -- the
documentation-only mapping above would need a real, hardware-verified
implementation, including the deferred descriptor-indexing comparison
baseline, at that point.
