# ADR-022: Vulkan Timeline Semaphore and Task Tier0/Tier1 (Code-Review-Only)

Status: Accepted

## Context

ADR-017 (B6) shipped Vulkan's linear-subset vertical slice with `sync2`
capability correctly probed and enabled at device-creation time but never
functionally used (no `vkCmdPipelineBarrier2`/`vkQueueSubmit2` call
anywhere), and explicitly removed the previously-unconditional
`TaskPublication` bit rather than claim a capability that did not yet
exist. `dispatch_and_wait` used classic `vkQueueSubmit`+`VkFence` with no
timeline semaphore at all.

Per `docs/vg-project/07-backend-linux-nvidia-vulkan.md` §11, timeline
wait-before-signal legality is validated by core (ADR-019), not the
adapter -- the adapter only needs to map parameters correctly. Per §18,
this spec's completion criteria are the only place in either backend spec
that uses "must... at least... Tier1" language: **Vulkan's Tier1 indirect
dispatch is a hard conformance floor, not a target**, in explicit
asymmetry with Metal's "Tier1 is a target" framing (ADR-021). This ADR
must preserve that asymmetry, not "fix" it into symmetry.

As with ADR-017, **this machine has no Linux/NVIDIA hardware.** Every
claim in this ADR about Vulkan-specific code is a code-review claim, not
an execution claim -- reinforced here rather than glossed over, per the
same convention ADR-017 established.

## Decision

**Timeline semaphore**: `VkSemaphore timeline_semaphore_`
(`VK_SEMAPHORE_TYPE_TIMELINE`, `initialValue = 0`), created lazily via
`ensure_timeline_semaphore()` on first wait/signal use -- mirroring
Metal's lazy `MTLSharedEvent` creation (ADR-020) rather than eagerly
allocating a semaphore no submission may ever need. `dispatch_and_wait`
chains a `VkTimelineSemaphoreSubmitInfo` onto the classic
`VkSubmitInfo.pNext` for the same queue submission that already carries a
`VkFence`; the fence remains the host-side `vkWaitForFences` completion
signal, and the timeline semaphore layers GPU-visible ordering on top of
it rather than replacing it. When `wait_value`/`signal_value` is 0, the
corresponding `VkTimelineSemaphoreSubmitInfo` field (and the matching
`pWaitSemaphores`/`pSignalSemaphores` entry) is omitted entirely, not
submitted as a literal 0 -- matching ADR-019's guarantee that a
`required_value == 0` plan is already rejected by `validate()` before
reaching any backend, so the backend never needs to treat 0 as a
meaningful wait/signal target.

**Value-domain alignment with no offset mapping**, matching ADR-020's
choice for Metal: `timeline_wait`/`timeline_signal` map directly onto the
`VkSemaphore`'s counter with no translation layer. Fault reporting reuses
the same three codes Metal defined (ADR-020):
`TIMELINE_UNAVAILABLE`/`TIMELINE_WAIT_UNSATISFIED`/
`TIMELINE_SIGNAL_NOT_MONOTONIC`, keeping the vocabulary cross-backend
rather than inventing a Vulkan-specific set. No host-side mirror of the
timeline value is kept -- every readback queries
`vkGetSemaphoreCounterValue` fresh, so nothing can drift out of sync with
what the GPU actually reached.

**Task ring Tier0**: `TaskRingBuffers` holds four ephemeral, per-submission
`VkBuffer`s (state, fields, inputs, indirect), recreated each `submit()`
call rather than cached in `allocation_map_` (task-graph size varies call
to call, and these buffers are backend-private, never exposed through
`core::Allocation`). The publish kernel
(`compiler::task_ring_vulkan_source()`, GLSL, sharing
`kTaskRingWordsPerRecord = 14` and the same word layout as Metal --
ADR-021) is addressed via `buffer_reference` (BDA) push constants, not
descriptor sets, matching this backend's established binding convention
(ADR-017). Unlike Metal's MSL kernel, GLSL's `atomicCompSwap`
(Empty->Writing) and `atomicExchange` (Writing->Published) have no
spurious-failure mode, so no CAS-retry loop is needed -- a single attempt
is sufficient, in deliberate contrast with Metal's retry loop.

**Tier1 (`vkCmdDispatchIndirect`, conformance floor)**: words 5/6/7 of the
14-word Task ring record (x/y/z) are byte-identical in layout to
`VkDispatchIndirectCommand{uint32_t x, y, z}` by construction -- this is
the basis for a zero-repack `vkCmdCopyBuffer` from the fields buffer
directly into the indirect buffer's per-task slot
(`srcOffset = order[slot]*14*4 + 5*4`, `dstOffset =
slot*sizeof(VkDispatchIndirectCommand)`), with no host-side struct
conversion step. `TaskRecord.x/y/z` are stored as-is (already
`uint32_t`-typed in `core::TaskRecord`, so no signed/unsigned conversion
is needed here -- this was verified during implementation rather than
assumed).

**sync2 first real use, scoped narrowly**: two `vkCmdPipelineBarrier2` +
`VkDependencyInfo` calls bracket the Task ring's write-then-consume
sequence within `dispatch_task_ring_and_tier1` -- a
`VK_ACCESS_2_SHADER_WRITE_BIT -> VK_ACCESS_2_TRANSFER_READ_BIT` barrier
before the `vkCmdCopyBuffer` calls, and a
`VK_ACCESS_2_TRANSFER_WRITE_BIT -> VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT`
barrier (at `VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT`, which despite its
"draw" naming correctly covers dispatch-indirect consumption in sync2)
before the `vkCmdDispatchIndirect` calls. This is the entire scope of
sync2 usage in this milestone -- the rest of the submission path
(`dispatch_and_wait`'s outer `VkSubmitInfo`/fence machinery) is
unchanged from ADR-017's classic-API choice; see Alternatives for why a
full `vkQueueSubmit2` migration was not undertaken here.

## Alternatives

- Migrate the entire submission path to `vkQueueSubmit2`/`VkDependencyInfo`:
  rejected for this milestone, per the approved plan's explicit scope
  boundary -- appending `VkTimelineSemaphoreSubmitInfo` to the existing
  core-1.2-standard `VkSubmitInfo` achieves the same timeline semantics
  with a much smaller unverifiable-on-this-machine diff. sync2's usage is
  deliberately confined to the one place (Task ring buffer barriers) that
  has no equally simple classic-API equivalent (a buffer-to-buffer
  write/read hazard genuinely needs an explicit barrier; the outer
  submit/fence flow does not need submit2 to express wait/signal
  semaphores).
- Repack Task ring fields into `VkDispatchIndirectCommand` on the host
  before issuing `vkCmdDispatchIndirect`: rejected -- this would require
  reading the fields buffer back to the host between Tier0 publish and
  Tier1 dispatch, defeating the point of Tier1 being a GPU-side indirect
  path; the layout-identity `vkCmdCopyBuffer` trick keeps everything on
  the GPU timeline.
- Give Metal and Vulkan symmetric Tier1 requirements (either both
  optional or both mandatory): rejected -- the two backend specs
  themselves are asymmetric (§8 vs §18), and ADR-021 already commits to
  preserving that asymmetry rather than erasing it for aesthetic
  consistency.
- Use `VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT` instead of
  `VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT` for the indirect-read barrier,
  reasoning from the "draw" name that it doesn't apply to
  `vkCmdDispatchIndirect`: rejected after confirming
  `VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT`/`VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT`
  are the spec-correct stage/access pair for both draw-indirect and
  dispatch-indirect consumption in Vulkan sync2, despite the
  draw-specific-sounding name.

## Consequences

Vulkan now has a complete, spec-legal (by code review), Tier0+Tier1+
timeline design symmetric in shape with Metal's Tier0+timeline design
(ADR-020/021) but asymmetric in Tier1 requirement level, exactly as the
two backend specs require. **None of this has been built or executed** --
`VG_ENABLE_VULKAN` cannot configure on this project's only development
machine. The `expect_task_publication`/`expect_timeline` conformance
flags in `device_hal_conformance_vulkan.cpp` are both `true` as of this
milestone, each carrying an explicit "logically correct but not
hardware-verified" comment rather than presenting review confidence as
execution evidence.

## Evidence

Code review only, performed on macOS. `src/backends/vulkan/vulkan_device_hal.h`/
`.cpp` were read in full (multiple passes, covering every edit site:
`pack_task_record`/`unpack_task_record`, `ensure_timeline_semaphore`,
`ensure_task_ring_pipeline`, `create_task_ring_buffers`/
`destroy_task_ring_buffers`, `dispatch_task_ring_and_tier1`,
`dispatch_and_wait`'s timeline chaining, `compile()`'s timeline rejection
path, `submit()`'s timeline pre-check block and Task ring block, the
destructor's handle cleanup, and `make_device_hal()`'s capability-bit
computation) and confirmed internally consistent. A new CTest,
`vertical-slice.vulkan.task-tier0`/`vertical-slice.vulkan.timeline`
(`tests/vertical_slice/vulkan_task_timeline_test.cpp`, mirroring
`metal_task_timeline_test.cpp`'s structure), was added and reviewed but
never built or run. No Linux/NVIDIA execution evidence exists.

## Revisit trigger

Revisit when this backend is first built/run on a Linux/NVIDIA server:
confirm the timeline semaphore, Task ring, and Tier1 barrier code
actually work as reviewed, and confirm the `VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT`
stage choice is accepted by real validation layers/drivers. Revisit the
"sync2 scoped to Task ring barriers only" decision if a future milestone
adopts `vkQueueSubmit2` more broadly, at which point this narrow-scope
framing should be superseded. Revisit if Metal's Tier1 (ADR-021, deferred)
is ever implemented, to confirm whether the two backends' Tier1
requirement asymmetry remains intentional or whether the specs
themselves have since been reconciled.
