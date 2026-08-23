# ADR-039: Envelope Overflow Buffer + Next Submit (E017)

Status: Accepted

## Context

E017 asks which portable continuation to pick when a submit would publish
more tasks than the envelope allows (`09-experiment-catalog.md` E017;
TASK-D5). ADR-010's `TaskGraphBuilder::set_quota` refuses at graph-build
time and is not a continuation. ADR-035 already defined `EnvelopeOverflow`
(`None` / `Rejected` / `Deferred`) and the unset-by-default
`ExecutionPlan::pending_overflow` / `Submission::envelope_overflow`
fields; `submit()` did not populate them.

This host has no DelegatedEnvelope hardware. Firmware envelope extension
is simulation-only research and must not be written as a DevicePass.
Publication-ring exhaustion (`"publication ring quota overflow"`) is a
different failure: that ring cannot accept one more write. Envelope
overflow is "this commit's authorization is used up".

## Decision

**Portable mechanism: overflow buffer + next submit**, host-split on
`TaskGraph::deterministic_order`. Classified `HostAssisted` because the
host slices the sealed graph. There is no DelegatedEnvelope and no
firmware enlarge.

**`ExecutionPlan::envelope_task_quota`**: `std::optional<uint32_t>`.
Unset means no envelope cap (every pre-D5 caller). A set value of 0 is a
cap of zero, not "unset". Distinct from ADR-010 `set_quota`.

**First submit over the cap**: publish the first N tasks in deterministic
order, mint a non-zero token into `core::EnvelopeContinuationTable`
(owned by `DeviceHal`), and set `Submission::envelope_overflow` to a
valid `Deferred` record (`overflow_task_count`, `continuation_token != 0`).
The submit itself succeeds; leftover is explicit, not a silent quota
increase.

**Next submit**: the caller must copy that record onto
`plan.pending_overflow`. The helper requires the token to match the
device table, publishes **only** the leftover, and clears the leftover.
A larger `envelope_task_quota` on the continuation submit does not
republish the prefix.

**Without the token**: leftover stays in the table. A second submit of
the same plan without `pending_overflow` is a new envelope (it may mint a
new token for a new leftover) and must not drain the first leftover.

**Rejected**: a `Rejected` pending record is a hard refuse
(`"envelope leftover was rejected"`). `EnvelopeOverflow::continued()`
stays false (D1). Bad tokens refuse with
`"envelope continuation token does not match"`.

**Errors**: envelope strings are `"envelope task quota exceeded"` /
`"leftover deferred"` / the refuse strings above. They must stay distinct
from `"publication ring quota overflow"`.

**Vulkan**: compile-review-only (ADR-035). Comments only; no execution
claim.

## Alternatives

- Silent enlarge of `envelope_task_quota` / `set_quota` so the first
  submit "succeeds": rejected -- ADR-010 and 02 §5.3 require a new commit
  (or a real DelegatedEnvelope) to cross the envelope.
- Drain leftover on any later submit of the same graph: rejected -- that
  is an implicit global queue and breaks envelope auditability.
- Treat `Rejected` as `continued()` so a later submit can retry: rejected
  -- D1; that is a silent enlarge.
- Implement DelegatedEnvelope / firmware micro-kernel: rejected for this
  cut -- no hardware; firmware remains simulation research.

## Consequences

- Large static quota remains the one-submit control path.
- Small quota is an honest two-submit HostAssisted continuation.
- Reference `submit()` consumes the helper. Metal GPU publish is a
  documented parent hook (`apply_envelope_continuation` then publish only
  `order`).
- E017's first-stage portable choice is overflow buffer + next submit.

## Evidence

- `src/core/core.{h,cpp}` — `EnvelopeContinuationTable`
- `src/backends/envelope_stage.cpp` — `apply_envelope_continuation`
- `src/backends/device_hal.h` — `envelope_task_quota`
- `src/backends/reference/reference_device_hal.cpp` — submit hook
- `tests/unit/envelope_continuation_test.cpp`
- `tests/vertical_slice/metal_envelope_continuation_test.cpp`
- `experiments/definitions/E017-envelope-quota-continuation.json`
- TASK-D5

## Revisit trigger

Revisit if DelegatedEnvelope hardware exists, or if a GPU-resident
continuation can publish leftover without a host split (then the
HostAssisted classification should be re-evaluated, not silently upgraded
to Direct/DevicePass).
