#include "api/vg_api_internal.h"
#include "api/vg_api_handle_registry.h"

#include "ir/ir.h"

#include <cstring>
#include <memory>

namespace vg_api {
namespace {
HandleRegistry<VgCodeObject_T> g_code_objects;
HandleRegistry<VgNode_T> g_nodes;
}  // namespace

bool is_valid_code_object(VgCodeObject code_object) { return g_code_objects.contains(code_object); }
bool is_valid_node(VgNode node) { return g_nodes.contains(node); }

VgResult VG_CALL load_code_object(VgDevice device, const VgCodeObjectDesc* desc,
                                   VgCodeObject* out_code_object) {
  if (!is_valid_device(device)) {
    set_diagnostic("device handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (desc == nullptr || out_code_object == nullptr || (desc->bytes == nullptr && desc->byte_size != 0)) {
    set_diagnostic("code object descriptor and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result =
      validate_header(desc->header, VG_STRUCTURE_CODE_OBJECT_DESC, sizeof(VgCodeObjectDesc));
  if (header_result != VG_SUCCESS) return header_result;

  auto wrapper = std::make_unique<VgCodeObject_T>();
  wrapper->owner_device = device;
  auto materialized = std::make_shared<vg::core::CodeObject>();
  const auto* bytes = static_cast<const uint8_t*>(desc->bytes);
  materialized->bytes.assign(bytes, bytes + desc->byte_size);
  materialized->format_tag = desc->format_tag != nullptr ? desc->format_tag : "";
  try {
    const std::string text(materialized->bytes.begin(), materialized->bytes.end());
    if (materialized->format_tag == "vg.msl.raster/v1")
      materialized->user_raster_shader = vg::ir::parse_msl_raster_envelope(text);
    else
      materialized->module = vg::ir::parse_module(text);
  } catch (const std::exception& e) {
    set_diagnostic(e.what());
    return VG_ERROR_INVALID_ARGUMENT;
  }
  wrapper->code = std::move(materialized);
  *out_code_object = g_code_objects.insert(std::move(wrapper));
  return VG_SUCCESS;
}

void VG_CALL destroy_code_object(VgCodeObject code_object) {
  if (!g_code_objects.contains(code_object)) return;
  g_code_objects.erase(code_object);
}

VgResult VG_CALL create_node(VgCodeObject code_object, const VgNodeDesc* desc, VgNode* out_node) {
  if (!is_valid_code_object(code_object)) {
    set_diagnostic("code object handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (desc == nullptr || desc->entry_name == nullptr || out_node == nullptr) {
    set_diagnostic("node descriptor and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result = validate_header(desc->header, VG_STRUCTURE_NODE_DESC, sizeof(VgNodeDesc));
  if (header_result != VG_SUCCESS) return header_result;

  auto wrapper = std::make_unique<VgNode_T>();
  wrapper->owner_device = code_object->owner_device;
  if (wrapper->owner_device == nullptr || !is_valid_device(wrapper->owner_device)) {
    set_diagnostic("code object's owner device is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  wrapper->ref = wrapper->owner_device->nodes.create(code_object->code, desc->entry_name);
  if (wrapper->ref.index == 0 && wrapper->ref.generation == 0) {
    set_diagnostic("NodeRef token space is exhausted");
    return VG_ERROR_UNSUPPORTED;
  }
  *out_node = g_nodes.insert(std::move(wrapper));
  return VG_SUCCESS;
}

void VG_CALL destroy_node(VgNode node) {
  if (!g_nodes.contains(node)) return;
  // The NodeTable is device-owned and the NodeEntry owns the program package,
  // so no raw CodeObject pointer is followed during retirement.
  if (is_valid_device(node->owner_device)) node->owner_device->nodes.destroy(node->ref);
  g_nodes.erase(node);
}

VgResult VG_CALL get_node_ref(VgNode node, VgNodeRef* out_ref) {
  if (!g_nodes.contains(node)) {
    set_diagnostic("node handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (out_ref == nullptr) {
    set_diagnostic("output node ref is required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  out_ref->index = node->ref.index;
  out_ref->generation = node->ref.generation;
  return VG_SUCCESS;
}

}  // namespace vg_api
