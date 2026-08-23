# ADR-036: DiscoverThenLease Walks a Seed Topology Smaller than Universe

Status: Accepted

## Context

ADR-025 implemented E004's three real AccessCertificate modes. On this
host's unified-memory Metal/reference model, `DiscoverThenLease`
degenerated to a timed host full-arena scan -- the same set as
`Universe`. ADR-025's Revisit trigger and ADR-035 keep that B-era row
as historical evidence: overwriting
`experiments/definitions/E004-access-certificate.json` would erase an
honest HostAssisted conclusion.

`02-principles-and-semantics.md` §7.2 asks for a side-effect-free
discovery Node that walks an already-resident seed topology, emits a
reachable-granule witness, then compresses that into a certificate and
lease, with `GraphEpoch` and address-selection inputs frozen. Phase D
(TASK-D2) is that revisit. The walk must be able to produce a set
strictly smaller than Universe on the same Arena.

A GPU-side compact of the reachable set that continues in the same
submit would be `DevicePass`. This milestone's default path is a host
walk and a host round-trip.

## Decision

**New core entry points, not a rewrite of `build_access_certificate`.**
That function keeps B-era `DiscoverThenLease` semantics (full Active
scan + `discovery_host_ns`) for existing callers. D2 adds
`core::discover_reachable`, `core::build_discovered_certificate`, and
`core::certificate_covers_discovery_witness`.

**Seeds live on `ExecutionPlan::discovery_seeds`.** Default empty means
no discovery stage, so every pre-D2 caller -- including B-era
`DiscoverThenLease` -- is unchanged. Seeds are caller-supplied
`PointerRef`s. The walk freezes `topology_epoch` at start and refuses
if it changes mid-walk (02 §7.2).

**The walk follows 12-byte refs stored in allocation bytes**, packed
the same way `load_ref` does (`{u64 allocation, u32 generation}`, not
`sizeof(PointerRef)` which may pad). Only well-formed refs that resolve
to Active allocations are followed. Discovery does not write business
data. Result = seeds + reachable.

**Certificate covers witness.** A forged extra allocation in the
witness, or a `WorkingSetLease` naming a ref absent from the discovered
set, is a refuse (`lease.add(ref, discovered, ...)` is the fill path).
`SoftwarePaged` / `FaultManaged` stay Unsupported.

**Classification is HostAssisted, never DevicePass.** The host walks
the bytes and the reachable set comes back to the host before a later
submit. This is a semantic reachable set / proxy working set, not an OS
page-migration claim (06 §10). Same-submit GPU compact is optional
future work, recorded only if it actually lands.

**B's E004 definition is not rewritten.** D2 adds
`experiments/definitions/E004-discovery-revisit.json` (id still
`"E004"`, name `access-certificate-discovery-revisit`). Vulkan remains
compile-review-only (ADR-024 / ADR-035).

## Alternatives

- Change `build_access_certificate(DiscoverThenLease)` to walk seeds:
  rejected -- that would silently change B-era callers and overwrite
  the historical full-arena result.
- Classify the host walk `DevicePass`: rejected -- there is no GPU
  discovery kernel and no same-submit compact.
- Invent a "recently touched" subset without walking stored refs:
  rejected -- 02 §7.2 and ADR-025 forbid a fabricated working set.

## Consequences

Reference `submit()` calls `hal::run_discovery_stage` when seeds are
set. Metal `.mm` is owned by another stream; the one-line hook is
documented on `run_discovery_stage`. Unit and Metal+reference tests
prove discovered < Universe on a 4-node chain.

## Evidence

- `src/core/core.h` / `.cpp` — `discover_reachable`,
  `build_discovered_certificate`
- `src/backends/discovery_stage.cpp` — `run_discovery_stage`
- `tests/unit/discovery_test.cpp`,
  `tests/vertical_slice/metal_discovery_test.cpp`
- `experiments/definitions/E004-discovery-revisit.json`

## Revisit trigger

Revisit if a discrete-GPU Metal target (or reachable Vulkan hardware)
makes a GPU-resident subset observable as something other than a
software reachability set, or if a same-submit GPU compact path lands
and can honestly be classified `DevicePass`.
