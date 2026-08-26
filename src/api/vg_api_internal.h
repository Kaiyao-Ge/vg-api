#ifndef VG_API_VG_API_INTERNAL_H_
#define VG_API_VG_API_INTERNAL_H_

#include "vg/vg.h"
#include "backends/device_hal.h"
#include "backends/probe.h"
#include "core/core.h"

#include <memory>
#include <string>
#include <vector>

struct VgRuntime_T {
  void* user{};
  VgAllocateFn allocate{};
  VgFreeFn free_fn{};
  VgLogFn log{};
  uint32_t validation_profile{};
  std::vector<vg::AdapterRecord> adapters;
};

// ---- v1.1 (ADR-043 Decision #2 / ADR-044) handle wrapper structs. Each
// follows the pre-existing VgRuntime_T pattern: a small heap-allocated,
// registry-tracked struct owning the real C++ object. VgAllocation is the one
// exception (a direct core::Allocation* cast -- see vg_api_arena.cpp). ----

struct VgAdapter_T {
  vg::AdapterRecord record;
};

struct VgDevice_T {
  std::unique_ptr<vg::hal::DeviceHal> hal;
  vg::core::Timeline timeline;
};

struct VgAddressDomain_T {
  vg::core::AddressDomain domain;
};

struct VgArena_T {
  vg::core::Arena arena;
};

struct VgCodeObject_T {
  vg::core::CodeObject code;
  vg::core::NodeTable nodes;
};

struct VgNode_T {
  VgCodeObject_T* code_object{};
  vg::core::NodeTable::Ref ref{};
};

struct VgTaskGraphBuilder_T {
  vg::core::TaskGraphBuilder builder;
  // v1.1 narrowing (ADR-044): every task appended runs against this one
  // CodeObject's module, per VgTaskGraphBuilderDesc.code_object.
  VgCodeObject_T* code_object{};
  uint32_t next_task_id{};
};

struct VgTaskGraph_T {
  vg::core::TaskGraph graph;
  VgCodeObject_T* code_object{};
};

struct VgExecutionEnvelope_T {
  vg::core::ExecutionEnvelope envelope;
  // The arena submit() runs against, per VgExecutionEnvelopeDesc.arena
  // (04-public-c-abi.md Sec.17: the envelope, not VgSubmitDesc, carries the
  // arena). Non-owning: the caller must keep the arena alive at least until
  // every submit() using this envelope has returned (ADR-044 caller
  // contract, matching the general handle-parent-lifetime discipline below).
  VgArena_T* arena{};
};

struct VgTimeline_T {
  // Points into the owning VgDevice_T's timeline (one timeline per device
  // for v1 -- ADR-044 disclosed narrowing). Non-owning: destroyTimeline only
  // erases this handle wrapper, never the device's core::Timeline.
  vg::core::Timeline* timeline{};
};

struct VgSubmission_T {
  vg::hal::Submission submission;
  std::string lowering_json;
  // v1.2 (ADR-045): submission.result was previously discarded entirely by
  // submit() -- this is the fix, mirroring lowering_json's pattern.
  std::string execution_result_json;
};

namespace vg_api {

// Shared helpers, moved here (external linkage) from the old vg_api.cpp
// anonymous namespace so every vg_api_*.cpp translation unit can use them.
void set_diagnostic(const char* message);
bool has_runtime(VgRuntime runtime);
VgResult validate_extensions(const VgStructHeader& header);
VgResult validate_header(const VgStructHeader& header, uint32_t type, size_t full_size);

// Per-handle-type liveness checks, backing every entry point's stale-handle
// rejection (04-public-c-abi.md Sec.16). Each is implemented next to the
// HandleRegistry<T> that owns that handle type, and exposed here so entry
// points in a different translation unit can validate a caller-supplied
// parent/sibling handle (e.g. submit() validating the VgArena an envelope
// carries). A handle's owning parent must outlive it -- this is a caller
// contract (matching Vulkan's VkDevice-outlives-its-children discipline),
// not something these checks cascade-enforce.
//
// Concurrency (ADR-044 "Concurrency"): HandleRegistry<T> is internally
// mutex-guarded, so these is_valid_* checks -- and create/destroy on
// *different* handles of the same type -- are already safe to call
// concurrently from multiple threads. What is NOT safe, and is a caller
// contract rather than something enforced here: a caller must never call
// destroyX(h) concurrently with any other API call that takes h, or a
// handle reachable through h (e.g. submit()'s envelope->arena and
// graph->code_object hops), as an argument. A destroy racing a live use of
// the same handle is a same-handle check-then-use TOCTOU that no amount of
// is_valid_* re-checking in the *called* function can fully close -- it can
// only narrow the window (see submit()'s pre-use re-validation in
// vg_api_execution.cpp). See 04-public-c-abi.md Sec.14 for the source
// design intent this codifies.
bool is_valid_adapter(VgAdapter adapter);
bool is_valid_device(VgDevice device);
bool is_valid_address_domain(VgAddressDomain domain);
bool is_valid_arena(VgArena arena);
bool is_valid_code_object(VgCodeObject code_object);
bool is_valid_node(VgNode node);
bool is_valid_task_graph_builder(VgTaskGraphBuilder builder);
bool is_valid_task_graph(VgTaskGraph graph);
bool is_valid_execution_envelope(VgExecutionEnvelope envelope);
bool is_valid_timeline(VgTimeline timeline);
bool is_valid_submission(VgSubmission submission);

// ---- v1.1 entry points (ADR-044). Definitions are split across
// vg_api_device.cpp, vg_api_arena.cpp, vg_api_code.cpp, vg_api_taskgraph.cpp
// and vg_api_execution.cpp; vgGetApi (vg_api.cpp) wires them into VgApi. ----

VgResult VG_CALL open_adapter(VgRuntime runtime, const uint8_t uuid[16], VgAdapter* out_adapter);
void VG_CALL close_adapter(VgAdapter adapter);
VgResult VG_CALL create_device(VgAdapter adapter, const VgDeviceDesc* desc, VgDevice* out_device);
void VG_CALL destroy_device(VgDevice device);

VgResult VG_CALL create_address_domain(VgDevice device, const VgAddressDomainDesc* desc,
                                        VgAddressDomain* out_domain);
void VG_CALL destroy_address_domain(VgAddressDomain domain);
VgResult VG_CALL create_arena(VgDevice device, const VgArenaDesc* desc, VgArena* out_arena);
void VG_CALL destroy_arena(VgArena arena);
VgResult VG_CALL arena_allocate(VgArena arena, uint64_t size, VgAllocation* out_allocation);
VgResult VG_CALL get_allocation_ref(VgAllocation allocation, uint64_t* out_id, uint32_t* out_generation);

VgResult VG_CALL load_code_object(VgDevice device, const VgCodeObjectDesc* desc,
                                   VgCodeObject* out_code_object);
void VG_CALL destroy_code_object(VgCodeObject code_object);
VgResult VG_CALL create_node(VgCodeObject code_object, const VgNodeDesc* desc, VgNode* out_node);
void VG_CALL destroy_node(VgNode node);
VgResult VG_CALL get_node_ref(VgNode node, VgNodeRef* out_ref);

VgResult VG_CALL create_task_graph_builder(VgDevice device, const VgTaskGraphBuilderDesc* desc,
                                            VgTaskGraphBuilder* out_builder);
void VG_CALL destroy_task_graph_builder(VgTaskGraphBuilder builder);
VgResult VG_CALL task_graph_append(VgTaskGraphBuilder builder, const VgTaskRecord* tasks,
                                    uint32_t task_count, VgTaskId* out_ids);
VgResult VG_CALL task_graph_add_dependency(VgTaskGraphBuilder builder, VgTaskId before, VgTaskId after);
VgResult VG_CALL seal_task_graph(VgTaskGraphBuilder builder, const VgSealDesc* desc, VgTaskGraph* out_graph);
void VG_CALL destroy_task_graph(VgTaskGraph graph);

VgResult VG_CALL create_execution_envelope(VgDevice device, const VgExecutionEnvelopeDesc* desc,
                                            VgExecutionEnvelope* out_envelope);
void VG_CALL destroy_execution_envelope(VgExecutionEnvelope envelope);
VgResult VG_CALL create_timeline(VgDevice device, VgTimeline* out_timeline);
void VG_CALL destroy_timeline(VgTimeline timeline);
VgResult VG_CALL wait_timeline(VgTimeline timeline, uint64_t value);
VgResult VG_CALL submit(VgDevice device, const VgSubmitDesc* submit_desc, VgSubmission* out_submission);
void VG_CALL destroy_submission(VgSubmission submission);
VgResult VG_CALL get_submission_lowering_report(VgSubmission submission, const char** out_json);
// v1.2 (ADR-045).
VgResult VG_CALL get_submission_execution_result(VgSubmission submission, const char** out_json);

// v1.3 (F2/ADR-046, F3.5/ADR-048): the public entry point onto
// core::FacetPool::acquire, defined in vg_api_facet.cpp.
VgResult VG_CALL acquire_facet(VgDevice device, VgArena arena, const VgCanonicalViewDesc* view,
                                uint32_t facet_kind, VgFacetRef* out_facet);

}  // namespace vg_api

#endif
