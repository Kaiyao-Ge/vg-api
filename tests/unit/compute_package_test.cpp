#include "compiler/compiler.h"

#include <cassert>

int main() {
  auto compiled = vg::compiler::compile_c_like("@node @effects store(7,0,4,9) atomic_add(7,8,8,3)");
  assert(compiled.ok);
  const auto package = vg::compiler::build_linear_compute_package(compiled.module);
  assert(package.ok);
  assert(package.package.bindings.size() == 1);
  assert(package.package.bindings[0].allocation == 7);
  assert(package.package.source_map.size() == 2);
  assert(package.package.metal_source.find("vg_linear_compute") != std::string::npos);
  assert(package.package.vulkan_glsl_source.find("atomicAdd") != std::string::npos);

  auto unsupported = vg::compiler::compile_c_like("@node @effects store(7,0,8,9)");
  assert(unsupported.ok);
  assert(!vg::compiler::build_linear_compute_package(unsupported.module).ok);
  return 0;
}
