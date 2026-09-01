#include "backends/metal/metal_device_hal.h"
#include "conformance_lib.h"

#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: vg_device_hal_conformance_metal <repo_root>\n";
    return 2;
  }
  auto device = vg::metal::make_device_hal();
  if (device == nullptr) {
    std::cerr << "no Metal device available on this host\n";
    return 1;
  }
  vg::conformance::ConformanceExpectation expectation;
  expectation.expect_task_publication = true;
  expectation.expect_timeline = true;
  return vg::conformance::run(*device, "metal", expectation, argv[1]) ? 0 : 1;
}
