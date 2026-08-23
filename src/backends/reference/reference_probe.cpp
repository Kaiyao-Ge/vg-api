#include "backends/probe.h"

namespace vg {
std::vector<AdapterRecord> reference_adapters() {
  AdapterRecord record{};
  const uint8_t uuid[16] = {0x56, 0x47, 0x50, 0x30, 0x52, 0x45, 0x46, 0x45,
                            0x52, 0x45, 0x4e, 0x43, 0x45, 0x00, 0x00, 0x01};
  for (size_t i = 0; i < 16; ++i) record.uuid[i] = uuid[i];
  record.backend_kind = VG_BACKEND_REFERENCE;
  record.adapter_class = VG_ADAPTER_CLASS_CPU;
  record.name = "VG CPU Reference";
  record.driver = "phase0";
  return {record};
}

#if !defined(VG_HAS_METAL)
std::vector<AdapterRecord> metal_adapters() { return {}; }
#endif

#if !defined(VG_HAS_VULKAN)
std::vector<AdapterRecord> vulkan_adapters() { return {}; }
#endif
}  // namespace vg
