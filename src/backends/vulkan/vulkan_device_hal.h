#ifndef VG_BACKENDS_VULKAN_DEVICE_HAL_H_
#define VG_BACKENDS_VULKAN_DEVICE_HAL_H_

#include "backends/device_hal.h"
#include <memory>

namespace vg::vulkan {

namespace detail { struct DeviceState; }
class AdapterHarness;

class DeviceHal final : public vg::hal::DeviceHal {
 public:
  ~DeviceHal() override;
  DeviceHal(const DeviceHal&) = delete;
  DeviceHal& operator=(const DeviceHal&) = delete;
  DeviceHal(DeviceHal&&) = delete;
  DeviceHal& operator=(DeviceHal&&) = delete;
  const vg::hal::CapabilitySnapshot& capabilities() const override;
  bool compile(const vg::core::ExecutionPlan& plan, vg::hal::CompiledPlan* compiled,
               std::string* error = nullptr) override;
  bool submit(const vg::hal::CompiledPlan& compiled, vg::core::Arena& arena,
              vg::hal::Submission* submission, std::string* error = nullptr) override;
 private:
  DeviceHal();
  static std::unique_ptr<DeviceHal> create_impl(const uint8_t* uuid, std::string* error);
  std::unique_ptr<detail::DeviceState> state_;
  friend class AdapterHarness;
  friend std::unique_ptr<DeviceHal> make_device_hal(std::string* error);
  friend std::unique_ptr<DeviceHal> make_device_hal(const uint8_t uuid[16], std::string* error);
};
std::unique_ptr<DeviceHal> make_device_hal(std::string* error = nullptr);
std::unique_ptr<DeviceHal> make_device_hal(const uint8_t uuid[16], std::string* error);

}  // namespace vg::vulkan

#endif
