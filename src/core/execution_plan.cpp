#include "core/execution_plan.h"

#include "core/scene_root.h"
#include "ir/sha256.h"

#include <algorithm>

namespace vg::core {
namespace {
bool validate_representation_requests(const std::vector<RepresentationRequest>& requests, std::string* error) {
  for (size_t index = 0; index < requests.size(); ++index) {
    const auto& request = requests[index];
    const std::string label = "representation request " + std::to_string(index);
    std::string view_error;
    if (!request.view.valid(&view_error)) {
      if (error) *error = label + " names a CanonicalView that cannot describe a Region: " + view_error;
      return false;
    }
    if (request.target_kind == FacetKind::Address || request.target_kind == FacetKind::Transfer) {
      if (error)
        *error = "representation request targets an AddressFacet or TransferFacet, which name how an existing "
                 "representation is reached rather than a representation a transform can produce; "
                 "a Stage 5 target must be a Sample, Storage, or Attachment facet";
      return false;
    }
    if (request.consume_input)
      if (const char* unmet = request.consume_proof.first_unmet()) {
        if (error) *error = label + " asks for ConsumeInput but its proof is incomplete: " + unmet;
        return false;
      }
    for (size_t earlier = 0; earlier < index; ++earlier) {
      if (requests[earlier].view.allocation == request.view.allocation) {
        if (error) *error = label + " and representation request " + std::to_string(earlier) +
                            " both transform the representation of allocation " +
                            std::to_string(request.view.allocation) +
                            "; one submission must not race two transforms of a single allocation";
        return false;
      }
    }
  }
  return true;
}

bool freeze_representation_plan(const std::vector<RepresentationRequest>& requests, const Arena& arena,
                                const FacetPool* pool, std::vector<RepresentationSemanticPlanItem>* out,
                                std::string* error) {
  if (requests.empty()) { out->clear(); return true; }
  if (pool == nullptr) { if (error) *error = "representation planning requires the device FacetPool snapshot"; return false; }
  if (!validate_representation_requests(requests, error)) return false;
  out->clear(); out->reserve(requests.size());
  for (uint32_t index = 0; index < requests.size(); ++index) {
    const auto& request = requests[index];
    if (!validate_facet_target(arena, request.view, request.target_kind, error)) return false;
    const Allocation* allocation = arena.lookup(PointerRef{request.view.allocation, request.view.allocation_generation});
    if (allocation == nullptr) { if (error) *error = "representation request names an inactive allocation generation"; return false; }
    if (request.consume_input && pool->references({request.view.allocation, request.view.allocation_generation,
                                                    allocation->representation_epoch})) {
      if (error) *error = "representation request asks for ConsumeInput while a live FacetRef names its source epoch";
      return false;
    }
    out->push_back({request.view, request.target_kind, request.consume_input, request.consume_proof,
                    allocation->representation_epoch, allocation->representation_epoch + 1, index});
  }
  return true;
}

bool same_view(const CanonicalView& a, const CanonicalView& b) {
  return a.allocation == b.allocation && a.allocation_generation == b.allocation_generation &&
         a.format == b.format && a.dimension == b.dimension && a.width == b.width && a.height == b.height &&
         a.array_layers == b.array_layers && a.mip_levels == b.mip_levels &&
         a.swizzle.red == b.swizzle.red && a.swizzle.green == b.swizzle.green &&
         a.swizzle.blue == b.swizzle.blue && a.swizzle.alpha == b.swizzle.alpha;
}
bool same_proof(const ConsumeProof& a, const ConsumeProof& b) {
  return a.envelope_complete == b.envelope_complete &&
         a.no_external_references == b.no_external_references &&
         a.no_replay_required == b.no_replay_required &&
         a.failure_semantics_accepted == b.failure_semantics_accepted;
}

bool representation_plan_matches(const ExecutionPlan& plan, std::string* error) {
  if (plan.representation_requests.empty()) return plan.representation_plan.empty() && !plan.representation_plan_derived;
  if (!plan.representation_plan_derived || plan.representation_plan.size() != plan.representation_requests.size()) {
    if (error) *error = "assembled execution plan is missing frozen representation semantic facts";
    return false;
  }
  for (size_t i = 0; i < plan.representation_requests.size(); ++i) {
    const auto& r = plan.representation_requests[i]; const auto& s = plan.representation_plan[i];
    if (!same_view(s.view, r.view) || s.target_kind != r.target_kind || !same_proof(s.consume_proof, r.consume_proof) ||
        s.consume_input != r.consume_input || s.transform_order != i || s.target_representation_epoch != s.source_representation_epoch + 1 ||
        (s.consume_input && s.consume_proof.first_unmet() != nullptr)) {
      if (error) *error = "representation request and frozen semantic plan item disagree";
      return false;
    }
  }
  return true;
}

bool validate_tier2_select(const ExecutionPlan& plan, std::string* error) {
  if (!plan.request_tier2_select) return true;
  if (plan.authorized_node_classes.size() < 2) {
    if (error) *error = "tier2 select requires at least two authorized node classes";
    return false;
  }
  if (plan.task_graph.tasks().empty()) {
    if (error) *error = "tier2 select requires a published task graph";
    return false;
  }
  for (size_t i = 0; i < plan.authorized_node_classes.size(); ++i)
    for (size_t j = i + 1; j < plan.authorized_node_classes.size(); ++j)
      if (plan.authorized_node_classes[i] == plan.authorized_node_classes[j]) {
        if (error) *error = "tier2 authorized node classes must be unique";
        return false;
      }
  return true;
}

bool same_ref(PointerRef left, PointerRef right) {
  return left.allocation == right.allocation && left.generation == right.generation;
}

bool same_node_ref(NodeTable::Ref left, NodeTable::Ref right) {
  return left.index == right.index && left.generation == right.generation;
}

template <typename T, typename U, typename Equal>
bool contains(const std::vector<T>& values, const U& value, Equal equal) {
  return std::ranges::any_of(values, [&](const T& candidate) { return equal(candidate, value); });
}

bool append_touched(PointerRef ref, const Arena& arena, std::vector<PointerRef>* out, std::string* error) {
  if (ref.allocation == 0 || ref.generation == 0 || arena.lookup(ref) == nullptr) {
    if (error) *error = "task/static effect names an inactive allocation";
    return false;
  }
  if (!contains(*out, ref, same_ref)) out->push_back(ref);
  return true;
}

bool same_ref_sets(const std::vector<PointerRef>& left, const std::vector<PointerRef>& right) {
  return std::ranges::all_of(left, [&](PointerRef ref) { return contains(right, ref, same_ref); }) &&
         std::ranges::all_of(right, [&](PointerRef ref) { return contains(left, ref, same_ref); });
}

bool sum_references(const Arena& arena, const std::vector<PointerRef>& references, uint64_t* out,
                    std::string* error) {
  uint64_t total = 0;
  for (const PointerRef ref : references) {
    const Allocation* allocation = arena.lookup(ref);
    if (allocation == nullptr) {
      if (error) *error = "access plan names an inactive allocation";
      return false;
    }
    total += allocation->size;
  }
  *out = total;
  return true;
}

bool build_complete_lease(const Arena& arena, const std::vector<PointerRef>& proven,
                          WorkingSetLease* out, std::string* error) {
  WorkingSetLease lease;
  for (const PointerRef ref : proven) {
    if (!lease.add(ref, proven, error)) return false;
  }
  if (!sum_references(arena, lease.allocations, &lease.byte_limit, error)) return false;
  lease.complete = true;
  *out = std::move(lease);
  return true;
}

bool sum_active_bytes(const Arena& arena, uint64_t* out) {
  uint64_t total = 0;
  for (const auto& [id, allocation] : arena.allocations()) {
    (void)id;
    if (allocation.state == ObjectState::Active) total += allocation.size;
  }
  *out = total;
  return true;
}

// ADR-028's via operations are not an unknown device-side pointer chase.
// ir::verify() has already proved each target through a declared PointerEdge;
// retain that finite target in the plan's access set without pretending it is
// an ordinary declared Effect (ADR-028 deliberately keeps those distinct).
bool append_bounded_pointer_targets(const ir::Module& module, const Arena& arena,
                                    std::vector<PointerRef>* out, std::string* error) {
  for (const auto& instruction : module.instructions) {
    if (instruction.op != "load_via" && instruction.op != "store_via") continue;
    if (!append_touched({instruction.allocation, instruction.generation}, arena, out, error)) return false;
  }
  return true;
}

bool envelope_authorizes(const ExecutionEnvelope& envelope, NodeTable::Ref ref) {
  return envelope.allowed_nodes.empty() ||
         contains(envelope.allowed_nodes, ref, same_node_ref);
}

bool require_raster_only(const TaskGraph& graph, std::string_view reason, std::string* error) {
  if (std::ranges::all_of(graph.tasks(), [](const TaskRecord& task) {
        return task.kind == TaskKind::Raster;
      })) {
    return true;
  }
  if (error) *error = std::string(reason);
  return false;
}

void add_requirement(std::vector<CapabilityRequirement>* requirements, CapabilityRequirement requirement) {
  if (!contains(*requirements, requirement,
                [](CapabilityRequirement left, CapabilityRequirement right) { return left == right; }))
    requirements->push_back(requirement);
}

bool derive_capability_requirements(ExecutionPlan* plan, std::string* error) {
  std::vector<CapabilityRequirement> requirements;
  // Every resolved task root and inferred effect is an address-domain access.
  add_requirement(&requirements, CapabilityRequirement::LinearAddress);
  if (!plan->task_graph.tasks().empty()) add_requirement(&requirements, CapabilityRequirement::TaskPublication);
  if (plan->timeline_wait != 0 || plan->timeline_signal != 0)
    add_requirement(&requirements, CapabilityRequirement::Timeline);
  // Ordinary load/store effects are semantic access facts, not a request for
  // a backend-owned EffectDag lowering.  The latter is only required for the
  // legacy explicit multi-pass form, whose scheduling contract an adapter
  // must actually implement.
  if (!plan->effect_dag_passes.empty())
    add_requirement(&requirements, CapabilityRequirement::EffectDag);
  if (!plan->representation_requests.empty())
    add_requirement(&requirements, CapabilityRequirement::RepresentationTransform);

  for (const auto& task : plan->task_graph.tasks()) {
    switch (task.kind) {
      case TaskKind::Compute:
        break;
      case TaskKind::Raster:
        add_requirement(&requirements, CapabilityRequirement::Raster);
        break;
      default:
        if (error) *error = "task execution domain is Unsupported by the canonical core assembler";
        return false;
    }
  }
  for (const auto& node : plan->resolved_nodes) {
    if (node.user_raster_shader.has_value()) add_requirement(&requirements, CapabilityRequirement::UserShaderImport);
    if (!node.module.has_value() && !node.user_raster_shader.has_value()) {
      if (error) *error = "resolved node format/domain is Unsupported by the canonical core assembler";
      return false;
    }
  }

  switch (plan->validation_profile) {
    case ValidationProfile::FastNative:
      break;
    case ValidationProfile::CheckedNative:
      add_requirement(&requirements, CapabilityRequirement::CheckedFacetGeneration);
      break;
    case ValidationProfile::ReferenceStrict:
      add_requirement(&requirements, CapabilityRequirement::ReferenceStrict);
      break;
    case ValidationProfile::Capture:
      add_requirement(&requirements, CapabilityRequirement::CaptureReplay);
      break;
    default:
      if (error) *error = "unknown validation profile is unsupported";
      return false;
  }

  // Transitional request_* fields are assembler inputs only.  They never
  // cross the Stage 5/6 boundary as requirements in their own right.
  if (plan->request_tier1_indirect) add_requirement(&requirements, CapabilityRequirement::IndirectTier1);
  // NodeRef identity is deliberately not a node class.  Until a real class
  // source exists in the node contract, Tier2 cannot be authorized safely.
  if (plan->request_tier2_select) {
    if (error) *error = "Tier2 selection is Unsupported: no canonical node-class contract is available";
    return false;
  }
  if (plan->request_indexed_binding) add_requirement(&requirements, CapabilityRequirement::IndexedBinding);
  std::sort(requirements.begin(), requirements.end(), [](auto left, auto right) {
    return static_cast<uint32_t>(left) < static_cast<uint32_t>(right);
  });
  plan->required_capabilities = std::move(requirements);
  plan->capability_requirements_derived = true;
  return true;
}

bool validate_instantiated_happens_before(const TaskGraph& graph,
                                          const std::vector<std::vector<ir::Effect>>& effects,
                                          std::string* error) {
  const uint32_t count = static_cast<uint32_t>(effects.size());
  std::vector<std::vector<uint32_t>> adjacency(count);
  for (const auto& [before, after] : graph.dependencies()) {
    if (before >= count || after >= count) {
      if (error) *error = "effect edge references an unknown task";
      return false;
    }
    adjacency[before].push_back(after);
  }
  const auto reaches = [&](uint32_t source, uint32_t destination) {
    std::vector<uint8_t> seen(count);
    std::vector<uint32_t> work{source};
    seen[source] = 1;
    for (size_t i = 0; i < work.size(); ++i) for (const auto next : adjacency[work[i]])
      if (!seen[next]) { seen[next] = 1; work.push_back(next); }
    return seen[destination] != 0;
  };
  for (uint32_t left = 0; left < count; ++left) for (uint32_t right = left + 1; right < count; ++right) {
    bool conflict = false;
    for (const auto& lhs : effects[left]) for (const auto& rhs : effects[right])
      conflict = conflict || EffectGraph::conflicts(lhs, rhs);
    if (conflict && !reaches(left, right) && !reaches(right, left)) {
      if (error) *error = "conflicting task effects have no happens-before edge";
      return false;
    }
  }
  return true;
}
}  // namespace

const char* capability_requirement_name(CapabilityRequirement requirement) {
  switch (requirement) {
    case CapabilityRequirement::LinearAddress: return "LinearAddress";
    case CapabilityRequirement::TaskPublication: return "TaskPublication";
    case CapabilityRequirement::Timeline: return "Timeline";
    case CapabilityRequirement::EffectDag: return "EffectDag";
    case CapabilityRequirement::CaptureReplay: return "CaptureReplay";
    case CapabilityRequirement::IndirectTier1: return "IndirectTier1";
    case CapabilityRequirement::IndirectTier2Select: return "IndirectTier2Select";
    case CapabilityRequirement::IndexedBinding: return "IndexedBinding";
    case CapabilityRequirement::Raster: return "Raster";
    case CapabilityRequirement::RepresentationTransform: return "RepresentationTransform";
    case CapabilityRequirement::CheckedFacetGeneration: return "CheckedFacetGeneration";
    case CapabilityRequirement::UserShaderImport: return "UserShaderImport";
    case CapabilityRequirement::ReferenceStrict: return "ReferenceStrict";
  }
  return "UnknownCapabilityRequirement";
}

bool ExecutionPlan::validate(std::string* error) const {
  if (assembled) {
    if (!representation_plan_matches(*this, error)) return false;
    if (!capability_requirements_derived) {
      if (error) *error = "assembled execution plan is missing sealed capability requirements";
      return false;
    }
    if (!std::is_sorted(required_capabilities.begin(), required_capabilities.end(), [](auto left, auto right) {
          return static_cast<uint32_t>(left) < static_cast<uint32_t>(right);
        }) || std::adjacent_find(required_capabilities.begin(), required_capabilities.end()) != required_capabilities.end()) {
      if (error) *error = "assembled execution plan capability requirements are not deterministic";
      return false;
    }
    if (task_order.size() != task_graph.tasks().size()) {
      if (error) *error = "assembled execution plan is missing its deterministic task order";
      return false;
    }
    for (const auto& task : task_graph.tasks()) {
      const NodeTable::Ref ref{task.node_index, task.node_generation};
      if (std::count_if(resolved_nodes.begin(), resolved_nodes.end(), [&](const ResolvedNode& node) {
            return same_node_ref(node.ref, ref);
          }) != 1) {
        if (error) *error = "assembled execution plan has a duplicate or missing resolved task node";
        return false;
      }
    }
    if (requested_certificate_mode.has_value()) {
      if (!access_plan_derived || !access_certificate.has_value()) {
        if (error) *error = "assembled execution plan is missing sealed access-planning facts";
        return false;
      }
      if (access_certificate->mode != *requested_certificate_mode) {
        if (error) *error = "assembled access certificate mode does not match the execution envelope";
        return false;
      }
      if (!certificate_covers_discovery_witness(*access_certificate, touched_allocations, error)) return false;
      if (discovery_result.has_value()) {
        if (discovery_result->frozen_topology_epoch != graph_epoch ||
            !same_ref_sets(discovery_result->reachable, access_certificate->epoch.references())) {
          if (error) *error = "assembled discovery witness and access certificate are inconsistent";
          return false;
        }
      }
    }
  }
  if (assembled) {
    for (const auto& node : resolved_nodes) {
      if (node.user_raster_shader.has_value()) {
        if (node.user_raster_shader->vertex_abi != ir::kRasterVertexAbiXyzuvPackedV1) {
          if (error) *error = "a resolved user_raster_shader requires vertex_abi vg.raster.vertex.xyzuv-packed/v1";
          return false;
        }
      } else if (node.module.has_value()) {
        const auto verification = ir::verify(*node.module);
        if (!verification.ok) { if (error) *error = verification.message; return false; }
        if (is_scene_root_raster_schema(node.module->root_schema) &&
            !require_raster_only(task_graph,
                                 "a SceneRoot raster submission may only contain raster tasks; compute+raster mixing is deferred",
                                 error)) return false;
      } else {
        if (error) *error = "assembled execution plan has a resolved node without a runnable program";
        return false;
      }
    }
  } else if (user_raster_shader.has_value()) {
    if (user_raster_shader->vertex_abi != ir::kRasterVertexAbiXyzuvPackedV1) {
      if (error) *error = "a user_raster_shader submission requires vertex_abi vg.raster.vertex.xyzuv-packed/v1";
      return false;
    }
    if (!require_raster_only(task_graph,
                             "a user_raster_shader submission may only contain raster tasks", error))
      return false;
  } else {
    const auto verification = ir::verify(module);
    if (!verification.ok) { if (error) *error = verification.message; return false; }
  }
  const std::string& root_schema = user_raster_shader ? user_raster_shader->root_schema : module.root_schema;
  if (!assembled && is_scene_root_raster_schema(root_schema) &&
      !require_raster_only(task_graph,
                           "a SceneRoot raster submission may only contain raster tasks; compute+raster mixing is deferred",
                           error))
    return false;
  if (timeline_signal != 0 && timeline_signal <= timeline_wait) {
    if (error) *error = "timeline signal does not advance past wait";
    return false;
  }
  if (!published && !task_graph.tasks().empty()) {
    if (error) *error = "execution plan contains unpublished tasks";
    return false;
  }
  if (published && !task_graph.tasks().empty() && !task_graph.validate_execution(error)) return false;
  if (!validate_representation_requests(representation_requests, error)) return false;
  if (working_set_budget && working_set_lease && working_set_budget->has_limit &&
      working_set_lease->byte_limit > working_set_budget->byte_limit) {
    if (error) *error = "working-set lease exceeds the plan's working-set budget";
    return false;
  }
  if (pending_overflow && !pending_overflow->valid(error)) return false;
  return validate_tier2_select(*this, error);
}

bool ExecutionPlan::graph_epoch_matches(const Arena& arena, std::string* error) const {
  if (task_graph.tasks().empty()) return true;
  if (graph_epoch != arena.topology_epoch()) {
    if (error) *error = "execution plan graph epoch does not match arena topology";
    return false;
  }
  return true;
}

bool ExecutionPlanAssembler::assemble(const ExecutionPlanAssemblerInputs& inputs, ExecutionPlan* out,
                                      std::string* error) {
  if (out == nullptr || inputs.task_graph == nullptr || inputs.nodes == nullptr ||
      inputs.envelope == nullptr || inputs.arena == nullptr) {
    if (error) *error = "execution-plan assembly requires graph, node table, envelope, arena, and output";
    return false;
  }
  const TaskGraph& graph = *inputs.task_graph;
  const ExecutionEnvelope& envelope = *inputs.envelope;
  const Arena& arena = *inputs.arena;
  if (!graph.sealed() || !graph.published()) {
    if (error) *error = "execution-plan assembly requires a sealed and published task graph";
    return false;
  }
  if (graph.tasks().empty()) {
    if (error) *error = "execution-plan assembly requires at least one task";
    return false;
  }
  if (!graph.validate_execution(error)) return false;

  ExecutionPlan plan;
  if (!graph.deterministic_order(&plan.task_order, error)) return false;
  plan.task_graph = graph;
  plan.published = true;
  plan.graph_epoch = inputs.graph_epoch == 0 ? arena.topology_epoch() : inputs.graph_epoch;
  plan.authorized_nodes = envelope.allowed_nodes;
  plan.timeline_wait = envelope.timeline_wait;
  plan.timeline_signal = envelope.timeline_signal;
  plan.request_tier1_indirect = inputs.request_tier1_indirect;
  plan.request_tier2_select = inputs.request_tier2_select;
  plan.request_indexed_binding = inputs.request_indexed_binding;
  if (inputs.effect_dag_passes != nullptr) plan.effect_dag_passes = *inputs.effect_dag_passes;
  if (inputs.effect_dag_dependencies != nullptr) plan.effect_dag_dependencies = *inputs.effect_dag_dependencies;
  if (inputs.representation_requests != nullptr) plan.representation_requests = *inputs.representation_requests;
  if (!freeze_representation_plan(plan.representation_requests, arena, inputs.facet_pool,
                                  &plan.representation_plan, error)) return false;
  plan.representation_plan_derived = !plan.representation_requests.empty();
  if (inputs.discovery_seeds != nullptr) plan.discovery_seeds = *inputs.discovery_seeds;
  if (inputs.working_set_budget != nullptr) plan.working_set_budget = *inputs.working_set_budget;
  if (inputs.working_set_lease != nullptr) plan.working_set_lease = *inputs.working_set_lease;
  if (inputs.pending_overflow != nullptr) plan.pending_overflow = *inputs.pending_overflow;
  if (envelope.has_task_quota) plan.envelope_task_quota = envelope.task_quota;
  if (envelope.has_certificate_mode) plan.requested_certificate_mode = envelope.certificate_mode;
  if (plan.graph_epoch != arena.topology_epoch()) {
    if (error) *error = "execution-plan assembly graph epoch does not match arena topology";
    return false;
  }

  // Validate caller-declared authority before looking at any Task access. It
  // is not a scratch vector into which assembly may silently add authority.
  for (const auto& touched : envelope.certificate_touched) {
    std::vector<PointerRef> checked;
    if (!append_touched(touched, arena, &checked, error)) {
      if (error) *error = "execution envelope contains an invalid touched allocation";
      return false;
    }
  }

  std::vector<PointerRef> actual_touched;
  std::vector<std::vector<ir::Effect>> task_effects(graph.tasks().size());
  for (const auto task_index : plan.task_order) {
    const TaskRecord& task = graph.tasks()[task_index];
    const NodeTable::Ref ref{task.node_index, task.node_generation};
    NodeEntry snapshot;
    if (!inputs.nodes->snapshot(ref, &snapshot) || !snapshot.code_object) {
      if (error) *error = "task graph references an unknown or stale node";
      return false;
    }
    if (!envelope_authorizes(envelope, ref)) {
      if (error) *error = "execution envelope does not authorize task node generation";
      return false;
    }
    // Raster task payloads may name all data through validated facet tokens;
    // their optional generic root is therefore null.  Compute roots remain a
    // required active allocation and never gain this exception.
    if (task.root_allocation != 0 || task.kind != TaskKind::Raster) {
      if (!append_touched({task.root_allocation, task.root_generation}, arena, &actual_touched, error)) {
        if (error) *error = "task root cannot be bound to an active allocation";
        return false;
      }
    }
    if (!contains(plan.resolved_nodes, ref, [](const ExecutionPlan::ResolvedNode& node, NodeTable::Ref wanted) {
          return same_node_ref(node.ref, wanted);
        })) {
      const bool compute_node = snapshot.code_object->module.has_value() && !snapshot.code_object->user_raster_shader.has_value();
      const bool raster_node = snapshot.code_object->user_raster_shader.has_value() && !snapshot.code_object->module.has_value();
      if (!compute_node && !raster_node) {
        if (error) *error = "node execution domain is Unsupported by the canonical core assembler";
        return false;
      }
      // Canonical IR remains usable for the legacy raster Task shape; only a
      // restricted imported raster package is raster-exclusive.
      if (raster_node && task.kind != TaskKind::Raster) {
        if (error) *error = "task kind does not match its resolved node execution domain";
        return false;
      }
      if (compute_node) {
        const ir::Module& module = *snapshot.code_object->module;
        const std::string canonical = ir::serialize_module(module);
        if (module.canonical_json != canonical || module.hash != ir::sha256_hex(canonical)) {
          if (error) *error = "materialized module canonical hash does not match its serialized module";
          return false;
        }
        const auto verified = ir::verify(module);
        if (!verified.ok) {
          if (error) *error = "materialized module failed canonical IR verification: " + verified.message;
          return false;
        }
        // A verified ADR-028 bounded graph contributes its statically named
        // targets to actual_touched.  Unknown pointer operations never reach
        // this point: canonical IR verification rejects them rather than
        // silently treating them as a certificate-free graph.
        if (!append_bounded_pointer_targets(module, arena, &actual_touched, error)) return false;
        plan.resolved_nodes.push_back({ref, snapshot.code_object, snapshot.entry_name, module, std::nullopt});
      } else {
        plan.resolved_nodes.push_back({ref, snapshot.code_object, snapshot.entry_name, std::nullopt,
                                       snapshot.code_object->user_raster_shader});
      }
    }
    // Effects are instantiated per Task, not per unique program.  Reusing a
    // Node for two Task records must retain both access facts for certificate
    // and happens-before validation.
    const auto resolved = std::find_if(plan.resolved_nodes.begin(), plan.resolved_nodes.end(), [&](const auto& node) {
      return same_node_ref(node.ref, ref);
    });
    const std::vector<ir::Effect> effects = resolved->module.has_value()
        ? ir::verify(*resolved->module).inferred_effects : std::vector<ir::Effect>{};
    task_effects[task_index] = effects;
    for (const auto& effect : effects) {
      plan.instantiated_effects.push_back(effect);
      if (!append_touched({effect.allocation, 1}, arena, &actual_touched, error)) return false;
    }
  }
  if (!validate_instantiated_happens_before(graph, task_effects, error)) return false;
  // The output list contains exactly one snapshot for each unique task ref.
  for (const auto& task : graph.tasks()) {
    const NodeTable::Ref ref{task.node_index, task.node_generation};
    if (std::count_if(plan.resolved_nodes.begin(), plan.resolved_nodes.end(), [&](const auto& node) {
          return same_node_ref(node.ref, ref);
        }) != 1) {
      if (error) *error = "resolved-node snapshot has a duplicate or missing task node";
      return false;
    }
  }
  if (plan.resolved_nodes.empty()) {
    if (error) *error = "task graph contains no resolvable node program";
    return false;
  }
  if (inputs.certificate != nullptr) {
    if (!validate_certificate(*inputs.certificate, plan.instantiated_effects, error)) return false;
    plan.certificate = *inputs.certificate;
  } else {
    plan.certificate.ranges = plan.instantiated_effects;
  }

  if (envelope.has_certificate_mode) {
    if (envelope.certificate_mode == AccessCertificateMode::SoftwarePaged ||
        envelope.certificate_mode == AccessCertificateMode::FaultManaged) {
      if (error) *error = "requested access-certificate mode is Unsupported by the canonical core assembler";
      return false;
    }
    // CertifiedPinned is caller authority: every real root/effect must have
    // been named by the Envelope, before certificate construction. Universe
    // has no caller subset.  DiscoverThenLease is a real seeded topology
    // walk, not the historical full-Arena scan dressed up as discovery.
    const auto envelope_covers_actual = [&] {
      return std::ranges::all_of(actual_touched, [&](PointerRef ref) {
        return contains(envelope.certificate_touched, ref, same_ref);
      });
    };
    if (envelope.certificate_mode == AccessCertificateMode::CertifiedPinned && !envelope_covers_actual()) {
      if (error) *error = "execution envelope certificate_touched does not authorize every task access";
      return false;
    }
    if (envelope.certificate_mode == AccessCertificateMode::DiscoverThenLease &&
        !envelope.certificate_touched.empty() && !envelope_covers_actual()) {
      if (error) *error = "execution envelope certificate_touched does not authorize every task access";
      return false;
    }
    std::vector<PointerRef> witness = actual_touched;
    if (envelope.certificate_mode == AccessCertificateMode::DiscoverThenLease) {
      if (plan.discovery_seeds.empty()) {
        if (error) *error = "DiscoverThenLease requires non-empty seed topology for a complete discovery witness";
        return false;
      }
      DiscoveryResult discovery;
      if (!discover_reachable(arena, plan.discovery_seeds, &discovery, error)) return false;
      if (discovery.frozen_topology_epoch != plan.graph_epoch || arena.topology_epoch() != plan.graph_epoch) {
        if (error) *error = "DiscoverThenLease discovery witness does not share the execution-plan topology epoch";
        return false;
      }
      if (inputs.discovery_witness != nullptr &&
          !same_ref_sets(*inputs.discovery_witness, discovery.reachable)) {
        if (error) *error = "caller discovery witness disagrees with the core-discovered reachable set";
        return false;
      }
      witness = std::move(discovery.reachable);
      if (!std::ranges::all_of(actual_touched, [&](PointerRef ref) { return contains(witness, ref, same_ref); })) {
        if (error) *error = "discovery witness does not cover every task access";
        return false;
      }
      discovery.reachable = witness;
      plan.discovery_result = std::move(discovery);
    }
    AccessCertificate generated;
    const AccessCertificate* access = inputs.access_certificate;
    if (access == nullptr) {
      if (envelope.certificate_mode == AccessCertificateMode::DiscoverThenLease) {
        if (!build_discovered_certificate(arena, *plan.discovery_result, &generated, error)) return false;
      } else if (!build_access_certificate(arena, envelope.certificate_mode, witness, &generated, error)) {
        return false;
      }
      access = &generated;
    }
    if (access->mode != envelope.certificate_mode || access->epoch.value() != plan.graph_epoch ||
        !certificate_covers_discovery_witness(*access, witness, error)) {
      if (access->mode != envelope.certificate_mode && error)
        *error = "execution-plan assembly access certificate mode does not match the envelope";
      else if (access->epoch.value() != plan.graph_epoch && error)
        *error = "execution-plan assembly access certificate does not share the frozen topology epoch";
      return false;
    }
    // CertifiedPinned is an Envelope authorization, not a caller-owned
    // certificate builder.  A supplied certificate may narrow a previously
    // authorized superset, but can never enlarge it with another allocation.
    if (envelope.certificate_mode == AccessCertificateMode::CertifiedPinned &&
        !std::ranges::all_of(access->epoch.references(), [&](PointerRef ref) {
          return contains(envelope.certificate_touched, ref, same_ref);
        })) {
      if (error) *error = "CertifiedPinned access certificate exceeds execution envelope authority";
      return false;
    }
    plan.access_certificate = *access;

    // Lease/budget decisions are Stage 0--5 facts.  In particular, a
    // DiscoverThenLease plan owns a complete lease over the witnessed set;
    // adapters cannot replace it with a fresh scan or a smaller guess.
    const std::vector<PointerRef>& proven = plan.access_certificate->epoch.references();
    if (envelope.certificate_mode == AccessCertificateMode::DiscoverThenLease) {
      if (inputs.working_set_lease != nullptr) {
        if (!inputs.working_set_lease->valid(witness, error) || !inputs.working_set_lease->complete ||
            !std::ranges::all_of(witness, [&](PointerRef ref) {
              return inputs.working_set_lease->covers(ref);
            })) {
          if (error && error->empty()) *error = "DiscoverThenLease requires a complete lease covering its witness";
          return false;
        }
      }
      WorkingSetLease discovered_lease;
      if (!build_complete_lease(arena, witness, &discovered_lease, error)) return false;
      plan.working_set_lease = std::move(discovered_lease);
    } else if (plan.working_set_lease.has_value() &&
               !plan.working_set_lease->valid(proven, error)) {
      return false;
    }

    if (plan.working_set_lease.has_value()) {
      if (!sum_references(arena, plan.working_set_lease->allocations,
                          &plan.working_set_requested_bytes, error)) return false;
    } else {
      // ADR-037: a budget without a lease is an explicit Universe request
      // for residency accounting, even when access proof was narrower.
      if (!sum_active_bytes(arena, &plan.working_set_requested_bytes)) return false;
    }
    if (plan.working_set_budget.has_value()) {
      std::string budget_error;
      if (!plan.working_set_budget->allows(plan.working_set_requested_bytes, &budget_error)) {
        if (error) *error = budget_error;
        return false;
      }
      plan.working_set_budget_checked = true;
    }
    plan.access_plan_derived = true;
  } else if (plan.working_set_budget.has_value() || plan.working_set_lease.has_value() ||
             !plan.discovery_seeds.empty()) {
    if (error) *error = "working-set or discovery planning requires an explicit existing access-certificate mode";
    return false;
  }
  plan.touched_allocations = std::move(actual_touched);
  if (!derive_capability_requirements(&plan, error)) return false;
  plan.assembled = true;
  if (!plan.validate(error)) return false;
  *out = std::move(plan);
  return true;
}
}  // namespace vg::core
