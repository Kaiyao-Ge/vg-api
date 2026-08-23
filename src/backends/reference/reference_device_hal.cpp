#include "backends/reference/reference_device_hal.h"

#include "backends/reference/reference_executor.h"

#include <chrono>

namespace vg::reference {
namespace {
class ReferenceDeviceHal final : public hal::DeviceHal {
 public:
  ReferenceDeviceHal() {
    capabilities_.backend = hal::BackendKind::Reference;
    capabilities_.adapter_name = "VG CPU Reference";
    capabilities_.driver = "phase-b-reference";
    capabilities_.capability_bits = static_cast<uint64_t>(hal::Capability::LinearAddress) |
        static_cast<uint64_t>(hal::Capability::TaskPublication) |
        static_cast<uint64_t>(hal::Capability::Timeline) |
        static_cast<uint64_t>(hal::Capability::EffectDag) |
        static_cast<uint64_t>(hal::Capability::CaptureReplay) |
        static_cast<uint64_t>(hal::Capability::IndirectTier1) |
        // Raster: reference_executor's raster_triangles() is a real software
        // rasterizer, so AttachmentWrite work is executed rather than
        // approximated. RepresentationTransform: Stage 5's epoch/facet/consume
        // bookkeeping is performed in full below. CheckedFacetGeneration: as
        // the semantic judge (03 §13) this backend checks a FacetRef's
        // generation exactly and host-side, which is stronger than the
        // in-shader check 06 §6.4 asks a GPU adapter for.
        static_cast<uint64_t>(hal::Capability::Raster) |
        static_cast<uint64_t>(hal::Capability::RepresentationTransform) |
        static_cast<uint64_t>(hal::Capability::CheckedFacetGeneration);
    capabilities_.max_buffer_size = UINT64_MAX;
    capabilities_.address_width = 64;
    capabilities_.min_buffer_alignment = 1;
    capabilities_.validation_available = true;
    capabilities_.timestamps_available = false;
  }

  const hal::CapabilitySnapshot& capabilities() const override { return capabilities_; }

  bool compile(const hal::ExecutionPlan& plan, hal::CompiledPlan* compiled,
               std::string* error) override {
    if (compiled == nullptr) { if (error) *error = "compiled plan output is required"; return false; }
    if (!plan.validate(error)) return false;
    if (plan.requested_certificate_mode == core::AccessCertificateMode::SoftwarePaged ||
        plan.requested_certificate_mode == core::AccessCertificateMode::FaultManaged) {
      compiled->abi_version = hal::kDeviceHalAbiVersion;
      compiled->plan = plan;
      compiled->report = {};
      compiled->report.backend = hal::BackendKind::Reference;
      compiled->report.supported = false;
      compiled->report.diagnostic = "requested access certificate mode is not implemented on this backend";
      compiled->report.add("access_certificate", hal::LoweringClass::Unsupported, 1, 0, compiled->report.diagnostic);
      if (error) *error = compiled->report.diagnostic;
      return false;
    }
    const auto package = compiler::build_linear_compute_package(plan.module);
    if (!package.ok) { if (error) *error = package.message; return false; }
    compiled->abi_version = hal::kDeviceHalAbiVersion;
    compiled->plan = plan;
    compiled->compute_package = package.package;
    compiled->report = {};
    compiled->report.backend = hal::BackendKind::Reference;
    compiled->report.supported = true;
    compiled->report.add("canonical_ir", hal::LoweringClass::Direct, 1, plan.module.instructions.size(), "reference interpreter");
    compiled->report.add("compute_package", hal::LoweringClass::Direct, 1, package.package.bindings.size(), "shared B4 linear package");
    compiled->report.add("linear_access", hal::LoweringClass::Direct, plan.module.instructions.size(), 0, "checked arena access");
    if (!plan.task_graph.tasks().empty()) compiled->report.add("task_publication", hal::LoweringClass::Direct, plan.task_graph.tasks().size(), 0, "immutable task graph");
    if (plan.timeline_signal != 0) compiled->report.add("timeline", hal::LoweringClass::Direct, 1, 0, "reference monotonic timeline");
    // plan.validate() above already rejected a malformed request set, so every
    // request reaching here is one this backend will really perform. Classified
    // Direct rather than DevicePass: a host byte array is already its own
    // optimal representation, so the accepted work is genuine Stage 5
    // epoch/facet/consume bookkeeping with no device pass behind it. Claiming
    // DevicePass would report a pass that does not exist; claiming Unsupported
    // would deny bookkeeping this backend does carry out.
    for (const auto& request : plan.representation_requests) {
      compiled->report.add("representation_transform", hal::LoweringClass::Direct, 1,
                           request.view.byte_size(),
                           "RepresentationEpoch/facet bookkeeping only; the host byte array is already the "
                           "reference backend's optimal representation, so no device pass is emitted");
      // ConsumeInput buys exactly one thing (06 §11): the superseded backing is
      // released at once rather than retained until completion. This backend's
      // transform is the identity -- the new representation *is* the host byte
      // array it supersedes -- so there is no superseded backing to hand back,
      // and "releasing" it would delete the data the freshly published facet
      // points at. Reported as an explicit Unsupported with the reason rather
      // than accepted and quietly not performed (START.md §4, invariant 10);
      // E005's watermark comparison is a Metal measurement for the same reason.
      if (request.consume_input) {
        compiled->representation_supported = false;
        compiled->report.supported = false;
        compiled->report.diagnostic =
            "ConsumeInput is not available on the reference backend: its representation transform is "
            "the identity, so no backing is superseded and nothing can be released early";
        compiled->report.add("consume_input", hal::LoweringClass::Unsupported, 1, 0,
                             compiled->report.diagnostic);
        if (error) *error = compiled->report.diagnostic;
        return false;
      }
    }
    return true;
  }

  bool submit(const hal::CompiledPlan& compiled, core::Arena& arena,
              hal::Submission* submission, std::string* error) override {
    if (submission == nullptr) { if (error) *error = "submission output is required"; return false; }
    if (!compiled.report.supported) { if (error) *error = "compiled plan is unsupported"; return false; }
    if (!compiled.plan.graph_epoch_matches(arena, error)) return false;
    submission->abi_version = hal::kDeviceHalAbiVersion;
    submission->report = compiled.report;
    // Stage 5 precedes Stage 6/7 (03 §7), and runs outside the cpu_submit_ns
    // window below so that counter keeps meaning exactly the interpreter's own
    // wall clock. The physical step reports what this backend can actually
    // account for: the new representation is the same host byte array the view
    // already names, reached with no staging copy and no device-optimal pass.
    std::string representation_error;
    if (!hal::run_representation_stage(
            compiled.plan.representation_requests, arena, facet_pool(),
            [](const hal::RepresentationRequest& request, core::FacetRef,
               hal::RepresentationTransformCost* cost, std::string*) {
              cost->new_backing_bytes = request.view.byte_size();
              cost->temporary_bytes = 0;
              cost->used_device_optimal = false;
              cost->distinct_backing = false;
              return true;
            },
            submission, &representation_error)) {
      if (error) *error = representation_error;
      return false;
    }
    // TASK-B12: this backend has no encoder/command-buffer/barrier concept
    // (it's a plain CPU interpreter), so those counters honestly stay 0;
    // cpu_submit_ns is the real wall-clock time of the interpreter run(s)
    // below, and cpu_encode_ns stays 0 since there is no separate encode
    // phase to distinguish it from.
    const auto submit_start = std::chrono::steady_clock::now();
    submission->result = execute(compiled.plan.module, arena,
                                 compiled.plan.certificate.ranges.empty() ? nullptr : &compiled.plan.certificate,
                                 &timeline_, compiled.plan.timeline_wait, compiled.plan.timeline_signal);
    submission->timeline_value = timeline_.value();
    if (!submission->result.ok) {
      submission->cpu_submit_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submit_start).count();
      return true;
    }
    if (!compiled.plan.task_graph.tasks().empty()) {
      auto task_result = execute_task_graph(compiled.plan.task_graph);
      if (!task_result.ok) {
        submission->result.ok = false;
        submission->result.message = task_result.message;
        submission->cpu_submit_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submit_start).count();
        return true;
      }
      submission->published_tasks = std::move(task_result.published_tasks);
    }
    submission->cpu_submit_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submit_start).count();
    if (compiled.plan.requested_certificate_mode.has_value()) {
      std::vector<core::PointerRef> touched;
      for (const auto& instruction : compiled.plan.module.instructions) {
        touched.push_back(core::PointerRef{instruction.allocation, instruction.generation});
      }
      core::AccessCertificate certificate;
      std::string cert_error;
      if (core::build_access_certificate(arena, *compiled.plan.requested_certificate_mode, touched, &certificate, &cert_error)) {
        submission->access_certificate = certificate;
        const auto classification = *compiled.plan.requested_certificate_mode == core::AccessCertificateMode::DiscoverThenLease
            ? hal::LoweringClass::HostAssisted : hal::LoweringClass::Direct;
        submission->report.add("access_certificate", classification, certificate.epoch.references().size(),
                               certificate.result_bytes, "reference arena scan");
      }
    }
    return true;
  }

 private:
  hal::CapabilitySnapshot capabilities_;
  core::Timeline timeline_;
};
}

std::unique_ptr<hal::DeviceHal> make_device_hal() { return std::make_unique<ReferenceDeviceHal>(); }
}
