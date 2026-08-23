#ifndef VG_BACKENDS_PROBE_H_
#define VG_BACKENDS_PROBE_H_

#include <string>
#include <vector>
#include "vg/vg.h"

namespace vg {
struct AdapterRecord {
  uint8_t uuid[16]{};
  uint32_t backend_kind{};
  uint32_t adapter_class{};
  std::string name;
  std::string driver;
};

std::vector<AdapterRecord> reference_adapters();
std::vector<AdapterRecord> metal_adapters();
std::vector<AdapterRecord> vulkan_adapters();
}
#endif
