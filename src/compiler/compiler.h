#ifndef VG_COMPILER_COMPILER_H_
#define VG_COMPILER_COMPILER_H_
#include "compiler/compute_package.h"
#include "compiler/shader_sources.h"
#include "ir/ir.h"
#include <string>
namespace vg::compiler {
struct CompileResult { bool ok{}; std::string message; ir::Module module; };
CompileResult compile_c_like(const std::string& source);
}  // namespace vg::compiler
#endif
