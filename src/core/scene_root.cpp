#include "core/scene_root.h"

#include "vg_scene_root.h"

#include <cmath>
#include <cstring>

namespace vg::core {
namespace {
bool absent(FacetRef ref) { return ref.index == 0 && ref.generation == 0; }

bool identity_tint(const std::array<float, 4>& tint) {
  return tint[0] == 1.0f && tint[1] == 1.0f && tint[2] == 1.0f && tint[3] == 1.0f;
}

}  // namespace

bool is_scene_root_raster_schema(std::string_view schema) {
  return schema == VG_SCHEMA_SCENEROOTRASTER_CONTRACT_NAME;
}

bool resolve_scene_root_raster(const Arena& arena, const TaskRecord& task,
                               ResolvedSceneRootRaster* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "scene root output is required"; return false; }
  if (task.kind != TaskKind::Raster) { if (error) *error = "SceneRoot is only valid for raster tasks"; return false; }
  if (!absent(task.raster_facets.source)) {
    if (error) *error = "SceneRoot raster task must leave raster_facets.source empty; material.albedo is authoritative";
    return false;
  }
  if (!identity_tint(task.raster_tint)) {
    if (error) *error = "SceneRoot raster task must use identity raster_tint; material.base_color is authoritative";
    return false;
  }
  const Allocation* allocation = arena.lookup(PointerRef{task.root_allocation, task.root_generation});
  if (allocation == nullptr) { if (error) *error = "SceneRoot allocation is stale or not owned by the execution arena"; return false; }
  if (allocation->bytes.size() < sizeof(VgSchema_SceneRootRaster)) {
    if (error) *error = "SceneRoot allocation is smaller than the generated SceneRootRaster schema";
    return false;
  }
  VgSchema_SceneRootRaster bytes{};
  std::memcpy(&bytes, allocation->bytes.data(), sizeof(bytes));
  // F6 deliberately remains the F4 2D/orthographic scope.  A perspective
  // camera would need clipping and homogeneous depth interpolation in the
  // Reference oracle before it can be claimed as Metal-equivalent.
  for (float value : bytes.camera_clip_from_local) {
    if (!std::isfinite(value)) { if (error) *error = "SceneRoot camera matrix contains a non-finite value"; return false; }
  }
  if (bytes.camera_clip_from_local[3] != 0.0f || bytes.camera_clip_from_local[7] != 0.0f ||
      bytes.camera_clip_from_local[11] != 0.0f || bytes.camera_clip_from_local[15] != 1.0f) {
    if (error) *error = "F6 SceneRoot camera must be affine with homogeneous w=1; perspective is not supported";
    return false;
  }
  out->allocation = allocation;
  out->albedo = {bytes.material.albedo.index, bytes.material.albedo.generation};
  for (size_t i = 0; i < out->base_color.size(); ++i) {
    if (!std::isfinite(bytes.material.base_color[i])) { if (error) *error = "SceneRoot material base_color contains a non-finite value"; return false; }
    out->base_color[i] = bytes.material.base_color[i];
  }
  std::memcpy(out->camera_clip_from_local.data(), bytes.camera_clip_from_local,
              sizeof(bytes.camera_clip_from_local));
  return true;
}

bool transform_scene_root_vertex(const ResolvedSceneRootRaster& root, float x, float y, float z,
                                 float* out_x, float* out_y, float* out_z, std::string* error) {
  const auto& m = root.camera_clip_from_local;  // column-major, matching MSL float4x4.
  const float transformed_x = m[0] * x + m[4] * y + m[8] * z + m[12];
  const float transformed_y = m[1] * x + m[5] * y + m[9] * z + m[13];
  const float transformed_z = m[2] * x + m[6] * y + m[10] * z + m[14];
  if (!std::isfinite(transformed_x) || !std::isfinite(transformed_y) || !std::isfinite(transformed_z) ||
      transformed_z < 0.0f || transformed_z > 1.0f) {
    if (error) *error = "SceneRoot camera transforms vertex depth outside normalized [0,1]";
    return false;
  }
  *out_x = transformed_x;
  *out_y = transformed_y;
  *out_z = transformed_z;
  return true;
}

}  // namespace vg::core
