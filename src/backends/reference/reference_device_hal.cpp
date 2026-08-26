#include "backends/reference/reference_device_hal.h"

#include "backends/reference/reference_executor.h"
#include "backends/reference/tier2_oracle.h"

#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

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
        static_cast<uint64_t>(hal::Capability::CheckedFacetGeneration) |
        // F3 (ADR-043 Decision #4): this backend accepts an
        // ExecutionPlan::user_raster_shader submission (compile()/submit()
        // below), but only against its declared effect contract -- it never
        // interprets the supplied MSL text, applying its own fixed C++
        // shading instead and disclosing that as HostAssisted.
        static_cast<uint64_t>(hal::Capability::UserShaderImport);
    capabilities_.max_buffer_size = UINT64_MAX;
    capabilities_.address_width = 64;
    capabilities_.min_buffer_alignment = 1;
    capabilities_.validation_available = true;
    capabilities_.timestamps_available = false;
  }

  [[nodiscard]] const hal::CapabilitySnapshot& capabilities() const override { return capabilities_; }

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
    if (plan.user_raster_shader.has_value()) {
      // F3 (ADR-043 Decision #4): plan.module is default/empty in this mode
      // (vg_api_execution.cpp's submit() leaves it unset when the code
      // object is vg.msl.raster/v1), so building a compute package from it
      // would be meaningless -- skip straight to the disclosure event below.
      // HostAssisted, not Direct: this backend accepts the caller's declared
      // root_schema/entry points but never interprets the supplied MSL text;
      // submit() below still runs its own fixed C++ shading regardless of
      // what that MSL says (unlike Metal, which really compiles and runs it).
      compiled->abi_version = hal::kDeviceHalAbiVersion;
      compiled->plan = plan;
      compiled->report = {};
      compiled->report.backend = hal::BackendKind::Reference;
      compiled->report.supported = true;
      compiled->report.add("raster_user_shader", hal::LoweringClass::HostAssisted, 1,
                           plan.user_raster_shader->source.size(),
                           "caller-declared effect contract accepted; shader logic not independently verified; "
                           "reference backend applies fixed C++ shading regardless of supplied MSL text");
    } else {
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
    }
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
    // TASK-D2 / ADR-036: DiscoverThenLease walk when seeds are set (02 §7.2).
    // Empty seeds leave the B-era full-arena scan below unchanged.
    if (!hal::run_discovery_stage(compiled.plan, arena, submission, error)) return false;
    // TASK-D3 / ADR-037: this-submit residency is not the address graph.
    // A set working_set_budget that the requested bytes exceed is a hard
    // refuse -- never a silent clamp, and never "unified memory is infinite".
    if (!hal::apply_working_set_budget(compiled.plan, arena, submission, error)) return false;
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
    if (compiled.plan.user_raster_shader.has_value()) {
      // F3 (ADR-043 Decision #4): compiled.plan.module is default/empty in
      // this mode, so there is no compute-module work for execute() to run --
      // calling it would additionally trip its internal ir::verify() rejection
      // of an empty module. Synthesize the trivial success result the raster
      // block below expects; the actual raster shading happens there,
      // unchanged, via raster_triangles().
      submission->result = core::ExecutionResult{};
      submission->result.ok = true;
    } else {
      submission->result = execute(compiled.plan.module, arena,
                                   compiled.plan.certificate.ranges.empty() ? nullptr : &compiled.plan.certificate,
                                   &timeline_, {.wait = compiled.plan.timeline_wait,
                                                .signal = compiled.plan.timeline_signal});
    }
    submission->timeline_value = timeline_.value();
    if (!submission->result.ok) {
      submission->cpu_submit_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submit_start).count();
      return true;
    }
    if (!compiled.plan.task_graph.tasks().empty()) {
      // TASK-D5 / ADR-039: host-split on deterministic_order. Unset quota
      // keeps the pre-D5 full publish. A set cap publishes a prefix and
      // parks leftover under a device token; the next submit must present
      // that record as pending_overflow.
      std::vector<uint32_t> publish_order;
      if (!hal::apply_envelope_continuation(compiled.plan, &envelope_continuations_, submission,
                                            &publish_order, error)) {
        submission->cpu_submit_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submit_start).count();
        return false;
      }
      if (!publish_order.empty()) {
        std::string task_error;
        core::PublicationRing ring(static_cast<uint32_t>(publish_order.size()));
        const auto& tasks = compiled.plan.task_graph.tasks();
        submission->published_tasks.clear();
        submission->published_tasks.reserve(publish_order.size());
        for (uint32_t index : publish_order) {
          uint32_t slot = 0;
          if (index >= tasks.size() || !ring.publish_task(tasks[index], &slot, &task_error) ||
              !ring.consume(slot, &task_error)) {
            submission->result.ok = false;
            submission->result.message = task_error.empty() ? "envelope task index is out of range"
                                                            : task_error;
            submission->cpu_submit_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                    submit_start)
                    .count();
            return true;
          }
          submission->published_tasks.push_back(tasks[index]);
        }
      }
      // F2 (ADR-043 Decision #3): a Raster task's vertex bytes are only
      // guaranteed visible in `arena` once execute() above has returned, so
      // this walks the sealed task graph now rather than during publish
      // above. task_index reports position in the task graph itself (not
      // publish order), matching RasterTaskResult's contract. `tasks` above
      // is scoped to the publish_order block, so re-fetch here.
      const auto& all_tasks = compiled.plan.task_graph.tasks();
      for (uint32_t task_index = 0; task_index < all_tasks.size(); ++task_index) {
        const core::TaskRecord& task = all_tasks[task_index];
        if (task.kind != core::TaskKind::Raster) continue;
        core::FacetStatus vertex_status = core::FacetStatus::Ok;
        const core::FacetSlot* vertex_slot = facet_pool().lookup(arena, task.vertex_buffer_ref, &vertex_status);
        if (vertex_slot == nullptr || vertex_slot->kind != core::FacetKind::Address) {
          submission->result.ok = false;
          submission->result.message = vertex_slot == nullptr
              ? std::string("raster task vertex buffer: ") + core::to_string(vertex_status)
              : std::string("raster task vertex buffer: facet kind mismatch");
          submission->cpu_submit_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - submit_start)
                                          .count();
          return true;
        }
        auto* vertex_allocation = arena.lookup(
            core::PointerRef{vertex_slot->view.allocation, vertex_slot->view.allocation_generation});
        if (vertex_allocation == nullptr) {
          submission->result.ok = false;
          submission->result.message = "raster task vertex buffer: allocation not found";
          submission->cpu_submit_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - submit_start)
                                          .count();
          return true;
        }
        if (vertex_allocation->bytes.size() % sizeof(RasterVertex) != 0) {
          submission->result.ok = false;
          submission->result.message = "raster task vertex buffer byte size is not a multiple of sizeof(RasterVertex)";
          submission->cpu_submit_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - submit_start)
                                          .count();
          return true;
        }
        const size_t vertex_count = vertex_allocation->bytes.size() / sizeof(RasterVertex);
        std::vector<RasterVertex> vertices(vertex_count);
        if (vertex_count > 0) {
          std::memcpy(vertices.data(), vertex_allocation->bytes.data(), vertex_count * sizeof(RasterVertex));
        }
        if (task.index_count != 0) {
          core::FacetStatus index_status = core::FacetStatus::Ok;
          const core::FacetSlot* index_slot = facet_pool().lookup(arena, task.index_buffer_ref, &index_status);
          if (index_slot == nullptr || index_slot->kind != core::FacetKind::Address) {
            submission->result.ok = false;
            submission->result.message = index_slot == nullptr
                ? std::string("raster task index buffer: ") + core::to_string(index_status)
                : "raster task index buffer: facet kind mismatch";
            return true;
          }
          const core::PixelFormat format = index_slot->view.format;
          const size_t element_size = format == core::PixelFormat::R16Uint ? sizeof(uint16_t) :
                                      format == core::PixelFormat::R32Uint ? sizeof(uint32_t) : 0;
          if (element_size == 0 || task.index_count % 3 != 0 ||
              task.index_count > std::numeric_limits<size_t>::max() / element_size) {
            submission->result.ok = false;
            submission->result.message = "raster task index buffer requires R16Uint/R32Uint and a triangle-list count";
            return true;
          }
          auto* index_allocation = arena.lookup(
              core::PointerRef{index_slot->view.allocation, index_slot->view.allocation_generation});
          const size_t byte_count = static_cast<size_t>(task.index_count) * element_size;
          if (index_allocation == nullptr || index_allocation->bytes.size() < byte_count) {
            submission->result.ok = false;
            submission->result.message = "raster task index buffer is shorter than index_count";
            return true;
          }
          std::vector<RasterVertex> indexed;
          indexed.reserve(task.index_count);
          for (uint32_t i = 0; i < task.index_count; ++i) {
            uint32_t index = 0;
            if (element_size == sizeof(uint16_t)) {
              uint16_t value{};
              std::memcpy(&value, index_allocation->bytes.data() + i * element_size, sizeof(value));
              index = value;
            } else {
              std::memcpy(&index, index_allocation->bytes.data() + i * element_size, sizeof(index));
            }
            if (index >= vertices.size()) {
              submission->result.ok = false;
              submission->result.message = "raster task index references a vertex outside the vertex buffer";
              return true;
            }
            indexed.push_back(vertices[index]);
          }
          vertices = std::move(indexed);
        }
        RasterDesc desc;
        desc.attachment = hal::f2_default_raster_attachment_config<AttachmentFacetDesc>();
        desc.filter = task.raster_filter;
        desc.wrap = task.raster_wrap;
        desc.tint = task.raster_tint;
        desc.depth_attachment_ref = task.depth_attachment_ref;
        desc.depth_test_enable = task.depth_test_enable;
        desc.depth_write_enable = task.depth_write_enable;
        desc.depth_compare_op = task.depth_compare_op;
        const RasterResult raster_result = raster_triangles(arena, facet_pool(), task.raster_facets, desc, vertices);
        if (!raster_result.ok) {
          submission->result.ok = false;
          submission->result.message = raster_result.message;
          submission->cpu_submit_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - submit_start)
                                          .count();
          return true;
        }
        hal::RasterTaskResult task_result;
        task_result.task_index = task_index;
        task_result.resolved_rgba = raster_result.resolved_rgba;
        task_result.resolved_depth = raster_result.resolved_depth;
        task_result.width = raster_result.width;
        task_result.height = raster_result.height;
        task_result.stored = raster_result.stored;
        task_result.contents_defined = raster_result.contents_defined;
        submission->raster_results.push_back(std::move(task_result));
      }
      if (compiled.plan.request_tier2_select) {
        // Host-walk of the sealed graph: Serialized, never DevicePass.
        // The Metal path is the emulated device pass; this backend is the
        // byte-level judge that path must match as a multiset.
        const auto selected =
            select_tier2_nodes(compiled.plan.task_graph, compiled.plan.authorized_node_classes);
        if (!selected.ok) {
          submission->result.ok = false;
          submission->result.message = selected.message;
          submission->report.add("tier2_node_select",
                                 selected.unauthorized ? hal::LoweringClass::Unsupported
                                                       : hal::LoweringClass::Serialized,
                                 1, 0, selected.message);
          submission->cpu_submit_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - submit_start)
                                          .count();
          return true;
        }
        submission->report.add("tier2_node_select", hal::LoweringClass::Serialized, selected.command_count,
                               0, "CPU oracle host-walk of authorized node classes; not a device pass");
        submission->report.add("tier2_bucket_count", hal::LoweringClass::Serialized, selected.bucket_count, 0,
                               "one host bucket per authorized node class");
      }
    }
    submission->cpu_submit_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submit_start).count();
    // B-era certificate path. A non-empty discovery_seeds list already
    // attached the discovered (strict subset) certificate above; running
    // build_access_certificate here would overwrite it with the historical
    // full-arena DiscoverThenLease scan (ADR-025 / ADR-035).
    if (compiled.plan.requested_certificate_mode.has_value() && compiled.plan.discovery_seeds.empty()) {
      std::vector<core::PointerRef> touched;
      touched.reserve(compiled.plan.module.instructions.size());
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
