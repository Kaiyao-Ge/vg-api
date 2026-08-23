# TASK-B8: Task Tier 0/1 (GPU-Generated Task Publication)

Status: complete. Metal Tier0: full local verification on real Apple
Silicon hardware. Vulkan Tier0+Tier1: compile-review-only, unverified on
Linux/NVIDIA. Metal Tier1: known limit, not implemented this milestone.

Normative docs: `docs/START.md`; `docs/vg-project/06-backend-macos-metal.md`
§8; `docs/vg-project/07-backend-linux-nvidia-vulkan.md` §18; ADR-004

## Goal

Implement GPU-side Task publication (Tier0: the release/acquire
`Empty -> Writing -> Published` state machine ADR-004 specified as a CPU
memory model, now expressed as real GPU atomics against a device buffer)
on both backends, plus Tier1 (GPU-encoded indirect dispatch sized from
the GPU-published Task ring, not host-side `TaskRecord.x/y/z`) wherever
each backend's spec requires it -- a hard conformance floor for Vulkan
(§18), a target (not required) for Metal (§8).

## Invariants

- Task ring buffer layout is 14 little-endian `uint32` words per record
  (`kTaskRingWordsPerRecord`, `src/compiler/compiler.h`), field-for-field
  mirroring `core::TaskRecord`, packed/unpacked explicitly word-by-word on
  both backends -- never a raw struct `memcpy`, to avoid MSL/GLSL vs. C++
  struct-layout divergence.
- Each task writes only its own disjoint ring slot (index = the task's
  own position in the graph's task vector); ring-wide dispatch/completion
  order across slots has no bearing on correctness.
- A slot must reach `Published` before being read back; a slot that
  doesn't is a hard execution failure (`submission->result.ok = false`),
  never silently skipped or reported as an empty/default task.
- `submission->published_tasks` is compared byte-exact, in the same
  order, against `vg::reference::execute_task_graph()`'s oracle output
  (ADR-019) -- the reference backend's task-graph execution is the
  single source of truth both GPU backends are checked against.
- Metal's Task ring publish kernel completes synchronously within one
  `submit()` call (`waitUntilCompleted`); it does not use the ADR-020
  timeline machinery for its own internal write-then-read step (only
  cross-`submit()`-call sequencing, if a caller chooses to use it, goes
  through the timeline).
- Vulkan's Task ring publish + Tier1 dispatch are one command buffer:
  Tier0 kernel dispatch -> `vkCmdPipelineBarrier2` (shader-write ->
  transfer-read) -> per-task `vkCmdCopyBuffer` (fields buffer's x/y/z
  words directly into the indirect buffer, zero host-side repack, since
  the word layout is byte-identical to `VkDispatchIndirectCommand`) ->
  second `vkCmdPipelineBarrier2` (transfer-write -> indirect-command-read)
  -> per-task `vkCmdDispatchIndirect`.
- `hal::Capability::TaskPublication` is set honestly per backend: Metal
  sets it unconditionally (Tier0 depends only on buffer atomics, always
  present); Vulkan gates it on `sync2` support (Tier0+Tier1 both need the
  barrier machinery). `IndirectTier1` is set honestly on both: Metal via
  a real ICB-creation probe (no longer the fabricated
  `MTLFeatureSet_macOS_GPUFamily1_v1` signal), Vulkan via `sync2`.

## Files

- `src/compiler/compiler.h`, `compute_package.cpp`
  (`kTaskRingWordsPerRecord`, `task_ring_metal_source()`,
  `task_ring_vulkan_source()`)
- `src/backends/metal/metal_device_hal.h`, `.mm` (`pack_task_record`/
  `unpack_task_record`, `ensure_task_ring_pipeline`,
  `dispatch_task_publish`, `submit()`'s Task ring block,
  `probe_indirect_command_buffers` honesty fix,
  `TaskPublication`/`IndirectTier1` capability bits)
- `src/backends/vulkan/vulkan_device_hal.h`, `.cpp`
  (`TaskRingBuffers`, `pack_task_record`/`unpack_task_record`,
  `ensure_task_ring_pipeline`, `create_task_ring_buffers`/
  `destroy_task_ring_buffers`, `dispatch_task_ring_and_tier1`,
  `submit()`'s Task ring block)
- `tests/vertical_slice/metal_task_timeline_test.cpp` (CTest
  `vertical-slice.metal.task-tier0`, gated `VG_ENABLE_METAL`)
- `tests/vertical_slice/vulkan_task_timeline_test.cpp` (CTest
  `vertical-slice.vulkan.task-tier0`, gated `VG_ENABLE_VULKAN`,
  compile-review-only on this machine)
- `CMakeLists.txt` (test registration for all four new CTest entries)
- `docs/decisions/ADR-021-metal-task-tier0.md`
- `docs/decisions/ADR-022-vulkan-task-tier0-tier1-timeline.md` (Tier0/Tier1 portion)
- `docs/decisions/ADR-004-task-publication-protocol.md` (Revisit trigger,
  now marked Triggered)

## Validation

Verified on real Apple Silicon hardware under the `dev-metal` preset:
`vertical-slice.metal.task-tier0` builds a two-task dependency graph
(explicit dependency edge, non-trivial x/y/z), publishes it through the
real MSL Task ring kernel, and asserts `submission->published_tasks` is
byte-identical, in the same order, to the reference oracle.
`conformance.device-hal.metal` passes with `expect_task_publication =
true`. `ctest --output-on-failure` (`dev-metal` preset, this machine,
2026-08-21): **20/20 tests passed.**

Vulkan's Tier0+Tier1 implementation
(`vulkan_device_hal.h`/`.cpp` Task ring sections,
`vulkan_task_timeline_test.cpp`) is **compile-review-only**. Full-file
review confirmed internal consistency across every new function
(buffer creation/teardown, barrier placement, push-constant layout,
zero-repack copy offsets). `VG_ENABLE_VULKAN` cannot be configured on
this machine; no Linux/NVIDIA build or execution evidence exists.
`device_hal_conformance_vulkan.cpp`'s `expect_task_publication = true`
carries an explicit "logically correct but not hardware-verified"
comment.

Static review checklist applied to the Vulkan Tier0/Tier1 code (in lieu
of runnable evidence): feature-struct `pNext` chain legality;
`VkTimelineSemaphoreSubmitInfo` wait/signal semaphore-value counts match
their corresponding count fields; push-constant layout matches the GLSL
side's field count/alignment; indirect-buffer field types/alignment/
offsets are 4-byte aligned and match `VkDispatchIndirectCommand`;
destructor covers every newly added handle type
(`task_ring_pipeline_`/`task_ring_pipeline_layout_`/
`task_ring_shader_module_`/`timeline_semaphore_`).

## Known limits

**Metal Tier1 (ICB same-Node indirect dispatch/draw) is not implemented
in this milestone.** This is a deliberate, spec-consistent deferral, not
an oversight: `06-backend-macos-metal.md` §8 states Tier1 is a target for
Metal, not an absolute requirement (unlike Vulkan's §18 hard floor). The
`IndirectTier1` capability probe was fixed to be honest (real ICB-creation
attempt, not a fabricated feature-set-name guess) but is not yet consumed
by any dispatch path. `PublicationSlot::Consumed` slot recycling is not
implemented on either backend (CPU reference has never implemented it
either -- GPU Task ring buffers are one-shot per `submit()` call, quota
used and discarded, matching the existing CPU-side gap rather than
being asked to solve it as a side effect of this milestone).
