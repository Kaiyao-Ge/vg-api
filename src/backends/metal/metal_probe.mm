#include "backends/probe.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace vg {
std::vector<AdapterRecord> metal_adapters() {
  NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
  if (devices.count == 0) {
    id<MTLDevice> default_device = MTLCreateSystemDefaultDevice();
    if (default_device != nil) devices = @[default_device];
  }

  std::vector<AdapterRecord> records;
  for (id<MTLDevice> device in devices) {
    AdapterRecord record{};
    const uint8_t prefix[8] = {0x56, 0x47, 0x50, 0x30, 0x4d, 0x45, 0x54, 0x4c};
    for (size_t i = 0; i < 8; ++i) record.uuid[i] = prefix[i];
    const uint64_t registry_id = [device registryID];
    for (size_t i = 0; i < 8; ++i) record.uuid[8 + i] = (registry_id >> (i * 8)) & 0xffu;
    record.backend_kind = VG_BACKEND_METAL;
    record.adapter_class = [device isLowPower] ? VG_ADAPTER_CLASS_INTEGRATED_GPU
                                                : VG_ADAPTER_CLASS_DISCRETE_GPU;
    record.name = [[device name] UTF8String];
    record.driver = [[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String];
    records.push_back(record);
  }
  return records;
}
}  // namespace vg
