#include "backends/reference/reference_device_hal.h"
#include "conformance_lib.h"

#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: vg_device_hal_conformance_reference <repo_root>\n";
    return 2;
  }
  auto device = vg::reference::make_device_hal();
  vg::conformance::ConformanceExpectation expectation;
  expectation.expect_task_publication = true;
  expectation.expect_timeline = true;
  return vg::conformance::run(*device, "reference", expectation, argv[1]) ? 0 : 1;
}
