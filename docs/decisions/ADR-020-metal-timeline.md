# ADR-020: Metal Timeline (MTLSharedEvent) Wiring

Status: Accepted

## Context

ADR-016 (B5) left `submit()` fully synchronous with no cross-submission
ordering primitive at all -- `command_buffer commit` +
`waitUntilCompleted` on every call, no way to express "this submission's
GPU work must not start until a previous submission reached some point."
`hal::Capability::Timeline`'s bit was already set on `shared_events`
probe success, but nothing behind that bit actually existed:
`ExecutionPlan::timeline_wait`/`timeline_signal` were accepted by the
struct but never read by the Metal adapter, and the `shared_events` probe
itself was a single `respondsToSelector:@selector(newSharedEvent)` check
-- selector presence, not a verified working object.

## Decision

**Double-guard fix**: `probe_shared_events()` now requires both
`[device respondsToSelector:@selector(newSharedEvent)]` **and** a
following `[device newSharedEvent]` call returning non-nil. Selector
presence alone does not guarantee the call succeeds on every
device/OS-version combination; a capability bit backed by an
untried-selector guess is exactly the kind of dishonest probe
`docs/START.md` §4 forbids.

**Value-domain alignment, no offset mapping**: `Impl` gains a lazily
created `id<MTLSharedEvent> timeline_event` (created on first
`ensure_timeline_semaphore`-equivalent use, matching the general "don't
allocate device objects until needed" convention already used for
`task_ring_pipeline`). `timeline_wait`/`timeline_signal` values are used
directly as the `MTLSharedEvent` value argument -- no host-side offset or
remapping layer. This keeps the reference oracle's `core::Timeline`
values, the Metal `MTLSharedEvent` value, and (in the not-locally-testable
Vulkan case, ADR-022) the `VkSemaphore` counter directly comparable
without a translation step to get wrong.

**Wiring**: `dispatch_and_wait` encodes `[command_buffer
encodeWaitForEvent:timeline_event value:wait_value]` before the compute
encoder is created when `wait_value != 0`, and `[command_buffer
encodeSignalEvent:timeline_event value:signal_value]` after the encoder
ends (before `commit`) when `signal_value != 0`. A zero wait/signal value
means "no timeline involvement for this side," matching core's guarantee
(ADR-019) that a plan with `timeline_signal != 0 && timeline_signal <=
timeline_wait` is already rejected before reaching the backend, so a
literal `0` is never a legitimate wait/signal target here.

**`compile()`** rejects with `Unsupported` when `timeline_wait != 0 ||
timeline_signal != 0` but the device failed the (now double-guarded)
`shared_events` probe -- never a silent no-op wait/signal. On success it
appends a `timeline`-tagged `LoweringReport` entry.

**`submit()`** fault reporting: rather than letting a real
`encodeWaitForEvent:value:` block forever on a value nothing will ever
signal (which would hang the test process, not fail it), `submit()`
pre-checks the wait/signal state before committing and reports one of
three honest fault codes via `submission->result.fault.code` while
`submit()` itself still returns `true` (matching the established
convention: `submit()` returns `false` only for host-side/precondition
failures): `TIMELINE_UNAVAILABLE` (device has no working shared event but
a wait/signal was requested), `TIMELINE_WAIT_UNSATISFIED` (wait value
exceeds what has been signaled so far), `TIMELINE_SIGNAL_NOT_MONOTONIC`
(would be redundant with ADR-019's `validate()` check but is kept as a
defense-in-depth backend-level check since `submit()` and `validate()`
are not required to run against the same `ExecutionPlan` instance in
every caller).

**Cross-submission consumption model**: the write side of a two-step
sequence signals a value in its own `submit()` call; a later, independent
`submit()` call on the same `Impl` waits for that value before proceeding
-- ordering is expressed purely through the shared `MTLSharedEvent` and
its monotonic counter, with no additional host-side handshake object.
This is the same primitive the Task ring's cross-submission design (had
it needed one) would have used, though in practice Task Tier0 (ADR-021)
turned out not to need cross-submission ordering at all -- it completes
synchronously within a single `submit()` call.

## Alternatives

- Keep the single-selector-check probe: rejected once the double-guard
  requirement was identified as necessary to avoid an untested-selector
  capability lie.
- Apply a host-side offset/epoch mapping between `ExecutionPlan`'s
  wait/signal values and the `MTLSharedEvent` value domain: rejected --
  no requirement calls for it, and a translation layer is one more place
  for a reference-vs-backend value mismatch to hide.
- Let an unsatisfiable wait actually block on `encodeWaitForEvent:value:`
  and rely on a test timeout to surface the failure: rejected -- an
  indefinite hang is a much worse failure mode than a fast, explicit
  `TIMELINE_WAIT_UNSATISFIED` fault, and the pre-check costs nothing since
  the current value is already queryable via the shared event before
  committing.

## Consequences

Metal now has a real, hardware-verified cross-submission ordering
primitive. `hal::Capability::Timeline` is now backed by a probe that
actually calls the API it claims to support, closing a capability-honesty
gap that predated this ADR. The three fault codes
(`TIMELINE_UNAVAILABLE`/`TIMELINE_WAIT_UNSATISFIED`/
`TIMELINE_SIGNAL_NOT_MONOTONIC`) are now shared vocabulary with the
reference backend (ADR-019) and Vulkan (ADR-022, code-review-only),
making cross-backend conformance checks meaningful.

## Evidence

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`vertical-slice.metal.timeline` exercises signal-to-5, wait-5-signal-10,
and stuck-wait-999 (asserting `TIMELINE_WAIT_UNSATISFIED`) end to end
against real `MTLSharedEvent` objects. `ctest --output-on-failure`:
20/20 tests passed, including `conformance.device-hal.metal` with
`expect_timeline = true`.

## Revisit trigger

Revisit if a future milestone needs a wait/signal value offset or
namespace separate from the reference/Vulkan value domain (e.g. multiple
independent timelines per device). Revisit the pre-check-before-blocking
fault design if a future milestone needs partial/streaming completion
semantics where "has this value been signaled yet" is not a simple
synchronous query.
