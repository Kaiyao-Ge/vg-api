#include "vg/vg.h"

#include <type_traits>

static_assert(std::is_standard_layout_v<VgApi>);
static_assert(std::is_standard_layout_v<VgRuntimeDesc>);
static_assert(std::is_standard_layout_v<VgFacetRef>);

int main() {
  VgApi api{};
  api.size = sizeof(api);
  return vgGetApi(VG_API_VERSION_1_0, &api) == VG_SUCCESS ? 0 : 1;
}
