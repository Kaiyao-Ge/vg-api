# ADR-045: F1 — Execution Result Reachable Through the Public C ABI (v1.1 → v1.2)

Status: Accepted

## Context

`docs/reports/phase-b-gate.md`'s Q1 review flagged a gap left open by
ADR-044 (v1.1): `submit()`'s public-ABI wrapper
(`src/api/vg_api_execution.cpp`) called `device->hal->submit(...)` and kept
only `wrapper->submission.report` (the lowering report, surfaced by
`getSubmissionLoweringReport`). `wrapper->submission.result` --
`core::ExecutionResult`, the actual outcome of running the task graph
(`ok`, `poison`, `message`, `fault`, `witness`, `missing_effects`,
`outputs_valid`) -- was discarded entirely. `submit()` returning
`VG_SUCCESS` therefore meant only "the submission mechanism accepted the
plan," never "the execution actually succeeded": a fault or poison outcome
was unreachable through the v1.1 public ABI. `tests/api/
vg_e002_pointer_graph_abi_test.cpp` had explicitly documented this as a
known, disclosed gap (its internal-experiment counterpart,
`run_pointer_graph()`, asserts `result.ok` directly; the ABI test could not).

ADR-044's Revisit trigger names exactly this case -- "a later Phase F
milestone needs a `VgApi` v1.2 growth" -- and states its version-negotiation
contract (append-only, `size`-gated) is "the mechanism to reuse, not
redesign." This ADR reuses it.

## Decision

Add one new v1.2 entry point, `getSubmissionExecutionResult`, following
`getSubmissionLoweringReport`'s exact shape and lifetime contract.

**`core::ExecutionResult::canonical_json()`** (`src/core/core.h`/`.cpp`) is
a new method, implemented the same way `hal::LoweringReport::canonical_json()`
already is: a `json::Value::Object` (auto-key-sorted by `json::Value::Object
= std::map<std::string, Value>`) serialized via `json::canonical()`. It
covers `ok`, `poison`, `message`, `trace` (raw `effect_array_json`, each
entry using `fault.effect`'s same access/allocation/offset/
representation_epoch/size encoding), `fault` (`code`, `effect`,
`instruction_index`, `message`, `task_index`), `witness` (raw `entries()`,
each `{effect, instruction_index}`), `missing_effects`, and `outputs_valid`.
`ir::effect_json` (`ir.cpp`) is anonymous-namespace-local to that
translation unit and not reusable from `core.cpp`, so a small local
duplicate, `effect_result_json`/`effect_array_json`, mirrors its exact key
set and access-name mapping instead.

**Scope is deliberately narrow**: only raw `witness.entries()` are
serialized, not `AccessWitness::diff()` output -- a `Certificate` is not
available from `ExecutionResult` alone, and reconstructing one would be
speculative. `submit()`'s return-code semantics are unchanged: it still
returns `VG_SUCCESS` when the submission mechanism accepts the plan, even if
`result.ok` is `false` (a fault/poison outcome). This ADR only makes that
truth queryable, not a different return code -- changing `VG_SUCCESS`'s
meaning would be a breaking change to every existing v1.0/v1.1 caller's
error-handling assumptions, which is out of scope for an additive ABI
growth.

**`VgSubmission_T`** (`src/api/vg_api_internal.h`) gains a second string
field, `execution_result_json`, populated in `submit()`
(`src/api/vg_api_execution.cpp`) right after `lowering_json`, from
`wrapper->submission.result.canonical_json()`.

**`vgGetApi` version negotiation** extends to a third tier, following
ADR-044's exact pattern: `v1_1_size = offsetof(VgApi,
getSubmissionExecutionResult)` (valid because that member is the new last
field in `VgApi`), `at_least_v1_2 = (requested_version ==
VG_API_VERSION_1_2)`, and `full_size` selects `sizeof(VgApi)` only for a
v1.2 request. `VG_API_VERSION_1_2 = 0x00010002u` is added to
`include/vg/vg_version.h`. Every member before `getSubmissionExecutionResult`
keeps its exact v1.1 offset and meaning -- strictly append-only, same
discipline as the v1.0/v1.1 boundary.

## Alternatives

- **Change `submit()`'s return code to reflect `result.ok`**: rejected --
  `VG_SUCCESS` has meant "submission mechanism accepted the plan" since
  v1.0; redefining it to also encode execution outcome would be a silent
  breaking change for every existing caller that treats `VG_SUCCESS` as
  "nothing more to check here." A new accessor is additive; a redefined
  return code is not.
- **Mirror `ExecutionResult` into a public C struct** instead of JSON:
  rejected for the same reason ADR-044 rejected it for `LoweringReport` --
  `witness`/`missing_effects`/`trace` are variable-length, and `vg.h` has no
  existing variable-length-array ABI pattern to reuse.
- **Surface `AccessWitness::diff()` (missing/unused ranges against a
  certificate)**: rejected -- no `Certificate` is reachable from
  `ExecutionResult` alone without reconstructing one speculatively from
  envelope state that has already been consumed by `compile()`/`submit()`;
  raw witness entries are the actual, honest data available at this layer.
- **Reuse `ir::effect_json` by giving it external linkage**: considered --
  rejected as broader-than-needed churn to `ir.cpp`'s public surface for one
  call site; a small local duplicate in `core.cpp`, documented as
  intentionally mirroring its key set, is a proportionate, self-contained
  fix.

## Consequences

- `VgApi` is now 0x00010002 (`VG_API_VERSION_1_2`); `vgGetApi(VG_API_VERSION_1_0
  | VG_API_VERSION_1_1, ...)` remain supported and byte-identical to their
  pre-v1.2 contracts.
- `tests/api/vg_c_abi_conformance_test.cpp`'s v1.1 `api.size == sizeof(VgApi)`
  assertion had to be repinned to `api.size == offsetof(VgApi,
  getSubmissionExecutionResult)` -- the same mechanical consequence of
  additive growth ADR-044 already documented for the v1.0 boundary, now
  recurring at the v1.1 boundary.
- `tests/api/vg_e002_pointer_graph_abi_test.cpp` now requests
  `VG_API_VERSION_1_2` and checks `getSubmissionExecutionResult`'s
  `"ok":1`, closing the exact gap its own comments had disclosed since
  ADR-044. Target-byte read-back remains a disclosed, unclosed gap (no
  host-visible-memory read-back entry point exists in `vg.h` at all).
- Callers that only need "did submission succeed as a mechanism" are
  unaffected; callers that need "did the graph actually execute without
  fault/poison" now have a reachable, canonical answer.

## Evidence

- `build/dev-metal` (Metal on): full build clean (102/102 targets, zero
  errors); all 54 ctests pass, including `api.c-abi-conformance` (repinned
  v1.1 boundary check) and `e002-pointer-graph-via-abi` (now requesting
  v1.2 and asserting `result.ok` on real Metal hardware).

## Revisit trigger

Revisit when `AccessWitness::diff()`/certificate-relative witness reporting
is needed through the public ABI (would require either reconstructing or
threading a `Certificate` through to `ExecutionResult`), when a
host-visible-memory read-back entry point is added (closing E002's
remaining disclosed gap), or when a later Phase F milestone needs a `VgApi`
v1.3 growth -- at which point this ADR's and ADR-044's shared
version-negotiation contract (append-only, `size`-gated) remains the
mechanism to reuse.
