# TASK-B16: E007 -- Root Pointer vs. Bindless Binding Cost, `IndexedComputeBinding`

Status: complete.

Normative docs: ADR-029 (`IndexedComputeBinding`, `build_indexed_compute_package`,
the `Direct`-not-`CachedObject` lowering choice and why it differs from
ADR-028's E002, the `probe_gpu_addresses()` stale-snapshot bypass, the
Vulkan descriptor-indexing scope deferral); ADR-024 (Phase B closure
criterion); ADR-028 (E002, whose IR/codegen toolchain conventions this
milestone reuses). This is the seventh milestone (TASK-B16) of the
eight-milestone plan approved this session.

## Goal

Give E007 ("root pointer vs. bindless binding cost") a real Metal +
reference result: the same load/store-only IR `build_linear_compute_package`
already handles, compiled a second way through a new sibling function that
collapses every distinct referenced allocation into one argument-buffer-
style table binding instead of N separate `buffer(N)` slots, so the
resulting binding-count contrast (N vs. 1) is directly measurable on real
Metal hardware. Vulkan stays compile-review-only per ADR-024
(documentation-only mapping, zero functional wiring).

## Files

- `src/compiler/compiler.h` -- new `IndexedComputeBinding{table_binding,
  stride, count}`, `IndexedComputePackage{version, canonical_ir_hash,
  root_schema, referenced_allocations, binding, source_map, metal_source,
  vulkan_glsl_source}`, `IndexedComputePackageResult{ok, message, package}`,
  and the `build_indexed_compute_package(const ir::Module&)` declaration,
  alongside the existing `ComputeBinding`/`ComputePackage` family.
- `src/compiler/compute_package.cpp` -- new `supported_indexed_instruction()`
  (accepts only `load`/`store`, 4-byte size, 4-byte alignment -- rejects
  `atomic_add` and any pointer-graph opcode) and
  `build_indexed_compute_package()`: collapses every distinct allocation the
  module references into one table binding
  (`table_binding=0, stride=sizeof(uint64_t), count=referenced_allocations.size()`,
  first-seen stable order via linear scan). Generated Metal kernel
  (`vg_indexed_compute`) takes `constant uint64_t* vg_table [[buffer(0)]]`
  and dereferences each instruction's target via `(device uint*)vg_table[K]`;
  generated GLSL reuses the existing `VgAllocationRef` buffer_reference
  convention as a `layout(push_constant)` table array. Store instructions
  use the existing `store_word_pattern` byte-broadcast convention.
- `src/backends/device_hal.h` -- new `ExecutionPlan::request_indexed_binding`
  (bool, default false) and `CompiledPlan::indexed_compute_package`
  (`std::optional<compiler::IndexedComputePackage>`, populated instead of
  `compute_package` when the indexed path is taken -- exactly one of the two
  is ever set on a successful `compile()`).
- `src/backends/metal/metal_device_hal.mm` -- new `Impl::probe_gpu_addresses()`
  (lazily-cached runtime check: allocates a throwaway 16-byte `MTLBuffer`
  and checks `[buffer respondsToSelector:@selector(gpuAddress)]`, caching
  the result in plain `bool` fields rather than trusting the existing,
  already-known-stale `CapabilitySnapshot::gpu_addresses` hardcoded-`false`
  bit); new `Impl::dispatch_indexed_and_wait(...)` (mirrors
  `dispatch_and_wait`'s command-buffer/encoder/`DispatchStats` structure,
  but builds a table `MTLBuffer` populated with each object buffer's real
  `[buffer gpuAddress]`, issues `useResource:usage:Read|Write` for every
  referenced allocation -- the real residency cost this milestone reports
  honestly rather than hides -- then binds only the table buffer at index 0
  before dispatching). `compile()`'s three-way branch (`pointer_graph` /
  `indexed_binding` / linear) rejects `indexed_binding` combined with any
  `effect_dag_passes`/`task_graph.tasks()` as `Unsupported` (mirrors
  ADR-026/027's scope-boundary precedent), gates on `probe_gpu_addresses()`
  (honest `Unsupported` with an explicit diagnostic if unavailable), and
  reports the `"compute_package"` event as `LoweringClass::Direct` with
  `count=1, bytes=1` -- a genuine `Direct` lowering, the opposite
  classification choice from ADR-028's E002 `CachedObject`, because this
  experiment specifically needs a table the GPU actually dereferences at
  runtime (see ADR-029's Decision section for why). `submit()`'s
  `compute_package.has_value()` guard was extended to also accept
  `indexed_compute_package.has_value()`; a new, self-contained
  indexed-dispatch branch builds `generation_by_allocation`, resolves each
  `referenced_allocations` entry via `arena.lookup`/`ensure_buffer`, calls
  `dispatch_indexed_and_wait`, memcpy's touched allocations back, and builds
  trace/witness via a simple `load`->`Read`/`store`->`Write` ternary
  (simpler than the linear path's, since indexed only supports load/store).
- `src/backends/reference/reference_device_hal.cpp` -- **no changes**.
  Confirmed by direct inspection: `compile()` unconditionally calls
  `build_linear_compute_package` and never reads
  `plan.request_indexed_binding`; `submit()` calls `execute()` directly
  against `ir::Module` semantics with no `compute_package.has_value()` guard
  at all. `build_indexed_compute_package`'s IR contract (load/store-only,
  4-byte-aligned) is a strict subset of what the linear path already
  executes correctly, so an indexed-binding-eligible module already runs
  correctly on the reference backend today via the ordinary linear path --
  there is no indexed-specific concept for the reference interpreter to
  distinguish, since binding-table shape is a device-HAL-layer-only concern.
- `src/backends/vulkan/vulkan_device_hal.cpp` -- no functional changes; a
  documentation-only comment block inserted before `DeviceHal::compile(...)`
  maps the hypothetical `VgIndexedPushConstants` table array (reusing
  E002/ADR-028's `VgAllocationRef` `buffer_reference` convention) onto this
  backend, explicitly recording that real descriptor indexing
  (`VK_EXT_descriptor_indexing`/`nonuniformEXT`) as a traditional bindless
  comparison baseline is out of scope -- this backend has no
  descriptor-set/pool infrastructure at all, and building that subsystem for
  a compile-review-only requirement would be disproportionate (ADR-029's
  largest single scope deferral in the whole Phase B gate plan).
- `tests/vertical_slice/metal_task_timeline_test.cpp` -- new
  `run_indexed_binding` mode, following the existing `run_pointer_graph`
  structural pattern. Builds a two-allocation module (one `load`, one
  `store`), asserts `compile()`'s `"compute_package"` event is
  `LoweringClass::Direct` with `bytes == 1` (in contrast to the linear
  path's two `buffer(N)` slots for the same module), honestly skips (returns
  `true`, not a failure) if `compile()` fails with a `gpuAddress`
  diagnostic, then asserts the real GPU-executed `store` reproduces
  `compute_package.cpp`'s byte-broadcast store pattern in the target
  allocation after `submit()`.
- `CMakeLists.txt` -- one new `add_test` entry,
  `vertical-slice.metal.indexed-binding`, on the existing
  `vg_metal_task_timeline_test` executable.
- `experiments/definitions/E007-root-pointer-vs-bindless.json` -- new
  experiment definition (schema `vg.experiment/v1`).
- `docs/decisions/ADR-029-root-pointer-vs-bindless-binding-cost.md` -- new
  ADR.

## Validation

`cmake --build build/dev-metal --target vg_backend_metal` rebuilt clean
after the Metal wiring change (`probe_gpu_addresses`/
`dispatch_indexed_and_wait`, the `submit()` guard update, and the new
indexed-dispatch branch), resolving the compile-blocking gap left from the
compiler-side work. Full `cmake --build build/dev-metal`: clean, no new
warnings beyond the pre-existing benign duplicate-library linker warning.
`ctest --output-on-failure` under the `dev-metal` preset: 26/26 tests
passed -- the 25 pre-existing tests (including
`vertical-slice.metal.pointer-graph` and `compiler.compute-package-golden`)
unchanged, plus the new `vertical-slice.metal.indexed-binding`. This
machine's Metal device was confirmed via that same test to genuinely
support `MTLBuffer.gpuAddress` -- the test passed through the real
device-pointer dispatch path, not the honest-`Unsupported`-skip fallback
that would fire on a device lacking that capability, so E007 has a
genuine, hardware-executed result on this hardware, not merely a documented
honest degradation. Vulkan documentation edit to `vulkan_device_hal.cpp`
verified by inspection only: `vg_backend_vulkan` is not a build target in
`build/dev-metal` (Vulkan-SDK-gated CMake conditional, confirmed absent),
so the edit carries zero compile risk on this machine and is not exercised
by any ctest. Reference-backend "no changes needed" finding verified by
direct reading of `reference_device_hal.cpp` in full (123 lines): no
`request_indexed_binding`/`indexed_compute_package` reference exists
anywhere in the file.

## Known limits

- **Residency cost is documented, not folded into the reported binding
  count** -- every allocation the table references still requires a
  `useResource:` call even though only the table itself occupies a
  `setBuffer:atIndex:` slot; this is a real, distinct cost from
  binding-table-slot occupancy, recorded in ADR-029's Decision section
  rather than mixed into the `bytes` metric, to keep the N-vs-1 binding
  contrast legible as specifically that metric.
- **Gated behind a runtime capability probe, not the shared
  `CapabilitySnapshot`** -- `Impl::probe_gpu_addresses()` is a narrowly
  scoped, local check that bypasses the pre-existing, already-known-stale
  `CapabilitySnapshot::gpu_addresses` hardcoded-`false` bit rather than
  fixing that shared field, since fixing shared snapshot construction risks
  affecting every other capability check that reads it, for a benefit this
  milestone can get equally well with a local probe. See ADR-029 Revisit
  trigger.
- **Vulkan wiring is documentation-only, zero functional change, and
  defers the traditional bindless comparison baseline entirely** -- real
  descriptor indexing (`VK_EXT_descriptor_indexing`/`nonuniformEXT`) is not
  implemented; this backend has no descriptor-set/pool infrastructure at
  all today. This is explicitly the largest single scope deferral in the
  entire Phase B gate plan, recorded as such in ADR-029 rather than
  downplayed.
- **Load/store only, no atomic_add, no pointer-graph opcodes** --
  `supported_indexed_instruction()`'s IR contract is a strict subset of the
  linear path's; a module combining indexed binding with `atomic_add` or
  any `load_ref`/`load_via`/`store_via` opcode from E002/ADR-028 is not
  expressible in a single module today (mutually exclusive scope, not
  combined in this milestone).
