# ADR-037: Working-Set Budget -- This-Submit Residency, Hard Refuse

Status: Accepted

## Context

E011 asks whether the address graph and "how much is resident this submit"
stay separable under working-set pressure (`09-experiment-catalog.md` E011;
START.md invariant 10). ADR-035 already put `WorkingSetBudget` /
`WorkingSetLease` on `ExecutionPlan` as unset-by-default optionals;
`validate()` rejects `lease.byte_limit > budget.byte_limit`. `submit()`
did not yet consume the budget, so a set limit could be ignored and
unified memory treated as infinite.

Metal has no public OS working-set counter this adapter can treat as
truth (06 §10). Vulkan sparse binding is explicit map/unmap, not
automatic page fault (07 §13). `max_in_flight_representations` is E016
(representation versions), not E011 residency.

## Decision

**`apply_working_set_budget` is the single submit-time check.** Requested
bytes are:

- lease set: sum of leased `allocation->size` (arena lookup; missing or
  stale is a refuse);
- budget set without a lease: Universe -- every Active `allocation->size`.

A set budget that does not `allows(requested)` fails with
`"working-set budget exceeded"`. Never silent clamp. Unset budget and
unset lease is a no-op.

**Report three named events**, plus sparse: `working_set_requested`,
`working_set_committed`, `working_set_proxy`. Reasons say `proxy` because
the figures are allocation-size stand-ins, not an OS residency counter.
Metal sparse heap/texture is `Unsupported`. Vulkan records the same
Unsupported classification as compile-review-only; no sparse runtime is
added.

**Discovery-lease uses `core::discover_reachable`.** The experiment fills
`WorkingSetLease` from that reachable set. A fake subset is not invented.

Core default is hard refuse. Application drop/quality stays a test-fixture
policy, not an adapter auto-strategy.

## Alternatives

- Soft-clamp requested bytes to the budget and succeed: rejected -- E011
  and ADR-035 require a predictable refuse.
- Treat unified memory as infinite / skip the check on Apple Silicon:
  rejected -- 06 §4 / 09 E011.
- Reuse `max_in_flight_representations` as the working-set knob: rejected
  -- that budget is representation-version cardinality (E016).
- Implement Metal/Vulkan sparse runtime to "have a real counter": rejected
  -- out of scope; honest Unsupported is the contract.

## Consequences

Reference `submit()` calls the helper after `graph_epoch_matches`. Metal
uses the same helper (hooked at the same point). Vulkan stays
compile-review-only for sparse. E011 can record per-platform policy
without claiming an OS residency counter or automatic fault.

## Evidence

- `src/backends/working_set_stage.cpp` (`apply_working_set_budget`)
- `tests/unit/working_set_test.cpp`
- `tests/vertical_slice/metal_working_set_test.cpp`
- `experiments/definitions/E011-residency-working-set.json`
- TASK-D3, ADR-035

## Revisit trigger

Revisit if a discrete-GPU Metal target exposes a real residency counter
distinct from allocation-size proxy, or if Vulkan hardware becomes
reachable and a genuine sparse-bind experiment is in scope.
