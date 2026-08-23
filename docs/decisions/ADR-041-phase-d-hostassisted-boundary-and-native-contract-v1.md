# ADR-041: Phase D HostAssisted Boundary and NativeContractResearch v1

Status: Accepted

## Context

`docs/vg-project/12-roadmap-and-risks.md` §5 names Phase D's exit as
E004 / E010 / E011 / E014 / E017, break-even curves, a HostAssisted
boundary list, and NativeContractResearch v1. ADR-035 set the evidence
shape and forbade writing those two products before D2–D6 had honest
classifications. Those rows now exist:

- E004 revisit (ADR-036): seed-topology set can be smaller than Universe;
  walk is HostAssisted.
- E010 (ADR-038): bucket + per-Node indirect is EmulatedDevicePass, not ICB.
- E011 (ADR-037): working-set bytes are proxy; sparse is Unsupported.
- E014 (ADR-040): same-environment reference replay; Metal↔Vulkan is
  semantic correspondence only.
- E017 (ADR-039): overflow buffer + next submit is HostAssisted.

B-era `E004-access-certificate.json` remains historical (ADR-025).
Vulkan remains compile-review-only (ADR-024). Break-even curves were
named in the roadmap exit sentence; the fixtures D2–D6 actually ran are
too small to plot one.

## Decision

**1. Phase D research closure is recorded, not product-closed.**
The five experiments have honest Metal+reference (or reference-only)
classifications plus Vulkan compile-review-only rows. That satisfies
the experiment half of §5. Break-even / cost curves are recorded as
`unmeasured` (sample insufficient). Inventing points is forbidden.

**2. The HostAssisted boundary list is the adapter breakpoint list.**
See `docs/reports/host-assisted-boundary.md`. The five answers are:
discovery must return to the host; multi-node select must bucket
(EmulatedDevicePass); continuation must be a second submit; working
set is proxy-only; cross-backend capture is semantic correspondence.
`HostAssisted` is not rewritten as `Direct`.

**3. NativeContractResearch v1 is a research note.**
See `docs/reports/native-contract-research-v1.md`. It names the UMD /
KMD / firmware questions that would be required to remove those
assists. It is not a KMD start order and is not implemented on this
host (START.md §3).

**4. `vg-exp phase-d` maps the D revisit definitions.**
`E004` loads `E004-discovery-revisit.json` only. Missing ctests are
`missing`, not passed. Vulkan is a fixed compile-review-only sample
per experiment.

**5. Roadmap text.** §5's original exit sentence is not rewritten. A
Correction points here, as ADR-035 already did for evidence policy.

## Alternatives

- Close Phase D as if break-even curves had been measured: rejected —
  the fixtures are 4-node / 8-task / 16-byte / 3-task.
- Rewrite HostAssisted rows as Direct so the gate looks native:
  rejected — 01 §5.4 counts a negative result as success.
- Start NativeContractResearch by implementing a stub KMD: rejected —
  START.md §3 and this project's charter forbid it.
- Wait for Vulkan hardware before recording D: rejected — ADR-024/035.

## Consequences

Phase E does not start from this ADR. Another reader can take the two
reports plus the five D definitions and say where VG stops on existing
APIs and which native-contract questions remain. `docs/reports/phase-d-gate.md`
/ `.json` are the machine- and human-readable gate records.

## Evidence

- `docs/reports/host-assisted-boundary.md`
- `docs/reports/native-contract-research-v1.md`
- `docs/reports/phase-d-gate.md` / `.json`
- `tools/vg-exp/vg_exp.py` `phase-d`
- TASK-D7; ADR-035 through ADR-040

## Revisit trigger

Revisit if a measured break-even curve exists, if Vulkan becomes
executable, or if any assist listed in the boundary document is
removed by a real native contract (not a host walk relabeled).
