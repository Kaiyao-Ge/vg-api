# ADR-043: Phase F Scope — Raster SDK Track and Public C ABI Expansion

Status: Accepted

## Context

`docs/START.md` §1 states the project's one-line goal as a unified GPU
semantic layer validated on CPU/reference, Metal, and Vulkan adapters
before any native UMD/KMD question is evaluated. §6 defines "phase one
complete" explicitly as *not* "having a triangle demo" — it is CPU
reference/Metal/Vulkan conformance, root-pointer/Region/Task-publication/
Timeline/capture evidence, adapter hidden-cost observability, a
pointer-graph tier comparison, and a RepresentationEpoch/ConsumeInput
peak-memory result. `docs/vg-project/12-roadmap-and-risks.md` Phases 0–E
never schedule a linkable rasterization SDK as an exit criterion. This is a
deliberate scope choice recorded in the project's own charter, not an
oversight — which is why the gap this ADR addresses exists by design and
needs a new phase, not a correction of a missed one.

`12-roadmap-and-risks.md` defines Phases 0 through E only; there is no
Phase F in that document. Phase E ("Research Alpha") is recorded
research-closed per ADR-042. ADR-042 Decision #4 froze the public C ABI at
v1.0 (`createRuntime`/`destroyRuntime`/`enumerateAdapters` only) and stated
Research Alpha "does not add a new semantic object, a new public C
function, or a new lowering path." ADR-042's own **Revisit trigger**
section names "if the public C ABI grows a new version" as a condition for
re-evaluation. That condition is what this ADR exercises — this is a cited
reopening, not a silent override, and ADR-042's text is not rewritten.

`docs/vg-project/04-public-c-abi.md` §17 already specifies the target
minimal-application call chain end to end — `openAdapter → createDevice →
createAddressDomain → createArena → arenaAllocate → loadCodeObject →
createNode → createTaskGraphBuilder → taskGraphAppend → sealTaskGraph →
createExecutionEnvelope → submit` — and states explicitly: "首版实现可以
暂时缩小函数集合，但不能通过隐式全局 Device、隐式 GPU 选择、隐式同步或
backend handle 泄漏来缩短样例。" Expanding the C ABI toward that chain
executes a documented spec target; it is not new API invention.

The user's stated goal: VG should become a linkable rendering-pipeline
SDK — an application writes `#include <vg/vg.h>`, links `libvg`, and
builds a real-time rasterizer against VG's own primitives, the way it
would against Vulkan or Metal directly. VG's intended position is an SDK
layer above the platform driver (comparable to MoltenVK/wgpu), not a
replacement for Metal/Vulkan as the hardware authority.

This session ran three parallel read-only audits (specs 03/04, specs
05/06, and current code) that independently converged on the same gap:

- The public C ABI (`include/vg/vg.h`, `src/api/vg_api.cpp`) exposes
  exactly 3 functions. Every other handle type `vg.h` already declares
  (`VgDevice`, `VgArena`, `VgAllocation`, `VgCodeObject`, `VgNode`,
  `VgTaskGraphBuilder`, `VgTaskGraph`, `VgExecutionEnvelope`, `VgTimeline`,
  `VgSubmission`, `VgCapture`) has zero entry points on `VgApi`.
- `run_raster_triangles` (`src/backends/metal/metal_device_hal.h/.mm`) is
  a real, hardware-verified textured-triangle rasterizer built on an
  actual `MTLRenderPipelineState`, but it is a method on the *concrete*
  `vg::metal::DeviceHal` class only. It is not declared on the abstract,
  cross-backend `hal::DeviceHal` interface (which has exactly `compile()`
  and `submit()`), and it is called only from
  `tests/vertical_slice/metal_task_timeline_test.cpp`. No
  `ExecutionPlan`/`TaskGraph`/`Submission` path reaches it.
- `core::TaskRecord` (`src/core/core.h`) is shaped purely for compute
  dispatch (`x`/`y`/`z` thread-group counts). There is no draw-shaped task
  (index count, topology, vertex/index buffer references).
- `core::PixelFormat` (`src/core/core.h:148`) has exactly two values,
  `RGBA8Unorm` and `R32Float`. There is no depth/stencil format, and
  `AttachmentFacetDesc` carries no depth fields.
- `compiler::PipelineKey`/`StateBlock` (`src/compiler/
  pipeline_classification.h`) — the classifier E013 validated — already
  has a `raster_state` field, but nothing has ever populated it with real
  depth-test/depth-write/compare-op state; it has only been fed
  compute-side constants so far.
- `compiler::compute_package.cpp`'s IR recognition is a regex over
  `load`/`store`/`atomic_add`/`publish`. There is no interpolate,
  clip-position, or fragment-output concept, and the one raster shader
  that exists (`raster_facet_metal_source()`) is a single hardcoded MSL
  string — no user-authored shader path exists.
- There is zero window/presentation implementation anywhere in `src/` —
  no `CAMetalLayer`, no drawable, no present call.
  `docs/vg-project/06-backend-macos-metal.md` §12 describes a
  `CAMetalLayer` token design that was never implemented.
- What is **not** missing: `CanonicalView`/`FacetPool`/
  `RepresentationEpoch`/`Arena::transform`/`Arena::consume` and the
  `VgFacetRef` index+generation capability-token model are real,
  Phase-C-verified infrastructure on real Metal hardware
  (`docs/reports/phase-c-gate.md`: `layer1-complete`) that the raster
  track builds on, not replaces.

`docs/vg-project/03-system-architecture.md` and `04-public-c-abi.md`
contain zero occurrences of "raster"/"render pass"/"draw"/"attachment"/
"depth"/"present"/"window" — not because these are excluded, but because
those two documents define a backend-agnostic compute submission skeleton
and never claimed to cover rasterization. `03-system-architecture.md` §2
already lists `vg_platform` (window/surface) as a future-scope component
row without elaboration, consistent with rasterization/presentation being
an expected later extension rather than an excluded concern.

## Decision

1. **This ADR opens a new roadmap phase, Phase F ("Raster SDK"), not
   present in `12-roadmap-and-risks.md`.** Unlike ADR-024/035/041/042,
   which corrected the literal text of an *existing* roadmap phase, Phase
   F has no prior text in that document — it is a new track the user has
   directed, built on the validated Phase A–E research core rather than
   replacing or reopening it. `12-roadmap-and-risks.md` is not rewritten;
   this ADR is the authorizing record.

2. **Public C ABI advances from v1.0 to v1.1.** `VgApi` gains entry points
   for Device/AddressDomain/Arena/Allocation/CodeObject/Node/
   TaskGraphBuilder/TaskGraph/ExecutionEnvelope/Timeline/Submission — the
   exact chain `04-public-c-abi.md` §17 already specifies — implemented as
   thin opaque-handle/result-code wrappers over the existing,
   already-hardware-verified C++ machinery (`core::Arena`,
   `hal::DeviceHal`, `ExecutionPlan`, `core::TaskGraph`). `vgGetApi`'s
   `requested_version` negotiation and the `VgStructHeader` extension-chain
   mechanism (§5–6) are the tools the spec already built for this step;
   v1.0 binaries keep working unmodified.

3. **Rasterization is added as an optional shape of Task/ExecutionPlan,
   not a parallel API.** `run_raster_triangles`'s already-verified Metal
   hardware path is moved — not rewritten — so that `compile()`/`submit()`
   recognize a raster pass and build a real `MTLRenderPipelineState`
   through it. A raster-shaped task record gains `index_count`/
   `topology`/`vertex_buffer_ref`/`index_buffer_ref` fields; the existing
   `Capability::Raster` bit (`device_hal.h`) is finally read by a real
   task type instead of sitting unused.

4. **User-authored vertex/fragment shaders enter through restricted
   import, not a new shading language.** Following
   `05-compiler-language-ir.md`'s already-specified restricted-interop
   layer, `CodeObject` accepts hand-written MSL source plus an explicit
   effect contract declaring which root schema it reads, which attachment
   it writes, which facet it samples. The compiler validates only the
   declared contract, not shader logic — this trust boundary is recorded
   as `HostAssisted`, per invariant 10 in `docs/START.md` §4 ("任何无法在
   当前硬件表达的语义必须返回 `Unsupported`、明确降级或进入 reference
   backend；不允许静默伪装"), never silently upgraded to a fully-verified
   status. Designing a new VG-native shading language is rejected (see
   Alternatives) — `12-roadmap-and-risks.md` §7 already names "自研语言
   吞噬项目" as a standing P2 risk.

5. **Depth becomes real, keyed pipeline state, reusing E013's classifier.**
   `PixelFormat` gains `Depth32Float`; depth-test-enable/depth-write-enable/
   compare-op populate the existing `PipelineKey::raster_state` field and
   compile into a real `MTLDepthStencilState`; viewport stays encode-time
   dynamic state, per the classifier's existing must-key/dynamic/
   plain-data split.

6. **The facet/capability-token model is not replaced.** Sample/Storage/
   Attachment access for the raster track continues through
   `core::FacetPool`/`VgFacetRef` (index+generation), per invariant 2 in
   `docs/START.md` §4 ("采样、attachment、storage、transfer 共享
   Region/Representation 语义，但允许不同 hardware facet"). No
   `MTLTexture` or other backend resource pointer is promoted to public
   API surface at any point in Phase F.

7. **Evidence policy is ADR-024/030/042 verbatim, continued.** Real Metal
   + reference (CPU) hardware results per milestone; Vulkan stays
   `compile-review-only`, never counted as passed, per the same
   permanent-host-constraint finding ADR-024 recorded. `HostAssisted`/
   `Unsupported`/`Deferred` remain legal, honestly labeled outcomes (e.g.
   the shader-import trust boundary in Decision 4).

8. **Windowing/presentation (`PlatformHAL`, `CAMetalLayer` bridging) is
   the one genuinely new subsystem** — `06-backend-macos-metal.md` §12
   describes it but nothing implements it. It lands last, after the
   offscreen and indexed-geometry milestones are real and verified, and
   offscreen conformance never gains a window dependency.

9. **Milestone breakdown and ordering are tracked outside this ADR**, in a
   plan document (mirroring the TASK-C1..C7 / TASK-D1..D7 precedent):
   F0 (this ADR) → F1 (C ABI expansion) → F2 (raster as a Task shape) →
   F3 (restricted shader import) → F4 (depth + real PSO) → F5 (index
   buffers) → F6 (per-frame SceneRoot) → F7 (host upload path) → F8
   (PlatformHAL/CAMetalLayer) → F9 (frames-in-flight via existing
   RepresentationEpoch backpressure) → F10 (BGRA8 drawable format). Each
   milestone gets its own implementation ADR and a real `dev-metal` ctest,
   per the project's standing discipline.

## Alternatives

- **Keep adding methods to the concrete `vg::metal::DeviceHal` class (as
  `run_raster_triangles` already does) instead of expanding the public C
  ABI**: rejected. No cross-backend or C-linkable application code can
  ever reach a method that isn't on the public API surface or the
  abstract `hal::DeviceHal` interface — that is exactly the gap the user
  is asking to close, not a way to close it.
- **Design a new, separate "VG2" raster API surface alongside the
  existing compute ABI**: rejected. Violates the project's standing
  don't-reinvent discipline (the same reasoning ADR-031 used to reuse
  `Arena`'s existing epoch/consume semantics rather than inventing new
  ones). The facet/Region/Task model exists precisely so rasterization is
  a domain extension of it, not a parallel system; `03-system-
  architecture.md` never described two systems.
- **Design and ship a VG-native shading language for vertex/fragment
  stages**: rejected. `12-roadmap-and-risks.md` §7 already lists "自研
  语言吞噬项目" as a named P2 risk with mitigation "IR/schema first，
  前端延后"; restricted MSL/SPIR-V import with an effect contract is the
  path `05-compiler-language-ir.md` §1's restricted-interop layer already
  designed for exactly this situation.
- **Wait for Vulkan hardware to become reachable before starting Phase F,
  so both backends carry raster evidence symmetrically from day one**:
  rejected, on the same permanent-host-constraint reasoning as ADR-024.
  Vulkan continues compile-review-only; this unblocks real Metal+
  reference progress instead of stalling on an unreachable dependency.
- **Treat this as reopening or amending ADR-042 in place**: rejected.
  ADR-042 was correct for what Research Alpha was — a recorded
  aggregation of Phases A–D that explicitly did not add new API surface.
  Phase F is a new, later decision triggered by ADR-042's own documented
  revisit condition, not a retraction of it. ADR-042's text is not
  rewritten.

## Consequences

The public C ABI stops being a 3-function skeleton and becomes the
application-linkable surface `04-public-c-abi.md` always specified,
versioned v1.1 with v1.0 compatibility preserved through `vgGetApi`
negotiation. Rasterization stops being a Metal-test-only capability and
becomes a first-class, `compile()`/`submit()`-reachable task shape,
without discarding any Phase C facet/representation infrastructure or the
project's evidence discipline. `12-roadmap-and-risks.md` gains an implicit
Phase F that a future reader finds only by following this ADR; a
`docs/reports/phase-f-gate.md` (mirroring phase-b/c/d/e-gate.md) should be
created once F-track milestones start landing, tracked as a follow-up
task rather than promised here. Every milestone ADR under Phase F (F1
onward) cites this ADR as its governance basis instead of re-litigating
the ABI-freeze question.

## Evidence

- `docs/vg-project/04-public-c-abi.md` §5, §6, §17 — version negotiation
  mechanism; explicit "first version may shrink the function set"
  permission; full target call chain.
- `docs/vg-project/03-system-architecture.md` §2, §4, §10 — component
  table incl. `vg_platform`; Region/CanonicalView/FacetRef
  capability-token design; zero raster/window content confirming
  out-of-original-scope, not excluded.
- `docs/vg-project/05-compiler-language-ir.md` §1 (restricted-interop
  import layer), `06-backend-macos-metal.md` §12 (`CAMetalLayer` token
  design, unimplemented).
- `docs/vg-project/12-roadmap-and-risks.md` §7 P2 row ("自研语言吞噬
  项目").
- `docs/START.md` §1, §4 (invariants 2, 8, 10), §6 (explicit "not a
  triangle demo" phase-one completion definition — the reason this gap
  exists by design, not oversight).
- `docs/decisions/ADR-024-...md`, `ADR-030-...md`, `ADR-042-...md` —
  evidence-policy precedent; ADR-042 Decision #4 and its Revisit trigger,
  the literal freeze this ADR reopens.
- `docs/reports/phase-c-gate.md` (`layer1-complete`) — the
  CanonicalView/FacetPool/facet-transform infrastructure this track
  builds on.
- Code audit citations: `include/vg/vg.h` (3-function `VgApi` table),
  `src/api/vg_api.cpp` (163 lines, 2 free functions),
  `src/backends/metal/metal_device_hal.h` (`run_raster_triangles` outside
  `hal::DeviceHal`), `src/core/core.h:148` (`PixelFormat`),
  `src/core/core.h` (`TaskRecord` x/y/z), `src/compiler/
  pipeline_classification.h` (`PipelineKey::raster_state`),
  `src/compiler/compute_package.cpp` (IR regex recognition).

## Revisit trigger

Revisit if the user changes the SDK-shape goal (for example, decides an
intermediate C++-only research API is sufficient and public C ABI
expansion should be deferred); if Vulkan hardware becomes reachable
(raster evidence policy would need the same re-evaluation ADR-024's own
revisit trigger names); if any Phase F milestone finds the facet/
capability-token model cannot represent a needed raster resource without
a public object escape (triggering the Phase C stop-check in
`12-roadmap-and-risks.md` §4: "若每次 facet 使用都产生昂贵对象/
descriptor update，重新评估 facet ABI/cache，而不是把 texture object
暴露回 public API"); or if restricted shader import proves unable to
carry a sound-enough effect contract for the raster domain (triggering a
redesign of F3, not silent relaxation of the contract).
