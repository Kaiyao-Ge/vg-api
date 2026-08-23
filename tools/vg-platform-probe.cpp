#include "vg/vg.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
const char* backend_name(uint32_t backend) {
  switch (backend) {
    case VG_BACKEND_REFERENCE: return "reference";
    case VG_BACKEND_METAL: return "metal";
    case VG_BACKEND_VULKAN: return "vulkan";
    default: return "unknown";
  }
}
}

int main(int argc, char** argv) {
  const bool validate = argc == 2 && std::strcmp(argv[1], "--validate") == 0;
  VgApi api{};
  api.size = sizeof(api);
  if (vgGetApi(VG_API_VERSION_1_0, &api) != VG_SUCCESS) return 1;
  VgRuntimeDesc desc = VG_INIT_STRUCT(VgRuntimeDesc, VG_STRUCTURE_RUNTIME_DESC);
  VgRuntime runtime = nullptr;
  if (api.createRuntime(&desc, &runtime) != VG_SUCCESS) return 2;
  uint32_t count = 0;
  if (api.enumerateAdapters(runtime, &count, nullptr) != VG_SUCCESS || count == 0) return 3;
  std::vector<VgAdapterInfo> adapters(count);
  for (auto& adapter : adapters) adapter = VG_INIT_STRUCT(VgAdapterInfo, VG_STRUCTURE_ADAPTER_INFO);
  const VgResult result = api.enumerateAdapters(runtime, &count, adapters.data());
  if (result != VG_SUCCESS || count == 0) return 4;
  bool has_reference = false;
  bool has_metal = false;
  std::printf("{\"schema\":\"vg.platform/v1\",\"adapters\":[");
  for (uint32_t i = 0; i < count; ++i) {
    if (i != 0) std::printf(",");
    has_reference |= adapters[i].backend_kind == VG_BACKEND_REFERENCE;
    has_metal |= adapters[i].backend_kind == VG_BACKEND_METAL;
    std::printf("{\"backend\":\"%s\",\"name\":\"%s\",\"driver\":\"%s\"}",
                backend_name(adapters[i].backend_kind), adapters[i].name, adapters[i].driver);
  }
  std::printf("]");
#if defined(VG_HAS_METAL)
  if (!has_metal) {
    std::printf(",\"unsupported\":[{\"feature\":\"metal_device\",\"reason\":\"no MTLDevice was exposed to this process; check sandbox/session context\"}]");
  }
#endif
  std::printf("}\n");
  api.destroyRuntime(runtime);
  return validate && !has_reference ? 5 : 0;
}
