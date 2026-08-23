# ADR-040: Capture View Report and Cross-Backend Semantic Scope

Status: Accepted

## Context

E014 asks whether a canonical capture can locate the same program and replay
it in a compatible environment: a viewable report, same-environment
deterministic hash match, and an explicit refusal when required capabilities
are missing. ADR-011 already shipped capture v1 (`vg.capture/v1`,
`schema_version` 2) with stable allocation IDs, IR hash, representation epoch,
fault/poison metadata, and reference replay. ConsumeInput snapshots remain
unrestorable and must keep the exact error `cannot restore a consumed
representation`.

Metal↔Vulkan "replay" is a catalog variant. This host cannot execute Vulkan
(ADR-024 / ADR-035). Treating an adapter re-execution, or a reference replay
of a Metal capture, as a cross-backend success would invent execution
evidence. TASK-D2's discovery pass is not available, so a dynamic-graph
capture would also have to invent data.

## Decision

**1. Schema stays `vg.capture/v1` / `schema_version` 2.** View and portability
metadata are optional fields (`source_backend`, `required_capabilities`,
`executed_backends`, `semantic_correspondence`). They are omitted when empty
so existing documents keep the same content hash. Unknown *required* fields
are still refused without repair (ADR-011). A schema major bump (v2 / version
3) is reserved for a breaking required-field change.

**2. `vg-capture-view` is a report, not a GUI.** It reads a capture and writes
markdown or JSON listing allocation id, generation, `representation_epoch`,
size, stored byte count, and fault code/message. It never prints GPU pointers
or device addresses (no `0x` address patterns). Dynamic-graph content is
reported as `blocked` until a discovery API exists; the tool does not invent
graph nodes.

**3. Same-environment replay is the only execution claim.** serialize →
deserialize → `replay()` on cpu-reference must keep `capture_hash` and
`ir_hash` stable across two serializations. Fault captures still replay as
`PartiallyProduced` / `Poisoned`. A consumed empty-bytes snapshot
(`size > 0`, `bytes: []`) remains readable so the view tool can list it,
but `replay()` still returns false with `cannot restore a consumed
representation`. Missing `bytes` on a legacy document still means
zero-initialized payload (ADR-011), not consumed.

**4. Incompatible capabilities are refused.** Optional
`required_capabilities` is checked against a `ReplayEnvironment`. The default
environment is the cpu-reference capability set. A missing capability fails
closed with `incompatible capabilities refused: <name>`.

**5. Metal↔Vulkan is semantic mapping only.** A report may compare stable
allocation IDs, IR hash, representation epoch, and fault taxonomy, and may
name a `semantic_counterpart`. `executed_backends` records what actually ran.
Writing or viewing a capture that lists both Metal and Vulkan as executed is
refused (`cannot claim Metal and Vulkan both executed`). A driver-upgrade
variant is skipped unless a second driver is present.

## Alternatives

- Bump capture schema to v2 for view metadata: rejected — every field is
  optional and old documents remain readable.
- Treat reference replay of a Metal capture as Metal↔Vulkan success:
  rejected — that is one backend executing, not two.
- Invent dynamic-graph rows from `graph_references` alone: rejected — those
  are sealed GraphEpoch pointers, not a discovery pass (TASK-D2).

## Consequences

- E014's same-environment compute-exact and fault rows are closable without
  a schema major version.
- Cross-backend evidence stays honest: semantic correspondence is labeled
  `not executed`.
- Phase D runners must not promote a view report into a dual-execution gate.

## Evidence

- `src/capture/capture.h` / `.cpp` — optional view fields, capability
  refusal, `write_view`
- `tools/vg-capture-view/vg_capture_view.cpp`
- `tests/unit/capture_view_test.cpp`
- `experiments/definitions/E014-capture-replay.json`
- ctest `capture.view`, `capture.view.cli`

## Revisit trigger

Revisit if a required capture field must change (then bump to schema v2 and
`schema_version` 3), if Vulkan hardware becomes reachable for a real second
execution, or if TASK-D2 ships a discovery API that can populate a
dynamic-graph snapshot without invention.
