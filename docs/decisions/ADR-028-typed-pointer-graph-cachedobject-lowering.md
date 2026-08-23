# ADR-028: Typed Pointer Graph -- `load_ref`/`load_via`/`store_via`, `PointerEdge`, CachedObject Lowering

Status: Accepted

## Context

E002 ("typed pointer graph") is one of the five Phase B gate experiments
ADR-024 committed to real Metal/reference implementations for. Before this
milestone (TASK-B15), the IR opcode set was fixed at `load`/`store`/
`atomic_add` since the very first four-opcode baseline -- this is the first
change to that set. The original 8-milestone plan's own framing of this
experiment's Vulkan side described "加载出的 PointerRef 直接转换成
buffer_reference 句柄，实现零描述符的真实指针追逐设计" (loading a
`PointerRef` and converting it directly into a `buffer_reference` handle --
a real, zero-descriptor device-pointer-chasing design). This ADR documents
what was actually built, which diverges from that framing on both backends,
and why.

## Decision

**Two new opcodes plus one new operand field, not a reuse of existing
fields.** `load_ref` reads a 12-byte `{u64 allocation, u32 generation}`
shape out of an allocation (`core::PointerRef`'s wire layout) via the
existing static-effect path -- `ir::verify()` treats it exactly like `load`
for effect inference (`Access::Read`, pushed to `inferred_effects`, must be
covered by a `declared_effects` entry). `load_via`/`store_via` dereference
the ref a prior `load_ref` produced; they carry a new `Instruction::
ref_operand` field (a 1-based index into `Module::instructions` naming that
`load_ref`), rather than overloading `Instruction::value` (which already
carries store/atomic_add's payload) -- the original plan's Fork 2 decision,
reaffirmed here: the 8-byte field cost is far cheaper than making every
consumer branch on op to disambiguate one field's two meanings.

**`load_via`/`store_via` do not get their own inferred effect and do not
need `declared_effects` coverage.** `ir::verify()`'s main loop
`continue`s immediately after checking pointer-edge coverage for these two
ops, before reaching the `inferred_effects.push_back(...)` line every other
op falls through to. Only `load_ref`'s own read effect needs
`declared_effects` coverage. This is a real asymmetry in the verification
model, not an oversight: `load_via`/`store_via`'s legality is established
entirely by the new **bounded-graph** check below, a reachability check
against `declared_pointer_edges` rather than the static range-overlap check
`effect_covers()` performs for ordinary loads/stores.

**`PointerEdge{from_allocation, field_offset, to_allocation}` plus
`Module::declared_pointer_edges` is a flat, single-hop declaration list, not
a graph structure with its own builder.** `ir::verify()`'s
`pointer_edge_covers(module, root, via)` does a direct linear scan for one
matching edge (`edge.from_allocation==root.allocation &&
edge.field_offset==root.offset && edge.to_allocation==via.allocation`) --
no DFS, no cycle detection, no `PointerGraphBuilder`. The original plan
proposed a `core::PointerGraph`/`PointerGraphBuilder` sibling to
`GraphEpoch`/`GraphEpochBuilder` with "cycle-safe DFS + visited" reachability
-- that was not built. A single `load_ref` name-checked directly against
`ref_operand` is a one-hop lookup, not a multi-hop graph walk; building
general graph-reachability infrastructure for a check that never needs more
than one direct-edge lookup would be exactly the kind of disproportionate
scope ADR-026/029's Vulkan-descriptor-indexing scope boundaries warn
against. Multi-hop chains remain expressible (each hop is its own
`load_ref`/edge pair keyed off the previous hop's target), the graph-walk
machinery to verify a chain longer than one hop was simply never needed
for this milestone's scope and was not spec-built ahead of a concrete need.

**CachedObject lowering on both backends, not real device-pointer
dereferencing -- this is the central divergence from the original plan's
Vulkan framing, and it applies for the identical reason on each backend.**
`load_via`/`store_via`'s target allocation is *already statically resolved*
host-side, in `ir::verify()`, via `declared_pointer_edges`, before
`compile()` ever runs. Given that, a real GPU-side pointer dereference
(reinterpreting `load_ref`'s loaded 8-byte allocation id as a raw
`gpuAddress`/`VkDeviceAddress` and dereferencing it in-shader) buys nothing
this milestone's scope needs: it would require the host to additionally
maintain a live allocation-id -> device-address table and keep it
synchronized with `core::Arena`'s actual buffer lifetimes, disproportionate
new infrastructure for a lowering whose targets are already known
statically. Instead:
- `compiler::build_pointer_graph_compute_package` (a new sibling function,
  not an extension of `supported_instruction()`/`build_linear_compute_
  package` -- disjoint opcode set, disjoint size/alignment rules: `load_ref`
  is 12-byte/4-byte-aligned, `load_via`/`store_via` are 4-byte/4-byte-aligned)
  elides `load_ref` from the generated kernel entirely on both the Metal
  and GLSL sides -- its value is never read on the GPU in this lowering.
  It exists only for the reference executor's dynamic dangling-ref check
  (does the loaded ref's `{allocation, generation}` actually match the
  statically-resolved dereferencing instruction's own `{allocation,
  generation}`?) and as a hook for a possible future real-device-pointer
  lowering.
- `load_via`/`store_via` targets are bound the same way ordinary loads/
  stores already are: Metal by `buffer(N)` index, GLSL by the existing
  `VgAllocationRef` push-constant array (`build_linear_compute_package`'s
  own BDA convention) -- selected by the statically-resolved binding index,
  never by reinterpreting `load_ref`'s loaded value as a live address.
- `compile()` reports the `"compute_package"` event as
  `hal::LoweringClass::CachedObject` (not `Direct`) for any pointer-graph
  module, honestly signaling "a real device object is used, but its
  identity was resolved by static/cached means, not a dynamic device-side
  chase" -- exactly `LoweringClass::CachedObject`'s documented meaning.

**Metal wiring (`metal_device_hal.mm`), this milestone's only functional
backend change beyond the compiler:**
- `is_pointer_graph_module(module)`: `true` iff any instruction's op is
  `load_ref`/`load_via`/`store_via`. Exhaustive and mutually exclusive with
  the linear path for one module: `ir::verify()` and both
  `build_*_compute_package()` functions each reject the other opcode set,
  so a module is provably either the linear subset or the pointer-graph
  subset, never both.
- `compile()` branches its package-building call and `"compute_package"`
  classification on this predicate. Everything downstream (the certificate-
  mode early return, the timeline-support check, the atomic-fallback
  `HostAssisted` block) is untouched -- `has_atomic` naturally evaluates
  false for pointer-graph modules (they never contain `atomic_add`), so
  that fallback path is simply never triggered for them.
- `Impl::ensure_pipeline` gained a `function_name` parameter (default
  `"vg_linear_compute"`, preserving its one pre-existing call site's
  behavior unchanged) so it can look up
  `compiler::build_pointer_graph_compute_package`'s differently-named
  kernel, `vg_pointer_graph_compute`. `Impl::ensure_effect_dag_pipeline`
  (E012's separate single-slot-per-hash cache) was deliberately left
  untouched -- effect-dag passes are always linear-subset modules, this
  parameter would never be exercised there.
- The pointer-graph package reuses the *single-slot* `pipeline`/`library`/
  `cached_ir_hash` cache (the one `build_linear_compute_package`'s primary
  path already uses), parametrized by kernel name, rather than adding a
  third cache alongside it and `effect_dag_pipelines` -- justified by the
  same mutual-exclusivity property `is_pointer_graph_module` relies on: a
  given `ExecutionPlan.module` is provably never both subsets at once, so
  one slot correctly serves both, keyed by IR hash exactly as before.
- `submit()`'s main instruction-trace/witness loop's `ir::Access`
  classification ternary was extended to map `load_ref`->`Read`,
  `load_via`->`Read`, `store_via`->`Write`, matching `ir::verify()`'s own
  mapping exactly. This was a real, self-detected correctness bug caught
  before it shipped: the chain previously defaulted every unrecognized op
  to `Access::Publish`, which would have silently mis-tagged every
  pointer-graph instruction's trace/witness entry. The structurally
  identical ternary inside the separate `effect_dag_passes` submit branch
  was deliberately left unchanged, since E012 passes are always
  linear-subset modules and can never reach a pointer op there.

**Vulkan stays compile-review-only (ADR-024), documentation-only, zero
functional change.** `vulkan_device_hal.cpp`'s `compile()` still calls
`build_linear_compute_package` unconditionally; a pointer-graph module would
fail there exactly as it would have on Metal before this milestone's
wiring. A documentation-only comment block, following the exact ADR-027/
E012 precedent, was inserted before `compile()` mapping the same
CachedObject-only design onto this backend's existing BDA (`buffer_
reference`) push-constant convention -- explicitly noting that a real
BDA-based device-pointer dereference (the plan's original framing) was not
built here either, for the identical "already statically resolved,
disproportionate new infrastructure" reason as Metal. No Vulkan hardware is
reachable from this machine (permanent constraint, per ADR-024); this
backend is not part of this build configuration at all (`vg_backend_vulkan`
is gated behind a Vulkan-SDK-found CMake conditional, absent from
`build/dev-metal`), so this edit carries zero compile risk and is verified
by inspection only, not by any ctest.

## Alternatives

- Build a real device-pointer dereference on Metal (reinterpreting
  `load_ref`'s loaded allocation id as a raw `gpuAddress` and dereferencing
  it in-shader): rejected -- would require a new host-side allocation-id ->
  device-address table synchronized with `core::Arena`'s buffer lifetimes,
  disproportionate for a lowering whose targets are already statically
  known via `declared_pointer_edges`. This is the same tradeoff argument
  ADR-029/TASK-B16 makes for deferring real Vulkan descriptor indexing.
- Build `core::PointerGraph`/`PointerGraphBuilder` as a general graph
  structure with DFS+visited reachability (the original plan's framing):
  rejected -- `ir::verify()`'s bounded-graph check never needs more than a
  one-hop direct-edge lookup at this milestone's scope; general multi-hop
  graph-walk infrastructure would be built ahead of any concrete need.
- Reuse `Instruction::value` for `ref_operand` instead of a new field:
  rejected in the original plan's Fork 2 and reaffirmed here -- `value`
  already carries store/atomic_add payload semantics; overloading it would
  force every consumer to branch on op to disambiguate, for a savings of 8
  bytes per instruction.
- Extend `build_linear_compute_package`/`supported_instruction()` to also
  accept `load_ref`/`load_via`/`store_via`: rejected -- disjoint opcode set
  and disjoint size/alignment rules (12-byte vs. 4-byte, no atomic_add
  analogue) make a separate sibling function
  (`build_pointer_graph_compute_package`/`supported_pointer_instruction`)
  the lower-risk choice, matching the project's established pattern for
  `cull_compact_metal_source()`/`task_ring_metal_source()` as independent
  hand-written kernels rather than generalized extensions.
- Add a third, dedicated pipeline cache for pointer-graph packages
  alongside the existing single-slot and `effect_dag_pipelines` caches:
  rejected -- the single-slot cache already correctly serves both subsets,
  since a module is provably never both at once; a third cache would add
  state with no behavioral benefit.
- Build a real second Vulkan BDA pointer-chase implementation: rejected --
  E002 only requires Vulkan evidence to be compile-review-only (ADR-024),
  and the CachedObject-only design is the correct lowering choice for this
  scope on Vulkan for the same reason as Metal, not merely a Vulkan-side
  compromise.

## Consequences

The IR opcode set changes for the first time since the four-opcode
baseline, without breaking any existing `version=1` fixture -- every
pre-existing `.vgir.json` file uses only `load`/`store`/`atomic_add` and
parses/verifies unchanged, confirmed by the full pre-existing ctest suite
staying green. `hal::LoweringClass::CachedObject` gets its first real
producer in this codebase (previously only `Direct`/`HostAssisted`/
`Unsupported` had live call sites), giving the honest-degradation taxonomy
a concrete precedent for "a real device object, resolved by static/cached
means" as distinct from both `Direct` (fully dynamic) and `HostAssisted`
(a host round-trip is involved). A future real-device-pointer lowering
(reinterpreting `load_ref`'s value as a live address) remains a strict
superset of what exists today -- `load_ref`'s value is preserved in the
reference executor's `ref_values` vector and available to a future Metal/
Vulkan lowering, it is simply unused by the GPU side of this one.

## Evidence

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`vertical-slice.metal.pointer-graph` builds a single-hop pointer graph (one
`load_ref` over a 16-byte allocation, one `store_via` targeting a separate
4-byte allocation via `ref_operand`, one `declared_pointer_edges` entry
covering the hop), asserts `compile()`'s `"compute_package"` report event
carries `LoweringClass::CachedObject` (not `Direct`), then asserts the
actual GPU-executed `store_via` reproduces `compute_package.cpp`'s
byte-broadcast store pattern (`store_word_pattern`) byte-for-byte in the
target allocation after `submit()` -- a real hardware round trip, not a
compile-only check. `cmake --build build/dev-metal --target
vg_backend_metal`: clean, no errors. Full `cmake --build build/dev-metal`:
clean. `ctest --output-on-failure` under `dev-metal`: 25/25 tests
passed -- the 24 pre-existing tests unchanged (including
`compiler.compute-package-golden`, confirming no regression to the
pre-existing linear-only golden fixtures), plus the new
`vertical-slice.metal.pointer-graph`. Vulkan documentation edit to
`vulkan_device_hal.cpp` verified by inspection only: `vg_backend_vulkan` is
not a build target in `build/dev-metal` (Vulkan-SDK-gated CMake
conditional), so the edit carries zero compile risk on this machine and no
ctest exercises it.

## Revisit trigger

Revisit if a future milestone needs multi-hop pointer chains verified
end-to-end (today's `pointer_edge_covers` only ever checks one hop per
`load_via`/`store_via`, though the `PointerEdge` list itself already
supports declaring multiple hops) -- at that point a real reachability
walk over `declared_pointer_edges` would be needed, not just the current
direct-edge lookup. Revisit if a real device-pointer lowering becomes
necessary (e.g. a future experiment specifically measuring dynamic
pointer-chase cost against this milestone's CachedObject baseline) --
`load_ref`'s value is already captured by the reference executor and
available for a Metal/Vulkan lowering to reinterpret as a live address,
this was simply out of scope here. Revisit if Vulkan hardware ever becomes
reachable from this environment (permanent constraint today, per ADR-024)
-- the documentation-only mapping above would need a real,
hardware-verified implementation at that point.
