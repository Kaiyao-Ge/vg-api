#ifndef VG_CORE_SCENE_ROOT_H_
#define VG_CORE_SCENE_ROOT_H_

#include "core/core.h"

#include <array>
#include <string>
#include <string_view>

namespace vg::core {

// The resolved, backend-neutral subset of the generated SceneRoot layout.
// It is intentionally data-only: facet acquisition and backend objects stay
// in the existing raster paths rather than becoming a second binding system.
struct ResolvedSceneRootRaster {
  const Allocation* allocation{};
  FacetRef albedo{};
  std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 16> camera_clip_from_local{};
};

[[nodiscard]] bool is_scene_root_raster_schema(std::string_view schema);
bool resolve_scene_root_raster(const Arena& arena, const TaskRecord& task,
                               ResolvedSceneRootRaster* out, std::string* error = nullptr);
bool transform_scene_root_vertex(const ResolvedSceneRootRaster& root, float x, float y, float z,
                                 float* out_x, float* out_y, float* out_z,
                                 std::string* error = nullptr);

}  // namespace vg::core

#endif
