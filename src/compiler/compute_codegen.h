#ifndef VG_COMPILER_COMPUTE_CODEGEN_H_
#define VG_COMPILER_COMPUTE_CODEGEN_H_
#include "compiler/compute_package.h"
#include <span>
#include <string>

namespace vg::compiler::detail {
// Internal emission seam: package validation and binding/source-map ownership
// remain in compute_package.cpp. Inputs have already passed its subset checks.
struct ComputeSources {
  std::string metal_source;
  std::string vulkan_glsl_source;
};
ComputeSources emit_linear_compute_sources(const ir::Module& module,
                                           std::span<const ComputeBinding> bindings,
                                           bool has_atomic);
ComputeSources emit_pointer_graph_compute_sources(const ir::Module& module,
                                                  std::span<const ComputeBinding> bindings);
ComputeSources emit_indexed_compute_sources(const ir::Module& module,
                                            uint32_t binding_count,
                                            std::span<const uint32_t> table_index_by_instruction);
}  // namespace vg::compiler::detail
#endif
