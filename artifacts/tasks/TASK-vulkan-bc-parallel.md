# Vulkan B/C parallel execution freeze

> Execution result (2026-09-04): this document records the original assignment. B/C are now integrated and pass the final 68/68 Linux suite, including 29/29 Vulkan tests on llvmpipe under Khronos validation with zero VUID. See `docs/reports/vulkan-bc-integration.md`.

Date: 2026-09-04. Workspace: /Users/gokyrie/projects/vg-api.
Baseline: fe2b97ba0dce710f1b96f32a8ab0ab162adeb341; initially clean.
A is accepted as complete by the user; this record does not claim a fresh A run.
User authorized two parallel subagents, GPT-5.6 Terra, medium reasoning.
No branch change, worktree creation, commit, push, remote connection, dependency
installation, repository upload/indexing, or further delegation is authorized.

## Contract

Only catch up with currently implemented Reference/Metal contracts. B is the
restricted declared-edge CachedObject pointer lowering, not arbitrary device
pointer chasing. C consumes Core-sealed access/discovery/budget/lease facts;
no HAL certificate, touched-set, topology-walk or schedule re-derivation.
Stage 0-5 assembler and Stage 6/7 ownership, complete NodeRef, lifetime,
representation, Timeline and compute-only ring remain unchanged.
Raster, indirect tiers, SoftwarePaged/FaultManaged and public ABI expansion are
excluded. Basis: remediation report 9.2.6-12, 10.1; START invariants 3-10;
ADR-028, ADR-036/037, ADR-053/054.

## Exclusive direct-edit inventory

B agent:
- tests/vertical_slice/vulkan_pointer_graph_test.cpp (new)
- tests/api/vg_vulkan_pointer_graph_abi_test.cpp (new)
- tests/unit/compute_package_test.cpp (only B coverage if needed)
- docs/reports/vulkan-b-pointer-graph.md (new)
- exclusive build/vulkan-b-reference; /tmp/vg-vulkan-b-* scratch

C agent:
- tests/vertical_slice/vulkan_discovery_working_set_test.cpp (new)
- docs/reports/vulkan-c-discovery-working-set.md (new)
- src/backends/working_set_stage.cpp (only necessary neutral reporting correction;
  notify coordinator before behavior changes)
- exclusive build/vulkan-c-reference; /tmp/vg-vulkan-c-* scratch

Coordinator alone integrates production patches to:
- src/backends/vulkan/vulkan_lowering.cpp
- src/backends/vulkan/vulkan_commit.cpp
- compiler/compute_package.cpp and compute_codegen.cpp only if B proves need
- cmake/tests.cmake
- tests/vertical_slice/vulkan_capability_contract_test.py
- this freeze and docs/reports/vulkan-bc-integration.md (new)
- exclusive build/vulkan-bc-reference, build/vulkan-bc-metal and OFF directories

Agents deliver unapplied /tmp/vg-vulkan-{b,c}-integration.patch; coordinator
reviews and applies sequentially. Additional file ownership requires an explicit
coordinator decision before edits; no overlapping whole-file writes.

## Acceptance inventory

B: actual assembler-driven Vulkan vs Reference bytes; nonzero store; multi-Node
and repeat/cache; immutable package tamper refusal before effects; pointer width,
alignment, stale and mismatch negatives at the owning boundary; Vulkan-selected
public C ABI without fallback; CachedObject disclosure. Retain A GLSL int64 fix.

C: CertifiedPinned/DiscoverThenLease/Universe; reachable subset; frozen witness
and HostAssisted report; exact/within budget and lease-only success; over-budget,
invalid lease and changed topology rejected before effects; lifetime release.
Start with linear Compute independent of B, then add pointer+Discovery integration.
Sparse unsupported reporting must not reject ordinary budget consumption.

Exactly four proposed new GPU CTests:
- vertical-slice.vulkan.pointer-graph
- api.vulkan-pointer-graph
- vertical-slice.vulkan.discovery
- vertical-slice.vulkan.working-set

Reference baseline39 and Metal73 unchanged; Linux baseline47 -> expected51.
Confirm actual registration by CTest JSON; do not treat arithmetic as a run.
No-device, unsupported, missing, skipped or inner-skip must not count as passing
these required tests. No synthetic sealed plans or fake GPU fallback.

Coordinator runs integrated Reference/compiler/source checks and relevant Metal
regression when shared code changes. Linux build and actual GPU execution remain
separate gates pending authorized execution/logs for the exact final source.
No network access is inferred from the user's acceptance of A.
Static gates: no first-Node projection, HAL graph rebuilding, tests dependency in
production, or unrelated capability advertising. Preserve existing tests except
explicit B/C Unsupported assertions replaced with the new contract checks.

## Ledger

| Work | Owner | Implementation | Local validation | Linux device | Documentation |
|---|---|---|---|---|---|
| B | vulkan_b + coordinator integration | integrated | passed; see report | pending | complete |
| C | vulkan_c + coordinator integration | integrated | passed; see report | pending | complete |
| B+C integration/review | coordinator | integrated | Reference 39/39; static/CPU passed | pending | integration report |

A dependent package is not unlocked by partial completion. Final report must
list exact files, inventory disposition, commands/counts, static results and
unverified platform gaps. No full completion claim without all required levels.

## Coordinator boundary clarification

ADR-028 explicitly elides GPU load_ref and retains the loaded-byte mismatch
check only in Reference. Core topology epoch is not the allocation content
epoch. B catches up with current Metal CachedObject behavior; it does not add
shared pointer-byte snapshot validation. A mismatched loaded pointer is recorded
as this existing Reference/static-GPU boundary, not claimed as a new Vulkan check.
B negatives still cover declaration, package width/alignment and stale identity.
Stage6 reports static declared-edge targets and the absence of GPU load_ref-byte
checks; actual pipeline creation/cache reuse remains separately classified.

Coordinator has integrated B/C production changes and the four CMake entries.
The revised source-contract check and non-VG_HAS_VULKAN syntax checks pass.
These are local static evidence only. Child tests received final review and
CPU semantic validation; initial fixture defects were corrected. Coordinator
strengthened B output sentinels and diagnostics after agent handoff. See
docs/reports/vulkan-bc-integration.md for the exact remaining platform gate.
