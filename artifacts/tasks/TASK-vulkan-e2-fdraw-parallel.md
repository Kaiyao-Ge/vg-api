# Vulkan E2 / F draw parallel execution freeze

> Execution result (2026-09-04): this freeze document records the original assignment. The previously pending paths are now integrated: formal built-in Raster, `vg.glsl.raster/v1`, direct/indexed Tier2, SceneRoot, and D32 depth all pass real llvmpipe execution under Khronos validation with zero VUID. See the corresponding `docs/reports/vulkan-*` completion reports.


Date: 2026-09-04. Work only in `/Users/gokyrie/projects/vg-api`. Baseline is HEAD `fe2b97ba0dce710f1b96f32a8ab0ab162adeb341` plus preserved uncommitted B/C/D/F/E1-preparation changes. Two GPT-5.6 Terra / medium subagents run E2 and F draw in parallel; coordinator owns shared integration.

## Dependencies and scope

E2 audits and, only where the existing contract permits, implements restricted user-raster import for Vulkan. The only current public restricted-raster format is `vg.msl.raster/v1`; its source and entry names are Metal Shading Language. Vulkan must not pass MSL to glslc, relabel it GLSL/SPIR-V, translate it heuristically, execute the built-in shader instead, or advertise `UserShaderImport`. If no backend-neutral or Vulkan source contract exists, E2's valid result is a precise semantic/format blocker plus enforced fail-closed tests and the exact ADR/API/schema requirements for a future implementation. E2 depends on E1 for any eventual draw execution but may audit/prepare in parallel.

F draw covers test-only draw-dependent physical experiments: GPU-authored `VkDrawIndirectCommand`, cull/compact feeding indirect draw without host count readback/re-encode, and preauthorized Tier2 bucketed draw batches. It must consume the formal E1 pipeline/descriptor contract, not D's legacy direct raster helper. Since E1 command encoding is not accepted, F draw may implement bounded command-generation/authorization components and CPU oracles, but real draw modes must fail closed until E1 exposes a usable formal pipeline. No production capability, DGC assumption, Tier3, new Task-ring discriminator, public ABI, presentation, or benchmark claim.

The packages can proceed in parallel because E2 owns shader-format trust and F owns indirect draw command generation. Their only intersections are coordinator-owned E1 pipeline access, DeviceState, CMake, runner/catalog and source contracts.

## Strict direct-edit inventory

E2 agent only:
- `src/backends/vulkan/vulkan_user_raster.h` and `.cpp` (new, only if existing contracts permit a meaningful component)
- `tests/vertical_slice/vulkan_user_raster_contract_test.cpp` (new)
- `docs/reports/vulkan-e2-user-raster.md` (new)
- `/tmp/vg-vulkan-e2-*`, `build/vulkan-e2-reference`

F draw agent only:
- `tests/support/vulkan_draw_experiments.h` and `.cpp` (new)
- `tests/vertical_slice/vulkan_draw_experiments_test.cpp` (new)
- `docs/reports/vulkan-f-draw-experiments.md` (new)
- `/tmp/vg-vulkan-f-draw-*`, `build/vulkan-f-draw-reference`

Coordinator only:
- all existing production, Core/HAL/IR/API/public files
- `src/backends/vulkan/vulkan_plan_raster.{h,cpp}`
- all CMake, runner/catalog and contract-test files
- this freeze and final integration report
- `/tmp/vg-vulkan-e2-fdraw-*`, `build/vulkan-e2-fdraw-reference`

Agents do not edit coordinator or other-agent files. Proposed shared changes go to unapplied `/tmp/vg-vulkan-{e2,f-draw}-integration.patch` with exact insertion points. No remote, install, commit, push, branch/worktree change.

## E2 acceptance

- Prove the actual loaded CodeObject format, parser and immutable `UserRasterShaderContract` fields; never infer portability from the generic C++ type name.
- Existing MSL contract must reject on Vulkan before pipeline creation/Commit with `Unsupported`, precise NodeRef/domain/format diagnostic, no partial effects, publication, timeline signal or Raster result.
- Do not set `Capability::UserShaderImport`; do not compile/execute built-in GLSL in place of caller source.
- If a future Vulkan restricted format is required, record the minimum superseding ADR, format tag, source kind, fixed bindings/root schema, entry contract, effect/trust classification and public version implications. Do not implement them without authority.
- CPU/source contract tests must be meaningful and registered separately from any GPU pass. No-device path is nonzero, never skipped/fallback.

## F draw acceptance

- Command records use exact Vulkan layouts: `VkDrawIndirectCommand` 16 bytes and indexed form 20 bytes; bounded count/stride/alignment and zero/overflow cases reject.
- Cull output feeds GPU-written indirect commands and then draw without host counter readback/re-encode; correct compute-write -> draw-indirect/vertex-input and attachment -> host barriers are specified.
- Tier2 uses complete preauthorized NodeRef/package handoff; bucket fallback is `EmulatedDevicePass`, unauthorized/stale/duplicate/out-of-range cases reject.
- Draw output must eventually compare nontrivial pixels/depth with Reference and report actual command buffers, encoders, barriers, draw counts, temporary bytes and waits.
- Until E1 formal pipeline is callable, real-device draw modes must explicitly fail; CPU command/oracle modes may be registered only as ordinary unit tests, never Vulkan passes.

## Ledger

| Package | Implementation | Local | Platform | Docs |
|---|---|---|---|---|
| E2 existing MSL-on-Vulkan contract | fail-closed audit and contract complete | CPU/source test passed | blocked by source-format contract and E1 | complete |
| F indirect/cull/Tier2 draw | command/authority preparation complete | CPU oracle passed | blocked by E1 and Linux GPU | complete |
| E2 Vulkan user import | not implemented; no authorized Vulkan format | not applicable | blocked | requirements recorded |
| F real draw execution | not implemented; GPU mode fails nonzero | not registered as pass | blocked | requirements recorded |
