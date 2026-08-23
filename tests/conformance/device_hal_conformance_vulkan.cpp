#include "backends/vulkan/vulkan_device_hal.h"
#include "conformance_lib.h"

#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: vg_device_hal_conformance_vulkan <repo_root>\n";
    return 2;
  }
  std::string device_error;
  auto device = vg::vulkan::make_device_hal(&device_error);
  if (device == nullptr) {
    std::cerr << "no Vulkan device available on this host: " << device_error << "\n";
    return 1;
  }
  vg::conformance::ConformanceExpectation expectation;
  expectation.expect_linear_subset_only = true;
  // Logically correct but not hardware-verified: this backend only ever
  // compiles on Linux (VG_ENABLE_VULKAN), so these two bits -- and the Task
  // ring Tier0/Tier1 + timeline semaphore code paths they assert -- have
  // been confirmed correct by full-file code review only, never run against
  // real Vulkan/NVIDIA hardware. See TASK-B7/TASK-B8 Known limits.
  expectation.expect_task_publication = true;
  expectation.expect_timeline = true;
  return vg::conformance::run(*device, "vulkan", expectation, argv[1]) ? 0 : 1;
}
