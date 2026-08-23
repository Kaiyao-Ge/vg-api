#include "backends/device_hal.h"

#include "ir/ir.h"
#include "ir/json.h"

namespace vg::hal {

void LoweringReport::add(std::string operation, LoweringClass classification,
                         uint64_t count, uint64_t bytes, std::string reason) {
  events.push_back({std::move(operation), classification, count, bytes, std::move(reason)});
}

uint64_t LoweringReport::count(LoweringClass classification) const {
  uint64_t total = 0;
  for (const auto& event : events) if (event.classification == classification) total += event.count;
  return total;
}

bool LoweringReport::has_hidden_host_wait() const {
  for (const auto& event : events) if (event.classification == LoweringClass::HostAssisted) return true;
  return false;
}

std::string LoweringReport::canonical_json() const {
  json::Value::Array serialized;
  for (const auto& event : events) serialized.emplace_back(json::Value(json::Value::Object{
      {"bytes", json::Value(static_cast<int64_t>(event.bytes))},
      {"classification", json::Value(static_cast<int64_t>(event.classification))},
      {"count", json::Value(static_cast<int64_t>(event.count))},
      {"operation", json::Value(event.operation)},
      {"reason", json::Value(event.reason)}}));
  return json::canonical(json::Value(json::Value::Object{
      {"abi_version", json::Value(static_cast<int64_t>(abi_version))},
      {"backend", json::Value(static_cast<int64_t>(backend))},
      {"diagnostic", json::Value(diagnostic)},
      {"events", json::Value(std::move(serialized))},
      {"supported", json::Value(static_cast<int64_t>(supported ? 1 : 0))}}));
}

bool ExecutionPlan::validate(std::string* error) const {
  if (abi_version != kDeviceHalAbiVersion) { if (error) *error = "execution plan ABI version is unsupported"; return false; }
  if (capabilities.abi_version != kDeviceHalAbiVersion) { if (error) *error = "capability snapshot ABI version is unsupported"; return false; }
  const auto verification = ir::verify(module);
  if (!verification.ok) { if (error) *error = verification.message; return false; }
  if (!capabilities.supports(Capability::LinearAddress)) { if (error) *error = "linear address capability is unsupported"; return false; }
  if (timeline_signal != 0 && timeline_signal <= timeline_wait) { if (error) *error = "timeline signal does not advance past wait"; return false; }
  if (!published && !task_graph.tasks().empty()) { if (error) *error = "execution plan contains unpublished tasks"; return false; }
  if (published && !task_graph.tasks().empty() && !task_graph.validate_execution(error)) { return false; }
  return true;
}

bool ExecutionPlan::graph_epoch_matches(const core::Arena& arena, std::string* error) const {
  if (task_graph.tasks().empty()) { return true; }
  if (graph_epoch != arena.topology_epoch()) {
    if (error) *error = "execution plan graph epoch does not match arena topology";
    return false;
  }
  return true;
}

}  // namespace vg::hal
