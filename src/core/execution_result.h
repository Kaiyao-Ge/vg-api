#ifndef VG_CORE_EXECUTION_RESULT_H_
#define VG_CORE_EXECUTION_RESULT_H_

#include "core/access.h"
#include "core/resource_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vg::core {

struct FaultRecord {
  uint32_t instruction_index{};
  ir::Effect effect{};
  std::string code;
  std::string message;
  uint32_t task_index{};
};

struct ExecutionResult {
  bool ok{};
  PoisonState poison{PoisonState::Valid};
  std::string message;
  std::vector<ir::Effect> trace;
  std::vector<ir::Effect> missing_effects;
  FaultRecord fault;
  AccessWitness witness;
  bool outputs_valid{true};

  // v1.2 (ADR-045): serializes the full execution outcome -- ok/poison/
  // message/fault/witness entries/missing_effects/outputs_valid -- so the
  // public C ABI's getSubmissionExecutionResult can surface it without a
  // second ABI-fragile struct mirror, matching LoweringReport::canonical_json().
  [[nodiscard]] std::string canonical_json() const;
};

}  // namespace vg::core

#endif
