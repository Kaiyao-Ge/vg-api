#ifndef VG_BACKENDS_DEVICE_HAL_H_
#define VG_BACKENDS_DEVICE_HAL_H_

#include "core/core.h"
#include "core/execution_plan.h"
#include "compiler/compiler.h"
#include "ir/ir.h"

#include <array>
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
  // core::ExecutionPlan representation item -- publish a new
  // RepresentationEpoch, retire what it invalidated, acquire the requested
  // facet (02 §8: a transform is not a barrier) -- and to report its real
  // cost. A backend that cannot leaves the bit clear and answers with an
  // Unsupported LoweringEvent, never with a silently dropped request.
  RepresentationTransform = 1u << 7,
  // Advertising this obliges the backend to genuinely verify a FacetRef's
  // generation under ValidationProfile::CheckedNative -- in the shader on a
  // GPU backend (06 §6.4), exactly and host-side on the reference judge. A
  // backend that dereferences facets unchecked leaves the bit clear rather
  // than letting a caller believe a stale token would be caught.
  CheckedFacetGeneration = 1u << 8,
  // F3 (ADR-043 Decision #4): advertising this obliges the backend to accept
  // a per-Node user raster contract -- compiling the caller's hand-written
  // MSL against its declared effect contract only,
  // never validating shader logic -- and to classify that trust boundary as
  // HostAssisted, never a silently upgraded fully-verified status. A backend
  // with no restricted-import path leaves the bit clear.
  UserShaderImport = 1u << 9,
  // Tier 2 is GPU selection among pre-authorized Node/state buckets.  It is
  // not Tier 1 indirect dispatch, and an adapter must advertise it separately.
  IndirectTier2Select = 1u << 10,
  // Indexed bindings have their own descriptor/address-table contract; they
  // must not be inferred from (or collapsed into) LinearAddress.
  IndexedBinding = 1u << 11,
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

  [[nodiscard]] bool supports(Capability capability) const {
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
  [[nodiscard]] uint64_t count(LoweringClass classification) const;
  [[nodiscard]] bool has_hidden_host_wait() const;
  [[nodiscard]] std::string canonical_json() const;
};

struct CompiledPlan {
  uint32_t abi_version{kDeviceHalAbiVersion};
  core::ExecutionPlan plan;
  // Stage-6 output is keyed only by the complete immutable NodeRef.  A raster
  // package intentionally has no compute payload: its materialized raster
  // contract remains in plan.resolved_nodes and Stage 7 executes it only for
  // Raster Tasks naming this exact generation.
  enum class NodePackageKind : uint32_t { CanonicalCompute, Raster };
  struct PerNodePackage {
    core::NodeTable::Ref ref;
    NodePackageKind kind{NodePackageKind::CanonicalCompute};
    std::optional<compiler::ComputePackage> package;
    bool host_assisted{};
  };
  std::vector<PerNodePackage> per_node_packages;
  LoweringReport report;
  // Stage-6 result: one descriptor per frozen Stage-5 item.  It contains no
  // authority or epoch derivation; submit only executes this already selected
  // physical form and fails closed on a snapshot mismatch.
  enum class RepresentationOperation : uint32_t { Identity, CopyToPrivate, CopyToImage, Unsupported };
  struct PhysicalRepresentationOperation {
    RepresentationOperation operation{RepresentationOperation::Unsupported};
    uint32_t semantic_order{};
    std::string diagnostic;
  };
  std::vector<PhysicalRepresentationOperation> representation_operations;
};

// Stage 6 capability gate.  The plan remains core-owned; adapters supply
// their own immutable snapshot here instead of receiving a caller snapshot.
bool preflight_stage6(const core::ExecutionPlan& plan, const CapabilitySnapshot& capabilities,
                      BackendKind expected_backend, CompiledPlan* compiled,
                      std::string* error = nullptr);

// Stage 7 identity boundary.  This deliberately validates only the immutable
// compiled-plan ABI and the adapter identity recorded by Stage 6; package
// contents and plan semantics remain backend/Stage-7 concerns.
bool validate_stage7_compiled_plan(const CompiledPlan& compiled,
                                   BackendKind expected_backend,
                                   std::string* error = nullptr);

// F2 (ADR-046): the backend-neutral subset of Metal's RasterResult /
// reference's RasterResult -- both already mirror each other field for
// field, so this carries only what a caller needs to check pixels against
// the reference oracle after a plan-driven raster task runs. Per-run
// diagnostics (encoder_count, facet_cache_hit, covered_fragment_count)
// stay backend-local and observable through LoweringReport instead of
// being duplicated here.
struct RasterTaskResult {
  uint32_t task_index{};
  std::vector<std::array<float, 4>> resolved_rgba;
  // F5 vertical-slice evidence also compares the F4 depth result. This is
  // internal execution evidence, not a new public C ABI readback API.
  std::vector<float> resolved_depth;
  uint32_t width{};
  uint32_t height{};
  bool stored{};
  bool contents_defined{true};
};

// F2 (ADR-046) Decision #3: single source of truth for the fixed raster-
// attachment defaults (load=Clear, store=Store, clear_rgba={0,0,0,1},
// sample_count=1, subresource={0,0}) that F2 deliberately keeps backend-
// private rather than promoting to core (see reference::AttachmentFacetDesc /
// metal::AttachmentFacetDesc's own comments -- promoting them here would be
// exactly the "adapter feature upgraded to core minimum capability"
// anti-pattern docs/START.md §5 rules out, ADR-046 Decision #3). Templated,
// not one shared struct returned directly, because reference::
// AttachmentFacetDesc and metal::AttachmentFacetDesc are deliberately two
// distinct backend-local types (same rationale) that merely happen to share
// field names/shape -- this fills in whichever one the caller names without
// requiring them to be unified. Used by both backends' submit-path RasterDesc
// construction (reference_device_hal.cpp, metal_device_hal.mm) and both
// raster tests' oracle construction (reference_raster_test.cpp,
// metal_task_timeline_test.cpp), so the five fixed values are hand-written
// exactly once.
template <typename AttachmentFacetDescT>
constexpr AttachmentFacetDescT f2_default_raster_attachment_config() {
  AttachmentFacetDescT desc{};
  desc.load = decltype(desc.load)::Clear;
  desc.store = decltype(desc.store)::Store;
  desc.clear_rgba = {0.0f, 0.0f, 0.0f, 1.0f};
  desc.sample_count = 1;
  desc.subresource = {};
  return desc;
}

struct Submission {
  uint32_t abi_version{kDeviceHalAbiVersion};
  core::ExecutionResult result;
  uint64_t timeline_value{};
  LoweringReport report;
  // Populated only when the submitted plan had a non-empty task_graph: the
  // tasks in the order they were actually published (Empty->Writing->
  // Published->Consumed), for byte-exact cross-backend comparison.
  std::vector<core::TaskRecord> published_tasks;
  // Populated only when core::ExecutionPlan::requested_certificate_mode was set
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
  // F2: one entry per Raster-kind TaskRecord in compiled.plan.task_graph
  // that actually ran during this submit(), in task_graph.tasks() order.
  // Empty when the plan carried no raster task, same convention as
  // published_tasks staying empty for a plan with no task_graph.
  std::vector<RasterTaskResult> raster_results;
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
  // reports false, and commit_representation_operations() then refuses a consume
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
bool commit_representation_operations(const core::ExecutionPlan& plan,
                              const std::vector<CompiledPlan::PhysicalRepresentationOperation>& operations,
                              core::Arena& arena, core::FacetPool& pool,
                              const std::function<bool(const core::RepresentationSemanticPlanItem&, const CompiledPlan::PhysicalRepresentationOperation&, core::FacetRef,
                                                       RepresentationTransformCost*,
                                                       std::string*)>& physical,
                              Submission* submission, std::string* error = nullptr);

// Per-submit, completion-owned lifetime retention for the one sealed plan.
// prepare() is read-only and may run before representation operations so a
// task facet that would be invalidated by a same-submit transform is rejected
// before any epoch is changed. acquire() runs after those physical operations
// have produced their final target FacetRefs and transactionally retains the
// complete allocation/facet set. Destruction releases exactly once; today's
// synchronous backends keep the owner on the submit stack through their real
// completion wait, while the movable owner can later be handed to an async
// completion object without changing the lifetime contract.
class SubmissionLifetimeHold {
 public:
  SubmissionLifetimeHold() = default;
  SubmissionLifetimeHold(const SubmissionLifetimeHold&) = delete;
  SubmissionLifetimeHold& operator=(const SubmissionLifetimeHold&) = delete;
  SubmissionLifetimeHold(SubmissionLifetimeHold&& other) noexcept;
  SubmissionLifetimeHold& operator=(SubmissionLifetimeHold&& other) noexcept;
  ~SubmissionLifetimeHold();

  bool prepare(const core::ExecutionPlan& plan, core::Arena& arena, core::FacetPool& pool,
               std::string* error = nullptr);
  bool acquire(const std::vector<core::FacetRef>& representation_facets,
               std::string* error = nullptr);
  void release() noexcept;

  [[nodiscard]] bool prepared() const { return arena_ != nullptr && pool_ != nullptr; }
  [[nodiscard]] bool held() const { return held_; }
  [[nodiscard]] size_t allocation_count() const { return acquired_allocations_.size(); }
  [[nodiscard]] size_t facet_count() const { return acquired_facets_.size(); }

 private:
  core::Arena* arena_{};
  core::FacetPool* pool_{};
  std::vector<core::PointerRef> allocation_inventory_;
  std::vector<core::FacetLifetimeUse> facet_inventory_;
  std::vector<core::FacetKind> representation_kinds_;
  std::vector<core::PointerRef> acquired_allocations_;
  std::vector<core::FacetRef> acquired_facets_;
  bool held_{};
};

// Stage 7 physical working-set accounting. Core Stage 0--5 already selects
// a proven lease (or Universe accounting), freezes its byte count, and
// applies the hard budget refusal. HAL only records that sealed request; it
// never scans the Arena or chooses an access policy.
//
// LoweringReport events (stable names): working_set_requested /
// working_set_committed / working_set_proxy. Reasons say "proxy" because
// this helper has no OS residency counter. Sparse residency is reported
// as Unsupported (Metal sparse heap/texture is not implemented; Vulkan
// sparse binding is explicit map/unmap, not automatic page fault).
bool apply_working_set_budget(const core::ExecutionPlan& plan, core::Arena& arena,
                              Submission* submission, std::string* error = nullptr);

// Stage 7 report/physical-operation hook for core-sealed discovery. The
// host walk, topology freeze, witness/certificate/lease construction and
// authority checks are solely ExecutionPlanAssembler work. HAL records the
// selected operation as HostAssisted and must not derive another set.
bool run_discovery_stage(const core::ExecutionPlan& plan, core::Arena& arena,
                         Submission* submission, std::string* error = nullptr);

// TASK-D5 / ADR-039: portable envelope continuation. Host-splits the
// assembler-sealed plan.task_order (HostAssisted). ADR-010 set_quota stays
// build-time only; this helper never silently enlarges envelope_task_quota or
// derives a second order from the raw TaskGraph.
//
// - quota unset and no valid Deferred pending: no-op, publish_order = full
//   sealed task_order, envelope_overflow left unset
// - task count <= quota and no valid Deferred pending: publish all
// - task count > quota and no valid Deferred pending: publish first N,
//   mint a token, set submission.envelope_overflow Deferred
// - pending Deferred valid: require table token match, publish leftovers
//   only, clear leftover. A larger quota on this submit does not republish
//   the prefix.
// - pending Rejected or a bad token: refuse (distinct from
//   "publication ring quota overflow")
bool apply_envelope_continuation(const core::ExecutionPlan& plan,
                                 core::EnvelopeContinuationTable* table,
                                 Submission* submission,
                                 std::vector<uint32_t>* publish_order,
                                 std::string* error = nullptr);

class DeviceHal {
 public:
  DeviceHal() = default;
  DeviceHal(const DeviceHal&) = delete;
  DeviceHal& operator=(const DeviceHal&) = delete;
  DeviceHal(DeviceHal&&) = delete;
  DeviceHal& operator=(DeviceHal&&) = delete;
  virtual ~DeviceHal() = default;
  [[nodiscard]] virtual const CapabilitySnapshot& capabilities() const = 0;
  virtual bool compile(const core::ExecutionPlan& plan, CompiledPlan* compiled,
                       std::string* error = nullptr) = 0;
  virtual bool submit(const CompiledPlan& compiled, core::Arena& arena,
                      Submission* submission, std::string* error = nullptr) = 0;

  // 06 §2 places the Facet Pool inside the adapter, and 06 §6.4 makes its
  // slots point at backend resources -- so the pool a submission's Stage 5
  // acquires into belongs to the device, not to each caller. Standalone
  // per-facet entry points still take an explicit pool so a test can drive one
  // it owns; this is the pool submit() itself uses.
  core::FacetPool& facet_pool() { return facet_pool_; }
  [[nodiscard]] const core::FacetPool& facet_pool() const { return facet_pool_; }

  // Issued continuation tokens live on the device so leftover cannot be
  // stolen by a later submit that omits pending_overflow (ADR-039).
  core::EnvelopeContinuationTable& envelope_continuations() { return envelope_continuations_; }
  [[nodiscard]] const core::EnvelopeContinuationTable& envelope_continuations() const {
    return envelope_continuations_;
  }

 protected:
  core::FacetPool facet_pool_;
  core::EnvelopeContinuationTable envelope_continuations_;
};

std::unique_ptr<DeviceHal> make_reference_device_hal();

}  // namespace vg::hal

#endif
