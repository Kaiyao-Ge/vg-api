#ifndef VG_BACKENDS_METAL_DEVICE_HAL_H_
#define VG_BACKENDS_METAL_DEVICE_HAL_H_

#include "backends/device_hal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vg::metal {

// Plain-data snapshot used by diagnostics and the backend loader. Objective-C
// objects remain private to the .mm implementation.
struct DeviceSnapshot {
  hal::CapabilitySnapshot hal;
  uint32_t gpu_family{};
  uint32_t argument_buffer_tier{};
  bool unified_memory{};
  bool supports_shared_events{};
  bool supports_indirect_command_buffers{};
  bool supports_gpu_addresses{};
  bool supports_counter_sampling{};
};

struct BufferSnapshot {
  size_t requested_length{};
  size_t allocated_length{};
  uint64_t gpu_address{};
  uint32_t storage_mode{};
  bool gpu_address_available{};
};

// Test-only observation of the arguments handed to Metal's real compute
// command encoder by the plan-driven Node-aware path. `pipeline_ordinal` is
// local to one submission: equal values mean the exact same
// MTLComputePipelineState was bound, without exposing an Objective-C object
// through this C++ header.
struct NodeAwareDispatchObservation {
  uint32_t task_index{};
  uint32_t node_index{};
  uint32_t node_generation{};
  std::array<uint32_t, 3> threadgroups{};
  uint32_t pipeline_ordinal{};
};

class DeviceHal final : public hal::DeviceHal {
 public:
  DeviceHal(const DeviceHal&) = delete;
  DeviceHal& operator=(const DeviceHal&) = delete;
  DeviceHal(DeviceHal&&) = delete;
  DeviceHal& operator=(DeviceHal&&) = delete;
  ~DeviceHal() override;
  [[nodiscard]] const hal::CapabilitySnapshot& capabilities() const override;
  bool compile(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled,
               std::string* error = nullptr) override;
  bool submit(const hal::CompiledPlan& compiled, core::Arena& arena,
              hal::Submission* submission, std::string* error = nullptr) override;

  [[nodiscard]] const DeviceSnapshot& snapshot() const;
  bool probe_buffer(size_t length, bool private_storage, BufferSnapshot* result,
                    std::string* error = nullptr) const;

  [[nodiscard]] const std::vector<NodeAwareDispatchObservation>&
  last_node_aware_dispatches() const;

 private:
  friend class AdapterHarness;
  bool compile_plan(const core::ExecutionPlan& plan, hal::CompiledPlan* compiled, std::string* error);
  bool submit_plan(const hal::CompiledPlan& compiled, core::Arena& arena,
                   hal::Submission* submission, std::string* error);
  struct Impl;
  struct CompileOps;
  struct SubmitOps;
  explicit DeviceHal(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend std::unique_ptr<DeviceHal> make_device_hal();
  friend std::unique_ptr<DeviceHal> make_device_hal(const uint8_t uuid[16], std::string* error);
};

std::unique_ptr<DeviceHal> make_device_hal();

// ADR-044 (F1): honors a caller's openAdapter/createDevice choice instead of
// always taking the system default device -- 04-public-c-abi.md Sec.17
// forbids implicit GPU selection. uuid must match one metal_adapters() (or
// metal_probe.mm) already produced (VGP0METL prefix + little-endian
// registryID). Returns nullptr and fills *error when no MTLDevice matches.
std::unique_ptr<DeviceHal> make_device_hal(const uint8_t uuid[16], std::string* error);

}  // namespace vg::metal

#endif
