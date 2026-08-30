#ifndef VG_CORE_EXECUTION_PLAN_H_
#define VG_CORE_EXECUTION_PLAN_H_

#include "core/core.h"

namespace vg::core {

// A sealed Stage 0--5 plan describes what its lowering must be able to do.
// This is deliberately a core vocabulary: it contains no adapter snapshot,
// backend choice, or caller-selected lowering policy.
enum class CapabilityRequirement : uint32_t {
  LinearAddress,
  TaskPublication,
  Timeline,
  EffectDag,
  CaptureReplay,
  IndirectTier1,
  IndirectTier2Select,
  IndexedBinding,
  Raster,
  RepresentationTransform,
  CheckedFacetGeneration,
  UserShaderImport,
  ReferenceStrict,
};

const char* capability_requirement_name(CapabilityRequirement requirement);

// Stage 5 request data is semantic input, not a backend-private descriptor.
struct RepresentationRequest {
  CanonicalView view;
  FacetKind target_kind{FacetKind::Sample};
  bool consume_input{};
  ConsumeProof consume_proof;
};

// Frozen Stage-5 semantic fact.  This is deliberately internal plan state,
// not another public representation vocabulary: it records the authority and
// versions the assembler observed before an adapter is allowed to lower work.
struct RepresentationSemanticPlanItem {
  CanonicalView view;
  FacetKind target_kind{FacetKind::Sample};
  bool consume_input{};
  ConsumeProof consume_proof;
  uint32_t source_representation_epoch{};
  uint32_t target_representation_epoch{};
  uint32_t transform_order{};
};

// The one internal submission-plan representation.  DeviceHAL receives this
// same type for Stage 6; capability snapshots remain a HAL concern.
struct ExecutionPlan {
  ir::Module module;
  Certificate certificate;
  TaskGraph task_graph;
  uint64_t graph_epoch{};
  uint64_t timeline_wait{};
  uint64_t timeline_signal{};
  bool published{};
  std::optional<AccessCertificateMode> requested_certificate_mode;
  bool request_tier1_indirect{};
  std::vector<ir::Module> effect_dag_passes;
  std::vector<std::pair<uint32_t, uint32_t>> effect_dag_dependencies;
  bool request_indexed_binding{};
  std::vector<RepresentationRequest> representation_requests;
  std::vector<RepresentationSemanticPlanItem> representation_plan;
  bool representation_plan_derived{};
  ValidationProfile validation_profile{ValidationProfile::CheckedNative};
  std::optional<WorkingSetBudget> working_set_budget;
  std::optional<WorkingSetLease> working_set_lease;
  std::optional<EnvelopeOverflow> pending_overflow;
  std::vector<PointerRef> discovery_seeds;
  std::optional<uint32_t> envelope_task_quota;
  bool request_tier2_select{};
  std::vector<NodeTable::Ref> authorized_nodes;
  std::vector<uint32_t> authorized_node_classes;
  struct ResolvedNode {
    NodeTable::Ref ref;
    std::shared_ptr<const CodeObject> code_object;
    std::string entry_name;
    std::optional<ir::Module> module;
    std::optional<ir::UserRasterShaderContract> user_raster_shader;
  };
  std::vector<ResolvedNode> resolved_nodes;
  std::optional<ir::UserRasterShaderContract> user_raster_shader;
  // Stage 0--4 facts produced by the sole core assembler.  They are not a
  // second submission representation: Stage 6 consumes this same plan.
  std::vector<uint32_t> task_order;
  std::vector<PointerRef> touched_allocations;
  std::vector<ir::Effect> instantiated_effects;
  std::optional<AccessCertificate> access_certificate;
  // Sealed access-planning facts.  These remain fields of the one execution
  // plan (rather than a backend-owned second plan): Stage 6/7 may record or
  // physically honour them, but may not re-walk the Arena to reinterpret
  // authority, discovery, or residency policy.
  std::optional<DiscoveryResult> discovery_result;
  uint64_t working_set_requested_bytes{};
  bool working_set_budget_checked{};
  bool access_plan_derived{};
  // Produced exactly once by ExecutionPlanAssembler after Stage 0--5 facts
  // have been validated.  Stage 6 compares this canonical, sorted, unique
  // list with its adapter snapshot and must not reinterpret request_* hints.
  std::vector<CapabilityRequirement> required_capabilities;
  bool capability_requirements_derived{};
  bool assembled{};

  bool validate(std::string* error = nullptr) const;
  bool graph_epoch_matches(const Arena& arena, std::string* error = nullptr) const;
};

// Internal construction inputs for the one ExecutionPlan.  This is purposely
// a core-only mechanism, not a public VG object or a parallel plan type.
struct ExecutionPlanAssemblerInputs {
  const TaskGraph* task_graph{};
  const NodeTable* nodes{};
  const ExecutionEnvelope* envelope{};
  const Arena* arena{};
  const Certificate* certificate{};
  const AccessCertificate* access_certificate{};
  // Discovery is a witnessed result supplied by an existing discovery helper,
  // never a claim inferred from the certificate being checked.
  const std::vector<PointerRef>* discovery_witness{};
  uint64_t graph_epoch{};
  // Compatibility-only semantic inputs from legacy submission descriptors.
  // The assembler translates them once into required_capabilities; adapters
  // never use these booleans to decide what a plan requires.
  bool request_tier1_indirect{};
  bool request_tier2_select{};
  bool request_indexed_binding{};
  // Semantic multi-pass and representation requests are also assembled here;
  // adapters receive only the resulting sealed plan.
  const std::vector<ir::Module>* effect_dag_passes{};
  const std::vector<std::pair<uint32_t, uint32_t>>* effect_dag_dependencies{};
  const std::vector<RepresentationRequest>* representation_requests{};
  // Existing device-owned pool, observed read-only while freezing Stage 5.
  // It is required for representation requests so ConsumeInput can reject
  // live old-epoch facet references before HAL compilation.
  const FacetPool* facet_pool{};
  // Core-only semantic inputs.  They deliberately reuse existing contracts;
  // no C ABI object is introduced for access planning.
  const std::vector<PointerRef>* discovery_seeds{};
  const WorkingSetBudget* working_set_budget{};
  const WorkingSetLease* working_set_lease{};
  // A validated continuation token is semantic submission input.  It is
  // copied into the one assembled plan; adapters never attach it afterwards.
  const EnvelopeOverflow* pending_overflow{};
};

class ExecutionPlanAssembler {
 public:
  // Performs semantic assembly: Node/task resolution, envelope and epoch
  // authority, effects, access/discovery/working-set facts, and validation
  // of submitted representation requests. Physical representation lowering,
  // lifetime retention, and Stage 6 backend lowering intentionally remain
  // outside this helper.
  static bool assemble(const ExecutionPlanAssemblerInputs& inputs, ExecutionPlan* out,
                       std::string* error = nullptr);
};

}  // namespace vg::core

#endif
