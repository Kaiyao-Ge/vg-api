#ifndef VG_BACKENDS_DEVICE_HAL_H_
#define VG_BACKENDS_DEVICE_HAL_H_

#include "core/core.h"
#include "compiler/compiler.h"
#include "ir/ir.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vg::hal {

constexpr uint32_t kDeviceHalAbiVersion = 1;

enum class BackendKind : uint32_t { Reference = 1, Metal = 2, Vulkan = 3 };
enum class Capability : uint32_t {
  LinearAddress = 1u << 0,
  TaskPublication = 1u << 1,
  Timeline = 1u << 2,
  EffectDag = 1u << 3,
  CaptureReplay = 1u << 4,
  IndirectTier1 = 1u << 5,
  // Advertising this obliges the backend to execute AttachmentWrite work
  // through a real raster path (05 §9's `region.attachment.store`) and produce
  // the image a conforming rasterizer would -- not to accept raster work and
  // approximate it with an untested compute blit. A backend with no raster
  // path leaves the bit clear and reports Unsupported.
  Raster = 1u << 6,
  // Advertising this obliges the backend to actually carry out every
  // ExecutionPlan::representation_requests item -- publish a new
  // RepresentationEpoch, retire what it invalidated, acquire the requested
  // facet (02 §8: a transform is not a barrier) -- and to report its real
  // cost. A backend that cannot leaves the bit clear and answers a request
  // with CompiledPlan::representation_supported = false plus an Unsupported
  // LoweringEvent, never with a silently dropped request.
  RepresentationTransform = 1u << 7,
  // Advertising this obliges the backend to genuinely verify a FacetRef's
  // generation under ValidationProfile::CheckedNative -- in the shader on a
  // GPU backend (06 §6.4), exactly and host-side on the reference judge. A
  // backend that dereferences facets unchecked leaves the bit clear rather
  // than letting a caller believe a stale token would be caught.
  CheckedFacetGeneration = 1u << 8,
};

struct CapabilitySnapshot {
  uint32_t abi_version{kDeviceHalAbiVersion};
  BackendKind backend{BackendKind::Reference};
  std::string adapter_name;
  std::string driver;
  uint64_t capability_bits{};
  uint64_t max_buffer_size{};
  uint32_t address_width{};
  uint32_t min_buffer_alignment{};
  bool validation_available{};
  bool timestamps_available{};

  bool supports(Capability capability) const {
    return (capability_bits & static_cast<uint64_t>(capability)) != 0;
  }
};

enum class LoweringClass : uint32_t {
  Direct,
  CachedObject,
  DevicePass,
  HostAssisted,
  Serialized,
  Unsupported,
  // TASK-D4 (E010): GPU-authored bucket + per-Node indirect, used when a
  // native ICB / DGC select did not run. Never use DevicePass for "host
  // read counts, then re-encode" -- that is Serialized/HostAssisted.
  EmulatedDevicePass,
};

struct LoweringEvent {
  std::string operation;
  LoweringClass classification{LoweringClass::Direct};
  uint64_t count{};
  uint64_t bytes{};
  std::string reason;
};

struct LoweringReport {
  uint32_t abi_version{kDeviceHalAbiVersion};
  BackendKind backend{BackendKind::Reference};
  bool supported{};
  std::string diagnostic;
  std::vector<LoweringEvent> events;
  // TASK-B12: shared observability counters, additive to the pre-existing
  // fields above. Each backend reports what it actually does; a backend with
  // no such concept (e.g. reference has no encoders/command buffers) reports
  // 0 rather than a fabricated count.
  uint64_t barrier_count{};
  uint64_t encoder_count{};
  uint64_t command_buffer_count{};
  uint64_t queue_wait_count{};
  // Shared representation-transform accounting, same additive/honest-zero
  // discipline as the counters above. Best-effort: a backend with no real
  // heap-fragmentation concept (reference, or Metal when it does not track
  // one) reports 0 rather than fabricating a number.
  uint64_t heap_fragmentation_bytes{};

  void add(std::string operation, LoweringClass classification, uint64_t count,
           uint64_t bytes, std::string reason);
  uint64_t count(LoweringClass classification) const;
  bool has_hidden_host_wait() const;
  std::string canonical_json() const;
};

// One Stage 5 (03 §7 "Representation") item: a Region's required facet and
// representation version, fixed before the adapter is allowed to see the plan
// (03 §8: "每个 Region 的所需 facet 与 representation version 已固定").
struct RepresentationRequest {
  core::CanonicalView view;
  core::FacetKind target_kind{core::FacetKind::Sample};
  // 02 §4.2 / 06 §11. Default false is the documented safe behaviour: the old
  // backing is retained until the relevant command buffer completes. Setting
  // it true additionally requires a complete `consume_proof`; the adapter is
  // forbidden from inferring a destructive transform on its own (06 §11:
  // "adapter 不自行推断破坏性转换").
  bool consume_input{};
  core::ConsumeProof consume_proof;
};

struct ExecutionPlan {
  uint32_t abi_version{kDeviceHalAbiVersion};
  CapabilitySnapshot capabilities;
  ir::Module module;
  core::Certificate certificate;
  core::TaskGraph task_graph;
  uint64_t graph_epoch{};
  uint64_t timeline_wait{};
  uint64_t timeline_signal{};
  bool published{};
  // E004: when set, submit() builds an AccessCertificate for this mode over
  // the live arena and reports it via Submission::access_certificate/report.
  // Unset (the default) preserves every pre-E004 caller's behavior exactly:
  // no certificate is built, no report event is added.
  std::optional<core::AccessCertificateMode> requested_certificate_mode;
  // TASK-B13 (E009): when true (and the backend advertises
  // Capability::IndirectTier1), submit() follows Tier0 task publication with
  // a real GPU-indirect dispatch pass -- each published task's own x/y/z
  // dispatch dims (already GPU-resident from Tier0, never read back to the
  // host before dispatching) drive dispatchThreadgroupsWithIndirectBuffer:.
  // Default false preserves every pre-B13 caller's behavior exactly
  // (Tier0-only, as before). Meaningless on backends with no real GPU
  // dispatch concept (reference); those simply ignore it.
  bool request_tier1_indirect{};
  // TASK-B14 (E012): when non-empty, describes a general Effect DAG of
  // independently-compiled passes rather than the single `module` above.
  // `module` must still be one of the passes (by convention, pass 0) so
  // ExecutionPlan::validate()'s existing ir::verify(module) call keeps
  // working unmodified -- effect_dag_passes is what actually drives
  // submit()'s multi-encoder/fence dispatch when non-empty; `module` alone
  // is never separately (re-)dispatched in that case. Default empty
  // preserves every pre-B14 caller's behavior exactly (single-module path).
  std::vector<ir::Module> effect_dag_passes;
  // Dependency edges between effect_dag_passes indices (before, after),
  // mirroring TaskGraphBuilder::add_dependency's (before, after) pair shape.
  // Meaningless when effect_dag_passes is empty.
  std::vector<std::pair<uint32_t, uint32_t>> effect_dag_dependencies;
  // TASK-B16 (E007): when true, compile() lowers plan.module through
  // compiler::build_indexed_compute_package instead of build_linear_compute_
  // package -- same load/store-only IR, deliberately compiled two different
  // ways so E007 can compare the resulting binding counts (N buffer(N)
  // slots vs. one argument-buffer-style table). Following the
  // requested_certificate_mode precedent above: this is an ExecutionPlan
  // field, not a second virtual method, since DeviceHal::compile/submit stay
  // fixed at two methods and all per-submission variance is a plan field.
  // Meaningless (ignored) when plan.module contains any pointer-graph opcode
  // (load_ref/load_via/store_via) -- those always take the CachedObject
  // path regardless of this flag. Default false preserves every pre-B16
  // caller's behavior exactly (linear lowering, as before).
  bool request_indexed_binding{};
  // Stage 5's input (03 §7): one entry per Region whose required facet and
  // representation version this submission fixes. Empty (the default)
  // preserves every existing caller exactly -- submit() then runs no Stage 5
  // bookkeeping, seals no RepresentationEpoch, and leaves every
  // representation field of Submission at its default.
  std::vector<RepresentationRequest> representation_requests;
  // 03 §12's four profiles. A profile changes diagnosis, instrumentation and
  // scheduling determinism only, never the meaning of a legal program, which
  // is why it is a plain per-submission input rather than a field of any
  // sealed object. CheckedNative (the default) asks for the in-shader
  // facet-generation check of 06 §6.4; a backend that cannot honour it must
  // say so by leaving Capability::CheckedFacetGeneration clear rather than
  // silently running the submission unchecked.
  core::ValidationProfile validation_profile{core::ValidationProfile::CheckedNative};
  // TASK-D1 / ADR-035: unset preserves every pre-D caller. TASK-D3:
  // submit() enforces a set working_set_budget via apply_working_set_budget()
  // -- requested bytes that exceed it are a hard refuse (the
  // WorkingSetBudget::allows error), never a silent clamp. validate() still
  // rejects lease.byte_limit > budget.byte_limit before submit.
  std::optional<core::WorkingSetBudget> working_set_budget;
  std::optional<core::WorkingSetLease> working_set_lease;
  std::optional<core::EnvelopeOverflow> pending_overflow;
  // TASK-D2 / ADR-036: caller-supplied seeds for a DiscoverThenLease walk
  // (02 §7.2). Default empty = no discovery stage, so every pre-D2 caller
  // -- including B-era DiscoverThenLease's full-arena scan -- is unchanged.
  std::vector<core::PointerRef> discovery_seeds;
  // TASK-D5 / ADR-039: per-submit envelope cap on how many tasks this
  // commit may publish. Unset = no envelope cap (every pre-D5 caller).
  // Distinct from TaskGraphBuilder::set_quota (ADR-010, build-time only).
  // 0 is a set cap of zero, not "unset". Crossing the cap is overflow
  // buffer + next submit, never a silent enlarge.
  std::optional<uint32_t> envelope_task_quota;
  // TASK-D4 (E010): when true, submit() selects among
  // `authorized_node_classes` (>=2 pre-authorized Node classes). Metal
  // prefers a GPU-encoded ICB (DevicePass) and falls back to bucket +
  // per-Node indirect (EmulatedDevicePass). Default false / empty list
  // leaves every pre-D4 caller unchanged. A backend that host-walks
  // selection must classify Serialized/HostAssisted -- never DevicePass.
  bool request_tier2_select{};
  std::vector<uint32_t> authorized_node_classes;

  bool validate(std::string* error = nullptr) const;
  // Checked separately from validate() because it needs the live arena: a
  // task graph is only meaningful against the topology it was built against,
  // so graph_epoch must match arena.topology_epoch() at submit() time. A
  // plan with no tasks is exempt (graph_epoch is unused/zero in that case,
  // matching every pre-B7 caller that never set it).
  bool graph_epoch_matches(const core::Arena& arena, std::string* error = nullptr) const;
};

struct CompiledPlan {
  uint32_t abi_version{kDeviceHalAbiVersion};
  ExecutionPlan plan;
  // B4's target-neutral package.  It is deliberately source/metadata only;
  // B5/B6 attach private pipeline objects outside this contract.
  std::optional<compiler::ComputePackage> compute_package;
  // TASK-B16 (E007): populated instead of compute_package when
  // plan.request_indexed_binding was true and plan.module qualified (load/
  // store only, no pointer-graph opcodes). Exactly one of compute_package /
  // indexed_compute_package is ever set for a given CompiledPlan -- never
  // both, never neither, on a successful compile().
  std::optional<compiler::IndexedComputePackage> indexed_compute_package;
  LoweringReport report;
  // TASK-B14 (E012): populated only when plan.effect_dag_passes was
  // non-empty and compile() accepted its shape. One ComputePackage per
  // pass, same order as plan.effect_dag_passes. effect_dag_shape is
  // Unsupported (and effect_dag_packages/effect_dag_graph left empty/
  // default) when classify_effect_graph_shape() did not recognize the
  // graph -- compile() reports that honestly via `report` and returns
  // false rather than guessing a lowering strategy.
  std::vector<compiler::ComputePackage> effect_dag_packages;
  core::EffectGraph effect_dag_graph;
  uint32_t effect_dag_node_count{};
  core::EffectGraphShape effect_dag_shape{core::EffectGraphShape::Unsupported};
  // False when compile() could not accept one of
  // plan.representation_requests because this backend has no lowering for the
  // requested transform. The rejection always carries an Unsupported
  // LoweringEvent naming the reason, per START.md §4's tenth invariant: a
  // backend never drops a request and then reports a successful compile as if
  // none had been asked for.
  bool representation_supported{true};
};

struct Submission {
  uint32_t abi_version{kDeviceHalAbiVersion};
  core::ExecutionResult result;
  uint64_t timeline_value{};
  LoweringReport report;
  // Populated only when the submitted plan had a non-empty task_graph: the
  // tasks in the order they were actually published (Empty->Writing->
  // Published->Consumed), for byte-exact cross-backend comparison.
  std::vector<core::TaskRecord> published_tasks;
  // Populated only when ExecutionPlan::requested_certificate_mode was set
  // and the mode has a real implementation (CertifiedPinned/Universe/
  // DiscoverThenLease); left unset for SoftwarePaged/FaultManaged, which
  // report Unsupported via `report` instead of fabricating a certificate.
  std::optional<core::AccessCertificate> access_certificate;
  // TASK-B12: real host-side wall-clock timing, std::chrono::steady_clock
  // nanoseconds. cpu_encode_ns covers command-buffer/encoder construction
  // through endEncoding (or the equivalent host-side setup on a backend with
  // no such concept); cpu_submit_ns covers commit/queue-submit through the
  // host observing completion. gpu_ns stays std::nullopt unless
  // capabilities().timestamps_available is true -- no backend in this
  // project sets that today, so it is never fabricated from CPU timing.
  uint64_t cpu_submit_ns{};
  uint64_t cpu_encode_ns{};
  std::optional<uint64_t> gpu_ns;
  // TASK-D5 / ADR-039: leftover from this submit. Unset means continuation
  // was not in play. A Deferred record carries a non-zero token for the
  // next submit's pending_overflow; Rejected never answers continued().
  std::optional<core::EnvelopeOverflow> envelope_overflow;
  // Stage 5's sealed output (03 §7). Sealed only when the submitted plan
  // carried representation_requests; otherwise left default-constructed, so
  // sealed() distinguishes "no Stage 5 ran" from "Stage 5 ran and froze
  // nothing".
  core::RepresentationEpoch representation_epoch;
  // One acquired target facet per request, in request order. These stay live
  // across a ConsumeInput: what a transform's consume destroys is the backing
  // the transform superseded, not the allocation, so the facet published by
  // the transform is exactly the thing that must keep resolving afterwards.
  std::vector<core::FacetRef> representation_facets;
  // Facets retired because a newly published RepresentationEpoch (or a
  // ConsumeInput) invalidated them, and allocations whose old backing a
  // ConsumeInput actually released. Both count work Stage 5 really did.
  // core::FacetPool::retire_stale() sweeps the whole device-owned pool, so a
  // device driven against more than one Arena also retires here the slots
  // whose allocation does not live in the submitted arena at all;
  // retired_facet_count reports what was actually retired rather than a
  // filtered estimate of it.
  uint32_t retired_facet_count{};
  uint32_t consumed_allocation_count{};
  // Stage 5's peak-memory accounting, the report 06 §11 requires ("峰值内存
  // 报告包括 old/new backing、temporary、heap fragmentation 与 completion
  // delay"; heap fragmentation lives on LoweringReport since it is an
  // adapter-heap property, not a per-submission one). run_representation_
  // stage() accumulates these across a plan's requests; a plan carrying no
  // request leaves all four at 0. old_backing_bytes is the backing each
  // transform superseded, counted whether it is released at once
  // (consume_input) or retained until the relevant command buffer completes
  // (06 §11's default). new_backing_bytes/temporary_bytes come from the
  // backend's own RepresentationTransformCost, so a backend whose transform
  // needs no staging copy reports 0 temporary rather than a guess, and a
  // backend with no distinct physical representation reports what it can
  // actually account for instead of a fabricated number.
  // completion_delay_ns is a real steady_clock measurement of the
  // host-observed delay between a transform becoming consume-eligible and
  // its old backing actually being released; it stays 0 when no request asked
  // for a consume, because then no release happens inside this submission at
  // all.
  uint64_t old_backing_bytes{};
  uint64_t new_backing_bytes{};
  uint64_t temporary_bytes{};
  uint64_t completion_delay_ns{};
  // The part of old_backing_bytes a ConsumeInput actually handed back inside
  // this submission, as measured by core::Arena::consume_representation()
  // rather than assumed to equal what was superseded. E005's watermark claim
  // is precisely (old_backing_bytes - released_backing_bytes): with no consume
  // the two differ by the whole superseded backing, which is the peak the
  // experiment is trying to remove.
  uint64_t released_backing_bytes{};
};

// What one request's *physical* transform cost the backend, reported back to
// the shared Stage 5 helper below. Separate from Submission because the
// epoch/facet/consume bookkeeping is core-semantic and identical everywhere,
// while these numbers are the only part that legitimately differs per backend.
// heap_fragmentation_bytes is best-effort in the same sense as
// LoweringReport's field: a backend with no real heap-fragmentation concept
// reports 0 rather than inventing one.
struct RepresentationTransformCost {
  uint64_t new_backing_bytes{};
  uint64_t temporary_bytes{};
  uint64_t heap_fragmentation_bytes{};
  bool used_device_optimal{};
  // Whether the new representation lives in storage distinct from the bytes it
  // supersedes. This is what makes ConsumeInput meaningful: releasing the old
  // backing only frees anything if the new representation is not that same
  // backing. A backend whose transform is the identity (the reference
  // interpreter: a host byte array is already its own optimal representation)
  // reports false, and run_representation_stage() then refuses a consume
  // instead of "releasing" the only copy of the data the facet points at.
  bool distinct_backing{};
};

// Runs `requests`' Stage 5 bookkeeping against `arena`/`pool`: for each
// request, advance the allocation's RepresentationEpoch
// (core::Arena::transform), retire the facets that epoch invalidated, acquire
// the requested target facet, and -- only when the request carries a complete
// ConsumeProof -- consume the old backing. Seals the resulting
// core::RepresentationEpoch and fills the Submission's byte/delay accounting.
//
// Shared rather than per-backend because only the physical transform differs
// per backend: if two backends kept their own epoch/facet/consume bookkeeping
// they could disagree about when a token goes stale, and the cross-backend
// differential of 10 §6 would be comparing two different semantics. `physical`
// is the backend's real transform work for one request. It is invoked *after*
// the target facet has been acquired and is handed that core::FacetRef,
// because on a real adapter the facet slot is the handle the backend resource
// is registered against (06 §6.4) -- a callback that ran before the acquire
// would have nowhere to publish the texture it just built. A backend with no
// physical step passes a callback that performs none and reports only what it
// can honestly account for, or an empty std::function to report all zeros.
//
// The consume is core::Arena::consume_representation(), not consume(): a
// transform supersedes an allocation's backing, it does not retire the
// allocation, and using the retiring form would stale the very facet the
// transform just published (06 §11). It runs only when the backend reported
// `distinct_backing`; a request asking to consume a transform that produced no
// distinct backing is refused rather than quietly skipped.
//
// A failure leaves `submission` claiming no successful transform.
bool run_representation_stage(const std::vector<RepresentationRequest>& requests,
                              core::Arena& arena, core::FacetPool& pool,
                              const std::function<bool(const RepresentationRequest&, core::FacetRef,
                                                       RepresentationTransformCost*,
                                                       std::string*)>& physical,
                              Submission* submission, std::string* error = nullptr);

// TASK-D3 / ADR-037: this-submit residency, not the address graph. If
// plan.working_set_lease is set, requested bytes are the sum of those
// allocations' sizes (missing/stale lookup is a refuse). Else if
// working_set_budget is set, requested bytes are Universe -- every Active
// allocation->size. A set budget that does not allow the request fails
// with WorkingSetBudget::allows's "working-set budget exceeded"; never a
// silent clamp, and unified memory is never treated as infinite. No-op
// when neither field is set, so every pre-D3 caller is unchanged.
//
// LoweringReport events (stable names): working_set_requested /
// working_set_committed / working_set_proxy. Reasons say "proxy" because
// this helper has no OS residency counter. Sparse residency is reported
// as Unsupported (Metal sparse heap/texture is not implemented; Vulkan
// sparse binding is explicit map/unmap, not automatic page fault).
bool apply_working_set_budget(const ExecutionPlan& plan, core::Arena& arena,
                              Submission* submission, std::string* error = nullptr);

// TASK-D2 / ADR-036: HostAssisted discovery stage (02 §7.2). Empty
// discovery_seeds is a no-op so pre-D2 callers are unchanged. A non-empty
// list host-walks 12-byte PointerRefs in allocation bytes, freezes
// topology_epoch, seals a certificate over seeds+reachable, and fills a
// WorkingSetLease from that set via lease.add(ref, discovered, ...).
// Witness beyond the certificate (including a plan.working_set_lease that
// names an extra allocation) is a refuse. Classification is always
// HostAssisted -- never DevicePass: this is a host walk / host
// round-trip, a semantic reachable set / proxy, not OS page migration.
// SoftwarePaged / FaultManaged stay Unsupported.
bool run_discovery_stage(const ExecutionPlan& plan, core::Arena& arena,
                         Submission* submission, std::string* error = nullptr);

// TASK-D5 / ADR-039: portable envelope continuation. Host-splits
// TaskGraph::deterministic_order (HostAssisted). ADR-010 set_quota stays
// build-time only; this helper never silently enlarges envelope_task_quota.
//
// - quota unset and no valid Deferred pending: no-op, publish_order = full
//   deterministic_order, envelope_overflow left unset
// - task count <= quota and no valid Deferred pending: publish all
// - task count > quota and no valid Deferred pending: publish first N,
//   mint a token, set submission.envelope_overflow Deferred
// - pending Deferred valid: require table token match, publish leftovers
//   only, clear leftover. A larger quota on this submit does not republish
//   the prefix.
// - pending Rejected or a bad token: refuse (distinct from
//   "publication ring quota overflow")
bool apply_envelope_continuation(const ExecutionPlan& plan,
                                 core::EnvelopeContinuationTable* table,
                                 Submission* submission,
                                 std::vector<uint32_t>* publish_order,
                                 std::string* error = nullptr);

class DeviceHal {
 public:
  virtual ~DeviceHal() = default;
  virtual const CapabilitySnapshot& capabilities() const = 0;
  virtual bool compile(const ExecutionPlan& plan, CompiledPlan* compiled,
                       std::string* error = nullptr) = 0;
  virtual bool submit(const CompiledPlan& compiled, core::Arena& arena,
                      Submission* submission, std::string* error = nullptr) = 0;

  // 06 §2 places the Facet Pool inside the adapter, and 06 §6.4 makes its
  // slots point at backend resources -- so the pool a submission's Stage 5
  // acquires into belongs to the device, not to each caller. Standalone
  // per-facet entry points still take an explicit pool so a test can drive one
  // it owns; this is the pool submit() itself uses.
  core::FacetPool& facet_pool() { return facet_pool_; }
  const core::FacetPool& facet_pool() const { return facet_pool_; }

  // Issued continuation tokens live on the device so leftover cannot be
  // stolen by a later submit that omits pending_overflow (ADR-039).
  core::EnvelopeContinuationTable& envelope_continuations() { return envelope_continuations_; }
  const core::EnvelopeContinuationTable& envelope_continuations() const {
    return envelope_continuations_;
  }

 protected:
  core::FacetPool facet_pool_;
  core::EnvelopeContinuationTable envelope_continuations_;
};

std::unique_ptr<DeviceHal> make_reference_device_hal();

}  // namespace vg::hal

#endif
