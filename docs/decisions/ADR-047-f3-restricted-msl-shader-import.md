# ADR-047: F3 — Restricted MSL Shader Import for Raster Tasks

Status: Accepted

## Context

ADR-043 opened Phase F ("Raster SDK"). Its Decision #4 states: "User-authored
vertex/fragment shaders enter through restricted import, not a new shading
language. Following `05-compiler-language-ir.md`'s already-specified
restricted-interop layer, `CodeObject` accepts hand-written MSL source plus
an explicit effect contract declaring which root schema it reads, which
attachment it writes, which facet it samples. The compiler validates only
the declared contract, not shader logic — this trust boundary is recorded as
`HostAssisted`, per invariant 10 in `docs/START.md` §4 ... never silently
upgraded to a fully-verified status. Designing a new VG-native shading
language is rejected." `05-compiler-language-ir.md` §1's layered input
strategy names this explicitly as its third layer: "Imported SPIR-V/Metal
library: 仅作为受限互操作输入，必须补充/推导 contract；不能因导入而绕过验证"
("import-only restricted interop input; must supply/derive a contract; import
must never bypass verification").

F2 (ADR-046) made rasterization a shape of `TaskRecord`/`ExecutionPlan`, but
every raster task still ran the compiler's own built-in, fixed-function
fragment formula (`raster_facet_metal_source()`, `src/compiler/
compute_package.cpp` — sample the source texture, multiply by a caller
tint). There was no way for a caller to supply their own vertex/fragment
logic. F3 closes that gap for the narrowest case ADR-043 authorized: a
caller-supplied MSL source, matching a fixed binding contract the compiler
already assumes, entering through the existing `CodeObject`/`loadCodeObject`
mechanism rather than a new API.

## Decision

**1. A new `CodeObject.code.format_tag`, `"vg.msl.raster/v1"`, selects the
restricted-import path — no C-ABI version bump.** `vg_api_execution.cpp`'s
`submit()` branches on this tag: `"vg.msl.raster/v1"` parses an envelope via
the new `ir::parse_msl_raster_envelope(text)` into `ExecutionPlan::
user_raster_shader` (`std::optional<ir::UserRasterShaderContract>`,
`device_hal.h`) and leaves `plan.module` at its default; every other tag
keeps the pre-F3 `ir::parse_module` path unchanged. `loadCodeObject`'s public
signature does not change — the same handle-creation call now simply accepts
a second format string. `ExecutionPlan::validate()` (`device_hal.cpp`) skips
`ir::verify(module)` whenever `user_raster_shader` is set, since there is no
linear IR to verify in that submission, and instead requires every task in
`task_graph` to be `TaskKind::Raster` — no mixed compute+raster in v1 (see
Decision #3 below).

**2. The envelope is a fixed four-field contract, not shader logic
inference.** `ir::UserRasterShaderContract` (`src/ir/ir.h`) is `{
std::string root_schema; std::string vertex_entry; std::string
fragment_entry; std::string source; }`. `ir::parse_msl_raster_envelope`
(`src/ir/ir.cpp`) requires all four fields present and non-empty, throwing
`std::runtime_error` with two distinct messages depending on which
malformation occurred: `"IR missing field: <name>"` (the existing `require()`
helper's message, reused verbatim, for a key absent from the JSON object) or
`"MSL raster envelope missing field: <name>"` (this function's own
post-extraction check, for a key present with an empty string value). Neither
the compiler nor either backend inspects `source`'s contents beyond handing
it to the platform shader compiler (Metal) or not at all (reference) — per
ADR-043 Decision #4, "the compiler validates only the declared contract, not
shader logic."

**3. The binding contract is fixed, not declared per-shader.** Every
restricted-import MSL vertex/fragment pair must match the exact struct
layout and buffer/texture/sampler indices the built-in shader uses:
`VgRasterVertex{float2 position; float2 uv;}`, `VgRasterVaryings{float4
position [[position]]; float2 uv;}`, `VgRasterFragment{float4 color
[[color(0)]];}`, and the fixed indices `vg::compiler::kRasterVertexBufferIndex`
/ `kRasterTintBufferIndex` / `kRasterTextureIndex` / `kRasterSamplerIndex`
(all literal `0`, `src/compiler/compiler.h`). `run_raster_pass`'s Metal
encoder (`metal_device_hal.mm`) unconditionally binds the vertex buffer,
source texture, sampler, and tint buffer at these fixed slots regardless of
whether the supplied fragment function reads them — a custom shader is free
to ignore any of the four, but the encoder does not inspect the shader to
decide what to bind. This is why the envelope carries only entry-point names
and source text, not a binding declaration: the contract is the one fixed
shape `05-compiler-language-ir.md` §1's "supplement/derive a contract" step
resolves to for v1, not a per-shader-declared one. A future milestone could
widen this (arbitrary bindings, multiple attachments); F3 deliberately does
not.

**4. Every restricted-import submission is `HostAssisted`, never silently
`Direct`.** Both backends that accept `user_raster_shader` record a
`"raster_user_shader"` `LoweringEvent` classified `HostAssisted` at
`compile()` time:
- Reference (`reference_device_hal.cpp:79-82`): `"caller-declared effect
  contract accepted; shader logic not independently verified; reference
  backend applies fixed C++ shading regardless of supplied MSL text"`.
- Metal (`metal_device_hal.mm:2357-2358`): `"caller-declared effect contract
  accepted; shader logic not independently verified"`.

This is the disclosure ADR-043 Decision #4 requires and `docs/START.md` §4
invariant 10 names directly: "任何无法在当前硬件表达的语义必须返回
`Unsupported`、明确降级或进入 reference backend；不允许静默伪装" ("any
semantic that cannot be honestly expressed on the current hardware must
return `Unsupported`, degrade explicitly, or fall through to the reference
backend; silent pretending is never allowed"). Restricted import is not
"unsupported" — the shader genuinely runs — but its correctness rests on a
trust boundary (declared, not verified) that must stay visible in the
`LoweringReport` for the lifetime of the submission, not be reclassified to
`Direct` once it happens to work.

**5. The reference backend is disclosed as not a pixel-correctness oracle
for user shading logic — this is a deliberate, cited break from ADR-018.**
ADR-018's one unconditional cross-backend invariant is: "if a backend's
`LoweringReport` claims `supported == true` for a fixture, its resulting
allocation bytes must match the reference oracle exactly." F3 cannot honor
that for restricted-import MSL: the reference backend's `raster_triangles()`
(`reference_executor.h/.cpp`) is completely unchanged by F3 — it never
parses or interprets the supplied MSL source, and always applies its
existing fixed C++ shading formula (`sample * tint`), regardless of what the
caller's custom fragment shader claims to compute. A caller who supplies a
custom shader that (for instance) always outputs solid green will see solid
green on Metal and the *original* fixed-formula output on reference — two
backends both claiming `supported == true`, producing genuinely different
pixels by design. This is the specific, disclosed exception to ADR-018's
invariant that restricted import introduces: reference remains the
pixel-correctness oracle for everything upstream of user shading logic
(facet resolution, vertex transform via the same fixed-function path,
attachment semantics), but is not, and cannot be, an oracle for a shading
program it never runs. The `"raster_user_shader"`/`HostAssisted` event
(Decision #4) is what keeps this honest rather than silent.

**6. Mixed compute+raster submissions are out of scope for v1 — a scope
cut, not an oversight.** `ExecutionPlan::validate()` rejects any
`user_raster_shader` submission whose `task_graph` contains a non-`Raster`
task with `"a user_raster_shader submission may only contain raster tasks"`.
This mirrors F3's envelope design: since `plan.module` stays empty for a
restricted-import submission, there is no linear IR a Compute task in the
same graph could run against — supporting mixed submissions would require
either a second, parallel IR-carrying channel on the same `ExecutionPlan` or
extending the envelope format to somehow interleave IR and MSL, neither of
which ADR-043 Decision #4 authorized. **Revisit trigger for this cut**: when
a future milestone needs a single submission to mix host-verified compute
work with restricted-import raster work in one dependency graph (most likely
F6's per-frame SceneRoot or F9's frames-in-flight work, both of which ADR-046
already flagged as needing compute<->raster cross-dependencies), this
restriction should be revisited together with whether `plan.module` and
`plan.user_raster_shader` can be set simultaneously.

**7. Naming collision, not a shared concept**: `ir::Module::root_schema`
(`src/ir/ir.h:23`) and `ir::UserRasterShaderContract::root_schema` (`src/ir/
ir.h:36`) share a field name but name unrelated things — the former is an IR
module's own root schema identity, the latter is the restricted-import
shader's *declared* root schema (which root schema it expects to read),
carried for future effect-contract widening. `ir.h`'s comment on
`UserRasterShaderContract` states this explicitly ("root_schema here is an
unrelated concept from `Module::root_schema`... and must never be
cross-assigned with it"). No code path assigns one to the other; this note
exists to prevent a future refactor from conflating them.

## Implementation

- `src/ir/ir.h` / `src/ir/ir.cpp`: `UserRasterShaderContract` struct and
  `parse_msl_raster_envelope(text)`, reusing the existing `require()` helper
  for the absent-key case and adding an empty-string check for the
  present-but-empty case (two distinct thrown messages, both exercised in
  `tests/unit/ir_test.cpp`).
- `src/backends/device_hal.h`: `Capability::UserShaderImport = 1u << 9`;
  `ExecutionPlan::user_raster_shader` (`std::optional<ir::
  UserRasterShaderContract>`, documented as "Meaningless (and never set)
  unless the backend advertises `Capability::UserShaderImport`").
- `src/backends/device_hal.cpp`: `ExecutionPlan::validate()`'s
  `user_raster_shader`-set branch (Decision #1, #6 above).
- `src/api/vg_api_execution.cpp`: `submit()`'s `"vg.msl.raster/v1"`
  format-tag branch (Decision #1).
- `src/backends/reference/reference_device_hal.cpp`: `capabilities()` sets
  `UserShaderImport`; `compile()` records the `"raster_user_shader"`/
  `HostAssisted` event (Decision #4) when `user_raster_shader` is set;
  `submit()`'s synthesized-success path drives the unchanged
  `raster_triangles()` oracle exactly as F2's plain raster path does
  (Decision #5).
- `src/backends/metal/metal_device_hal.mm`: `ensure_raster_pipeline`/
  `run_raster_pass` gained an optional `user_shader` parameter (entry-point
  names + source, substituted for the built-in `raster_facet_metal_source()`
  when present); `CompileOps::select_package`/`pipelines` record the
  `"raster_user_shader"`/`HostAssisted` event; the render pipeline is still
  built lazily at `submit()` time (unchanged from F2/ADR-046's Decision #2),
  so a malformed `fragment_entry`/`vertex_entry` surfaces as a clean
  submit-time failure (`"Metal raster pipeline compile failed: " +
  pipeline_error`, folded into `submission.result` via the same `finish()`
  lambda ADR-046 introduced), never a crash or a silent fallback to the
  built-in shader.
- `src/backends/vulkan/vulkan_device_hal.cpp`: **zero new code.** `compile()`
  already calls `plan.validate(error)` before its pre-existing
  `task.kind == Raster` rejection loop (`"raster tasks not supported on
  Vulkan backend"`, `Unsupported` `"raster_task"` event). Once `validate()`
  stopped choking on a default/empty `module` whenever `user_raster_shader`
  is set (Decision #1), that pre-existing rejection loop already runs
  correctly against a restricted-import submission's all-`Raster` task
  graph — the two features compose without any Vulkan-specific change.
  Vulkan does not set `Capability::UserShaderImport`.
- Tests: `tests/unit/ir_test.cpp` (envelope round-trip + one absent-field and
  one empty-field rejection per field); `tests/unit/reference_raster_test.cpp`
  (plan-driven restricted-import submission, asserting pixel output matches
  F2's fixed-shading oracle exactly and the `HostAssisted` event is present);
  `tests/vertical_slice/metal_task_timeline_test.cpp` (`run_task_graph_raster_
  user_shader`: happy path with a real hand-written MSL shader producing
  solid-green output distinguishable from the built-in formula, malformed
  entry point producing a clean submit-time failure, mixed compute+raster
  rejection at `compile()`); `tests/vertical_slice/vulkan_task_timeline_test.cpp`
  (`run_raster_msl_rejected`, compile-review-only, proving Decision on
  Vulkan needing zero new code).

## Alternatives

- **A new VG-native shading language for vertex/fragment stages**: rejected
  by ADR-043 Decision #4 itself, citing `12-roadmap-and-risks.md` §7's
  standing P2 risk "自研语言吞噬项目" ("a self-designed language swallows
  the project").
- **Per-shader declared bindings instead of a fixed binding contract**:
  rejected for v1 — see Decision #3. Would require the envelope to carry a
  binding manifest and the encoder to bind dynamically instead of at fixed
  indices, a materially larger change than ADR-043 Decision #4 scoped.
- **Verifying shader logic (e.g., interpreting or partially executing the
  supplied MSL) so reference could reproduce it**: rejected — this is
  exactly the "compiler validates only the declared contract, not shader
  logic" boundary ADR-043 Decision #4 draws. Doing so would also reopen the
  self-hosted-shading-language risk from a different angle (an MSL
  subset interpreter is still a new front-end).
- **Supporting mixed compute+raster submissions in v1 by adding a second IR
  channel**: rejected — see Decision #6's revisit trigger. Not authorized by
  ADR-043 Decision #4 and not needed until a milestone that actually
  requires cross-dependencies between host-verified compute and
  restricted-import raster work in one graph.

## Consequences

- No public C-ABI version bump (`VG_API_VERSION` unchanged) — F3 reuses the
  existing `CodeObject.code.format_tag` mechanism, exactly as ADR-043's
  layered-input strategy anticipated.
- Reference is no longer a pixel-correctness oracle for user shading logic
  specifically (Decision #5) — a disclosed, permanent exception to ADR-018's
  cross-backend invariant, not a bug to be fixed later. Everything upstream
  of the fragment/vertex program (facet resolution, attachment semantics,
  vertex layout) remains cross-backend-verified as before.
- Mixed compute+raster submissions remain out of scope until a milestone
  that genuinely needs them revisits Decision #6.
- Full effect inference (§6's nine-step `ExecutionContract` derivation in
  `05-compiler-language-ir.md`) is not run against restricted-import MSL —
  the caller-declared four-field envelope stands in its place, disclosed as
  `HostAssisted` rather than silently treated as equivalent to a
  fully-inferred contract.
- **Stale doc comment flagged, not fixed (out of this task's edit scope)**:
  `device_hal.h`'s `CompiledPlan::compute_package`/`indexed_compute_package`
  comment states "Exactly one of compute_package / indexed_compute_package
  is ever set for a given CompiledPlan -- never both, never neither, on a
  successful compile()." A successful `compile()` of a pure-raster
  `user_raster_shader` submission leaves **both** unset (`plan.module` was
  never populated, so nothing produces either package) — "never neither" no
  longer holds. See `artifacts/tasks/TASK-F3-restricted-shader-import.md`'s
  appendix for the follow-up.
- `Capability::UserShaderImport` is unconditionally advertised by reference
  but, per manual code review during this task's verification pass,
  **not** currently set by Metal's `capabilities()` (`metal_device_hal.mm`'s
  capability-bits assembly, lines 154-158) even though Metal's `compile()`/
  `submit()` fully implement and accept `user_raster_shader` — see the
  flagged-bug appendix in TASK-F3.

## Evidence

- `build/dev-reference`: full build clean; **26/26 ctest passed** (no new
  ctest target — `ir_test.cpp`'s and `reference_raster_test.cpp`'s new cases
  run under the pre-existing `ir.unit` and `reference.facet-oracles`
  targets), including the new restricted-import envelope round-trip/
  rejection cases and the new plan-driven raster case asserting pixel parity
  with F2's fixed-shading oracle plus the `HostAssisted` disclosure event.
- `build/dev-metal`: full build clean; **56/56 ctest passed** (55 baseline +
  1 new: `vertical-slice.metal.task-graph-raster-user-shader`), covering all
  three required sub-cases (custom-shader happy path with exact solid-green
  pixel match, malformed-entry-point clean submit-time failure, mixed
  compute+raster rejection at `compile()`).
- `build/dev-vulkan`: not built (pre-existing `FATAL_ERROR` guard for
  non-Linux hosts; this task ran on macOS). `run_raster_msl_rejected`
  (`vulkan_task_timeline_test.cpp`) and the actual `compile()` ordering in
  `vulkan_device_hal.cpp` were manually re-reviewed instead: `plan.validate(
  error)` runs first (line 2141), then the pre-existing `task.kind ==
  Raster` rejection loop (line 2157) — confirmed by direct source read, not
  executed.

## Revisit trigger

Revisit when a future milestone (most likely F6's per-frame SceneRoot or
F9's frames-in-flight work) needs a single submission to mix host-verified
compute with restricted-import raster work in one dependency graph (Decision
#6); when the binding contract needs to widen beyond the fixed four-index
shape (Decision #3); or when Metal's `capabilities()` is corrected to set
`Capability::UserShaderImport` (flagged bug, this ADR's Consequences
section) — at which point this ADR's Consequences section's note about the
gap should be removed.
