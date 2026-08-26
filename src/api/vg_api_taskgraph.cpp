#include "api/vg_api_internal.h"
#include "api/vg_api_handle_registry.h"

#include <cstdint>
#include <memory>
#include <string>

namespace vg_api {
namespace {
HandleRegistry<VgTaskGraphBuilder_T> g_builders;
HandleRegistry<VgTaskGraph_T> g_task_graphs;

// ABI-versioned raw records are decoded by their thin wrappers below, but all
// handle validation, builder append, diagnostics, and task-ID publication live
// here. Keep new task semantics on this one latest path rather than allowing
// V1/V2 entry points to become duplicate implementations.
VgResult append_normalized_task(VgTaskGraphBuilder builder, const vg::core::TaskRecord& record,
                                VgTaskId* out_id) {
  const vg::core::NodeTable::Ref node_ref{record.node_index, record.node_generation};
  if (builder->code_object->nodes.lookup(node_ref) == nullptr) {
    set_diagnostic("task record references an unknown or stale node");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  std::string error;
  if (!builder->builder.append(record, &error)) {
    set_diagnostic(error.c_str());
    return VG_ERROR_INVALID_ARGUMENT;
  }
  if (out_id != nullptr) *out_id = builder->next_task_id;
  builder->next_task_id += 1;
  return VG_SUCCESS;
}
}  // namespace

bool is_valid_task_graph_builder(VgTaskGraphBuilder builder) { return g_builders.contains(builder); }
bool is_valid_task_graph(VgTaskGraph graph) { return g_task_graphs.contains(graph); }

VgResult VG_CALL create_task_graph_builder(VgDevice device, const VgTaskGraphBuilderDesc* desc,
                                            VgTaskGraphBuilder* out_builder) {
  if (!is_valid_device(device)) {
    set_diagnostic("device handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (desc == nullptr || out_builder == nullptr) {
    set_diagnostic("task graph builder descriptor and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result =
      validate_header(desc->header, VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC, sizeof(VgTaskGraphBuilderDesc));
  if (header_result != VG_SUCCESS) return header_result;
  if (!is_valid_code_object(desc->code_object)) {
    set_diagnostic("code object handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }

  auto wrapper = std::make_unique<VgTaskGraphBuilder_T>();
  wrapper->code_object = desc->code_object;
  const uint32_t max_tasks = desc->max_tasks != 0 ? desc->max_tasks : UINT32_MAX;
  const uint64_t max_payload_bytes = desc->max_payload_bytes != 0 ? desc->max_payload_bytes : UINT64_MAX;
  std::string error;
  if (!wrapper->builder.set_quota(max_tasks, max_payload_bytes, &error)) {
    set_diagnostic(error.c_str());
    return VG_ERROR_INVALID_ARGUMENT;
  }
  *out_builder = g_builders.insert(std::move(wrapper));
  return VG_SUCCESS;
}

void VG_CALL destroy_task_graph_builder(VgTaskGraphBuilder builder) {
  if (!g_builders.contains(builder)) return;
  g_builders.erase(builder);
}

VgResult VG_CALL task_graph_append(VgTaskGraphBuilder builder, const VgTaskRecord* tasks,
                                    uint32_t task_count, VgTaskId* out_ids) {
  if (!g_builders.contains(builder)) {
    set_diagnostic("task graph builder handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (tasks == nullptr && task_count != 0) {
    set_diagnostic("task records are required when task_count is non-zero");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  // Sequential UAF guard: builder->code_object may have been destroyed any
  // time after createTaskGraphBuilder() succeeded (destroyCodeObject doesn't
  // -- and can't -- cascade-invalidate builders holding it). Re-validate
  // before the first dereference below, mirroring destroy_node()'s
  // is_valid_code_object() guard (vg_api_code.cpp) and submit()'s equivalent
  // re-check (vg_api_execution.cpp).
  if (!is_valid_code_object(builder->code_object)) {
    set_diagnostic("task graph builder's code object handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  for (uint32_t i = 0; i < task_count; ++i) {
    vg::core::TaskRecord record;
    record.node_index = tasks[i].node.index;
    record.node_generation = tasks[i].node.generation;
    record.root_allocation = tasks[i].root;
    record.root_generation = tasks[i].root_generation;
    record.x = tasks[i].shape.x;
    record.y = tasks[i].shape.y;
    record.z = tasks[i].shape.z;
    record.flags = tasks[i].shape.flags;
    record.contract_index = tasks[i].contract_index;
    record.payload_size = tasks[i].payload_size;
    record.payload_or_offset = tasks[i].payload_or_offset;
    // v1.3 (F2/ADR-046, F3.5/ADR-048): blind field-for-field copy, no
    // default-substitution -- kind/topology/raster_filter/raster_wrap are
    // raw-cast uint32_t ordinals that already match the internal enums 1:1
    // (see the v1.3 additions comment in vg.h), and raster_facets/
    // vertex_buffer_ref/index_buffer_ref are VgFacetRef->core::FacetRef
    // translated the same way node/root are above.
    record.kind = static_cast<vg::core::TaskKind>(tasks[i].kind);
    record.topology = static_cast<vg::core::Topology>(tasks[i].topology);
    record.raster_facets.source = {tasks[i].raster_facets.source.index, tasks[i].raster_facets.source.generation};
    record.raster_facets.target = {tasks[i].raster_facets.target.index, tasks[i].raster_facets.target.generation};
    record.vertex_buffer_ref = {tasks[i].vertex_buffer_ref.index, tasks[i].vertex_buffer_ref.generation};
    record.index_buffer_ref = {tasks[i].index_buffer_ref.index, tasks[i].index_buffer_ref.generation};
    record.index_count = tasks[i].index_count;
    record.raster_filter = static_cast<vg::core::FilterMode>(tasks[i].raster_filter);
    record.raster_wrap = static_cast<vg::core::WrapMode>(tasks[i].raster_wrap);
    record.raster_tint = {tasks[i].raster_tint[0], tasks[i].raster_tint[1], tasks[i].raster_tint[2],
                           tasks[i].raster_tint[3]};
    const VgResult result = append_normalized_task(builder, record, out_ids != nullptr ? &out_ids[i] : nullptr);
    if (result != VG_SUCCESS) return result;
  }
  return VG_SUCCESS;
}

VgResult VG_CALL task_graph_append_v2(VgTaskGraphBuilder builder, const VgTaskRecordV2* tasks,
                                       uint32_t task_count, VgTaskId* out_ids) {
  if (!g_builders.contains(builder)) {
    set_diagnostic("task graph builder handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (tasks == nullptr && task_count != 0) {
    set_diagnostic("task records are required when task_count is non-zero");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  if (!is_valid_code_object(builder->code_object)) {
    set_diagnostic("task graph builder's code object handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }

  for (uint32_t i = 0; i < task_count; ++i) {
    vg::core::TaskRecord record;
    record.node_index = tasks[i].node.index;
    record.node_generation = tasks[i].node.generation;
    record.root_allocation = tasks[i].root;
    record.root_generation = tasks[i].root_generation;
    record.x = tasks[i].shape.x;
    record.y = tasks[i].shape.y;
    record.z = tasks[i].shape.z;
    record.flags = tasks[i].shape.flags;
    record.contract_index = tasks[i].contract_index;
    record.payload_size = tasks[i].payload_size;
    record.payload_or_offset = tasks[i].payload_or_offset;
    record.kind = static_cast<vg::core::TaskKind>(tasks[i].kind);
    record.topology = static_cast<vg::core::Topology>(tasks[i].topology);
    record.raster_facets.source = {tasks[i].raster_facets.source.index, tasks[i].raster_facets.source.generation};
    record.raster_facets.target = {tasks[i].raster_facets.target.index, tasks[i].raster_facets.target.generation};
    record.vertex_buffer_ref = {tasks[i].vertex_buffer_ref.index, tasks[i].vertex_buffer_ref.generation};
    record.index_buffer_ref = {tasks[i].index_buffer_ref.index, tasks[i].index_buffer_ref.generation};
    record.index_count = tasks[i].index_count;
    record.raster_filter = static_cast<vg::core::FilterMode>(tasks[i].raster_filter);
    record.raster_wrap = static_cast<vg::core::WrapMode>(tasks[i].raster_wrap);
    record.raster_tint = {tasks[i].raster_tint[0], tasks[i].raster_tint[1], tasks[i].raster_tint[2],
                          tasks[i].raster_tint[3]};
    record.depth_attachment_ref = {tasks[i].depth_attachment_ref.index,
                                   tasks[i].depth_attachment_ref.generation};
    record.depth_test_enable = tasks[i].depth_test_enable != VG_FALSE;
    record.depth_write_enable = tasks[i].depth_write_enable != VG_FALSE;
    record.depth_compare_op = static_cast<vg::core::DepthCompareOp>(tasks[i].depth_compare_op);
    const VgResult result = append_normalized_task(builder, record, out_ids != nullptr ? &out_ids[i] : nullptr);
    if (result != VG_SUCCESS) return result;
  }
  return VG_SUCCESS;
}

VgResult VG_CALL task_graph_add_dependency(VgTaskGraphBuilder builder, VgTaskId before, VgTaskId after) {
  if (!g_builders.contains(builder)) {
    set_diagnostic("task graph builder handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  std::string error;
  if (!builder->builder.add_dependency(before, after, &error)) {
    set_diagnostic(error.c_str());
    return VG_ERROR_INVALID_ARGUMENT;
  }
  return VG_SUCCESS;
}

VgResult VG_CALL seal_task_graph(VgTaskGraphBuilder builder, const VgSealDesc* desc, VgTaskGraph* out_graph) {
  if (!g_builders.contains(builder)) {
    set_diagnostic("task graph builder handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (desc == nullptr || out_graph == nullptr) {
    set_diagnostic("seal descriptor and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result = validate_header(desc->header, VG_STRUCTURE_SEAL_DESC, sizeof(VgSealDesc));
  if (header_result != VG_SUCCESS) return header_result;
  // Sequential UAF guard: see the identical is_valid_code_object() check in
  // task_graph_append() above. Without this, sealing a builder whose code
  // object was destroyed after createTaskGraphBuilder() would copy a
  // dangling VgCodeObject_T* into the new VgTaskGraph_T -- submit() already
  // re-validates that copy before use, but rejecting it here, at the point
  // the stale pointer would otherwise be captured, is the earlier and
  // clearer failure point.
  if (!is_valid_code_object(builder->code_object)) {
    set_diagnostic("task graph builder's code object handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }

  auto wrapper = std::make_unique<VgTaskGraph_T>();
  wrapper->code_object = builder->code_object;
  std::string error;
  if (!builder->builder.seal(&wrapper->graph, &error)) {
    set_diagnostic(error.c_str());
    return VG_ERROR_INVALID_ARGUMENT;
  }
  *out_graph = g_task_graphs.insert(std::move(wrapper));
  return VG_SUCCESS;
}

void VG_CALL destroy_task_graph(VgTaskGraph graph) {
  if (!g_task_graphs.contains(graph)) return;
  g_task_graphs.erase(graph);
}

}  // namespace vg_api
