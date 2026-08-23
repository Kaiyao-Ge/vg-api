#include "compiler/compiler.h"
#include "ir/sha256.h"
#include <regex>
#include <sstream>

namespace vg::compiler {
CompileResult compile_c_like(const std::string& source) {
  if (source.find("for") != std::string::npos || source.find("while") != std::string::npos || source.find("import") != std::string::npos || source.find("#include") != std::string::npos) return {false,"Phase A frontend rejects loops and imports",{}};
  if (source.find("@node") == std::string::npos || source.find("@effects") == std::string::npos) return {false,"source requires @node and @effects declarations",{}};
  ir::Module module; module.root_schema="frontend"; std::regex op(R"((load|store|atomic_add|publish)\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)(?:\s*,\s*(-?\d+))?\s*\))");
  for(std::sregex_iterator it(source.begin(),source.end(),op),end;it!=end;++it){const auto& match=*it;ir::Instruction instruction;instruction.op=match[1];instruction.allocation=std::stoull(match[2]);instruction.offset=std::stoull(match[3]);instruction.size=std::stoull(match[4]);instruction.generation=1;if(match[5].matched)instruction.value=std::stoll(match[5]);module.instructions.push_back(instruction);ir::Access access=instruction.op=="load"?ir::Access::Read:instruction.op=="store"?ir::Access::Write:instruction.op=="atomic_add"?ir::Access::Atomic:ir::Access::Publish;module.declared_effects.push_back({instruction.allocation,instruction.offset,instruction.size,access,0});}
  if(module.instructions.empty())return {false,"source contains no supported memory operation",{}}; module.canonical_json=ir::serialize_module(module);module.hash=ir::sha256_hex(module.canonical_json); auto verified=ir::verify(module);return {verified.ok,verified.message,std::move(module)};
}
}
