#include "capture/capture.h"
#include "core/scene_root.h"
#include "vg_scene_root_layout.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace {

VgSchemaLayout_SceneRootRaster valid_root(vg::core::FacetRef albedo) {
  VgSchemaLayout_SceneRootRaster root{};
  root.camera_clip_from_local[0] = 1.0f;
  root.camera_clip_from_local[5] = 1.0f;
  root.camera_clip_from_local[10] = 1.0f;
  root.camera_clip_from_local[15] = 1.0f;
  root.material.base_color[0] = 1.0f;
  root.material.base_color[1] = 0.5f;
  root.material.base_color[2] = 0.25f;
  root.material.base_color[3] = 1.0f;
  root.material.albedo.index = albedo.index;
  root.material.albedo.generation = albedo.generation;
  return root;
}

void write_root(vg::core::Allocation& allocation, const VgSchemaLayout_SceneRootRaster& root) {
  assert(allocation.bytes.size() >= sizeof(root));
  std::memcpy(allocation.bytes.data(), &root, sizeof(root));
}

vg::core::TaskRecord scene_task(const vg::core::Allocation& allocation) {
  vg::core::TaskRecord task{};
  task.kind = vg::core::TaskKind::Raster;
  task.root_allocation = allocation.id;
  task.root_generation = allocation.generation;
  task.raster_tint = {1.0f, 1.0f, 1.0f, 1.0f};
  return task;
}

}  // namespace

int main() {
  vg::core::Arena arena;
  auto& root_allocation = arena.allocate(VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE);
  const auto sample_ref = vg::core::FacetRef{7, 3};
  auto root = valid_root(sample_ref);
  write_root(root_allocation, root);
  auto task = scene_task(root_allocation);

  vg::core::ResolvedSceneRootRaster resolved;
  std::string error;
  assert(vg::core::resolve_scene_root_raster(arena, task, &resolved, &error));
  assert(resolved.allocation == &root_allocation);
  assert(resolved.albedo.index == sample_ref.index && resolved.albedo.generation == sample_ref.generation);
  assert(resolved.base_color[1] == 0.5f);

  // ADR-052 assigns sample and tint authority to root bytes, not legacy task
  // fields.  Reject the ambiguity before either backend sees it.
  task.raster_facets.source = sample_ref;
  assert(!vg::core::resolve_scene_root_raster(arena, task, &resolved, &error));
  assert(error == "SceneRoot raster task must leave raster_facets.source empty; material.albedo is authoritative");
  task.raster_facets.source = {};
  task.raster_tint[0] = 0.5f;
  assert(!vg::core::resolve_scene_root_raster(arena, task, &resolved, &error));
  assert(error == "SceneRoot raster task must use identity raster_tint; material.base_color is authoritative");
  task.raster_tint = {1.0f, 1.0f, 1.0f, 1.0f};

  root.camera_clip_from_local[0] = std::numeric_limits<float>::quiet_NaN();
  write_root(root_allocation, root);
  assert(!vg::core::resolve_scene_root_raster(arena, task, &resolved, &error));
  assert(error == "SceneRoot camera matrix contains a non-finite value");

  root = valid_root(sample_ref);
  root.camera_clip_from_local[3] = 1.0f;
  write_root(root_allocation, root);
  assert(!vg::core::resolve_scene_root_raster(arena, task, &resolved, &error));
  assert(error == "F6 SceneRoot camera must be affine with homogeneous w=1; perspective is not supported");

  root = valid_root(sample_ref);
  root.material.base_color[2] = std::numeric_limits<float>::infinity();
  write_root(root_allocation, root);
  assert(!vg::core::resolve_scene_root_raster(arena, task, &resolved, &error));
  assert(error == "SceneRoot material base_color contains a non-finite value");

  root = valid_root(sample_ref);
  write_root(root_allocation, root);
  assert(vg::core::resolve_scene_root_raster(arena, task, &resolved, &error));
  float x = 0.0f, y = 0.0f, z = 0.0f;
  assert(vg::core::transform_scene_root_vertex(resolved, 0.0f, 0.0f, 0.5f, &x, &y, &z, &error));
  root.camera_clip_from_local[14] = 1.0f;
  write_root(root_allocation, root);
  assert(vg::core::resolve_scene_root_raster(arena, task, &resolved, &error));
  assert(!vg::core::transform_scene_root_vertex(resolved, 0.0f, 0.0f, 0.5f, &x, &y, &z, &error));
  assert(error == "SceneRoot camera transforms vertex depth outside normalized [0,1]");

  auto& short_allocation = arena.allocate(VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE - 1);
  auto short_task = scene_task(short_allocation);
  assert(!vg::core::resolve_scene_root_raster(arena, short_task, &resolved, &error));
  assert(error == "SceneRoot allocation is smaller than the generated SceneRootRaster schema");
  short_task.root_generation += 1;
  assert(!vg::core::resolve_scene_root_raster(arena, short_task, &resolved, &error));
  assert(error == "SceneRoot allocation is stale or not owned by the execution arena");

  // Capture v1 deliberately has no relocation/reacquisition authority for a
  // facet token embedded in root bytes.  It must refuse instead of replaying
  // the token as though it were an address.
  vg::ir::Module scene_module;
  scene_module.version = 1;
  scene_module.root_schema = VG_SCHEMA_SCENEROOTRASTER_CONTRACT_NAME;
  const auto capture = vg::capture::make_capture(scene_module, arena);
  vg::capture::ReplayResult replay;
  assert(!vg::capture::replay(capture, &replay, &error));
  assert(error == "F6 SceneRoot capture replay is unsupported until facet relocation is implemented");
  return 0;
}
