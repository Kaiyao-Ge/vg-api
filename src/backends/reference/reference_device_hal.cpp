#include "backends/reference/reference_device_hal.h"

#include "backends/reference/reference_executor.h"
#include "backends/reference/tier2_oracle.h"
#include "core/scene_root.h"

#include <algorithm>
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
        // per-Node user raster contract (compile()/submit() below), but only
        // against its declared effect contract -- it never
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

  bool compile(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
               std::string* error) override {
    if (compiled == nullptr) { if (error) *error = "compiled plan output is required"; return false; }
    *compiled = {};
    if (!plan.validate(error)) return false;
    if (!hal::preflight_stage6(plan, capabilities_, hal::BackendKind::Reference, compiled, error)) return false;
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
    compiled->abi_version = hal::kDeviceHalAbiVersion;
    compiled->plan = plan;
    compiled->report = {};
    compiled->report.backend = hal::BackendKind::Reference;
    compiled->report.supported = true;
    // ADR-054 opens canonical mixed execution, not the restricted-MSL
    // contract. Keep its raster-only narrowing even for fixed C++ shading.
    const bool has_compute = std::ranges::any_of(plan.task_graph.tasks(), [](const auto& task) {
      return task.kind == core::TaskKind::Compute;
    });
    if (has_compute) {
      for (const auto& node : plan.resolved_nodes) {
        if (node.execution_domain != core::TaskKind::Raster || !node.user_raster_shader) continue;
        compiled->report.supported = false;
        compiled->report.diagnostic =
            "Reference Unsupported: restricted user raster NodeRef{" +
            std::to_string(node.ref.index) + "," + std::to_string(node.ref.generation) +
            "} domain=Raster cannot participate in a mixed-domain ExecutionSchedule";
        compiled->report.add("mixed_domain_user_raster_shader", hal::LoweringClass::Unsupported,
                             1, 0, compiled->report.diagnostic);
        if (error) *error = compiled->report.diagnostic;
        return false;
      }
    }
    // The reference backend is a deterministic CPU interpreter.  It does not
    // invent GPU synchronization. Only a non-prelude wave dependency is a
    // physical serialized fallback; prelude facet/representation admission is
    // bookkeeping, not an execution boundary.
    for (auto& transition : compiled->transition_operations) {
      transition.state = hal::CompiledPlan::TransitionLoweringState::Lowered;
      if (transition.covers_execution_completion) {
        transition.serialized_fallback = true;
        ++compiled->report.transition_serialized_fallback_count;
      }
    }
    if (!plan.execution_schedule.task_order.empty())
      compiled->report.add("schedule_program_order", hal::LoweringClass::Serialized,
                           plan.execution_schedule.task_order.size(), 0,
                           "reference CPU interpreter serializes sealed schedule tasks in program order");
    for (const auto& node : plan.resolved_nodes) {
      hal::CompiledPlan::PerNodePackage per_node;
      per_node.ref = node.ref;
      const auto task = std::ranges::find_if(plan.task_graph.tasks(), [&](const auto& candidate) {
        return candidate.node_index == node.ref.index &&
               candidate.node_generation == node.ref.generation;
      });
      if (task == plan.task_graph.tasks().end()) {
        if (error) *error = "reference lowering found a resolved Node with no Task";
        return false;
      }
      if (task->kind == core::TaskKind::Raster) {
        per_node.kind = hal::CompiledPlan::NodePackageKind::Raster;
        if (node.user_raster_shader.has_value()) {
          per_node.host_assisted = true;
          compiled->report.add("raster_user_shader", hal::LoweringClass::HostAssisted, 1,
                               node.user_raster_shader->source.size(),
                               "caller-declared effect contract accepted; shader logic not independently verified; "
                               "reference backend applies fixed C++ shading regardless of supplied MSL text");
        } else {
          compiled->report.add("node_raster_package", hal::LoweringClass::Direct, 1, 0,
                               "canonical NodeRef materialized as the built-in reference raster contract");
        }
      } else if (node.module.has_value()) {
        const bool pointer_graph = std::ranges::any_of(node.module->instructions,
            [](const ir::Instruction& instruction) {
              return instruction.op == "load_ref" || instruction.op == "load_via" ||
                     instruction.op == "store_via";
            });
        const auto package = pointer_graph
            ? compiler::build_pointer_graph_compute_package(*node.module)
            : compiler::build_linear_compute_package(*node.module);
        if (!package.ok) {
          compiled->report.supported = false;
          compiled->report.diagnostic = "reference per-Node package compilation failed: " + package.message;
          compiled->report.add("node_compute_package", hal::LoweringClass::Unsupported,
                               1, 0, compiled->report.diagnostic);
          if (error) *error = compiled->report.diagnostic;
          return false;
        }
        per_node.kind = hal::CompiledPlan::NodePackageKind::CanonicalCompute;
        per_node.package = package.package;
        compiled->report.add("node_compute_package", hal::LoweringClass::Direct, 1,
                             package.package.bindings.size(), "reference per-Node compute package");
      } else {
        if (error) *error = "reference lowering found a resolved Node without a program";
        return false;
      }
      compiled->per_node_packages.push_back(std::move(per_node));
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
    for (const auto& request : plan.representation_plan) {
      compiled->representation_operations.push_back({hal::CompiledPlan::RepresentationOperation::Identity,
                                                     request.transform_order, "reference identity representation"});
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
    // Stage 7 consumes an immutable Stage-6 artifact.  Reject an ABI/backend
    // mismatch before reading its report, plan, or per-Node packages.
    if (!hal::validate_stage7_compiled_plan(compiled, hal::BackendKind::Reference, error)) return false;
    if (!compiled.report.supported) { if (error) *error = "compiled plan is unsupported"; return false; }
    if (!compiled.plan.validate(error)) return false;
    if (compiled.per_node_packages.size() != compiled.plan.resolved_nodes.size()) {
      if (error) *error = "compiled plan does not contain exactly one package per resolved NodeRef";
      return false;
    }
    for (const auto& node : compiled.plan.resolved_nodes) {
      const size_t matches = std::count_if(compiled.per_node_packages.begin(),
                                           compiled.per_node_packages.end(), [&](const auto& package) {
        return package.ref.index == node.ref.index && package.ref.generation == node.ref.generation;
      });
      if (matches != 1) {
        if (error) *error = "compiled plan contains a duplicate or missing NodeRef package";
        return false;
      }
      const auto package = std::ranges::find_if(compiled.per_node_packages, [&](const auto& candidate) {
        return candidate.ref.index == node.ref.index && candidate.ref.generation == node.ref.generation;
      });
      const auto task = std::ranges::find_if(compiled.plan.task_graph.tasks(), [&](const auto& candidate) {
        return candidate.node_index == node.ref.index && candidate.node_generation == node.ref.generation;
      });
      const bool raster = task != compiled.plan.task_graph.tasks().end() &&
                          task->kind == core::TaskKind::Raster;
      if (package == compiled.per_node_packages.end() ||
          (raster && (package->kind != hal::CompiledPlan::NodePackageKind::Raster ||
                      package->package.has_value())) ||
          (!raster && (package->kind != hal::CompiledPlan::NodePackageKind::CanonicalCompute ||
                       !package->package.has_value()))) {
        if (error) *error = "compiled NodeRef package kind disagrees with the sealed Task domain";
        return false;
      }
      // Package identity is a Stage-7 admission condition, not a per-Task
      // runtime check: a tampered package must not allow an earlier Task to
      // write output before a later Task discovers it.
      if (!raster &&
          (package->package->canonical_ir_hash != node.module->hash ||
           package->package->root_schema != node.module->root_schema)) {
        if (error) *error = "compiled per-Node package disagrees with the resolved immutable module";
        return false;
      }
    }
    if (!compiled.plan.graph_epoch_matches(arena, error)) return false;
    *submission = {};
    submission->abi_version = hal::kDeviceHalAbiVersion;
    submission->report = compiled.report;
    // The compiled schedule describes planned work. Submission reports only
    // reached interpreter steps and wave boundaries, retaining Node compile
    // history without counting cancelled/unstarted work as execution.
    submission->report.transition_serialized_fallback_count = 0;
    std::erase_if(submission->report.events, [](const auto& event) {
      return event.operation == "schedule_program_order";
    });
    // TASK-D2 / ADR-036: record the core-sealed discovery/access operation;
    // this backend does not rescan the Arena to derive another touched set.
    if (!hal::run_discovery_stage(compiled.plan, arena, submission, error)) return false;
    // TASK-D3 / ADR-037: this-submit residency is not the address graph.
    // A set working_set_budget that the requested bytes exceed is a hard
    // refuse -- never a silent clamp, and never "unified memory is infinite".
    if (!hal::apply_working_set_budget(compiled.plan, arena, submission, error)) return false;
    // Preserve Timeline admission before consuming a continuation token.
    if (compiled.plan.timeline_wait != 0) {
      std::string wait_error;
      if (!timeline_.validate_wait(compiled.plan.timeline_wait, &wait_error)) {
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.poison = core::PoisonState::Poisoned;
        submission->result.message = wait_error;
        submission->result.fault.code = "TIMELINE_WAIT_UNSATISFIED";
        submission->result.fault.message = wait_error;
        submission->timeline_value = timeline_.value();
        return true;
      }
    }
    // Admission must precede representation/lifetime side effects. Keep the
    // single helper's result for publication; never mint/take a token twice.
    std::vector<uint32_t> publish_order;
    if (!hal::apply_envelope_continuation(compiled.plan, &envelope_continuations_, submission,
                                          &publish_order, error)) return false;
    hal::SubmissionLifetimeHold lifetime_hold;
    if (!lifetime_hold.prepare(compiled.plan, arena, facet_pool(), error)) return false;
    // Stage 5 precedes Stage 6/7 (03 §7), and runs outside the cpu_submit_ns
    // window below so that counter keeps meaning exactly the interpreter's own
    // wall clock. The physical step reports what this backend can actually
    // account for: the new representation is the same host byte array the view
    // already names, reached with no staging copy and no device-optimal pass.
    std::string representation_error;
    if (!hal::commit_representation_operations(
            compiled.plan, compiled.representation_operations, arena, facet_pool(),
            [](const core::RepresentationSemanticPlanItem& request, const hal::CompiledPlan::PhysicalRepresentationOperation&, core::FacetRef,
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
    if (!lifetime_hold.acquire(submission->representation_facets, error)) return false;
    // TASK-B12: this backend has no encoder/command-buffer/barrier concept
    // (it's a plain CPU interpreter), so those counters honestly stay 0;
    // cpu_submit_ns is the real wall-clock time of the interpreter run(s)
    // below, and cpu_encode_ns stays 0 since there is no separate encode
    // phase to distinguish it from.
    const auto submit_start = std::chrono::steady_clock::now();
    submission->result = core::ExecutionResult{};
    submission->result.ok = true;
    const auto& tasks = compiled.plan.task_graph.tasks();
    const auto record_raster_failure = [&](uint32_t task_index, std::string code,
                                           std::string message) {
      auto& result = submission->result;
      if (code.empty()) code = "RASTER_TASK_FAILED";
      if (message.empty()) message = "reference raster task failed";
      result.ok = false;
      result.outputs_valid = false;
      result.message = std::move(message);
      result.fault.task_index = task_index;
      result.fault.code = std::move(code);
      result.fault.message = result.message;
      result.poison = core::PoisonState::Poisoned;
    };
    const auto execute_compute_task = [&](uint32_t task_index) {
        const auto& task = tasks[task_index];
        const core::NodeTable::Ref ref{task.node_index, task.node_generation};
        const auto found = std::ranges::find_if(compiled.plan.resolved_nodes, [ref](const auto& node) {
          return node.ref.index == ref.index && node.ref.generation == ref.generation;
        });
        if (found == compiled.plan.resolved_nodes.end()) {
          submission->result.ok = false;
          submission->result.message = "reference per-Node execution could not resolve the Task NodeRef";
          return false;
        }
        const auto package = std::ranges::find_if(compiled.per_node_packages, [ref](const auto& item) {
          return item.ref.index == ref.index && item.ref.generation == ref.generation;
        });
        if (package == compiled.per_node_packages.end()) {
          submission->result.ok = false;
          submission->result.message = "reference per-Node execution package is missing";
          return false;
        }
        if (task.kind == core::TaskKind::Compute) {
          if (!found->module.has_value() ||
              package->kind != hal::CompiledPlan::NodePackageKind::CanonicalCompute) {
            submission->result.ok = false;
            submission->result.message = "reference compute Task resolved a non-compute NodeRef package";
            return false;
          }
          if (!package->package.has_value() ||
              package->package->canonical_ir_hash != found->module->hash ||
              package->package->root_schema != found->module->root_schema) {
            submission->result.ok = false;
            submission->result.message =
                "reference per-Node execution package disagrees with the resolved immutable module";
            return false;
          }
          auto result = execute(*found->module, arena,
                                compiled.plan.certificate.ranges.empty() ? nullptr : &compiled.plan.certificate,
                                &timeline_, {},
                                &compiled.plan.task_effects[task_index]);
          if (!result.ok) {
            result.fault.task_index = task_index;
            result.trace.insert(result.trace.begin(), submission->result.trace.begin(),
                                submission->result.trace.end());
            result.missing_effects.insert(result.missing_effects.begin(),
                                          submission->result.missing_effects.begin(),
                                          submission->result.missing_effects.end());
            core::AccessWitness merged;
            for (const auto& entry : submission->result.witness.entries())
              merged.record(entry.effect, entry.instruction_index);
            for (const auto& entry : result.witness.entries())
              merged.record(entry.effect, entry.instruction_index);
            result.witness = std::move(merged);
            submission->result = std::move(result);
            return false;
          }
          submission->result.trace.insert(submission->result.trace.end(),
                                          result.trace.begin(), result.trace.end());
          for (const auto& entry : result.witness.entries())
            submission->result.witness.record(entry.effect, entry.instruction_index);
          submission->result.outputs_valid = submission->result.outputs_valid && result.outputs_valid;
        }
        return true;
    };
    const auto publish_tasks = [&] {
      // TASK-D5 / ADR-039: host-split on the admitted canonical order. Unset quota
      // keeps the pre-D5 full publish. A set cap publishes a prefix and
      // parks leftover under a device token; the next submit must present
      // that record as pending_overflow.
      if (!publish_order.empty()) {
        std::string task_error;
        core::PublicationRing ring(static_cast<uint32_t>(publish_order.size()));
        const auto& tasks = compiled.plan.task_graph.tasks();
        submission->published_tasks.clear();
        submission->published_tasks.reserve(publish_order.size());
        for (uint32_t index : publish_order) {
          uint32_t slot = 0;
          if (index >= tasks.size() ||
              (tasks[index].kind == core::TaskKind::Compute &&
               (!ring.publish_task(tasks[index], &slot, &task_error) ||
                !ring.consume(slot, &task_error)))) {
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
      // Execute one sealed component/wave schedule.  The reference interpreter
      // serializes ready Tasks, but never derives a second order from the raw
      // graph or the legacy plan task_order observation view.
      const auto& all_tasks = compiled.plan.task_graph.tasks();
      std::vector<uint32_t> scheduled_tasks;
      for (const auto& component : compiled.plan.execution_schedule.components)
        for (const auto& wave : component.waves)
          scheduled_tasks.insert(scheduled_tasks.end(), wave.tasks.begin(), wave.tasks.end());
      std::vector<uint8_t> started(all_tasks.size());
      std::vector<uint8_t> cancelled(all_tasks.size());
      std::vector<core::FaultRecord> logical_failures;
      // A trace records an attempted effect before all runtime preconditions
      // have passed. Keep actual output production separate for poison state.
      bool produced_output = false;
      const auto cancel_descendants = [&](uint32_t failed_task) {
        std::vector<uint32_t> pending{failed_task};
        for (size_t i = 0; i < pending.size(); ++i) {
          for (uint32_t successor :
               compiled.plan.execution_schedule.structural_successors[pending[i]]) {
            if (!started[successor] && !cancelled[successor]) {
              cancelled[successor] = 1;
              pending.push_back(successor);
            }
          }
        }
      };
      bool made_progress = false;
      do {
        made_progress = false;
        submission->result.ok = true;
        submission->result.outputs_valid = true;
        uint32_t failed_task = UINT32_MAX;
        for (uint32_t task_index : scheduled_tasks) {
        if (started[task_index] || cancelled[task_index]) continue;
        started[task_index] = 1;
        made_progress = true;
        const core::TaskRecord& task = all_tasks[task_index];
        if (task.kind == core::TaskKind::Compute) {
          if (!execute_compute_task(task_index)) {
            failed_task = task_index;
            break;
          }
          produced_output = produced_output || std::ranges::any_of(
              compiled.plan.task_effects[task_index], [](const ir::Effect& effect) {
                return effect.access != ir::Access::Read;
              });
          continue;
        }
        const core::NodeTable::Ref task_ref{task.node_index, task.node_generation};
        const auto resolved = std::ranges::find_if(compiled.plan.resolved_nodes, [task_ref](const auto& node) {
          return node.ref.index == task_ref.index && node.ref.generation == task_ref.generation;
        });
        if (resolved == compiled.plan.resolved_nodes.end()) {
          record_raster_failure(task_index, "RASTER_NODE_REF_MISSING",
                                "raster task NodeRef is missing from the immutable plan snapshot");
          break;
        }
        const auto package = std::ranges::find_if(compiled.per_node_packages, [task_ref](const auto& item) {
          return item.ref.index == task_ref.index && item.ref.generation == task_ref.generation;
        });
        if (package == compiled.per_node_packages.end() ||
            package->kind != hal::CompiledPlan::NodePackageKind::Raster ||
            package->package.has_value()) {
          record_raster_failure(task_index, "RASTER_PACKAGE_KIND_MISMATCH",
                                "reference raster Task resolved a non-raster NodeRef package");
          break;
        }
        const std::string& root_schema = resolved->user_raster_shader.has_value()
            ? resolved->user_raster_shader->root_schema : resolved->module->root_schema;
        const bool uses_scene_root = core::is_scene_root_raster_schema(root_schema);
        core::FacetStatus vertex_status = core::FacetStatus::Ok;
        const core::FacetSlot* vertex_slot = facet_pool().lookup(arena, task.vertex_buffer_ref, &vertex_status);
        if (vertex_slot == nullptr || vertex_slot->kind != core::FacetKind::Address) {
          record_raster_failure(task_index, "RASTER_VERTEX_FACET_INVALID", vertex_slot == nullptr
              ? std::string("raster task vertex buffer: ") + core::to_string(vertex_status)
              : std::string("raster task vertex buffer: facet kind mismatch"));
          break;
        }
        auto* vertex_allocation = arena.lookup(
            core::PointerRef{vertex_slot->view.allocation, vertex_slot->view.allocation_generation});
        if (vertex_allocation == nullptr) {
          record_raster_failure(task_index, "RASTER_VERTEX_ALLOCATION_MISSING",
                                "raster task vertex buffer: allocation not found");
          break;
        }
        if (vertex_allocation->bytes.size() % sizeof(RasterVertex) != 0) {
          record_raster_failure(task_index, "RASTER_VERTEX_LAYOUT_INVALID",
                                "raster task vertex buffer byte size is not a multiple of sizeof(RasterVertex)");
          break;
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
            record_raster_failure(task_index, "RASTER_INDEX_FACET_INVALID", index_slot == nullptr
                ? std::string("raster task index buffer: ") + core::to_string(index_status)
                : "raster task index buffer: facet kind mismatch");
            break;
          }
          if (!submission->result.ok) break;
          const core::PixelFormat format = index_slot->view.format;
          const size_t element_size = format == core::PixelFormat::R16Uint ? sizeof(uint16_t) :
                                      format == core::PixelFormat::R32Uint ? sizeof(uint32_t) : 0;
          if (element_size == 0 || task.index_count % 3 != 0 ||
              task.index_count > std::numeric_limits<size_t>::max() / element_size) {
            record_raster_failure(task_index, "RASTER_INDEX_LAYOUT_INVALID",
                                  "raster task index buffer requires R16Uint/R32Uint and a triangle-list count");
            break;
          }
          if (!submission->result.ok) break;
          auto* index_allocation = arena.lookup(
              core::PointerRef{index_slot->view.allocation, index_slot->view.allocation_generation});
          const size_t byte_count = static_cast<size_t>(task.index_count) * element_size;
          if (index_allocation == nullptr || index_allocation->bytes.size() < byte_count) {
            record_raster_failure(task_index, "RASTER_INDEX_ALLOCATION_INVALID",
                                  "raster task index buffer is shorter than index_count");
            break;
          }
          if (!submission->result.ok) break;
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
              record_raster_failure(task_index, "RASTER_INDEX_OUT_OF_RANGE",
                                    "raster task index references a vertex outside the vertex buffer");
              break;
            }
            indexed.push_back(vertices[index]);
          }
          if (!submission->result.ok) break;
          vertices = std::move(indexed);
        }
        core::RasterFacetPair facets = task.raster_facets;
        RasterDesc desc;
        desc.attachment = hal::f2_default_raster_attachment_config<AttachmentFacetDesc>();
        desc.filter = task.raster_filter;
        desc.wrap = task.raster_wrap;
        desc.tint = task.raster_tint;
        desc.depth_attachment_ref = task.depth_attachment_ref;
        desc.depth_test_enable = task.depth_test_enable;
        desc.depth_write_enable = task.depth_write_enable;
        desc.depth_compare_op = task.depth_compare_op;
        if (uses_scene_root) {
          core::ResolvedSceneRootRaster root;
          std::string root_error;
          if (!core::resolve_scene_root_raster(arena, task, &root, &root_error)) {
            record_raster_failure(task_index, "RASTER_SCENE_ROOT_INVALID", root_error);
            break;
          }
          facets.source = root.albedo;
          desc.tint = root.base_color;
          for (RasterVertex& vertex : vertices) {
            // Preserve F4's producer contract before the F6 transform. Metal
            // validates these source bytes before binding the camera, so the
            // Reference oracle must not accept an out-of-range input merely
            // because an affine matrix happens to scale it back into range.
            if (!std::isfinite(vertex.z) || vertex.z < 0.0f || vertex.z > 1.0f) {
              record_raster_failure(task_index, "RASTER_VERTEX_DEPTH_INVALID",
                                    "raster task vertex z must be finite and normalized to [0,1] (F4 vertex ABI)");
              break;
            }
            if (!core::transform_scene_root_vertex(root, vertex.x, vertex.y, vertex.z,
                                                    &vertex.x, &vertex.y, &vertex.z, &root_error)) {
              record_raster_failure(task_index, "RASTER_SCENE_ROOT_TRANSFORM_FAILED", root_error);
              break;
            }
          }
          if (!submission->result.ok) break;
        }
        const RasterResult raster_result = raster_triangles(arena, facet_pool(), facets, desc, vertices);
        if (!raster_result.ok) {
          record_raster_failure(task_index, "RASTER_EXECUTION_FAILED", raster_result.message);
          break;
        }
        // raster_triangles writes canonical bytes directly; publish the same
        // content transition Metal commits after its command buffer completes.
        core::FacetStatus write_status = core::FacetStatus::Ok;
        if (const auto* target_slot = facet_pool().lookup(arena, task.raster_facets.target, &write_status)) {
          if (auto* target = arena.lookup(core::PointerRef{target_slot->view.allocation,
                                                           target_slot->view.allocation_generation}))
            arena.mark_content_modified(*target);
        }
        if (task.depth_attachment_ref.index != 0 || task.depth_attachment_ref.generation != 0) {
          if (const auto* depth_slot = facet_pool().lookup(arena, task.depth_attachment_ref, &write_status)) {
            if (auto* depth = arena.lookup(core::PointerRef{depth_slot->view.allocation,
                                                            depth_slot->view.allocation_generation}))
              arena.mark_content_modified(*depth);
          }
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
        produced_output = true;
        for (const auto& effect : compiled.plan.task_effects[task_index]) {
          submission->result.trace.push_back(effect);
          submission->result.witness.record(
              effect, static_cast<uint32_t>(submission->result.witness.entries().size()));
        }
        if (!submission->result.ok) break;
      }
      if (!submission->result.ok) {
        if (failed_task == UINT32_MAX)
          failed_task = submission->result.fault.task_index;
        logical_failures.push_back(submission->result.fault);
        produced_output = produced_output ||
            submission->result.poison == core::PoisonState::PartiallyProduced;
        cancel_descendants(failed_task);
      }
      } while (made_progress);
      const auto executed_steps = std::ranges::count(started, uint8_t{1});
      if (executed_steps != 0)
        submission->report.add("schedule_program_order", hal::LoweringClass::Serialized,
                               executed_steps, 0,
                               "reference CPU interpreter entered these sealed Task steps, including failed attempts");
      for (const auto& transition : compiled.transition_operations) {
        if (!transition.covers_execution_completion) continue;
        const auto& consumer_wave = compiled.plan.execution_schedule
            .components[transition.component].waves[transition.after_wave];
        // A cancelled consumer never crosses this boundary. A failed consumer
        // that was actually entered does, just as on the Metal schedule path.
        if (std::ranges::any_of(consumer_wave.tasks, [&](uint32_t task) { return started[task] != 0; }))
          ++submission->report.transition_serialized_fallback_count;
      }
      if (!logical_failures.empty()) {
        std::vector<uint32_t> canonical_rank(all_tasks.size());
        for (uint32_t rank = 0; rank < compiled.plan.execution_schedule.task_order.size(); ++rank)
          canonical_rank[compiled.plan.execution_schedule.task_order[rank]] = rank;
        const auto primary = std::ranges::min_element(logical_failures, [&](const auto& left, const auto& right) {
          return canonical_rank[left.task_index] < canonical_rank[right.task_index];
        });
        submission->result.ok = false;
        submission->result.outputs_valid = false;
        submission->result.fault = *primary;
        submission->result.message = primary->message;
        submission->result.poison = produced_output ? core::PoisonState::PartiallyProduced
                                             : core::PoisonState::Poisoned;
      }
      if (logical_failures.empty() && compiled.plan.timeline_signal != 0) {
        std::string signal_error;
        if (!timeline_.signal(compiled.plan.timeline_signal, &signal_error)) {
          submission->result.ok = false;
          submission->result.outputs_valid = false;
          submission->result.poison = core::PoisonState::PartiallyProduced;
          submission->result.message = signal_error;
          submission->result.fault.code = "TIMELINE_SIGNAL_NOT_MONOTONIC";
          submission->result.fault.message = signal_error;
        }
      }
      return submission->result.ok;
    };
    // Logical Task failure is an ExecutionResult, not a Stage-7 API failure.
    // The continuation helper itself has already returned false above for
    // structural/admission errors before any interpreter work starts.
    if (submission->result.ok && !publish_tasks() && submission->result.ok) return false;
    submission->timeline_value = timeline_.value();
    submission->cpu_submit_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submit_start).count();
    return true;
  }

 private:
  hal::CapabilitySnapshot capabilities_;
  core::Timeline timeline_;
};
}

std::unique_ptr<hal::DeviceHal> make_device_hal() { return std::make_unique<ReferenceDeviceHal>(); }
}
