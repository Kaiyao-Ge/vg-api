#include "core/execution_plan.h"

#include "core/scene_root.h"
#include "ir/sha256.h"
#include "vg_scene_root_layout.h"

#include <algorithm>

namespace vg::core {

struct ExecutionPlan::SemanticSeal {
  explicit SemanticSeal(const ExecutionPlan& plan)
      : tasks(plan.task_graph.tasks()),
        dependencies(plan.task_graph.dependencies()),
        task_effects(plan.task_effects),
        task_facet_uses(plan.task_facet_uses),
        instantiated_effects(plan.instantiated_effects),
        effect_edges(plan.validated_effect_graph.edges()),
        task_order(plan.task_order),
        execution_schedule(plan.execution_schedule),
        required_capabilities(plan.required_capabilities),
        certificate_ranges(plan.certificate.ranges),
        touched_allocations(plan.touched_allocations),
        lifetime_facets(plan.lifetime_facets) {
    resolved_node_refs.reserve(plan.resolved_nodes.size());
    for (const auto& node : plan.resolved_nodes) {
      resolved_node_refs.push_back(node.ref);
      resolved_node_domains.push_back(node.execution_domain);
    }
  }

  std::vector<TaskRecord> tasks;
  std::vector<std::pair<uint32_t, uint32_t>> dependencies;
  std::vector<std::vector<ir::Effect>> task_effects;
  std::vector<std::vector<TaskFacetSemanticUse>> task_facet_uses;
  std::vector<ir::Effect> instantiated_effects;
  std::vector<EffectEdge> effect_edges;
  std::vector<uint32_t> task_order;
  ExecutionSchedule execution_schedule;
  std::vector<CapabilityRequirement> required_capabilities;
  std::vector<ir::Effect> certificate_ranges;
  std::vector<PointerRef> touched_allocations;
  std::vector<FacetLifetimeUse> lifetime_facets;
  std::vector<NodeTable::Ref> resolved_node_refs;
  std::vector<TaskKind> resolved_node_domains;
};

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

bool same_ref(PointerRef left, PointerRef right) {
  return left.allocation == right.allocation && left.generation == right.generation;
}

bool same_node_ref(NodeTable::Ref left, NodeTable::Ref right) {
  return left.index == right.index && left.generation == right.generation;
}

bool same_facet_ref(FacetRef left, FacetRef right) {
  return left.index == right.index && left.generation == right.generation;
}

bool same_task_facet_use(const TaskFacetSemanticUse& left,
                         const TaskFacetSemanticUse& right) {
  return same_facet_ref(left.ref, right.ref) && left.kind == right.kind &&
         same_view(left.view, right.view) &&
         left.representation_epoch == right.representation_epoch &&
         left.access == right.access;
}

bool same_task_facet_uses(
    const std::vector<std::vector<TaskFacetSemanticUse>>& left,
    const std::vector<std::vector<TaskFacetSemanticUse>>& right) {
  if (left.size() != right.size()) return false;
  for (size_t task = 0; task < left.size(); ++task) {
    if (left[task].size() != right[task].size() ||
        !std::equal(left[task].begin(), left[task].end(), right[task].begin(),
                    same_task_facet_use))
      return false;
  }
  return true;
}

bool same_effect(const ir::Effect& left, const ir::Effect& right) {
  return left.allocation == right.allocation && left.offset == right.offset &&
         left.size == right.size && left.access == right.access &&
         left.representation_epoch == right.representation_epoch;
}

bool same_effects(const std::vector<ir::Effect>& left,
                  const std::vector<ir::Effect>& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), same_effect);
}

bool same_task_effects(const std::vector<std::vector<ir::Effect>>& left,
                       const std::vector<std::vector<ir::Effect>>& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), same_effects);
}

bool same_effect_edge(const EffectEdge& left, const EffectEdge& right) {
  return left.before == right.before && left.after == right.after &&
         left.kind == right.kind && left.timeline_value == right.timeline_value;
}

bool same_effect_edges(const std::vector<EffectEdge>& left,
                       const std::vector<EffectEdge>& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), same_effect_edge);
}

bool same_schedule(const ExecutionSchedule& left,
                   const ExecutionSchedule& right) {
  if (left.task_order != right.task_order ||
      left.structural_successors != right.structural_successors ||
      left.components.size() != right.components.size() ||
      left.transitions.size() != right.transitions.size())
    return false;
  for (size_t component = 0; component < left.components.size(); ++component) {
    const auto& lhs = left.components[component];
    const auto& rhs = right.components[component];
    if (lhs.tasks != rhs.tasks || lhs.waves.size() != rhs.waves.size()) return false;
    for (size_t wave = 0; wave < lhs.waves.size(); ++wave)
      if (lhs.waves[wave].tasks != rhs.waves[wave].tasks) return false;
  }
  for (size_t index = 0; index < left.transitions.size(); ++index) {
    const auto& lhs = left.transitions[index];
    const auto& rhs = right.transitions[index];
    if (lhs.component != rhs.component || lhs.before_wave != rhs.before_wave ||
        lhs.after_wave != rhs.after_wave ||
        lhs.requires_execution_completion != rhs.requires_execution_completion ||
        lhs.representation_operations != rhs.representation_operations ||
        lhs.region_visibility.size() != rhs.region_visibility.size() ||
        lhs.facet_requirements.size() != rhs.facet_requirements.size())
      return false;
    for (size_t visibility = 0; visibility < lhs.region_visibility.size(); ++visibility) {
      const auto& a = lhs.region_visibility[visibility];
      const auto& b = rhs.region_visibility[visibility];
      if (a.producer_task != b.producer_task || a.consumer_task != b.consumer_task ||
          a.allocation != b.allocation || a.offset != b.offset || a.size != b.size ||
          a.representation_epoch != b.representation_epoch ||
          a.producer_access != b.producer_access || a.consumer_access != b.consumer_access)
        return false;
    }
    for (size_t facet = 0; facet < lhs.facet_requirements.size(); ++facet) {
      const auto& a = lhs.facet_requirements[facet];
      const auto& b = rhs.facet_requirements[facet];
      if (a.task != b.task || !same_task_facet_use(a.use, b.use)) return false;
    }
  }
  return true;
}

std::vector<ScheduleRepresentationFact> schedule_representation_facts(
    const std::vector<RepresentationSemanticPlanItem>& plan) {
  std::vector<ScheduleRepresentationFact> facts;
  facts.reserve(plan.size());
  for (const auto& item : plan)
    facts.push_back({item.transform_order, item.view, item.target_kind,
                     item.target_representation_epoch});
  return facts;
}

bool absent(FacetRef ref) {
  return ref.index == 0 && ref.generation == 0;
}

bool same_task_record(const TaskRecord& left, const TaskRecord& right) {
  return left.node_index == right.node_index &&
         left.node_generation == right.node_generation &&
         left.root_allocation == right.root_allocation &&
         left.root_generation == right.root_generation && left.x == right.x &&
         left.y == right.y && left.z == right.z && left.flags == right.flags &&
         left.contract_index == right.contract_index &&
         left.payload_size == right.payload_size &&
         left.payload_or_offset == right.payload_or_offset &&
         left.kind == right.kind && left.topology == right.topology &&
         same_facet_ref(left.raster_facets.source, right.raster_facets.source) &&
         same_facet_ref(left.raster_facets.target, right.raster_facets.target) &&
         same_facet_ref(left.depth_attachment_ref, right.depth_attachment_ref) &&
         left.depth_test_enable == right.depth_test_enable &&
         left.depth_write_enable == right.depth_write_enable &&
         left.depth_compare_op == right.depth_compare_op &&
         same_facet_ref(left.vertex_buffer_ref, right.vertex_buffer_ref) &&
         same_facet_ref(left.index_buffer_ref, right.index_buffer_ref) &&
         left.index_count == right.index_count &&
         left.raster_filter == right.raster_filter &&
         left.raster_wrap == right.raster_wrap &&
         left.raster_tint == right.raster_tint;
}

bool same_task_records(const std::vector<TaskRecord>& left,
                       const std::vector<TaskRecord>& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), same_task_record);
}

bool same_node_refs(const std::vector<NodeTable::Ref>& left,
                    const std::vector<NodeTable::Ref>& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), same_node_ref);
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

bool append_lifetime_facet(FacetRef ref, FacetKind kind,
                           std::vector<FacetLifetimeUse>* out, std::string* error) {
  // The all-zero value is the Task ABI's absent optional token. Required
  // raster operands are diagnosed by the backend's semantic operation; there
  // is no capability to retain until then. A partially-zero token is retained
  // here so the unified lifetime owner can reject it as stale before Stage 7.
  if (ref.index == 0 && ref.generation == 0) return true;
  const auto found = std::ranges::find_if(*out, [&](const FacetLifetimeUse& use) {
    return same_facet_ref(use.ref, ref);
  });
  if (found != out->end()) {
    if (found->kind != kind) {
      if (error) *error = "one raster FacetRef is used with incompatible facet kinds";
      return false;
    }
    return true;
  }
  out->push_back({ref, kind});
  return true;
}

bool facet_lifetime_less(const FacetLifetimeUse& left, const FacetLifetimeUse& right) {
  if (left.ref.index != right.ref.index) return left.ref.index < right.ref.index;
  if (left.ref.generation != right.ref.generation)
    return left.ref.generation < right.ref.generation;
  return static_cast<uint32_t>(left.kind) < static_cast<uint32_t>(right.kind);
}

bool same_pointer_ref_list(const std::vector<PointerRef>& left,
                           const std::vector<PointerRef>& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), same_ref);
}

bool same_lifetime_uses(const std::vector<FacetLifetimeUse>& left,
                        const std::vector<FacetLifetimeUse>& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const FacetLifetimeUse& lhs, const FacetLifetimeUse& rhs) {
    return same_facet_ref(lhs.ref, rhs.ref) && lhs.kind == rhs.kind;
  });
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

// ir::verify deliberately keeps bounded pointer-edge authorization separate
// from declared_effects, but Stage 3 still needs the concrete dereference as
// an executable access fact.  The target is finite and immutable here: every
// via instruction already passed PointerEdge verification and names its full
// allocation/range/representation epoch.
std::vector<ir::Effect> instantiate_node_effects(const ir::Module& module) {
  std::vector<ir::Effect> effects = ir::verify(module).inferred_effects;
  for (const auto& instruction : module.instructions) {
    if (instruction.op != "load_via" && instruction.op != "store_via") continue;
    effects.push_back({instruction.allocation, instruction.offset, instruction.size,
                       instruction.op == "load_via" ? ir::Access::Read : ir::Access::Write,
                       instruction.representation_epoch});
  }
  return effects;
}

enum class RasterEffectRange { View, FullBacking, IndexElements };

bool append_raster_facet_effect(const Arena& arena, const FacetPool* pool,
                                FacetRef ref, FacetKind expected_kind,
                                ir::Access access, RasterEffectRange range,
                                uint32_t element_count, const char* label,
                                std::vector<ir::Effect>* effects,
                                std::vector<PointerRef>* touched,
                                std::vector<FacetLifetimeUse>* lifetime,
                                std::vector<TaskFacetSemanticUse>* task_facets,
                                std::string* error) {
  if (absent(ref)) return true;
  if (pool == nullptr) {
    if (error) *error = std::string("raster semantic assembly requires the submitting FacetPool to resolve ") + label;
    return false;
  }
  FacetStatus status = FacetStatus::Ok;
  const FacetSlot* slot = pool->lookup(arena, ref, &status);
  if (slot == nullptr) {
    if (error) *error = std::string(label) + ": " + to_string(status);
    return false;
  }
  if (slot->kind != expected_kind) {
    if (error) *error = std::string(label) + ": facet kind mismatch";
    return false;
  }
  if (!append_touched({slot->view.allocation, slot->view.allocation_generation},
                      arena, touched, error) ||
      !append_lifetime_facet(ref, expected_kind, lifetime, error))
    return false;
  const Allocation* allocation =
      arena.lookup(PointerRef{slot->view.allocation,
                              slot->view.allocation_generation});
  if (allocation == nullptr) {
    if (error) *error = std::string(label) + ": backing allocation is inactive";
    return false;
  }
  uint64_t size = slot->view.byte_size();
  if (range == RasterEffectRange::FullBacking) {
    size = allocation->bytes.size();
  } else if (range == RasterEffectRange::IndexElements) {
    const uint64_t stride = slot->view.format == PixelFormat::R16Uint ? 2u :
                            slot->view.format == PixelFormat::R32Uint ? 4u : 0u;
    if (stride == 0 || element_count % 3 != 0 ||
        element_count > UINT64_MAX / stride) {
      if (error) *error = "raster index facet requires R16Uint/R32Uint and a triangle-list count";
      return false;
    }
    size = static_cast<uint64_t>(element_count) * stride;
  }
  if (size == 0) {
    if (error) *error = std::string(label) + ": facet view has an empty effect range";
    return false;
  }
  if (size > allocation->bytes.size()) {
    if (error) *error = std::string(label) + ": effect range exceeds backing allocation";
    return false;
  }
  effects->push_back({slot->view.allocation, 0, size, access,
                      slot->representation_epoch});
  task_facets->push_back({ref, expected_kind, slot->view,
                          slot->representation_epoch, access});
  return true;
}

bool instantiate_raster_task_effects(const TaskRecord& task,
                                     std::string_view root_schema,
                                     const Arena& arena, const FacetPool* pool,
                                     std::vector<ir::Effect>* effects,
                                     std::vector<PointerRef>* touched,
                                     std::vector<FacetLifetimeUse>* lifetime,
                                     std::vector<TaskFacetSemanticUse>* task_facets,
                                     std::string* error) {
  effects->clear();
  task_facets->clear();
  FacetRef source = task.raster_facets.source;
  if (is_scene_root_raster_schema(root_schema)) {
    ResolvedSceneRootRaster root;
    if (!resolve_scene_root_raster(arena, task, &root, error)) return false;
    source = root.albedo;
    effects->push_back({task.root_allocation, 0,
                        VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE,
                        ir::Access::Read, root.allocation->representation_epoch});
  }
  if (absent(source)) {
    if (error) *error = "raster source facet is required";
    return false;
  }
  if (absent(task.raster_facets.target)) {
    if (error) *error = "raster target facet is required";
    return false;
  }
  if (absent(task.vertex_buffer_ref)) {
    if (error) *error = "raster vertex facet is required";
    return false;
  }
  if (task.index_count != 0 && absent(task.index_buffer_ref)) {
    if (error) *error = "raster index facet is required when index_count is non-zero";
    return false;
  }
  if (absent(task.depth_attachment_ref) &&
      (task.depth_test_enable || task.depth_write_enable)) {
    if (error) *error = "raster depth state requires a depth attachment facet";
    return false;
  }
  if (!append_raster_facet_effect(arena, pool, source, FacetKind::Sample,
                                  ir::Access::Read, RasterEffectRange::View, 0,
                                  "raster source facet",
                                  effects, touched, lifetime, task_facets, error) ||
      !append_raster_facet_effect(arena, pool, task.raster_facets.target,
                                  FacetKind::Attachment, ir::Access::Write,
                                  RasterEffectRange::View, 0,
                                  "raster target facet", effects, touched,
                                  lifetime, task_facets, error) ||
      !append_raster_facet_effect(arena, pool, task.vertex_buffer_ref,
                                  FacetKind::Address, ir::Access::Read,
                                  RasterEffectRange::FullBacking, 0,
                                  "raster vertex facet", effects, touched,
                                  lifetime, task_facets, error))
    return false;
  if (task.index_count != 0 &&
      !append_raster_facet_effect(arena, pool, task.index_buffer_ref,
                                  FacetKind::Address, ir::Access::Read,
                                  RasterEffectRange::IndexElements,
                                  task.index_count,
                                  "raster index facet", effects, touched,
                                  lifetime, task_facets, error))
    return false;
  if (!absent(task.depth_attachment_ref)) {
    if (task.depth_test_enable &&
        !append_raster_facet_effect(arena, pool, task.depth_attachment_ref,
                                    FacetKind::Attachment, ir::Access::Read,
                                    RasterEffectRange::View, 0,
                                    "raster depth facet", effects, touched,
                                    lifetime, task_facets, error))
      return false;
    // A present depth attachment is cleared/stored by the fixed raster
    // attachment contract even when depth testing and depth writes are off.
    // Keep that physical write in the sealed certificate; depth testing adds
    // the independent read above.
    if (!append_raster_facet_effect(arena, pool, task.depth_attachment_ref,
                                    FacetKind::Attachment, ir::Access::Write,
                                    RasterEffectRange::View, 0,
                                    "raster depth facet", effects, touched,
                                    lifetime, task_facets, error))
      return false;
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
  // A multi-Task graph requires the adapter to preserve the assembler-sealed
  // effect ordering; there is no parallel list of executable passes.
  if (plan->task_graph.tasks().size() > 1)
    add_requirement(&requirements, CapabilityRequirement::EffectDag);
  if (!plan->representation_requests.empty())
    add_requirement(&requirements, CapabilityRequirement::RepresentationTransform);

  bool has_compute = false;
  bool has_raster = false;
  for (const auto& task : plan->task_graph.tasks()) {
    switch (task.kind) {
      case TaskKind::Compute:
        has_compute = true;
        break;
      case TaskKind::Raster:
        has_raster = true;
        add_requirement(&requirements, CapabilityRequirement::Raster);
        break;
      default:
        if (error) *error = "task execution domain is Unsupported by the canonical core assembler";
        return false;
    }
  }
  if (has_compute && has_raster) {
    if (error) *error = "compute+raster mixed-domain TaskGraphs remain Unsupported";
    return false;
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

  std::sort(requirements.begin(), requirements.end(), [](auto left, auto right) {
    return static_cast<uint32_t>(left) < static_cast<uint32_t>(right);
  });
  plan->required_capabilities = std::move(requirements);
  plan->capability_requirements_derived = true;
  return true;
}

bool build_validated_effect_graph(const TaskGraph& graph,
                                  const std::vector<std::vector<ir::Effect>>& effects,
                                  EffectGraph* out, std::string* error) {
  if (out == nullptr) {
    if (error) *error = "validated EffectGraph output is required";
    return false;
  }
  const uint32_t count = static_cast<uint32_t>(effects.size());
  EffectGraph sealed;
  std::vector<std::vector<uint32_t>> adjacency(count);
  for (const auto& [before, after] : graph.dependencies()) {
    if (before >= count || after >= count ||
        !sealed.add_edge(before, after, EffectEdgeKind::Explicit, 0, error))
      return false;
    adjacency[before].push_back(after);
  }
  const auto reaches = [&](uint32_t source, uint32_t destination) {
    std::vector<uint8_t> seen(count);
    std::vector<uint32_t> work{source};
    seen[source] = 1;
    for (size_t index = 0; index < work.size(); ++index) {
      for (uint32_t next : adjacency[work[index]]) {
        if (!seen[next]) { seen[next] = 1; work.push_back(next); }
      }
    }
    return seen[destination] != 0;
  };
  for (uint32_t left = 0; left < count; ++left) {
    for (uint32_t right = left + 1; right < count; ++right) {
      bool conflict = false;
      for (const auto& lhs : effects[left]) for (const auto& rhs : effects[right])
        conflict = conflict || EffectGraph::conflicts(lhs, rhs);
      if (!conflict || reaches(left, right) || reaches(right, left)) continue;
      if (!sealed.add_edge(left, right, EffectEdgeKind::InferredConflict, 0, error)) return false;
      adjacency[left].push_back(right);
    }
  }
  if (!sealed.valid() || !sealed.validate_happens_before(effects, error)) {
    if (error && error->empty()) *error = "assembler could not seal the validated EffectGraph";
    return false;
  }
  *out = std::move(sealed);
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
    if (!lifetime_plan_derived) {
      if (error) *error = "assembled execution plan is missing sealed lifetime facts";
      return false;
    }
    if (!std::is_sorted(lifetime_facets.begin(), lifetime_facets.end(), facet_lifetime_less) ||
        std::adjacent_find(lifetime_facets.begin(), lifetime_facets.end(),
                           [](const FacetLifetimeUse& left, const FacetLifetimeUse& right) {
                             return same_facet_ref(left.ref, right.ref);
                           }) != lifetime_facets.end()) {
      if (error) *error = "assembled execution plan facet lifetime facts are not deterministic";
      return false;
    }
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
    ExecutionPlan derived_requirements = *this;
    derived_requirements.required_capabilities.clear();
    derived_requirements.capability_requirements_derived = false;
    std::string requirement_error;
    if (!derive_capability_requirements(&derived_requirements, &requirement_error) ||
        derived_requirements.required_capabilities != required_capabilities) {
      if (error) *error = requirement_error.empty()
          ? "sealed capability requirements disagree with the execution semantics"
          : requirement_error;
      return false;
    }
    if (task_order.size() != task_graph.tasks().size() ||
        task_effects.size() != task_graph.tasks().size() ||
        task_facet_uses.size() != task_graph.tasks().size()) {
      if (error) *error = "assembled execution plan is missing its deterministic task order";
      return false;
    }
    if (!execution_schedule_derived) {
      if (error) *error = "assembled execution plan is missing its sealed execution schedule";
      return false;
    }
    std::vector<uint8_t> seen_task(task_effects.size());
    for (uint32_t task_index : task_order) {
      if (task_index >= task_effects.size() || seen_task[task_index] != 0) {
        if (error) *error = "assembled execution plan task order is out of range or contains a duplicate";
        return false;
      }
      seen_task[task_index] = 1;
    }
    if (!validated_effect_graph.valid() ||
        !validated_effect_graph.validate_happens_before(task_effects, error)) {
      if (error && error->empty()) *error = "assembled execution plan is missing its validated EffectGraph";
      return false;
    }
    if (validated_effect_graph_shape != classify_effect_graph_shape(
            validated_effect_graph, static_cast<uint32_t>(task_effects.size()))) {
      if (error) *error = "validated EffectGraph shape was tampered after semantic assembly";
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
    std::vector<NodeTable::Ref> unique_task_refs;
    for (const auto& task : task_graph.tasks()) {
      const NodeTable::Ref ref{task.node_index, task.node_generation};
      if (!contains(unique_task_refs, ref, same_node_ref))
        unique_task_refs.push_back(ref);
    }
    if (resolved_nodes.size() != unique_task_refs.size()) {
      if (error) *error = "assembled execution plan contains a resolved Node with no Task";
      return false;
    }
    for (const auto& node : resolved_nodes) {
      if (!node.code_object ||
          node.module.has_value() != node.code_object->module.has_value() ||
          node.user_raster_shader.has_value() != node.code_object->user_raster_shader.has_value()) {
        if (error) *error = "resolved Node program disagrees with its immutable CodeObject snapshot";
        return false;
      }
      if (node.module.has_value()) {
        const std::string resolved_canonical = ir::serialize_module(*node.module);
        const std::string object_canonical = ir::serialize_module(*node.code_object->module);
        if (resolved_canonical != object_canonical ||
            node.module->canonical_json != node.code_object->module->canonical_json ||
            node.module->hash != node.code_object->module->hash) {
          if (error) *error = "resolved Node module drifted from its immutable CodeObject snapshot";
          return false;
        }
      }
      if (node.user_raster_shader.has_value()) {
        const auto& resolved = *node.user_raster_shader;
        const auto& object = *node.code_object->user_raster_shader;
        if (resolved.root_schema != object.root_schema ||
            resolved.vertex_entry != object.vertex_entry ||
            resolved.fragment_entry != object.fragment_entry ||
            resolved.vertex_abi != object.vertex_abi ||
            resolved.source != object.source) {
          if (error) *error = "resolved raster contract drifted from its immutable CodeObject snapshot";
          return false;
        }
      }
      bool observed_domain = false;
      TaskKind expected_domain = TaskKind::Compute;
      for (const auto& task : task_graph.tasks()) {
        if (!same_node_ref(node.ref,
                           NodeTable::Ref{task.node_index, task.node_generation}))
          continue;
        if (!observed_domain) {
          expected_domain = task.kind;
          observed_domain = true;
        } else if (expected_domain != task.kind) {
          if (error) *error = "one resolved NodeRef is used by multiple execution domains";
          return false;
        }
      }
      if (!observed_domain || node.execution_domain != expected_domain ||
          (node.user_raster_shader.has_value() &&
           node.execution_domain != TaskKind::Raster)) {
        if (error) *error = "resolved Node execution domain disagrees with its Tasks or contract";
        return false;
      }
    }
    for (size_t task_index = 0; task_index < task_graph.tasks().size(); ++task_index) {
      const auto& task = task_graph.tasks()[task_index];
      const NodeTable::Ref ref{task.node_index, task.node_generation};
      const auto resolved = std::ranges::find_if(resolved_nodes, [&](const ResolvedNode& node) {
        return same_node_ref(node.ref, ref);
      });
      if (resolved == resolved_nodes.end()) {
        if (error) *error = "assembled execution plan is missing its immutable per-Task Node snapshot";
        return false;
      }
      if (task.kind == TaskKind::Compute) {
        if (!task_facet_uses[task_index].empty()) {
          if (error) *error = "compute Task contains sealed raster facet semantics";
          return false;
        }
        const auto expected = resolved->module.has_value()
            ? instantiate_node_effects(*resolved->module) : std::vector<ir::Effect>{};
        if (expected.size() != task_effects[task_index].size() ||
            !std::equal(expected.begin(), expected.end(),
                        task_effects[task_index].begin(), same_effect)) {
          if (error) *error = "sealed per-Task effects disagree with the immutable Node program";
          return false;
        }
      } else {
        if (task_facet_uses[task_index].empty()) {
          if (error) *error = "raster Task is missing sealed per-Task facet semantics";
          return false;
        }
        for (const auto& use : task_facet_uses[task_index]) {
          std::string view_error;
          if (absent(use.ref) || !use.view.valid(&view_error) ||
              !std::ranges::any_of(task_effects[task_index], [&](const ir::Effect& effect) {
                return effect.allocation == use.view.allocation &&
                       effect.representation_epoch == use.representation_epoch &&
                       effect.access == use.access;
              })) {
            if (error) *error = "sealed raster facet semantics disagree with per-Task effects";
            return false;
          }
        }
      }
    }
    EffectGraph expected_effect_graph;
    std::string graph_error;
    if (!build_validated_effect_graph(task_graph, task_effects,
                                      &expected_effect_graph, &graph_error) ||
        expected_effect_graph.edges().size() != validated_effect_graph.edges().size() ||
        !std::equal(expected_effect_graph.edges().begin(),
                    expected_effect_graph.edges().end(),
                    validated_effect_graph.edges().begin(), same_effect_edge)) {
      if (error) *error = graph_error.empty()
          ? "validated EffectGraph disagrees with canonical TaskGraph/effect derivation"
          : graph_error;
      return false;
    }
    std::vector<ir::Effect> actual_flattened;
    for (uint32_t task_index : task_order)
      actual_flattened.insert(actual_flattened.end(), task_effects[task_index].begin(),
                              task_effects[task_index].end());
    if (actual_flattened.size() != instantiated_effects.size() ||
        !std::equal(actual_flattened.begin(), actual_flattened.end(),
                    instantiated_effects.begin(), same_effect)) {
      if (error) *error = "flattened effect certificate facts disagree with the per-Task facts";
      return false;
    }
    std::vector<NodeTable::Ref> resolved_node_refs;
    std::vector<TaskKind> resolved_node_domains;
    resolved_node_refs.reserve(resolved_nodes.size());
    resolved_node_domains.reserve(resolved_nodes.size());
    for (const auto& node : resolved_nodes) {
      resolved_node_refs.push_back(node.ref);
      resolved_node_domains.push_back(node.execution_domain);
    }
    if (!semantic_seal ||
        !same_task_records(task_graph.tasks(), semantic_seal->tasks) ||
        task_graph.dependencies() != semantic_seal->dependencies ||
        !same_node_refs(resolved_node_refs, semantic_seal->resolved_node_refs) ||
        resolved_node_domains != semantic_seal->resolved_node_domains ||
        !same_task_effects(task_effects, semantic_seal->task_effects) ||
        !same_task_facet_uses(task_facet_uses, semantic_seal->task_facet_uses) ||
        !same_effects(instantiated_effects, semantic_seal->instantiated_effects) ||
        !same_effect_edges(validated_effect_graph.edges(), semantic_seal->effect_edges) ||
        task_order != semantic_seal->task_order ||
        !same_schedule(execution_schedule, semantic_seal->execution_schedule) ||
        required_capabilities != semantic_seal->required_capabilities ||
        !same_effects(certificate.ranges, semantic_seal->certificate_ranges) ||
        !same_pointer_ref_list(touched_allocations, semantic_seal->touched_allocations) ||
        !same_lifetime_uses(lifetime_facets, semantic_seal->lifetime_facets)) {
      if (error) *error = "assembled execution plan semantic facts disagree with the immutable assembler seal";
      return false;
    }
    ExecutionSchedule expected_schedule;
    const auto representation_facts = schedule_representation_facts(representation_plan);
    std::string schedule_error;
    if (!derive_execution_schedule(validated_effect_graph, task_effects,
                                   task_facet_uses, representation_facts,
                                   task_order, &expected_schedule,
                                   &schedule_error) ||
        !same_schedule(expected_schedule, execution_schedule)) {
      if (error) *error = schedule_error.empty()
          ? "sealed execution schedule disagrees with Stage-5 semantic facts"
          : schedule_error;
      return false;
    }
    std::vector<uint32_t> effect_order;
    if (!effect_graph_deterministic_order(validated_effect_graph,
                                          static_cast<uint32_t>(task_effects.size()),
                                          &effect_order, error) || effect_order != task_order) {
      if (error && error->empty())
        *error = "validated EffectGraph order disagrees with the sealed TaskGraph order";
      return false;
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
  } else {
    if (error) *error = "execution plan must be produced by the canonical core assembler";
    return false;
  }
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
  return true;
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
  plan.task_graph = graph;
  plan.published = true;
  plan.graph_epoch = inputs.graph_epoch == 0 ? arena.topology_epoch() : inputs.graph_epoch;
  plan.authorized_nodes = envelope.allowed_nodes;
  plan.timeline_wait = envelope.timeline_wait;
  plan.timeline_signal = envelope.timeline_signal;
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
  for (const auto& item : plan.representation_plan) {
    if (!append_touched({item.view.allocation, item.view.allocation_generation}, arena,
                        &actual_touched, error)) {
      if (error) *error = "representation plan names an inactive allocation generation";
      return false;
    }
  }
  plan.task_effects.resize(graph.tasks().size());
  plan.task_facet_uses.resize(graph.tasks().size());
  for (uint32_t task_index = 0; task_index < graph.tasks().size(); ++task_index) {
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
      // Canonical IR remains usable for the built-in raster Task contract; only a
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
        plan.resolved_nodes.push_back({ref, snapshot.code_object, snapshot.entry_name,
                                       module, std::nullopt, task.kind});
      } else {
        plan.resolved_nodes.push_back({ref, snapshot.code_object, snapshot.entry_name, std::nullopt,
                                       snapshot.code_object->user_raster_shader,
                                       TaskKind::Raster});
      }
    }
    // Effects are instantiated per Task, not per unique program.  Reusing a
    // Node for two Task records must retain both access facts for certificate
    // and happens-before validation.
    const auto resolved = std::find_if(plan.resolved_nodes.begin(), plan.resolved_nodes.end(), [&](const auto& node) {
      return same_node_ref(node.ref, ref);
    });
    std::vector<ir::Effect> effects;
    if (task.kind == TaskKind::Compute) {
      if (!resolved->module.has_value()) {
        if (error) *error = "compute Task does not resolve to a canonical module";
        return false;
      }
      effects = instantiate_node_effects(*resolved->module);
    } else {
      const std::string& root_schema = resolved->user_raster_shader.has_value()
          ? resolved->user_raster_shader->root_schema : resolved->module->root_schema;
      if (!instantiate_raster_task_effects(task, root_schema, arena,
                                           inputs.facet_pool, &effects,
                                           &actual_touched,
                                           &plan.lifetime_facets,
                                           &plan.task_facet_uses[task_index], error))
        return false;
    }
    plan.task_effects[task_index] = effects;
    if (task.kind == TaskKind::Compute)
      for (const auto& effect : effects)
        if (!append_touched({effect.allocation, 1}, arena, &actual_touched, error)) return false;
  }
  if (!build_validated_effect_graph(graph, plan.task_effects,
                                    &plan.validated_effect_graph, error)) return false;
  if (!effect_graph_deterministic_order(plan.validated_effect_graph,
                                        static_cast<uint32_t>(plan.task_effects.size()),
                                        &plan.task_order, error)) return false;
  const auto representation_facts =
      schedule_representation_facts(plan.representation_plan);
  if (!derive_execution_schedule(plan.validated_effect_graph, plan.task_effects,
                                 plan.task_facet_uses, representation_facts,
                                 plan.task_order, &plan.execution_schedule,
                                 error))
    return false;
  plan.execution_schedule_derived = true;
  plan.instantiated_effects.clear();
  for (uint32_t task_index : plan.task_order)
    plan.instantiated_effects.insert(plan.instantiated_effects.end(),
                                     plan.task_effects[task_index].begin(),
                                     plan.task_effects[task_index].end());
  plan.validated_effect_graph_shape = classify_effect_graph_shape(
      plan.validated_effect_graph, static_cast<uint32_t>(plan.task_effects.size()));
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
  std::sort(plan.lifetime_facets.begin(), plan.lifetime_facets.end(), facet_lifetime_less);
  plan.lifetime_plan_derived = true;
  if (!derive_capability_requirements(&plan, error)) return false;
  plan.assembled = true;
  plan.semantic_seal = std::make_shared<const ExecutionPlan::SemanticSeal>(plan);
  if (!plan.validate(error)) return false;
  *out = std::move(plan);
  return true;
}
}  // namespace vg::core
