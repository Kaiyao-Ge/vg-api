# ADR-038: Tier2 Heterogeneous Node Selection (Bucket, not ICB)

Status: Accepted

## Context

`docs/vg-project/06-backend-macos-metal.md` §8 names Tier2 as GPU selection
among multiple pre-authorized Nodes / pipelines. ADR-021 and ADR-026 already
settled that ICB is a Tier2 distinguishing capability, not a Tier1
requirement: Metal Tier0 publishes into a task ring, and Tier1 is
`dispatchThreadgroupsWithIndirectBuffer:` driven by GPU-resident x/y/z
(never ICB). TASK-D4 / E010 is the first milestone that actually has to
choose among ≥2 Node classes.

This host's Metal device can allocate a concurrent-dispatch ICB (the
existing `probe_indirect_command_buffers` path), but ICB command types,
resource inheritance, and encoder lifetime have never been exercised in
this codebase. Forcing ICB as the E010 floor would either (a) ship an
under-verified native-select path or (b) silently degrade to a host walk
and still claim GPU-driven work. TASK-D4's own decision note is: do not
require ICB; have it if we can, otherwise bucket + honest classification.

Vulkan remains compile-review-only on this machine (ADR-024). Tier3 (GPU
invents a Node, grows the envelope, crosses domains) stays Unsupported.

## Decision

**Default mechanism: GPU bucket compute + per-Node indirect, classified
`LoweringClass::EmulatedDevicePass`.** After Tier0 publication, a dedicated
MSL kernel (`vg_tier2_bucket` in `metal_tier2.mm`) reads each published
task's `node_index` (word 0 of the 14-word ring record), matches it
against `ExecutionPlan::authorized_node_classes`, atomically appends the
task into that class's bucket, and writes `selected_classes[gid]`. A
second kernel fills `MTLDispatchThreadgroupsIndirectArguments` from those
atomic counts. One `dispatchThreadgroupsWithIndirectBuffer:` is then
issued per authorized class inside the same command buffer. The host does
not read the counts before those dispatches.

**ICB is an optional capability upgrade, not this milestone's floor.**
This implementation does not encode or execute an ICB. Advertising
`DevicePass` is reserved for a future native ICB (or Vulkan DGC)
multi-pipeline select that actually ran. Reading counts back and
re-encoding per-task commands is `Serialized` / `HostAssisted` and must
never be labeled `DevicePass` or "native GPU-driven."

**Opt-in plan fields, default off.** `ExecutionPlan::request_tier2_select`
(bool, default false) and `authorized_node_classes` (empty by default)
leave every pre-D4 caller unchanged. `validate()` only checks the
structural shape (≥2 unique classes, non-empty task graph) when the flag
is set. An unauthorized `node_index` is a submit-time refuse so the GPU
kernel, not only the host validator, is the thing that rejects it.

**Reference oracle is a host walk (`Serialized`).**
`reference::select_tier2_nodes` copies each task's `node_index` if it is
authorized and refuses otherwise. Metal's post-wait
`last_selected_node_classes()` is compared to that result as a sorted
multiset. The reference `submit()` path reports `Serialized`, never
`EmulatedDevicePass` or `DevicePass`.

**No public C texture or pipeline objects.** Implementation lives in
`metal_tier2.{h,mm}` as free functions taking opaque Metal pointers.
`metal_device_hal.mm` adds a single `request_tier2_select` call next to
the existing Tier0/1 block.

**Vulkan: comments only.** `vulkan_device_hal.{h,cpp}` document the
DGC-or-bucket analogue and do not consume `request_tier2_select`.

**Tier3 remains Unsupported.** A node class outside the authorized set is
a refuse, not a newly invented Node.

## Alternatives

- Require ICB for E010: rejected. ICB is the upgrade ADR-021/026 already
  assigned to Tier2; making it the floor would either under-verify a
  native-select path or force a dishonest DevicePass label on a host
  walk. Bucket + `EmulatedDevicePass` is the honest runnable default.
- Host-walk the published tasks and encode one dispatch per class,
  labeled DevicePass: rejected. That is exactly the "CPU read counts then
  re-encode" pattern 06 §8 and TASK-D4 forbid marking as GPU-driven.
- Add a public pipeline-object API so each Node class is a first-class
  object: rejected. TASK-D4 forbids new public C texture/pipeline
  objects; authorized classes are `uint32_t` ids on the plan.
- Fold the test into `metal_task_timeline_test.cpp` as a `tier2-nodes`
  mode: rejected for this slice. That file is owned by other Phase C/D
  work; E010 lands in `tests/vertical_slice/metal_tier2_test.cpp`.

## Consequences

E010 has a real Metal + reference implementation with honest
classification. ICB remains available as a later upgrade without
rewriting the plan fields. D5 can reuse the authorized-set refuse when it
wants "selection then quota overflow." D7 can plot bucket/command/
temporary/pipeline-switch counters; this ADR does not claim a performance
win.

## Evidence

- `experiments/definitions/E010-heterogeneous-node-lowering.json`
- `src/backends/metal/metal_tier2.{h,mm}` (bucket + per-Node indirect)
- `src/backends/reference/tier2_oracle.{h,cpp}`
- ctest `unit.tier2-oracle` and `vertical-slice.metal.tier2-nodes`
- Vulkan compile-review comments in `vulkan_device_hal.{h,cpp}`

This host's recorded path is **bucket, not ICB**.

## Revisit trigger

Revisit if a later milestone implements a real ICB (or Vulkan DGC)
multi-pipeline select that the GPU itself picks among. At that point the
successful ICB path may report `DevicePass`, the bucket path stays
`EmulatedDevicePass`, and this ADR's "ICB unused" evidence sentence
should be superseded rather than rewritten into a success it did not
earn. Revisit if M-series ICB command types are shown to be insufficient
even as an upgrade -- that would confirm, not retract, the bucket
default.
