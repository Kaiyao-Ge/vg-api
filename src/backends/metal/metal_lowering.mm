#include "backends/metal/metal_device_internal.h"
#include "compiler/compute_package.h"
#include <algorithm>
#include <utility>

namespace vg::metal {

// TASK-B15 (E002): a plan's module is either the linear (load/store/
// atomic_add) subset or the pointer-graph (load_ref/load_via/store_via)
// subset -- ir::verify() already rejects any other op, and neither
// build_*_compute_package() accepts the other's opcodes -- so this dispatch
// is exhaustive and mutually exclusive, never both true for one module.
bool is_pointer_graph_module(const ir::Module& module) {
  return std::ranges::any_of(module.instructions, [](const ir::Instruction& i) {
    return i.op == "load_ref" || i.op == "load_via" || i.op == "store_via";
  });
}

namespace {
// Stage 5 consume releases the allocation's linear representation before
// Stage 6/7 dispatch (03 §7). A plan that also computes over that same
// allocation is asking for two incompatible things.
bool plan_computes_over_allocation(const core::ExecutionPlan& plan, uint64_t allocation) {
  return std::ranges::any_of(plan.task_effects, [allocation](const auto& effects) {
    return std::ranges::any_of(effects, [allocation](const ir::Effect& effect) {
      return effect.allocation == allocation;
    });
  });
}
}  // namespace

struct DeviceHal::CompileOps {
  static void init(hal::CompiledPlan* compiled, const core::ExecutionPlan& plan) {
    compiled->abi_version = hal::kDeviceHalAbiVersion;
    compiled->plan = plan;
    compiled->report = {};
    compiled->report.backend = hal::BackendKind::Metal;
  }

  static bool fail(hal::CompiledPlan* compiled, const char* operation, std::string diagnostic,
                   std::string* error, uint64_t count = 1, uint64_t bytes = 0, const char* reason = nullptr) {
    compiled->report.supported = false;
    compiled->report.diagnostic = std::move(diagnostic);
    compiled->report.add(operation, hal::LoweringClass::Unsupported, count, bytes,
                         reason != nullptr ? std::string(reason) : compiled->report.diagnostic);
    if (error) *error = compiled->report.diagnostic;
    return false;
  }

  static bool reject_unsupported(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
                                 std::string* error) {
    if (plan.requested_certificate_mode == core::AccessCertificateMode::SoftwarePaged ||
        plan.requested_certificate_mode == core::AccessCertificateMode::FaultManaged) {
      init(compiled, plan);
      return fail(compiled, "access_certificate",
                  "requested access certificate mode is not implemented on this backend", error);
    }
    return true;
  }

  static bool select_packages(DeviceHal& metal, const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
                              std::string* error) {
    init(compiled, plan);
    for (const auto& node : plan.resolved_nodes) {
      hal::CompiledPlan::PerNodePackage per_node;
      per_node.ref = node.ref;
      const auto task = std::ranges::find_if(plan.task_graph.tasks(), [&](const auto& candidate) {
        return candidate.node_index == node.ref.index &&
               candidate.node_generation == node.ref.generation;
      });
      if (task == plan.task_graph.tasks().end())
        return fail(compiled, "node_package", "resolved Node has no Task", error);
      if (task->kind == core::TaskKind::Raster) {
        per_node.kind = hal::CompiledPlan::NodePackageKind::Raster;
        compiled->per_node_packages.push_back(std::move(per_node));
        if (node.user_raster_shader.has_value()) {
          compiled->report.add("raster_user_shader", hal::LoweringClass::HostAssisted, 1,
                               node.user_raster_shader->source.size(),
                               "caller-declared effect contract accepted; shader logic not independently verified");
        } else {
          compiled->report.add("node_raster_package", hal::LoweringClass::Direct, 1, 0,
                               "canonical NodeRef materialized as the built-in Metal raster contract");
        }
        continue;
      }
      if (!node.module.has_value())
        return fail(compiled, "node_package", "resolved Node has no materialized program", error);
      const bool pointer_graph = is_pointer_graph_module(*node.module);
      auto package = pointer_graph ? compiler::build_pointer_graph_compute_package(*node.module)
                                   : compiler::build_linear_compute_package(*node.module);
      if (!package.ok)
        return fail(compiled, "node_package", "per-Node package compilation failed: " + package.message, error);
      per_node.kind = hal::CompiledPlan::NodePackageKind::CanonicalCompute;
      per_node.package = std::move(package.package);
      id<MTLComputePipelineState> pipeline = nil;
      bool cache_hit = false;
      std::string pipeline_error;
      const char* entry = pointer_graph ? "vg_pointer_graph_compute" : "vg_linear_compute";
      if (!metal.impl_->ensure_node_pipeline({per_node.package->canonical_ir_hash,
                                              per_node.package->metal_source},
                                             &pipeline, &pipeline_error, entry, &cache_hit)) {
        const bool has_atomic = std::ranges::any_of(node.module->instructions,
                                                    [](const ir::Instruction& i) { return i.op == "atomic_add"; });
        if (!has_atomic)
          return fail(compiled, "metal_pipeline", "Metal per-Node pipeline compilation failed: " + pipeline_error,
                      error, 1, 0, pipeline_error.c_str());
        per_node.host_assisted = true;
        compiled->report.add("metal_pipeline", hal::LoweringClass::HostAssisted, 1, 0,
                             "native 64-bit atomic compile failed for Node; host execution: " + pipeline_error);
      } else {
        compiled->report.add("metal_pipeline",
                             cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::Direct,
                             1, 0, cache_hit ? "per-Node MTLComputePipelineState cache hit"
                                             : "per-Node MTLComputePipelineState compiled");
      }
      compiled->report.add("node_compute_package",
                           pointer_graph ? hal::LoweringClass::CachedObject : hal::LoweringClass::Direct,
                           1, per_node.package->bindings.size(),
                           pointer_graph ? "NodeRef-keyed CachedObject package" : "NodeRef-keyed linear package");
      compiled->per_node_packages.push_back(std::move(per_node));
    }
    const bool any_host_assisted = std::ranges::any_of(
        compiled->per_node_packages, [](const auto& package) { return package.host_assisted; });
    const bool any_native_compute = std::ranges::any_of(
        compiled->per_node_packages, [](const auto& package) {
          return package.kind == hal::CompiledPlan::NodePackageKind::CanonicalCompute &&
                 !package.host_assisted;
        });
    if (any_host_assisted && any_native_compute)
      return fail(compiled, "node_compute_package",
                  "Metal mixed native and host-assisted per-Node compute lowering is Unsupported",
                  error);
    const bool has_compute = std::ranges::any_of(plan.task_graph.tasks(), [](const auto& task) {
      return task.kind == core::TaskKind::Compute;
    });
    const bool has_raster = std::ranges::any_of(plan.task_graph.tasks(), [](const auto& task) {
      return task.kind == core::TaskKind::Raster;
    });
    if (has_compute && has_raster &&
        std::ranges::any_of(plan.resolved_nodes, [](const auto& node) {
          return node.execution_domain == core::TaskKind::Raster && node.user_raster_shader.has_value();
        }))
      return fail(compiled, "mixed_domain_user_raster_shader",
                  "Metal Unsupported: restricted user raster shaders cannot participate in a native mixed-domain ExecutionSchedule",
                  error);
    return true;
  }

  static bool representation_requests(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
                                     std::string* error) {
    for (size_t index = 0; index < plan.representation_plan.size(); ++index) {
      const auto& request = plan.representation_plan[index];
      std::string request_error;
      if (!view_expressible(request.view, request.target_kind, request.view.byte_size(), &request_error)) {
        return fail(compiled, "representation_transform",
                    "representation request " + std::to_string(index) +
                        " is not expressible on this Metal device: " + request_error,
                    error, 1, request.view.byte_size());
      }
      if (request.consume_input && plan_computes_over_allocation(plan, request.view.allocation)) {
        return fail(compiled, "consume_input",
                    "representation request " + std::to_string(index) +
                        " is Unsupported: it asks for ConsumeInput on allocation " +
                        std::to_string(request.view.allocation) +
                        ", whose linear representation this plan's compute module also reads or writes; the "
                        "consume releases that backing before the dispatch could run",
                    error);
      }
      compiled->report.add("representation_transform", hal::LoweringClass::DevicePass, 1,
                           request.view.byte_size(),
                           "blit every subresource of the linear backing into a Private device-optimal "
                           "MTLTexture and publish a new RepresentationEpoch at submit()");
      compiled->representation_operations.push_back({hal::CompiledPlan::RepresentationOperation::CopyToPrivate,
                                                     request.transform_order, "Metal private texture copy"});
      if (request.consume_input)
        compiled->report.add("consume_input", hal::LoweringClass::Direct, 1, 0,
                             "recorded for submit(): the Private texture is storage distinct from the linear "
                             "backing it supersedes, so a complete ConsumeProof can release that backing at "
                             "once instead of holding it to command-buffer completion (06 §11)");
    }
    return true;
  }

  static bool timeline(DeviceHal& metal, const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
                       std::string* error) {
    if ((plan.timeline_wait != 0 || plan.timeline_signal != 0) && !metal.impl_->snapshot.supports_shared_events) {
      return fail(compiled, "timeline", "timeline requested but device does not support MTLSharedEvent", error);
    }
    return true;
  }

  static void execution_schedule(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled) {
    uint64_t wave_count = 0;
    for (const auto& component : plan.execution_schedule.components)
      wave_count += component.waves.size();
    const auto& tasks = plan.task_graph.tasks();
    const auto submits_device_command = [&](uint32_t task_index) {
      if (task_index >= tasks.size()) return false;
      const auto& task = tasks[task_index];
      if (task.kind == core::TaskKind::Raster) return true;
      const auto package = std::ranges::find_if(
          compiled->per_node_packages, [&](const auto& candidate) {
            return candidate.ref.index == task.node_index &&
                   candidate.ref.generation == task.node_generation;
          });
      return package != compiled->per_node_packages.end() &&
             package->kind == hal::CompiledPlan::NodePackageKind::CanonicalCompute &&
             !package->host_assisted;
    };
    const bool has_device_commands = std::ranges::any_of(
        plan.execution_schedule.task_order, submits_device_command);
    std::vector<uint8_t> representation_owned(plan.representation_plan.size());
    for (auto& transition : compiled->transition_operations) {
      transition.state = hal::CompiledPlan::TransitionLoweringState::Lowered;
      uint64_t representation_steps = 0;
      for (uint32_t operation : transition.representation_operations) {
        if (operation < representation_owned.size() && representation_owned[operation] == 0) {
          representation_owned[operation] = 1;
          ++representation_steps;
        }
      }
      // MD-4's deliberately conservative implementation completes and host-
      // waits every producer-wave device command before beginning the
      // consumer wave. Host-assisted Compute tasks execute synchronously and
      // therefore contribute no fictional Metal encoder or queue wait.
      transition.encoder_boundary_count = representation_steps;
      transition.host_wait_count = representation_steps;
      if (transition.covers_execution_completion &&
          transition.component < plan.execution_schedule.components.size()) {
        const auto& component =
            plan.execution_schedule.components[transition.component];
        if (transition.before_wave < component.waves.size()) {
          const uint64_t producer_device_commands = std::ranges::count_if(
              component.waves[transition.before_wave].tasks,
              submits_device_command);
          transition.encoder_boundary_count += producer_device_commands;
          transition.host_wait_count += producer_device_commands;
        }
      }
      transition.serialized_fallback = transition.covers_execution_completion;
      compiled->report.transition_encoder_boundary_count += transition.encoder_boundary_count;
      compiled->report.transition_host_wait_count += transition.host_wait_count;
      if (transition.serialized_fallback)
        ++compiled->report.transition_serialized_fallback_count;
    }
    compiled->report.add("execution_schedule", hal::LoweringClass::Serialized,
                         plan.task_graph.tasks().size(), 0,
                         std::string(has_device_commands
                             ? "Metal consumes Core-sealed components/waves and conservatively completes each device command before the next schedule step; "
                             : "Metal consumes Core-sealed components/waves in the host interpreter; ") +
                             std::to_string(plan.execution_schedule.components.size()) + " component(s), " +
                             std::to_string(wave_count) + " wave(s)");
  }

  static bool pipelines(DeviceHal& metal, const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
                        std::string* error) {
    (void)metal;
    (void)error;
    compiled->report.supported = true;
    const uint64_t compute_tasks = std::ranges::count_if(plan.task_graph.tasks(), [](const auto& task) {
      return task.kind == core::TaskKind::Compute;
    });
    const bool compute_only = compute_tasks == plan.task_graph.tasks().size();
    compiled->report.add("task_publication",
                         compute_only ? hal::LoweringClass::Direct : hal::LoweringClass::HostAssisted,
                         compute_only ? compute_tasks : plan.task_graph.tasks().size(), 0,
                         compute_only
                             ? "compute-only Metal task ring publication in canonical schedule order"
                             : "complete cross-domain canonical publication is host-side; Raster Tasks are never packed into the compute ring");
    if (plan.timeline_signal != 0 || plan.timeline_wait != 0)
      compiled->report.add("timeline", hal::LoweringClass::HostAssisted, 1, 0,
                           "submission-wide host observation/signal of MTLSharedEvent around the sealed schedule");
    execution_schedule(plan, compiled);
    return true;
  }
};

bool DeviceHal::compile_plan(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled, std::string* error) {
  if (compiled == nullptr) {
    if (error) *error = "compiled plan output is required";
    return false;
  }
  *compiled = {};
  if (!plan.validate(error)) return false;
  if (!hal::preflight_stage6(plan, capabilities(), hal::BackendKind::Metal, compiled, error)) return false;
  if (!CompileOps::reject_unsupported(plan, compiled, error)) return false;
  if (!CompileOps::select_packages(*this, plan, compiled, error)) return false;
  if (!CompileOps::representation_requests(plan, compiled, error)) return false;
  if (!CompileOps::timeline(*this, plan, compiled, error)) return false;
  return CompileOps::pipelines(*this, plan, compiled, error);
}

}  // namespace vg::metal
