#include "compiler/compiler.h"
#include "golden_format.h"
#include "ir/ir.h"

#include <fstream>
#include <iostream>
#include <sstream>

namespace {
std::string read_file(const std::string& path, bool* ok) {
  std::ifstream input(path);
  if (!input) { *ok = false; return {}; }
  std::stringstream buffer;
  buffer << input.rdbuf();
  *ok = true;
  return buffer.str();
}

bool check(const std::string& name, const std::string& kind, const std::string& actual,
           const std::string& golden_path) {
  bool ok = false;
  const std::string expected = read_file(golden_path, &ok);
  if (!ok) { std::cerr << name << " " << kind << ": missing golden file " << golden_path << "\n"; return false; }
  if (expected != actual) {
    std::cerr << name << " " << kind << ": mismatch against " << golden_path << "\n"
               << "--- expected ---\n" << expected << "--- actual ---\n" << actual << "\n";
    return false;
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "usage: vg_compute_package_golden_test <repo_root>\n"; return 2; }
  const std::string root = argv[1];
  bool all_ok = true;
  for (const char* name : vg::golden::kFixtureNames) {
    bool read_ok = false;
    const std::string ir_text = read_file(root + "/tests/fixtures/ir/" + std::string(name) + ".vgir.json", &read_ok);
    if (!read_ok) { std::cerr << name << ": missing IR fixture\n"; all_ok = false; continue; }

    vg::ir::Module module;
    try {
      module = vg::ir::parse_module(ir_text);
    } catch (const std::exception& e) {
      std::cerr << name << ": IR parse failed: " << e.what() << "\n";
      all_ok = false;
      continue;
    }

    const auto package = vg::compiler::build_linear_compute_package(module);
    if (!package.ok) { std::cerr << name << ": codegen failed: " << package.message << "\n"; all_ok = false; continue; }

    const std::string prefix = root + "/tests/fixtures/golden/" + name;
    all_ok = check(name, "msl", package.package.metal_source, prefix + ".msl.golden") && all_ok;
    all_ok = check(name, "glsl", package.package.vulkan_glsl_source, prefix + ".glsl.golden") && all_ok;
    all_ok = check(name, "sourcemap", vg::golden::format_source_map(package.package), prefix + ".sourcemap.golden") && all_ok;
  }
  return all_ok ? 0 : 1;
}
