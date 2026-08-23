# TASK-B7: Effect/Timeline Lowering to Real Backends

Status: complete. Core + reference: full local verification. Metal: full local
verification on real Apple Silicon hardware. Vulkan: compile-review-only,
unverified on Linux/NVIDIA.

Normative docs: `docs/START.md`; `docs/vg-project/06-backend-macos-metal.md`;
`docs/vg-project/07-backend-linux-nvidia-vulkan.md`

## Goal

Give `ExecutionPlan`'s `task_graph`/`timeline_wait`/`timeline_signal`
fields real, cross-validated semantics in core, a byte-exact CPU oracle
in the reference backend, and hardware-backed (Metal) / code-reviewed
(Vulkan) timeline wiring in the GPU backends -- closing the gap where
these fields existed in the struct but nothing actually consumed or
validated them.

## Invariants

- `ExecutionPlan::validate()` rejects `timeline_signal != 0 &&
  timeline_signal <= timeline_wait`, and (when `published` and the task
  graph is non-empty) requires `task_graph.validate_execution()` to
  succeed.
- `ExecutionPlan::graph_epoch_matches(const core::Arena&)` is a separate
  method (not folded into `validate()`, which has no arena access) that
  all three backends call at the top of `submit()`; a task graph built
  against a stale `Arena` topology is rejected before any device state is
  touched.
- `vg::reference::execute()` genuinely enforces timeline wait/signal
  semantics via `core::Timeline::validate_wait`/`signal`, not
  pass-through; `vg::reference::execute_task_graph()` is the byte-exact
  oracle GPU backends' published-task output is checked against.
- Metal/Vulkan use the same three fault codes for timeline failures:
  `TIMELINE_UNAVAILABLE`, `TIMELINE_WAIT_UNSATISFIED`,
  `TIMELINE_SIGNAL_NOT_MONOTONIC`. `submit()` returns `false` only for
  host-side/precondition failures; a timeline fault is reported via
  `submission->result.ok = false` + `fault.code` while `submit()` itself
  still returns `true`.
- Metal's `MTLSharedEvent` probe is double-guarded (selector presence
  *and* a real `newSharedEvent` call returning non-nil), not a
  single-selector-check guess.
- Wait/signal values map directly onto each backend's native timeline
  primitive (`MTLSharedEvent` value / `VkSemaphore` counter) with no host
  offset/translation layer.

## Files

- `src/backends/device_hal.h`, `.cpp` (`ExecutionPlan::validate()`,
  `graph_epoch_matches()`)
- `src/backends/reference/reference_executor.h`, `.cpp` (`execute()`
  timeline enforcement, `execute_task_graph()`)
- `src/backends/reference/reference_device_hal.cpp` (adapted call sites)
- `src/backends/metal/metal_device_hal.h`, `.mm` (`timeline_event`,
  `probe_shared_events` double-guard, `dispatch_and_wait` wait/signal
  encoding, fault-code pre-checks)
- `src/backends/vulkan/vulkan_device_hal.h`, `.cpp`
  (`timeline_semaphore_`, `ensure_timeline_semaphore`, `dispatch_and_wait`
  `VkTimelineSemaphoreSubmitInfo` chaining)
- `tests/vertical_slice/metal_task_timeline_test.cpp` (CTest
  `vertical-slice.metal.timeline`, gated `VG_ENABLE_METAL`)
- `tests/vertical_slice/vulkan_task_timeline_test.cpp` (CTest
  `vertical-slice.vulkan.timeline`, gated `VG_ENABLE_VULKAN`,
  compile-review-only on this machine)
- `docs/decisions/ADR-019-execution-plan-cross-validation-reference-task-timeline.md`
- `docs/decisions/ADR-020-metal-timeline.md`
- `docs/decisions/ADR-022-vulkan-task-tier0-tier1-timeline.md` (timeline portion)

## Validation

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`core.unit`, `conformance.device-hal.reference`,
`conformance.device-hal.metal` (with `expect_timeline = true`), and
`vertical-slice.metal.timeline` all pass -- the latter exercises
signal-to-5, a satisfied wait-5-signal-10, and an unsatisfied wait-999
(asserting `TIMELINE_WAIT_UNSATISFIED`) against real `MTLSharedEvent`
objects. `ctest --output-on-failure` (`dev-metal` preset, this machine,
2026-08-21): **20/20 tests passed.**

Vulkan's timeline path (`vulkan_device_hal.h`/`.cpp` timeline sections,
`vulkan_task_timeline_test.cpp`) is **compile-review-only**.
`VG_ENABLE_VULKAN` cannot be configured on this project's only
development machine (`CMakeLists.txt` hard-fails with `FATAL_ERROR` on
non-Linux). `device_hal_conformance_vulkan.cpp`'s `expect_timeline = true`
carries an explicit "logically correct but not hardware-verified"
comment. No Linux/NVIDIA execution evidence exists.

## Known limits

No host-side mirror of timeline state on either backend -- every readback
queries the native primitive fresh (deliberate: prevents host/device
drift, at the cost of an extra query per check). Vulkan's timeline
wiring has never been built or run; see ADR-022's Revisit trigger.
