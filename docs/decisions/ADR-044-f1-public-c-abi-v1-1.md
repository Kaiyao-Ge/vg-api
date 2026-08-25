# ADR-044: F1 — Public C ABI Expansion v1.0 → v1.1

Status: Accepted

## Context

ADR-043 (Accepted) opened Phase F and, in Decision #2, committed to
expanding `VgApi` from its ADR-042-frozen v1.0 skeleton
(`createRuntime`/`destroyRuntime`/`enumerateAdapters`) to the call chain
`04-public-c-abi.md` §17 already specifies:
`openAdapter → createDevice → createAddressDomain → createArena →
arenaAllocate → loadCodeObject → createNode → createTaskGraphBuilder →
taskGraphAppend → taskGraphAddDependency → sealTaskGraph →
createExecutionEnvelope → createTimeline → submit → waitTimeline →
getSubmissionLoweringReport`, plus full teardown, implemented as thin
wrappers over already-hardware-verified C++ machinery. This is the
Checkpoint-A precondition: an offscreen triangle app reachable through the
public C ABI alone. Every milestone ADR under Phase F cites ADR-043 as its
governance basis instead of re-litigating the ABI-freeze question; this ADR
does that.

`04-public-c-abi.md` gives literal C prototypes for only five functions
(`vgCreateTaskGraphBuilder`, `vgTaskGraphAppend`, `vgTaskGraphAddDependency`,
`vgSealTaskGraph`, `vgSubmit`) and explicitly licenses narrowing the v1
function set, provided that is not done via implicit global Device,
implicit GPU selection, implicit sync, or backend handle leakage (quoted in
ADR-043's Context). Roughly nine structs the example needs are left
undefined by design — the spec text calls itself an illustration, not a
frozen header.

A pre-implementation survey of `core::Arena`, `core::TaskGraph`/
`TaskGraphBuilder`, `hal::DeviceHal`/`ExecutionPlan`/`CompiledPlan`/
`Submission`, and the existing vertical-slice tests found that Arena/
Allocation, TaskGraphBuilder/TaskGraph, and compile+submit+LoweringReport
were already thin-wrappable — matching what every existing C++ test already
exercises. Adapter→Device selection, AddressDomain, CodeObject, Node,
ExecutionEnvelope, and Timeline had no C++ backing at all and needed new,
small classes that aggregate fields already scattered across
`ExecutionPlan` (`envelope_task_quota`, `authorized_node_classes`,
`timeline_wait`/`timeline_signal`) or wrap a factory call that previously
ignored adapter identity.

## Decision

Implement the full v1.1 chain in one pass (F1), not split into a
thin-wrap-only sub-phase and a new-objects sub-phase.

**Handle lifetime.** Every new opaque handle (`VgAdapter`, `VgDevice`,
`VgAddressDomain`, `VgArena`, `VgCodeObject`, `VgNode`,
`VgTaskGraphBuilder`, `VgTaskGraph`, `VgExecutionEnvelope`, `VgTimeline`,
`VgSubmission`) is a heap-allocated wrapper struct owning (or referencing)
the real C++ object, tracked in a generic `HandleRegistry<T>`
(`src/api/vg_api_handle_registry.h`) that every entry point consults before
touching the handle, returning `VG_ERROR_STALE_HANDLE` on a miss. This
satisfies the §16 stale-handle requirement deterministically without
inventing generation numbers for objects that don't already have one.
`VgAllocation` is the one exception: `Arena::allocate` returns a stable,
never-erased pointer, so `VgAllocation` stays a direct
`reinterpret_cast<core::Allocation*>` — no registry, matching the pattern
the pre-F1 `VgRuntime_T` already used.

**Concurrency.** `HandleRegistry<T>::insert`/`contains`/`erase`
(`src/api/vg_api_handle_registry.h`) are each internally mutex-guarded, so
distinct handles of the same type may be freely created, destroyed, and
validated concurrently from different threads -- this is already true of
the shipping implementation, not a future promise. What the registry does
not and cannot make safe is same-handle check-then-use: a caller must not
call `destroyX(h)` concurrently with any other API call that takes `h`, or
a handle reachable through `h` (e.g. `submit()`'s `envelope`→`arena` and
`graph`→`code_object` hops), as an argument. This matches Vulkan's
external-synchronization model already invoked above for handle-parent
lifetime, and codifies -- for the implementation surface, where a reviewer
can actually find it next to the code -- the intent already recorded in
`04-public-c-abi.md` §14's thread-safety table: query/immutable-object
calls are fully concurrent; runtime/device creation is concurrent given a
thread-safe allocator; builder mutation requires external synchronization
unless an interface explicitly documents a concurrent-chunk pattern; arena
allocation/free is internally synchronized; host map writes are the
calling application's responsibility to keep race-free; submit/timeline
query is fully concurrent; and destroy must not be called concurrently
with any other call on the same host handle. §14 further states that
in-flight GPU lifetime is automatically deferred by epoch retirement on
destroy. That promise is disclosed here as aspirational, not implemented:
no current backend (reference, Metal, or Vulkan) performs asynchronous,
non-blocking submission -- every `submit()` call today blocks until the
GPU work completes -- so there is no in-flight window yet for
epoch-deferred reclamation to actually cover. This is not a violation of
§14 (nothing observable contradicts it today), only an unexercised code
path; revisit once a backend adds real async submission.

**Adapter-selective Device creation.** `openAdapter`/`createDevice` honor
the specific adapter the caller chose from `enumerateAdapters` (the "no
implicit GPU selection" constraint) for all three backends. `vg_api_device.cpp`'s
`create_device` dispatches on `adapter->record.backend_kind` and calls a
uuid-selective `make_device_hal(uuid, &error)` overload per backend:

- `vg::metal::make_device_hal(uuid, error)` (`metal_device_hal.mm`) decodes
  the uuid's `VGP0METL`-prefixed, little-endian-`registryID` encoding
  (matching what `metal_probe.mm` already produces for `enumerateAdapters`),
  enumerates `MTLCopyAllDevices()`, and matches on `registryID` — falling
  back to `MTLCreateSystemDefaultDevice()` only when the enumeration itself
  is empty, never as a substitute for a uuid that fails to match.
- `vg::vulkan::make_device_hal(uuid, error)` (`vulkan_device_hal.cpp`) matches
  on `vendorID`/`deviceID`/`pipelineCacheUUID` against the enumerated
  `VkPhysicalDevice` list the same way `vulkan_probe.cpp` derives adapter
  identity; a `nullptr` uuid keeps the pre-F1 default-device behavior for
  the existing no-uuid `make_device_hal(error)` overload other tests still
  use directly. Compile-review-only per ADR-024's standing evidence policy —
  implemented but not counted as passed hardware evidence.
- `vg::reference::make_device_hal()` is unchanged (a single reference device
  exists; its `VgAdapter` is a fixed singleton record and adapter identity
  is trivially honored by having only one).

A uuid that matches no device on the target backend fails with
`VG_ERROR_UNSUPPORTED` rather than silently substituting a different
device — this is what makes "no implicit GPU selection" an enforced
property of `create_device`, not just a documented intention.

**Five new core classes**, each intentionally minimal and disclosed as
scoped-down for v1 rather than matching the spec's eventual ambition:

- `core::AddressDomain` — `{ uint32_t kind; }`. One domain per `VgDevice`
  for v1; multi-domain support deferred past F1. `kind` is recorded from
  `VgAddressDomainDesc::kind` at creation (`create_address_domain`,
  `src/api/vg_api_arena.cpp`) purely for forward-compatibility with that
  future multi-domain design — with exactly one domain per device, nothing
  in F1 yet needs to branch on it, so it is stored but not read anywhere in
  the current implementation.
- `core::CodeObject` — owns the raw bytes/JSON text handed to
  `loadCodeObject` plus a `format_tag`. No caching or precompilation:
  `submit()` parses fresh via `ir::parse_module` on every call, exactly
  matching the pre-F1 vertical-slice pattern. Not a regression — it gives
  the bytes a handle/lifetime without pretending compilation caching
  exists yet.
- `core::NodeTable` — an id+generation allocator (`create`/`destroy`/
  `lookup`) so `TaskRecord.node_index`/`node_generation` reference real
  entries instead of caller-picked-by-convention integers.
- `core::ExecutionEnvelope` — bundles `allowed_node_classes`
  (→ `authorized_node_classes`), `has_certificate_mode`/`certificate_mode`,
  `has_task_quota`/`task_quota` (→ `envelope_task_quota`), and
  `timeline_wait`/`timeline_signal`, with one method,
  `apply_to(hal::ExecutionPlan&) const`, that splices them into a plan
  before `compile()`/`submit()`. `VgAccessCertificateDesc`'s range-granular
  `{allocation, offset, size, access_mask}` array is translated to
  whole-allocation `PointerRef`s for v1 — a disclosed narrowing, not a
  silent drop; offset/size/access_mask are accepted on the wire but not yet
  enforced at sub-allocation granularity. `allowed_node_classes` is a
  `std::vector<uint32_t>` built from `VgExecutionEnvelopeDesc::allowed_nodes`
  (`create_execution_envelope`, `src/api/vg_api_execution.cpp`) by copying
  only each `VgNodeRef::index` and dropping `.generation` — unlike
  `VgTaskRecord.node`, which `task_graph_append` (`src/api/
  vg_api_taskgraph.cpp`) validates against the live `core::NodeTable` of the
  one `VgCodeObject` its `VgTaskGraphBuilder` was created against.
  `createExecutionEnvelope` takes only a `VgDevice`/`VgArena`, never a
  `VgCodeObject`, so no single `NodeTable` is in scope to validate an
  index+generation pair against at envelope-creation time — an envelope is
  device-scoped and can be applied across submits of different task graphs
  built from different code objects. `allowed_node_classes` therefore is not
  a live per-CodeObject node identity at all: it is a plain node-class
  allow-list, matched by value at submit time against whichever graph's
  already-`NodeTable`-validated `TaskRecord.node_index` values are present
  (`vg::reference::select_tier2_nodes`, `src/backends/reference/
  tier2_oracle.cpp`; `hal::validate_tier2_select`, `src/backends/
  device_hal.cpp`), and never dereferenced as an array index (Metal's ICB
  path uses it only as a GPU function-constant/cache key,
  `icb_node_pipeline`, `src/backends/metal/metal_tier2.mm`). An
  envelope-declared class with no matching node in whatever graph is
  eventually submitted is simply inert, not unsafe.
- Public per-device `core::Timeline` surfacing — each `VgDevice` wrapper
  owns one `core::Timeline`; `createTimeline`/`destroyTimeline` manage its
  handle, `waitTimeline` calls the existing `validate_wait()`, and
  `submit()` reads/writes `ExecutionPlan.timeline_wait`/`timeline_signal`
  from/into it. One timeline per device, not the spec's eventual
  multi-timeline ideal — matches the reference backend's existing
  single-`Timeline`-member pattern.

**`TaskGraph::publish()` is called from `submit()`.** `ExecutionPlan.published`
(a plain bool the caller sets) and `TaskGraph::publish()` (a state
transition with its own sealed/published invariants, required before
`validate_execution()` will pass) are different things. The public C ABI's
call chain has no explicit "publish task graph" step between
`sealTaskGraph` and `submit`, so `submit()` calls `graph.publish(&error)`
itself, guarded by `!graph.published()` so a graph is never asked to
publish twice.

**`VgTaskRecord` gains `root_generation`.** `core::TaskRecord` already
splits `root_allocation`/`root_generation`; `04-public-c-abi.md`'s
illustrative `VgTaskRecord.root` is a bare `uint64_t` with no room for a
generation, and the spec text says its example fields "can be adjusted per
ADR." Adding `uint32_t root_generation` is a disclosed, cited extension
beyond the illustrative text, not silent drift.

**`getSubmissionLoweringReport` returns JSON, not a mirrored struct.**
`hal::LoweringReport::canonical_json()` already exists; exposing that
string (valid until the submission is destroyed) avoids a large,
ABI-fragile struct for data that is inherently variable-shaped (an event
list).

**`vgGetApi` version negotiation.** `VgApi` grows by strict append —
`version`, `size`, and the original three v1.0 function pointers keep their
exact offsets; all 24 v1.1 entries are appended after. `vgGetApi` computes
`v1_0_size = offsetof(VgApi, openAdapter)` and a v1.1 `full_size`, validates
the caller's declared `size` against whichever the requested version needs,
and `memcpy`s only that many bytes into the caller's buffer. A v1.0 request
against the v1.1 library gets exactly the v1.0-shaped table; a caller built
against the old, smaller `VgApi` layout still works unmodified against the
v1.1 library, since none of its bytes moved.

## Alternatives

- **Split F1 into a thin-wrap-only pass and a new-objects pass**: rejected
  by explicit user decision — the full chain is more useful to verify
  end-to-end in one Checkpoint-A-shaped test than as two partial ABIs
  neither of which reaches Checkpoint A alone.
- **Mirror `hal::LoweringReport` into a public C struct** instead of
  returning JSON: rejected — the event list is variable-shaped, and a
  struct large enough to hold it either over-allocates or requires a second
  variable-length-array ABI pattern the rest of `vg.h` doesn't use.
- **Range-granular access certificates in v1.1**: rejected for now — the
  backend-side enforcement at sub-allocation granularity doesn't exist yet;
  building the wire format without the enforcement would silently promise
  something not delivered, so range/offset/size are accepted and recorded
  but only whole-allocation `PointerRef`s are actually authorized this
  version.
- **Reference-only device opening for F1, deferring Metal/Vulkan
  uuid-selective overloads**: considered, rejected — F1's gate is public-ABI
  reachability, but `04-public-c-abi.md`'s "no implicit GPU selection"
  constraint is only actually enforced once a uuid that doesn't match is
  observably rejected on a backend that has more than one theoretically
  selectable device; implementing the Metal/Vulkan overloads was small
  enough (each backend already enumerates devices/physical devices for its
  `enumerateAdapters` probe) that deferring them would have left the
  constraint honored by accident (only one adapter existed to pick) rather
  than by mechanism.

## Consequences

- `VgApi` is now 0x00010001 (`VG_API_VERSION_1_1`); `vgGetApi(VG_API_VERSION_1_0,
  ...)` remains supported and byte-identical to the pre-F1 contract.
- A new ctest, `api.c-abi-conformance` (`tests/api/vg_c_abi_conformance_test.cpp`),
  includes only `<vg/vg.h>` and links only `vg_api` — proving the full
  golden path, stale-handle rejection, and both version-skew directions
  (§16) are reachable through the public ABI alone, never touching
  `vg_core`/`vg_backend_reference` directly. This is the actual
  Checkpoint-A precondition, not an internal unit test.
- `tests/abi/abi_c_test.c`'s too-small-`size` check had to be repinned from
  `sizeof(VgApi) - 1` to `offsetof(VgApi, openAdapter) - 1`: the old
  expression tested "one byte less than the whole struct," which was
  equivalent to "one byte less than the v1.0 boundary" only while `VgApi`
  had exactly three functions. Once `VgApi` grew, `sizeof(VgApi) - 1` still
  exceeded the v1.0 boundary, so the intended too-small-for-v1.0 case
  stopped being too-small. This is a mechanical consequence of additive
  growth, not a change in what the test asserts.
- Metal/Vulkan adapter-selective device opening is implemented and wired
  through `openAdapter`/`createDevice`; a uuid that doesn't match any
  enumerated device on that backend fails with `VG_ERROR_UNSUPPORTED`
  instead of silently opening a different device.
- Range-granular access certificate enforcement remains future work; the
  wire format anticipates it but the backend only authorizes at
  whole-allocation granularity today.

## Evidence

- `build/dev-reference` (Metal off, Vulkan off): full build clean; all 26
  ctests pass, including `api.c-abi-conformance` (against the reference
  adapter, the only one enumerated in this configuration).
- `build/dev-metal` (Metal on): full build clean; all 53 ctests pass,
  including `api.c-abi-conformance` — which, in this configuration,
  `enumerateAdapters` returns a Metal-class adapter for, so the test
  exercises `openAdapter`/`createDevice`'s uuid-selective
  `vg::metal::make_device_hal(uuid, ...)` path on real Metal hardware, not
  just the reference fallback — and every pre-existing
  `vertical-slice.metal.*` test, confirming the additive `core.h`/
  `device_hal.h` changes did not regress any hardware-verified path.
- Vulkan path (`vg::vulkan::make_device_hal(uuid, ...)`) is compile-reviewed
  only, no Vulkan SDK on this machine; per ADR-024's standing evidence
  policy this is disclosed, not counted as passed hardware evidence.

## Revisit trigger

Revisit when Metal/Vulkan adapter-selective `make_device_hal` overloads are
added (the deferred item from this ADR's Alternatives), when range-granular
access certificates gain backend enforcement, or when a later Phase F
milestone needs a `VgApi` v1.2 growth — at which point this ADR's
version-negotiation contract (append-only, `size`-gated) is the mechanism
to reuse, not redesign.
