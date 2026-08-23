#ifndef VG_BACKENDS_DEVICE_HAL_H_
#define VG_BACKENDS_DEVICE_HAL_H_

#include "core/core.h"
#include "compiler/compiler.h"
#include "ir/ir.h"

#include <cstdint>
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
  // Per-submission representation-transform accounting. Populated only when
  // a submission itself performs a representation transform; left at 0
  // otherwise, never fabricated. No backend does that yet: today's transform
  // is a standalone device method that reports through its own result, the
  // same shape run_cull_compact() already uses, so these stay zero until a
  // transform is folded into an ExecutionPlan. old_backing_bytes/
  // new_backing_bytes are the retiring and newly-created backing
  // allocations' sizes; temporary_bytes is any transient staging allocation
  // live only for the duration of the transform (0 if it needed none).
  // completion_delay_ns is the real host-observed wall-clock delay between
  // the transform becoming consume-eligible (in_flight drops to 0) and the
  // old backing actually being released -- 0 when released immediately.
  uint64_t old_backing_bytes{};
  uint64_t new_backing_bytes{};
  uint64_t temporary_bytes{};
  uint64_t completion_delay_ns{};
};

class DeviceHal {
 public:
  virtual ~DeviceHal() = default;
  virtual const CapabilitySnapshot& capabilities() const = 0;
  virtual bool compile(const ExecutionPlan& plan, CompiledPlan* compiled,
                       std::string* error = nullptr) = 0;
  virtual bool submit(const CompiledPlan& compiled, core::Arena& arena,
                      Submission* submission, std::string* error = nullptr) = 0;
};

std::unique_ptr<DeviceHal> make_reference_device_hal();

}  // namespace vg::hal

#endif
