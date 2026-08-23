#include "compiler/compiler.h"
#include <fstream>
#include <iostream>
#include <sstream>
int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "usage: vg-compile source.vg\n"; return 2; }
  std::ifstream input(argv[1]); std::stringstream source; source << input.rdbuf();
  auto result = vg::compiler::compile_c_like(source.str());
  if (!result.ok) { std::cerr << result.message << "\n"; return 1; }
  std::cout << result.module.canonical_json << "\n"; return 0;
}
