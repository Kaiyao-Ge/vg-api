#include "backends/probe.h"

#include <vulkan/vulkan.h>

#include <cstring>

namespace vg {
std::vector<AdapterRecord> vulkan_adapters() {
  VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app_info.pApplicationName = "VG Phase 0 probe";
  app_info.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  create_info.pApplicationInfo = &app_info;
  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) return {};

  uint32_t count = 0;
  if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS) {
    vkDestroyInstance(instance, nullptr);
    return {};
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  std::vector<AdapterRecord> records;
  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    AdapterRecord record{};
    std::memcpy(record.uuid, &properties.vendorID, sizeof(properties.vendorID));
    std::memcpy(record.uuid + 4, &properties.deviceID, sizeof(properties.deviceID));
    std::memcpy(record.uuid + 8, properties.pipelineCacheUUID, 8);
    record.backend_kind = VG_BACKEND_VULKAN;
    record.adapter_class = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
        ? VG_ADAPTER_CLASS_DISCRETE_GPU : VG_ADAPTER_CLASS_INTEGRATED_GPU;
    record.name = properties.deviceName;
    record.driver = "Vulkan API " + std::to_string(VK_API_VERSION_MAJOR(properties.apiVersion)) + "." +
        std::to_string(VK_API_VERSION_MINOR(properties.apiVersion));
    records.push_back(record);
  }
  vkDestroyInstance(instance, nullptr);
  return records;
}
}  // namespace vg
