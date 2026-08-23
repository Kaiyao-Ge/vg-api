# TASK-B15: E002 -- Typed Pointer Graph, CachedObject Lowering

Status: complete.

Normative docs: ADR-028 (typed pointer graph opcodes, `PointerEdge`,
CachedObject lowering, and the documented divergence from this plan's
original Vulkan BDA-pointer-chase framing); ADR-024 (Phase B closure
criterion); ADR-027 (`EffectGraphBuilder`/CachedObject-precedent sibling
pattern this milestone follows for `PointerEdge` and
`build_pointer_graph_compute_package`). This is the sixth milestone
(TASK-B15) of the eight-milestone plan approved this session.

## Goal

Give E002 ("typed pointer graph") a real Metal + reference result: two new
IR opcodes (`load_ref`, plus `load_via`/`store_via` sharing one dereference
form) that read a `core::PointerRef`-shaped value and dereference it
against a statically-declared target, lowered on Metal to a real, hardware-
executed `CachedObject` compute package. Vulkan stays compile-review-only
per ADR-024 (documentation-only mapping, zero functional wiring).

## Files

- `src/ir/ir.h` / `src/ir/ir.cpp` -- new `Instruction::ref_operand` field
  (1-based index into `Module::instructions`, meaningful only for
  `load_via`/`store_via`); new `PointerEdge{from_allocation, field_offset,
  to_allocation}` and `Module::declared_pointer_edges`; `verify()` gained a
  bounded-graph check (`pointer_edge_covers`) that `continue`s past the
  ordinary `inferred_effects`/`declared_effects` coverage path for
  `load_via`/`store_via` -- only `load_ref`'s own read effect needs
  `declared_effects` coverage. `parse_module`/`serialize_module` extended
  for the new field and the new top-level `pointer_edges` JSON array.
- `src/compiler/compiler.h` / `src/compiler/compute_package.cpp` -- new
  `ComputePackageResult build_pointer_graph_compute_package(const
  ir::Module&)`, a sibling to `build_linear_compute_package` (disjoint
  opcode set and disjoint size/alignment rules, not an extension):
  `load_ref` requires a 12-byte, 4-byte-aligned access; `load_via`/
  `store_via` require 4-byte, 4-byte-aligned accesses matching the linear
  package's own granularity. Generated Metal/GLSL elides `load_ref`
  entirely and binds `load_via`/`store_via` targets by static index,
  exactly like ordinary loads/stores.
- `src/backends/metal/metal_device_hal.mm` -- new
  `is_pointer_graph_module(module)` predicate (any `load_ref`/`load_via`/
  `store_via` instruction; exhaustive and mutually exclusive with the
  linear path per module, since `ir::verify()`/both `build_*_compute_
  package()` functions each reject the other's opcodes); `compile()`
  branches its package-building call and the `"compute_package"` report
  classification (`CachedObject` vs. `Direct`) on this predicate;
  `Impl::ensure_pipeline` gained a `function_name` parameter (default
  `"vg_linear_compute"`, `Impl::ensure_effect_dag_pipeline` deliberately
  left untouched) to look up the differently-named
  `vg_pointer_graph_compute` kernel; the pointer-graph package reuses the
  existing single-slot pipeline cache (parametrized by kernel name) rather
  than adding a third cache, since a module is provably never both
  subsets at once. `submit()`'s main trace/witness loop's `ir::Access`
  classification ternary was extended to map `load_ref`->`Read`,
  `load_via`->`Read`, `store_via`->`Write`, matching `ir::verify()`'s own
  mapping -- fixing a self-detected latent bug where these ops would
  otherwise have silently defaulted to `Access::Publish`.
- `src/backends/reference/reference_executor.cpp` -- `execute()`'s per-
  instruction dispatch gained `load_ref` (captures a `core::PointerRef`
  into a per-module `ref_values` scratch vector, keyed by instruction
  index) and `load_via`/`store_via` (dynamic dangling-ref check: the
  captured ref's `{allocation, generation}` must match the dereferencing
  instruction's own `{allocation, generation}`, else `DANGLING_POINTER_REF`
  fault; `store_via` then performs the same byte-fill as `store`).
- `src/backends/vulkan/vulkan_device_hal.cpp` -- no functional changes; a
  documentation-only comment block inserted before `DeviceHal::compile(...)`
  maps the CachedObject-only design onto this backend's existing BDA
  (`buffer_reference`) push-constant convention, explicitly noting the
  divergence from this plan's original "real BDA pointer-chase" framing
  and why (ADR-028).
- `tests/vertical_slice/metal_task_timeline_test.cpp` -- new
  `run_pointer_graph` mode, following the existing `run_task_tier0`/
  `run_access_certificate`/`run_effect_dag` structural pattern and
  `main()`'s mode-dispatch. Builds a single-hop pointer graph (one
  `load_ref` over a 16-byte allocation, one `store_via` targeting a
  separate 4-byte allocation via `ref_operand`, one `declared_pointer_edges`
  entry), asserts `compile()`'s `"compute_package"` event is
  `LoweringClass::CachedObject`, then asserts the real GPU-executed
  `store_via` reproduces `compute_package.cpp`'s byte-broadcast store
  pattern in the target allocation after `submit()`.
- `CMakeLists.txt` -- one new `add_test` entry,
  `vertical-slice.metal.pointer-graph`, on the existing
  `vg_metal_task_timeline_test` executable.
- `experiments/definitions/E002-typed-pointer-graph.json` -- new
  experiment definition (schema `vg.experiment/v1`).
- `docs/decisions/ADR-028-typed-pointer-graph-cachedobject-lowering.md` --
  new ADR.

## Validation

`cmake --build build/dev-metal --target vg_backend_metal` rebuilt clean
after the Metal wiring change, before any test/fixture work began. Full
`cmake --build build/dev-metal`: clean, no new warnings beyond the
pre-existing benign duplicate-library linker warning. Standalone run of
`vg_metal_task_timeline_test pointer-graph <repo_root>` passes. `ctest
--output-on-failure` under the `dev-metal` preset: 25/25 tests passed --
the 24 pre-existing tests unchanged (including
`compiler.compute-package-golden`, confirming the existing linear-only
golden fixtures were untouched by this milestone's IR/opcode changes, and
every pre-existing `version=1` `.vgir.json` fixture still parses/verifies
unchanged -- the first opcode-set change since the four-opcode baseline),
plus the new `vertical-slice.metal.pointer-graph`. Vulkan documentation
edit to `vulkan_device_hal.cpp` verified by inspection only:
`vg_backend_vulkan` is not a build target in `build/dev-metal`
(Vulkan-SDK-gated CMake conditional, confirmed absent via `cmake --build
build/dev-metal --target help`), so the edit carries zero compile risk on
this machine and is not exercised by any ctest.

## Known limits

- **Single-hop only** -- `ir::verify()`'s `pointer_edge_covers` checks
  exactly one `PointerEdge` hop per `load_via`/`store_via`; a multi-hop
  chain is expressible in the IR (each hop its own `load_ref`/edge pair)
  but was never exercised end-to-end by this milestone's fixture/ctest.
  See ADR-028 Revisit trigger.
- **CachedObject, not a dynamic device-pointer dereference** -- this is
  the deliberate scope decision documented in ADR-028: `load_ref`'s loaded
  value is never read on the GPU in this lowering, only used by the
  reference executor's dynamic dangling-ref check. A real device-pointer
  lowering remains a strict superset of what exists today, not precluded
  by it.
- **Vulkan wiring is documentation-only, zero functional change** --
  `vulkan_device_hal.cpp` still unconditionally calls
  `build_linear_compute_package` and would reject a pointer-graph module;
  this milestone does not add a second, independently-runnable Vulkan
  pointer-graph dispatch path, since E002 only requires Vulkan evidence to
  be compile-review-only (ADR-024).
- **Two self-detected latent bugs were fixed before they could ship** --
  `Impl::ensure_pipeline`'s hardcoded `"vg_linear_compute"` kernel name
  (would have failed pipeline creation for every pointer-graph module) and
  `submit()`'s `ir::Access` classification ternary defaulting unrecognized
  ops to `Publish` (would have silently mis-tagged every pointer-graph
  instruction's trace/witness entry). Neither was caught by any
  pre-existing test, since no pointer-graph module had ever reached this
  code path before this milestone. Both are documented in ADR-028's
  Decision section and fixed prior to writing the new ctest.
