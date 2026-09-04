#include "core/execution_result.h"

#include "ir/json.h"

#include <utility>

namespace vg::core {

namespace {
// ir::effect_json (ir.cpp) is anonymous-namespace-local to that translation
// unit, so it is not reusable here -- this mirrors its exact key set
// ("access","allocation","offset","representation_epoch","size") and access
// name mapping so canonical_json() output is consistent with the IR module
// serializer's effect representation.
json::Value effect_result_json(const ir::Effect& effect) {
  const char* access_name = "unknown";
  switch (effect.access) {
    case ir::Access::None: access_name = "none"; break;
    case ir::Access::Read: access_name = "read"; break;
    case ir::Access::Write: access_name = "write"; break;
    case ir::Access::Atomic: access_name = "atomic"; break;
    case ir::Access::Publish: access_name = "publish"; break;
  }
  return json::Value(json::Value::Object{
      {"access", json::Value(std::string(access_name))},
      {"allocation", json::Value(static_cast<int64_t>(effect.allocation))},
      {"offset", json::Value(static_cast<int64_t>(effect.offset))},
      {"representation_epoch", json::Value(static_cast<int64_t>(effect.representation_epoch))},
      {"size", json::Value(static_cast<int64_t>(effect.size))}});
}

json::Value effect_array_json(const std::vector<ir::Effect>& effects) {
  json::Value::Array serialized;
  serialized.reserve(effects.size());
  for (const auto& effect : effects) serialized.emplace_back(effect_result_json(effect));
  return json::Value(std::move(serialized));
}
}  // namespace

std::string ExecutionResult::canonical_json() const {
  json::Value::Array witness_entries;
  witness_entries.reserve(witness.entries().size());
  for (const auto& entry : witness.entries()) {
    witness_entries.emplace_back(json::Value(json::Value::Object{
        {"effect", effect_result_json(entry.effect)},
        {"instruction_index", json::Value(static_cast<int64_t>(entry.instruction_index))}}));
  }
  return json::canonical(json::Value(json::Value::Object{
      {"fault", json::Value(json::Value::Object{
                    {"code", json::Value(fault.code)},
                    {"effect", effect_result_json(fault.effect)},
                    {"instruction_index", json::Value(static_cast<int64_t>(fault.instruction_index))},
                    {"message", json::Value(fault.message)},
                    {"task_index", json::Value(static_cast<int64_t>(fault.task_index))}})},
      {"message", json::Value(message)},
      {"missing_effects", effect_array_json(missing_effects)},
      {"ok", json::Value(static_cast<int64_t>(ok ? 1 : 0))},
      {"outputs_valid", json::Value(static_cast<int64_t>(outputs_valid ? 1 : 0))},
      {"poison", json::Value(static_cast<int64_t>(poison))},
      {"trace", effect_array_json(trace)},
      {"witness", json::Value(std::move(witness_entries))}}));
}

}  // namespace vg::core
