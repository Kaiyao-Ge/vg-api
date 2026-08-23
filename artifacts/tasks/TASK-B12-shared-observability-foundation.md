# TASK-B12: Shared Observability/Timing Foundation (LoweringReport/Submission Counters and Timing)

Status: complete.

Normative docs: ADR-024 (Phase B closure criterion); this milestone has no
independent ADR of its own -- its Context is folded into ADR-026 (TASK-B13),
per the approved plan's stated rationale (E009/E012 both need this same
groundwork, so it is built once here rather than twice).

## Goal

Add real CPU-side timing and structural dispatch counters to the shared
`hal::LoweringReport`/`hal::Submission` structs, as purely additive fields
with zero impact on any pre-existing caller, so TASK-B13 (E009) and TASK-B14
(E012) have a common place to report real cost data instead of each
inventing its own ad hoc mechanism. This is the third milestone (TASK-B12)
of the eight-milestone plan approved this session.

## Files

- `src/backends/device_hal.h` -- `LoweringReport` gains `barrier_count`,
  `encoder_count`, `command_buffer_count`, `queue_wait_count` (all
  `uint64_t`, default-zero); `Submission` gains `cpu_submit_ns`,
  `cpu_encode_ns` (`uint64_t`, default-zero) and `gpu_ns`
  (`std::optional<uint64_t>`, stays `std::nullopt` on every backend in this
  project since none has `capabilities().timestamps_available == true` --
  never fabricated from CPU timing).
- `src/backends/metal/metal_device_hal.mm` -- new file-private
  `DispatchStats` struct (encode/submit `std::chrono::steady_clock`
  nanoseconds plus the four counters), accumulated across however many
  command buffers one `submit()` call issues. `Impl::dispatch_and_wait`/
  `Impl::dispatch_task_publish` each take a `DispatchStats*` out-param,
  timing the encode span (command-buffer/encoder creation through
  `endEncoding`) separately from the submit span (`commit` through
  `waitUntilCompleted`), and incrementing `encoder_count`/
  `command_buffer_count`/`queue_wait_count` by 1 each per call (Metal's
  Tier0 path issues no explicit barrier -- hazard tracking is implicit --
  so `barrier_count` stays 0 on this backend). `DeviceHal::submit()` writes
  the accumulated stats into `submission->cpu_encode_ns`/`cpu_submit_ns`/
  `report.*_count` on the GPU-dispatch path; the host-assisted fallback path
  (native-atomic-unavailable) instead times the whole
  `reference::execute()`/`execute_task_graph()` call as `cpu_submit_ns`,
  leaving `cpu_encode_ns` and the counters at 0 since no GPU dispatch
  happens on that path.
- `src/backends/reference/reference_device_hal.cpp` -- `submit()` times the
  real `execute()`/`execute_task_graph()` wall-clock span as
  `cpu_submit_ns`; `cpu_encode_ns` and all four counters stay at their
  default 0, honestly, since this is a plain CPU interpreter with no
  encoder/command-buffer/barrier/queue-wait concept to count.
- `src/backends/vulkan/vulkan_device_hal.cpp` -- compile-review-only
  (unbuildable on this machine; ADR-024). `submit()` wraps its two existing
  dispatch call sites (`dispatch_and_wait`, the Tier0-only path; and
  `dispatch_task_ring_and_tier1`, the Tier0+Tier1 task-graph path) in
  `std::chrono::steady_clock` timing written to `cpu_submit_ns`, and sets
  the four counters to literal values matching exactly what each function's
  body already issues: `dispatch_and_wait` is 1 command buffer, 1
  bind+push+dispatch scope, 0 explicit barriers, 1 host-blocking
  `vkWaitForFences`; `dispatch_task_ring_and_tier1` is 1 command buffer, 2
  bind+dispatch scopes (task-ring publish, then the Tier1 indirect compute
  dispatch), 2 explicit `vkCmdPipelineBarrier2` calls (ring-write-to-copy,
  copy-to-indirect-read), 1 fence wait. `cpu_encode_ns` stays 0 on this
  backend -- unlike Metal, this path does not separately instrument
  `vkEndCommandBuffer` as a distinct boundary, so the whole record-through-
  fence-wait span is reported as `cpu_submit_ns` rather than a guessed
  split. None of this Vulkan code has executed on real hardware in this
  environment (nor will it, per the permanent constraint); it is reviewed
  against the Vulkan calls the existing code actually issues, not measured.

## Validation

`cmake --build build/dev-metal` rebuilds clean (no new warnings besides the
pre-existing benign duplicate-library linker warning). `ctest
--output-on-failure` under the `dev-metal` preset: 21/21 tests passed,
identical to the pre-TASK-B12 baseline -- confirming these are non-breaking,
purely additive fields as the plan's checkpoint requires. `vulkan_device_hal.cpp`
is not part of the `dev-metal` build (`VG_HAS_VULKAN` is Linux-only) and so
could not be compiled here; its changes are compile-review-only by
construction, matching every other Vulkan-side change in this plan.

## Known limits

- **`gpu_ns` is `std::nullopt` on every backend** -- no backend in this
  project sets `capabilities().timestamps_available`, so no GPU-side
  hardware timestamp is ever reported; this is the honest result, not a gap
  to close within this milestone.
- **Metal's `barrier_count` is 0** -- the Tier0 dispatch paths instrumented
  here rely on Metal's implicit hazard tracking rather than any explicit
  fence/barrier API, so 0 is the accurate count for the paths that exist
  today. TASK-B14 (E012, fork-join effect shapes) is expected to be the
  first Metal path with a real nonzero `barrier_count` (explicit
  `MTLFence`).
- **Vulkan's counters are literal constants keyed to which code branch
  `submit()` takes, not computed by instrumenting the callees themselves**
  -- deliberate: threading a stats out-param through
  `dispatch_and_wait`/`dispatch_task_ring_and_tier1`'s existing signatures
  (as Metal's equivalent functions were changed to do) would require
  editing this backend's header on a machine that cannot compile or
  otherwise verify the result; the literal counts were instead derived by
  reading exactly what each function's body issues, so they are still an
  honest structural fact about the existing code, not a guess.
- **Vulkan's `cpu_encode_ns` stays 0** -- this backend's structural fill
  does not separately mark a "recording ends" boundary distinct from
  "fence wait ends," unlike Metal's `endEncoding`/`waitUntilCompleted`
  split; the whole span is attributed to `cpu_submit_ns` instead of an
  unverifiable guessed split.
- **No independent ADR** -- per the approved plan, this milestone's design
  rationale (why these fields live on the existing structs rather than a
  new type, and why they're purely additive) is folded into ADR-026's
  Context section when TASK-B13 is written, rather than duplicated in its
  own record.
