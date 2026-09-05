# Vulkan D/F parallel execution freeze

> Execution result (2026-09-04): this freeze document records the original assignment. D/F and their dependent formal Raster/Tier2 work are now integrated. Linux llvmpipe passes every registered Vulkan test under Khronos validation with zero VUID; see `docs/reports/vulkan-df-integration.md`.


Date: 2026-09-04. Work only in /Users/gokyrie/projects/vg-api.
Baseline HEAD fe2b97ba0dce710f1b96f32a8ab0ab162adeb341 plus preserved, uncommitted
B/C integration. A user accepted; B/C local verified, Linux device gates pending.
User requested two fresh GPT-5.6 Terra / medium subagents for D and F.

## Scope and dependencies

D: existing Sample/Storage/checked generation physical mechanisms, facet cache,
representation and ConsumeInput. Formal transforms must use Core assembler ->
compile -> submit; harness only supplies physical observations. Existing narrow
legacy facet raster and pipeline-classification harness calls receive coverage,
without opening production Raster or changing current vertex/depth/SceneRoot.
F: compute-only narrow Tier1 dispatch-indirect, GPU cull/compact, indexed-address
table and preauthorized Tier2 compute selection. Match current Metal experiment
level, not production TaskGraph capability. Draw/facet-dependent F extensions
remain dependent on E1/D platform acceptance and are excluded from this slice.
D and F-compute depend on accepted A, not B/C platform closure. They may develop
in parallel. No E1/E2 implementation or dependent capability advertisement.

Basis: original approved Vulkan A-F scope in task history, remediation report
sections 9.2.6-12 and 10.1, START invariants 3-10, ADR-053/054, current Phase C/D
and Metal physical harness contracts. Read applicable original ADRs before edits.
No public ABI/core authority changes, HAL re-derivation, synthetic sealed plans,
profiling/residency subsystem, Tier3, new execution domains or GPU fallback.
No remote connection, installation, commit, push, branch/worktree change.

## Strict direct-edit inventory

D agent only:
- tests/support/vulkan_adapter_harness.cpp (existing four physical methods;
  necessary test observation methods allowed with coordinator header integration)
- tests/vertical_slice/vulkan_facet_representation_test.cpp (new)
- docs/reports/vulkan-d-facet-representation.md (new)
- exclusive build/vulkan-d-reference and /tmp/vg-vulkan-d-* scratch

F agent only:
- tests/support/vulkan_gpu_experiments.h (new result/config types)
- tests/support/vulkan_tier2_harness.cpp (new AdapterHarness compute-experiment
  method definitions using existing production physical helpers)
- tests/vertical_slice/vulkan_gpu_experiments_test.cpp (new)
- docs/reports/vulkan-f-gpu-experiments.md (new)
- exclusive build/vulkan-f-reference and /tmp/vg-vulkan-f-* scratch

Coordinator only:
- tests/support/vulkan_adapter_harness.h (shared declarations)
- src/backends/vulkan/{vulkan_resources.cpp,vulkan_encoding.cpp,
  vulkan_pipelines.cpp,vulkan_commit.cpp,vulkan_lowering.cpp} only D/F proven fixes
- src/compiler/{compute_codegen.cpp,shaders/facet.cpp,shaders/cull_compact.cpp}
  only proven existing-contract fixes; no new production experimental API
- cmake/tests.cmake, cmake/g4-vulkan-tests.cmake
- tests/vertical_slice/vulkan_capability_contract_test.py
- this freeze and docs/reports/vulkan-df-integration.md
- tools/vg-exp/phase_catalog.json, tools/vg-exp/vg_exp.py,
  tests/tools/test_phase_runner_contract.py only required explicit runner coverage
- exclusive build/vulkan-df-reference and /tmp/vg-vulkan-df-* scratch

Agents send exact shared-file changes as unapplied /tmp/vg-vulkan-{d,f}-integration.patch
or declaration text; coordinator reviews and applies sequentially. Never modify
another owner's files/build. Scope additions require coordinator decision first.

## Frozen tests and acceptance

D proposed five real-device CTests:
- vertical-slice.vulkan.facets (Sample/Storage image and linear; checked generation)
- vertical-slice.vulkan.representation (assembler path, epoch/cache, post-transform observation)
- vertical-slice.vulkan.consume-input (assembler path, old backing reclaimed, negatives/cleanup)
- vertical-slice.vulkan.facet-raster (existing narrow physical harness only)
- vertical-slice.vulkan.pipeline-classification (existing narrow physical harness only)

F proposed four real-device CTests:
- vertical-slice.vulkan.indirect
- vertical-slice.vulkan.cull-compact
- vertical-slice.vulkan.indexed-address
- vertical-slice.vulkan.tier2

Reference baseline39, Metal baseline73 unchanged unless shared source adds a
necessary test. Linux expected51 after B/C -> expected60 after these nine,
subject to actual Linux configure/CTest inventory. Arithmetic is not evidence.
All GPU tests require real Vulkan, no skip/no-device/Unsupported success. CPU-only
fixture/oracle modes must be separately named, unregistered as Vulkan passes.

D DoD: meaningful output vs current Reference sampling/storage/raster oracle;
cache/epoch and stale tokens; shader checked-generation evidence (host stale
rejection not mislabeled GPU guard); representation-only formal path observable;
ConsumeInput releases both host and old Vulkan backing with explicit physical
observation, retains new facet, failures release holds and avoid partial effects.
Reject unsupported formats/profiles truthfully. Report descriptor/host/device costs.

F DoD: actual Vulkan commands for each claimed GPU path, independent CPU oracle,
nontrivial output, empty/mixed/tail/invalid authorization cases, bounded indirect
sizes, correct producer->indirect/compute->host synchronization, lifetime cleanup.
Tier2 may be bucketed EmulatedDevicePass, must disclose host preprocessing and
actual passes; must not claim unrestricted device-selected PSO or public capability.
No pretending host-written results are GPU execution. Source stays BUILD_TESTING only.

Coordinator validates inventory/ownership, reviews all code and meaningful CPU
fixtures, runs integrated Reference39 and static contracts, adds runner wiring
where applicable. Shared compiler edits require relevant Metal regression.
Linux SDK/glslc/real-device validation remains a separate pending gate; no full
D/F completion claim until required platform evidence exists for final source.

## Ledger

| Package | Implementation | Local | Platform | Docs |
|---|---|---|---|---|
| D | complete for frozen source scope | passed | pending Linux SDK/GPU | complete |
| F compute slice | complete for frozen source scope | passed | pending Linux SDK/GPU | complete |
| F draw-dependent extensions | deferred to E1 dependency | not run | pending E1 | scope recorded |

## Coordinator inventory clarification

Review found the legacy physical raster helper gated on production Raster,
which intentionally remains unadvertised. Coordinator adds
src/backends/vulkan/vulkan_raster.cpp to its exclusive inventory solely to
check actual enabled dynamicRendering, sync2 and graphics queue for physical
clear/draw. This restores the existing narrow harness without enabling
ExecutionPlan Raster, changing the legacy vertex shader contract, or doing E1.
