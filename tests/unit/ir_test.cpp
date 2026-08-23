#include "compiler/compiler.h"
#include "ir/ir.h"
#include <cassert>

int main() {
  const std::string source = "@node @effects load(1,0,8) store(1,8,8,7)";
  auto compiled = vg::compiler::compile_c_like(source);
  assert(compiled.ok);
  assert(compiled.module.instructions.size() == 2);
  auto parsed = vg::ir::parse_module(compiled.module.canonical_json);
  assert(parsed.hash == compiled.module.hash);
  assert(vg::ir::verify(parsed).ok);
  auto invalid_module = parsed;
  invalid_module.instructions[0].generation = 0;
  assert(!vg::ir::verify(invalid_module).ok);
  invalid_module = parsed;
  invalid_module.instructions[0].offset = UINT64_MAX;
  invalid_module.instructions[0].size = 2;
  assert(!vg::ir::verify(invalid_module).ok);
  invalid_module = parsed;
  invalid_module.declared_effects.clear();
  assert(!vg::ir::verify(invalid_module).ok);
  auto invalid = vg::compiler::compile_c_like("@node @effects for (;;) {}");
  assert(!invalid.ok);
  return 0;
}
