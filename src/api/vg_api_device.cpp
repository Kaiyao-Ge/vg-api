#include "api/vg_api_internal.h"
#include "api/vg_api_handle_registry.h"

#include "backends/reference/reference_device_hal.h"
#if defined(VG_HAS_METAL)
#include "backends/metal/metal_device_hal.h"
#endif
#if defined(VG_HAS_VULKAN)
#include "backends/vulkan/vulkan_device_hal.h"
#endif

#include <cstring>
#include <memory>
#include <string>

namespace vg_api {
namespace {
HandleRegistry<VgAdapter_T> g_adapters;
HandleRegistry<VgDevice_T> g_devices;
}  // namespace

bool is_valid_adapter(VgAdapter adapter) { return g_adapters.contains(adapter); }
bool is_valid_device(VgDevice device) { return g_devices.contains(device); }

VgResult VG_CALL open_adapter(VgRuntime runtime, const uint8_t uuid[16], VgAdapter* out_adapter) {
  if (uuid == nullptr || out_adapter == nullptr || !has_runtime(runtime)) {
    set_diagnostic("runtime, uuid and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  for (const auto& record : runtime->adapters) {
    if (std::memcmp(record.uuid, uuid, sizeof(record.uuid)) == 0) {
      auto wrapper = std::make_unique<VgAdapter_T>();
      wrapper->record = record;
      *out_adapter = g_adapters.insert(std::move(wrapper));
      return VG_SUCCESS;
    }
  }
  set_diagnostic("no adapter matches the requested uuid");
  return VG_ERROR_INVALID_ARGUMENT;
}

void VG_CALL close_adapter(VgAdapter adapter) {
  if (!g_adapters.contains(adapter)) return;
  g_adapters.erase(adapter);
}

VgResult VG_CALL create_device(VgAdapter adapter, const VgDeviceDesc* desc, VgDevice* out_device) {
  if (!g_adapters.contains(adapter)) {
    set_diagnostic("adapter handle is stale or invalid");
    return VG_ERROR_STALE_HANDLE;
  }
  if (desc == nullptr || out_device == nullptr) {
    set_diagnostic("device descriptor and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result = validate_header(desc->header, VG_STRUCTURE_DEVICE_DESC, sizeof(VgDeviceDesc));
  if (header_result != VG_SUCCESS) return header_result;

  std::unique_ptr<vg::hal::DeviceHal> hal;
  std::string error;
  switch (adapter->record.backend_kind) {
    case VG_BACKEND_REFERENCE:
      hal = vg::reference::make_device_hal();
      break;
    case VG_BACKEND_METAL:
#if defined(VG_HAS_METAL)
      hal = vg::metal::make_device_hal(adapter->record.uuid, &error);
#else
      error = "this build was configured without VG_ENABLE_METAL";
#endif
      break;
    case VG_BACKEND_VULKAN:
#if defined(VG_HAS_VULKAN)
      hal = vg::vulkan::make_device_hal(adapter->record.uuid, &error);
#else
      error = "this build was configured without VG_ENABLE_VULKAN";
#endif
      break;
    default:
      error = "adapter has an unrecognized backend kind";
      break;
  }
  if (hal == nullptr) {
    set_diagnostic(error.empty() ? "adapter backend is unavailable in this build" : error.c_str());
    return VG_ERROR_UNSUPPORTED;
  }
  auto wrapper = std::make_unique<VgDevice_T>();
  wrapper->hal = std::move(hal);
  *out_device = g_devices.insert(std::move(wrapper));
  return VG_SUCCESS;
}

void VG_CALL destroy_device(VgDevice device) {
  if (!g_devices.contains(device)) return;
  g_devices.erase(device);
}

}  // namespace vg_api
