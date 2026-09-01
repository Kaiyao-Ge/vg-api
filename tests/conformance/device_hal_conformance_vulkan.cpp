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
  // These checks execute only in the Linux VG_ENABLE_VULKAN lane. Task
  // publication is a dedicated ring pass; canonical compute execution is the
  // separate NodeRef-keyed per-Task path exercised by the Vulkan vertical
  // slice. Do not infer IndirectTier1 support from this expectation.
  expectation.expect_task_publication = true;
  expectation.expect_timeline = true;
  return vg::conformance::run(*device, "vulkan", expectation, argv[1]) ? 0 : 1;
}
