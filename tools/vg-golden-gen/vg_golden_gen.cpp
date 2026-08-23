#include "compiler/compiler.h"
#include "golden_format.h"
#include "ir/ir.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
std::string read_file(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open: " + path);
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}
void write_file(const std::string& path, const std::string& content) {
  std::ofstream output(path);
  if (!output) throw std::runtime_error("cannot write: " + path);
  output << content;
}
}  // namespace

// Regenerates tests/fixtures/golden/*.golden from tests/fixtures/ir/*.vgir.json.
// Never run automatically by CI or CTest -- review the diff before committing.
int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: vg-golden-gen <repo_root>\n";
    return 2;
  }
  const std::string root = argv[1];
  for (const char* name : vg::golden::kFixtureNames) {
    const std::string ir_path = root + "/tests/fixtures/ir/" + name + ".vgir.json";
    const std::string golden_prefix = root + "/tests/fixtures/golden/" + name;
    try {
      const auto module = vg::ir::parse_module(read_file(ir_path));
      const auto package = vg::compiler::build_linear_compute_package(module);
      if (!package.ok) { std::cerr << name << ": " << package.message << "\n"; return 1; }
      write_file(golden_prefix + ".msl.golden", package.package.metal_source);
      write_file(golden_prefix + ".glsl.golden", package.package.vulkan_glsl_source);
      write_file(golden_prefix + ".sourcemap.golden", vg::golden::format_source_map(package.package));
      std::cout << "wrote golden for " << name << "\n";
    } catch (const std::exception& e) {
      std::cerr << name << ": " << e.what() << "\n";
      return 1;
    }
  }
  return 0;
}
