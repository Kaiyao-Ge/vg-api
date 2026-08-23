#include "backends/reference/reference_executor.h"
#include "ir/ir.h"
#include <fstream>
#include <iostream>
#include <sstream>
int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "usage: vg-reference module.json\n"; return 2; }
  std::ifstream input(argv[1]); std::stringstream text; text << input.rdbuf();
  try {
    auto module = vg::ir::parse_module(text.str());
    vg::core::Arena arena;
    auto& allocation = arena.allocate(1u << 20);
    for (const auto& instruction : module.instructions) {
      if (instruction.allocation != allocation.id) {
        std::cerr << "vg-reference only accepts allocation id 1 in this Phase A tool\n";
        return 1;
      }
    }
    auto result = vg::reference::execute(module, arena);
    if (!result.ok) { std::cerr << result.message << "\n"; return 1; }
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
}
