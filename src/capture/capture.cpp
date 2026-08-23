#include "capture/capture.h"

#include "backends/reference/reference_executor.h"
#include "ir/json.h"
#include "ir/sha256.h"

#include <stdexcept>

namespace vg::capture {
namespace {
using json::Value;

Value effect_value(const ir::Effect& effect) {
  const char* access = effect.access == ir::Access::Read ? "read" : effect.access == ir::Access::Write ? "write" : effect.access == ir::Access::Atomic ? "atomic" : "publish";
  return Value(Value::Object{{"access", Value(std::string(access))}, {"allocation", Value(static_cast<int64_t>(effect.allocation))},
      {"offset", Value(static_cast<int64_t>(effect.offset))}, {"representation_epoch", Value(static_cast<int64_t>(effect.representation_epoch))}, {"size", Value(static_cast<int64_t>(effect.size))}});
}

ir::Effect effect_from_value(const Value& value) {
  const auto& object = value.object();
  auto required = [&](const char* key) -> const Value& { auto it = object.find(key); if (it == object.end()) throw std::runtime_error(std::string("capture effect missing ") + key); return it->second; };
  const auto access = required("access").string();
  ir::Access kind = access == "read" ? ir::Access::Read : access == "write" ? ir::Access::Write : access == "atomic" ? ir::Access::Atomic : access == "publish" ? ir::Access::Publish : throw std::runtime_error("capture invalid access");
  return {static_cast<uint64_t>(required("allocation").integer()), static_cast<uint64_t>(required("offset").integer()), static_cast<uint64_t>(required("size").integer()), kind, static_cast<uint32_t>(required("representation_epoch").integer())};
}

Value allocation_value(const AllocationSnapshot& allocation) {
  Value::Array bytes; for (uint8_t byte : allocation.bytes) bytes.emplace_back(static_cast<int64_t>(byte));
  return Value(Value::Object{{"bytes", Value(std::move(bytes))}, {"generation", Value(static_cast<int64_t>(allocation.generation))}, {"id", Value(static_cast<int64_t>(allocation.id))}, {"representation_epoch", Value(static_cast<int64_t>(allocation.representation_epoch))}, {"size", Value(static_cast<int64_t>(allocation.size))}, {"state", Value(std::string(allocation.state == core::ObjectState::Active ? "active" : "retired"))}});
}

AllocationSnapshot allocation_from_value(const Value& value) {
  const auto& o = value.object();
  auto number = [&](const char* key) -> uint64_t { auto it = o.find(key); if (it == o.end() || !it->second.is_int() || it->second.integer() < 0) throw std::runtime_error(std::string("capture invalid allocation ") + key); return static_cast<uint64_t>(it->second.integer()); };
  AllocationSnapshot result; result.id = number("id"); result.generation = static_cast<uint32_t>(number("generation")); result.size = number("size"); result.representation_epoch = static_cast<uint32_t>(number("representation_epoch"));
  auto state = o.find("state"); if (state == o.end() || !state->second.is_string()) throw std::runtime_error("capture allocation state is missing");
  if (state->second.string() == "active") result.state = core::ObjectState::Active; else if (state->second.string() == "retired") result.state = core::ObjectState::Retired; else throw std::runtime_error("capture invalid allocation state");
  auto bytes = o.find("bytes");
  if (bytes == o.end()) result.bytes.resize(static_cast<size_t>(result.size), 0);
  else { if (!bytes->second.is_array()) throw std::runtime_error("capture allocation bytes are invalid"); for (const auto& byte : bytes->second.array()) { if (!byte.is_int() || byte.integer() < 0 || byte.integer() > 255) throw std::runtime_error("capture invalid allocation byte"); result.bytes.push_back(static_cast<uint8_t>(byte.integer())); } }
  if (result.bytes.size() != result.size) throw std::runtime_error("capture allocation byte size mismatch");
  return result;
}

Value witness_value(const core::AccessWitness& witness) {
  Value::Array entries; for (const auto& entry : witness.entries()) entries.emplace_back(Value(Value::Object{{"effect", effect_value(entry.effect)}, {"instruction", Value(static_cast<int64_t>(entry.instruction_index))}}));
  return Value(std::move(entries));
}

void parse_witness(const Value& value, core::AccessWitness* witness) {
  if (!value.is_array()) throw std::runtime_error("capture witness must be an array");
  for (const auto& item : value.array()) { const auto& o = item.object(); witness->record(effect_from_value(o.at("effect")), static_cast<uint32_t>(o.at("instruction").integer())); }
}
}

Capture make_capture(const ir::Module& module, const core::Arena& arena) {
  Capture capture; capture.module = module;
  for (const auto& [id, allocation] : arena.allocations()) capture.allocations.push_back({id, allocation.generation, allocation.size, allocation.representation_epoch, allocation.state, allocation.bytes});
  capture.compiler_hash = module.hash;
  capture.schema_hash = "sha256:vg.capture/v1";
  return capture;
}

std::string serialize(const Capture& capture) {
  Value::Array allocations; for (const auto& allocation : capture.allocations) allocations.push_back(allocation_value(allocation));
  Value::Array certificate; for (const auto& effect : capture.certificate.ranges) certificate.push_back(effect_value(effect));
  Value::Array references; for (const auto& reference : capture.graph_references) references.emplace_back(Value(Value::Object{{"allocation", Value(static_cast<int64_t>(reference.allocation))}, {"generation", Value(static_cast<int64_t>(reference.generation))}}));
  Value::Object root{{"allocations", Value(std::move(allocations))}, {"certificate", Value(std::move(certificate))}, {"compiler_hash", Value(capture.compiler_hash)}, {"graph_epoch", Value(static_cast<int64_t>(capture.graph_epoch))}, {"graph_references", Value(std::move(references))}, {"ir", json::parse(capture.module.canonical_json)}, {"ir_hash", Value(capture.module.hash)}, {"schema", Value(std::string("vg.capture/v1"))}, {"schema_hash", Value(capture.schema_hash.empty() ? "sha256:vg.capture/v1" : capture.schema_hash)}, {"schema_version", Value(int64_t{2})}, {"source_hash", Value(capture.source_hash)}, {"timeline_value", Value(static_cast<int64_t>(capture.timeline_value))}, {"witness", witness_value(capture.witness)}};
  if (capture.has_execution) root.emplace("execution", Value(Value::Object{{"fault_code", Value(capture.execution.fault.code)}, {"fault_effect", effect_value(capture.execution.fault.effect)}, {"fault_instruction", Value(static_cast<int64_t>(capture.execution.fault.instruction_index))}, {"fault_message", Value(capture.execution.fault.message)}, {"message", Value(capture.execution.message)}, {"ok", Value(int64_t(capture.execution.ok ? 1 : 0))}, {"outputs_valid", Value(int64_t(capture.execution.outputs_valid ? 1 : 0))}, {"poison", Value(static_cast<int64_t>(capture.execution.poison))}}));
  const auto content = json::canonical(Value(root));
  root.emplace("capture_hash", Value(ir::sha256_hex(content)));
  return json::canonical(Value(std::move(root)));
}

std::string serialize(const ir::Module& module, const core::Arena& arena) { return serialize(make_capture(module, arena)); }

bool deserialize(const std::string& text, Capture* capture, std::string* error) {
  try {
    if (capture == nullptr) throw std::runtime_error("capture output is required");
    auto document = json::parse(text); if (!document.is_object()) throw std::runtime_error("capture root must be an object");
    if (document.find("required") != nullptr) throw std::runtime_error("capture contains unknown required fields");
    auto schema = document.find("schema"); if (schema == nullptr || schema->string() != "vg.capture/v1") throw std::runtime_error("unsupported capture schema");
    capture->schema_hash = document.find("schema_hash") ? document.find("schema_hash")->string() : "sha256:vg.capture/v1";
    if (auto content_hash = document.find("capture_hash"); content_hash != nullptr) { auto without = document.object(); without.erase("capture_hash"); if (ir::sha256_hex(json::canonical(Value(std::move(without)))) != content_hash->string()) throw std::runtime_error("capture content hash mismatch"); }
    auto version = document.find("schema_version"); if (version != nullptr && version->integer() > 2) throw std::runtime_error("unsupported capture schema version");
    auto ir_value = document.find("ir"); if (ir_value == nullptr) throw std::runtime_error("capture is missing IR"); capture->module = ir::parse_module(json::canonical(*ir_value));
    auto hash = document.find("ir_hash"); if (hash == nullptr || hash->string() != capture->module.hash) throw std::runtime_error("capture IR hash mismatch");
    capture->allocations.clear(); if (auto values = document.find("allocations"); values != nullptr) for (const auto& value : values->array()) capture->allocations.push_back(allocation_from_value(value));
    capture->certificate.ranges.clear(); if (auto values = document.find("certificate"); values != nullptr) for (const auto& value : values->array()) capture->certificate.ranges.push_back(effect_from_value(value));
    capture->graph_epoch = document.find("graph_epoch") ? static_cast<uint64_t>(document.find("graph_epoch")->integer()) : 0; capture->timeline_value = document.find("timeline_value") ? static_cast<uint64_t>(document.find("timeline_value")->integer()) : 0;
    capture->source_hash = document.find("source_hash") ? document.find("source_hash")->string() : ""; capture->compiler_hash = document.find("compiler_hash") ? document.find("compiler_hash")->string() : "";
    capture->graph_references.clear(); if (auto values = document.find("graph_references"); values != nullptr) for (const auto& value : values->array()) capture->graph_references.push_back({static_cast<uint64_t>(value.object().at("allocation").integer()), static_cast<uint32_t>(value.object().at("generation").integer())});
    capture->witness = core::AccessWitness{}; if (auto value = document.find("witness"); value != nullptr) parse_witness(*value, &capture->witness);
    capture->has_execution = false; if (auto execution = document.find("execution"); execution != nullptr) { const auto& o = execution->object(); capture->has_execution = true; capture->execution.ok = o.at("ok").integer() != 0; capture->execution.poison = static_cast<core::PoisonState>(o.at("poison").integer()); capture->execution.outputs_valid = o.at("outputs_valid").integer() != 0; capture->execution.message = o.at("message").string(); capture->execution.fault.code = o.at("fault_code").string(); capture->execution.fault.message = o.at("fault_message").string(); capture->execution.fault.instruction_index = static_cast<uint32_t>(o.at("fault_instruction").integer()); capture->execution.fault.effect = effect_from_value(o.at("fault_effect")); }
    return true;
  } catch (const std::exception& exception) { if (error) *error = exception.what(); return false; }
}

bool deserialize(const std::string& text, ir::Module* module, std::string* error) { Capture capture; if (!deserialize(text, &capture, error)) return false; if (module == nullptr) return false; *module = capture.module; return true; }

bool replay(const Capture& capture, ReplayResult* result, std::string* error) {
  try {
    if (result == nullptr) throw std::runtime_error("replay output is required"); core::Arena arena;
    // ConsumeInput releases linear bytes but leaves Allocation::size. A
    // snapshot of that state is not a restoreable representation (02 §4.2 /
    // 09 E005: the lost replay must be named, not reported as a generic
    // import failure).
    for (const auto& snapshot : capture.allocations) {
      if (snapshot.state == core::ObjectState::Active && snapshot.size > 0 && snapshot.bytes.empty()) {
        if (error) *error = "cannot restore a consumed representation";
        return false;
      }
    }
    for (const auto& snapshot : capture.allocations) { if (!arena.import_allocation(snapshot.id, snapshot.generation, snapshot.size, snapshot.representation_epoch, snapshot.state, snapshot.bytes, error)) return false; result->relocation[snapshot.id] = snapshot.id; }
    if (!capture.graph_references.empty()) {
      core::GraphEpochBuilder graph_builder(&arena, capture.graph_epoch == 0 ? 1 : capture.graph_epoch);
      for (const auto& reference : capture.graph_references) { auto it = result->relocation.find(reference.allocation); if (it == result->relocation.end()) throw std::runtime_error("capture graph relocation missing allocation"); if (!graph_builder.add_reference(arena, {it->second, reference.generation}, error)) return false; }
      core::GraphEpoch graph_epoch; if (!graph_builder.seal(&graph_epoch, error)) return false;
    }
    auto module = capture.module; for (auto& instruction : module.instructions) { auto it = result->relocation.find(instruction.allocation); if (it == result->relocation.end()) throw std::runtime_error("capture relocation missing allocation"); instruction.allocation = it->second; }
    result->execution = reference::execute(module, arena, capture.certificate.ranges.empty() ? nullptr : &capture.certificate); return true;
  } catch (const std::exception& exception) { if (error) *error = exception.what(); return false; }
}
}
