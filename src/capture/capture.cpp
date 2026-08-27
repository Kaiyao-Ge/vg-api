#include "capture/capture.h"

#include "core/scene_root.h"

#include "backends/reference/reference_executor.h"
#include "ir/json.h"
#include "ir/sha256.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace vg::capture {
namespace {
using json::Value;

const char* capture_access_name(ir::Access access) {
  if (access == ir::Access::Read) return "read";
  if (access == ir::Access::Write) return "write";
  if (access == ir::Access::Atomic) return "atomic";
  return "publish";
}

ir::Access capture_access_from_name(const std::string& access) {
  if (access == "read") return ir::Access::Read;
  if (access == "write") return ir::Access::Write;
  if (access == "atomic") return ir::Access::Atomic;
  if (access == "publish") return ir::Access::Publish;
  throw std::runtime_error("capture invalid access");
}

Value effect_value(const ir::Effect& effect) {
  return Value(Value::Object{{"access", Value(std::string(capture_access_name(effect.access)))}, {"allocation", Value(static_cast<int64_t>(effect.allocation))},
      {"offset", Value(static_cast<int64_t>(effect.offset))}, {"representation_epoch", Value(static_cast<int64_t>(effect.representation_epoch))}, {"size", Value(static_cast<int64_t>(effect.size))}});
}

ir::Effect effect_from_value(const Value& value) {
  const auto& object = value.object();
  auto required = [&](const char* key) -> const Value& { auto it = object.find(key); if (it == object.end()) throw std::runtime_error(std::string("capture effect missing ") + key); return it->second; };
  const auto access = required("access").string();
  ir::Access kind = capture_access_from_name(access);
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
  // A consumed representation is serialized as size>0 with an empty bytes
  // array. View must still read it; replay refuses with the fixed wording.
  if (result.bytes.size() != result.size && !(result.bytes.empty() && result.size > 0))
    throw std::runtime_error("capture allocation byte size mismatch");
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

std::string lower_ascii(std::string value) {
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

bool names_backend(const std::string& name, const char* backend) { return lower_ascii(name) == backend; }

bool claims_metal_and_vulkan_executed(const ViewMetadata& view) {
  bool metal = false;
  bool vulkan = false;
  for (const auto& backend : view.executed_backends) {
    if (names_backend(backend, "metal")) metal = true;
    if (names_backend(backend, "vulkan")) vulkan = true;
  }
  return metal && vulkan;
}

Value string_array(const std::vector<std::string>& values) {
  Value::Array array;
  for (const auto& value : values) array.emplace_back(Value(value));
  return Value(std::move(array));
}

std::vector<std::string> parse_string_array(const Value& value, const char* field) {
  if (!value.is_array()) throw std::runtime_error(std::string("capture ") + field + " must be an array");
  std::vector<std::string> result;
  for (const auto& item : value.array()) {
    if (!item.is_string()) throw std::runtime_error(std::string("capture ") + field + " entries must be strings");
    result.push_back(item.string());
  }
  return result;
}

const char* poison_name(core::PoisonState poison) {
  switch (poison) {
    case core::PoisonState::Valid: return "Valid";
    case core::PoisonState::PartiallyProduced: return "PartiallyProduced";
    case core::PoisonState::Poisoned: return "Poisoned";
  }
  return "Unknown";
}

const char* state_name(core::ObjectState state) { return state == core::ObjectState::Active ? "active" : "retired"; }

bool contains_gpu_address_pattern(const std::string& text) {
  for (size_t i = 0; i + 2 < text.size(); ++i) {
    if (text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X') &&
        std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      return true;
    }
  }
  return false;
}

Value pointer_ref_value(const core::PointerRef& reference) {
  return Value(Value::Object{{"allocation", Value(static_cast<int64_t>(reference.allocation))},
                             {"generation", Value(static_cast<int64_t>(reference.generation))}});
}

core::PointerRef pointer_ref_from_value(const Value& value) {
  const auto& object = value.object();
  return {static_cast<uint64_t>(object.at("allocation").integer()),
          static_cast<uint32_t>(object.at("generation").integer())};
}

Value pointer_ref_array(const std::vector<core::PointerRef>& references) {
  Value::Array array;
  for (const auto& reference : references) array.emplace_back(pointer_ref_value(reference));
  return Value(std::move(array));
}

std::vector<core::PointerRef> parse_pointer_ref_array(const Value& value, const char* field) {
  if (!value.is_array()) throw std::runtime_error(std::string("capture ") + field + " must be an array");
  std::vector<core::PointerRef> result;
  for (const auto& item : value.array()) result.push_back(pointer_ref_from_value(item));
  return result;
}

bool pointer_ref_in(const std::vector<core::PointerRef>& refs, const core::PointerRef& wanted) {
  return std::ranges::any_of(refs, [&](const core::PointerRef& ref) {
    return ref.allocation == wanted.allocation && ref.generation == wanted.generation;
  });
}

void apply_optional_discovery(const Capture& capture, Value::Object* root) {
  if (!capture.has_discovery) return;
  root->emplace("discovery",
                Value(Value::Object{{"frozen_topology_epoch", Value(static_cast<int64_t>(capture.frozen_topology_epoch))},
                                    {"reachable", pointer_ref_array(capture.discovered_reachable)},
                                    {"seeds", pointer_ref_array(capture.discovery_seeds)}}));
}

void apply_optional_view(const Capture& capture, Value::Object* root) {
  if (!capture.view.source_backend.empty()) root->emplace("source_backend", Value(capture.view.source_backend));
  if (!capture.view.required_capabilities.empty())
    root->emplace("required_capabilities", string_array(capture.view.required_capabilities));
  if (!capture.view.executed_backends.empty())
    root->emplace("executed_backends", string_array(capture.view.executed_backends));
  if (capture.view.semantic_correspondence_only || !capture.view.semantic_counterpart.empty()) {
    root->emplace("semantic_correspondence",
                  Value(Value::Object{{"counterpart", Value(capture.view.semantic_counterpart)},
                                      {"only", Value(int64_t{capture.view.semantic_correspondence_only ? 1 : 0})}}));
  }
}

bool environment_has(const ReplayEnvironment& environment, const std::string& capability) {
  return std::ranges::find(environment.capabilities, capability) !=
         environment.capabilities.end();
}

bool refuse_incompatible_capabilities(const Capture& capture, const ReplayEnvironment& environment,
                                      std::string* error) {
  return std::ranges::any_of(capture.view.required_capabilities, [&](const std::string& required) {
    if (environment_has(environment, required)) return false;
    if (error) *error = "incompatible capabilities refused: " + required;
    return true;
  });
}
}

Capture make_capture(const ir::Module& module, const core::Arena& arena) {
  Capture capture; capture.module = module;
  for (const auto& [id, allocation] : arena.allocations()) capture.allocations.push_back({id, allocation.generation, allocation.size, allocation.representation_epoch, allocation.state, allocation.bytes});
  capture.compiler_hash = module.hash;
  capture.schema_hash = "sha256:vg.capture/v1";
  return capture;
}

bool attach_discovery(Capture* capture, const core::Arena& arena, const std::vector<core::PointerRef>& seeds,
                      std::string* error) {
  if (capture == nullptr) {
    if (error) *error = "capture output is required";
    return false;
  }
  core::DiscoveryResult discovery;
  if (!core::discover_reachable(arena, seeds, &discovery, error)) return false;
  capture->has_discovery = true;
  capture->discovery_seeds = seeds;
  capture->discovered_reachable = discovery.reachable;
  capture->frozen_topology_epoch = discovery.frozen_topology_epoch;
  std::vector<AllocationSnapshot> kept;
  for (const auto& snapshot : capture->allocations) {
    if (pointer_ref_in(discovery.reachable, {snapshot.id, snapshot.generation})) kept.push_back(snapshot);
  }
  capture->allocations = std::move(kept);
  std::vector<core::PointerRef> kept_refs;
  for (const auto& reference : capture->graph_references) {
    if (pointer_ref_in(discovery.reachable, reference)) kept_refs.push_back(reference);
  }
  capture->graph_references = std::move(kept_refs);
  return true;
}

std::string serialize(const Capture& capture) {
  Value::Array allocations; for (const auto& allocation : capture.allocations) allocations.push_back(allocation_value(allocation));
  Value::Array certificate; for (const auto& effect : capture.certificate.ranges) certificate.push_back(effect_value(effect));
  Value::Array references; for (const auto& reference : capture.graph_references) references.emplace_back(Value(Value::Object{{"allocation", Value(static_cast<int64_t>(reference.allocation))}, {"generation", Value(static_cast<int64_t>(reference.generation))}}));
  Value::Object root{{"allocations", Value(std::move(allocations))}, {"certificate", Value(std::move(certificate))}, {"compiler_hash", Value(capture.compiler_hash)}, {"graph_epoch", Value(static_cast<int64_t>(capture.graph_epoch))}, {"graph_references", Value(std::move(references))}, {"ir", json::parse(capture.module.canonical_json)}, {"ir_hash", Value(capture.module.hash)}, {"schema", Value(std::string("vg.capture/v1"))}, {"schema_hash", Value(capture.schema_hash.empty() ? "sha256:vg.capture/v1" : capture.schema_hash)}, {"schema_version", Value(int64_t{2})}, {"source_hash", Value(capture.source_hash)}, {"timeline_value", Value(static_cast<int64_t>(capture.timeline_value))}, {"witness", witness_value(capture.witness)}};
  if (capture.has_execution) root.emplace("execution", Value(Value::Object{{"fault_code", Value(capture.execution.fault.code)}, {"fault_effect", effect_value(capture.execution.fault.effect)}, {"fault_instruction", Value(static_cast<int64_t>(capture.execution.fault.instruction_index))}, {"fault_message", Value(capture.execution.fault.message)}, {"message", Value(capture.execution.message)}, {"ok", Value(static_cast<int64_t>(capture.execution.ok ? 1 : 0))}, {"outputs_valid", Value(static_cast<int64_t>(capture.execution.outputs_valid ? 1 : 0))}, {"poison", Value(static_cast<int64_t>(capture.execution.poison))}}));
  apply_optional_view(capture, &root);
  apply_optional_discovery(capture, &root);
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
    const auto* schema = document.find("schema"); if (schema == nullptr || schema->string() != "vg.capture/v1") throw std::runtime_error("unsupported capture schema");
    capture->schema_hash = document.find("schema_hash") ? document.find("schema_hash")->string() : "sha256:vg.capture/v1";
    if (const auto* content_hash = document.find("capture_hash"); content_hash != nullptr) { auto without = document.object(); without.erase("capture_hash"); if (ir::sha256_hex(json::canonical(Value(std::move(without)))) != content_hash->string()) throw std::runtime_error("capture content hash mismatch"); }
    const auto* version = document.find("schema_version"); if (version != nullptr && version->integer() > 2) throw std::runtime_error("unsupported capture schema version");
    const auto* ir_value = document.find("ir"); if (ir_value == nullptr) throw std::runtime_error("capture is missing IR"); capture->module = ir::parse_module(json::canonical(*ir_value));
    const auto* hash = document.find("ir_hash"); if (hash == nullptr || hash->string() != capture->module.hash) throw std::runtime_error("capture IR hash mismatch");
    capture->allocations.clear(); if (const auto* values = document.find("allocations"); values != nullptr) for (const auto& value : values->array()) capture->allocations.push_back(allocation_from_value(value));
    capture->certificate.ranges.clear(); if (const auto* values = document.find("certificate"); values != nullptr) for (const auto& value : values->array()) capture->certificate.ranges.push_back(effect_from_value(value));
    capture->graph_epoch = document.find("graph_epoch") ? static_cast<uint64_t>(document.find("graph_epoch")->integer()) : 0; capture->timeline_value = document.find("timeline_value") ? static_cast<uint64_t>(document.find("timeline_value")->integer()) : 0;
    capture->source_hash = document.find("source_hash") ? document.find("source_hash")->string() : ""; capture->compiler_hash = document.find("compiler_hash") ? document.find("compiler_hash")->string() : "";
    capture->graph_references.clear(); if (const auto* values = document.find("graph_references"); values != nullptr) for (const auto& value : values->array()) capture->graph_references.push_back({static_cast<uint64_t>(value.object().at("allocation").integer()), static_cast<uint32_t>(value.object().at("generation").integer())});
    capture->witness = core::AccessWitness{}; if (const auto* value = document.find("witness"); value != nullptr) parse_witness(*value, &capture->witness);
    capture->has_execution = false; if (const auto* execution = document.find("execution"); execution != nullptr) { const auto& o = execution->object(); capture->has_execution = true; capture->execution.ok = o.at("ok").integer() != 0; capture->execution.poison = static_cast<core::PoisonState>(o.at("poison").integer()); capture->execution.outputs_valid = o.at("outputs_valid").integer() != 0; capture->execution.message = o.at("message").string(); capture->execution.fault.code = o.at("fault_code").string(); capture->execution.fault.message = o.at("fault_message").string(); capture->execution.fault.instruction_index = static_cast<uint32_t>(o.at("fault_instruction").integer()); capture->execution.fault.effect = effect_from_value(o.at("fault_effect")); }
    capture->has_discovery = false;
    capture->discovery_seeds.clear();
    capture->discovered_reachable.clear();
    capture->frozen_topology_epoch = 0;
    if (const auto* discovery = document.find("discovery"); discovery != nullptr) {
      if (!discovery->is_object()) throw std::runtime_error("capture discovery must be an object");
      const auto& object = discovery->object();
      auto seeds = object.find("seeds");
      auto reachable = object.find("reachable");
      if (seeds == object.end() || reachable == object.end()) throw std::runtime_error("capture discovery is missing seeds or reachable");
      capture->discovery_seeds = parse_pointer_ref_array(seeds->second, "discovery.seeds");
      capture->discovered_reachable = parse_pointer_ref_array(reachable->second, "discovery.reachable");
      if (auto frozen = object.find("frozen_topology_epoch"); frozen != object.end()) {
        if (!frozen->second.is_int() || frozen->second.integer() < 0)
          throw std::runtime_error("capture discovery frozen_topology_epoch is invalid");
        capture->frozen_topology_epoch = static_cast<uint64_t>(frozen->second.integer());
      }
      capture->has_discovery = true;
    }
    capture->view = {};
    if (const auto* value = document.find("source_backend"); value != nullptr) {
      if (!value->is_string()) throw std::runtime_error("capture source_backend must be a string");
      capture->view.source_backend = value->string();
    }
    if (const auto* value = document.find("required_capabilities"); value != nullptr)
      capture->view.required_capabilities = parse_string_array(*value, "required_capabilities");
    if (const auto* value = document.find("executed_backends"); value != nullptr)
      capture->view.executed_backends = parse_string_array(*value, "executed_backends");
    if (const auto* value = document.find("semantic_correspondence"); value != nullptr) {
      if (!value->is_object()) throw std::runtime_error("capture semantic_correspondence must be an object");
      const auto& object = value->object();
      if (auto only = object.find("only"); only != object.end())
        capture->view.semantic_correspondence_only = only->second.is_int() && only->second.integer() != 0;
      if (auto counterpart = object.find("counterpart"); counterpart != object.end()) {
        if (!counterpart->second.is_string()) throw std::runtime_error("capture semantic counterpart must be a string");
        capture->view.semantic_counterpart = counterpart->second.string();
      }
    }
    return true;
  } catch (const std::exception& exception) { if (error) *error = exception.what(); return false; }
}

bool deserialize(const std::string& text, ir::Module* module, std::string* error) { Capture capture; if (!deserialize(text, &capture, error)) return false; if (module == nullptr) return false; *module = capture.module; return true; }

bool replay(const Capture& capture, ReplayResult* result, std::string* error) {
  return replay(capture, reference_replay_environment(), result, error);
}

bool replay(const Capture& capture, const ReplayEnvironment& environment, ReplayResult* result, std::string* error) {
  try {
    if (result == nullptr) throw std::runtime_error("replay output is required"); core::Arena arena;
    // A SceneRoot stores FacetRef capabilities in generated bytes. Capture v1
    // has no FacetPool snapshot/reacquire table, so replaying those bytes would
    // silently resurrect stale tokens. Refuse until the generated relocation
    // metadata is wired through a capture schema revision.
    if (core::is_scene_root_raster_schema(capture.module.root_schema)) {
      if (error) *error = "F6 SceneRoot capture replay is unsupported until facet relocation is implemented";
      return false;
    }
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
    if (refuse_incompatible_capabilities(capture, environment, error)) return false;
    for (const auto& snapshot : capture.allocations) { if (!arena.import_allocation(core::RepresentationRef{snapshot.id, snapshot.generation, snapshot.representation_epoch}, snapshot.size, snapshot.state, snapshot.bytes, error)) return false; result->relocation[snapshot.id] = snapshot.id; }
    if (!capture.graph_references.empty()) {
      core::GraphEpochBuilder graph_builder(&arena, capture.graph_epoch == 0 ? 1 : capture.graph_epoch);
      for (const auto& reference : capture.graph_references) { auto it = result->relocation.find(reference.allocation); if (it == result->relocation.end()) throw std::runtime_error("capture graph relocation missing allocation"); if (!graph_builder.add_reference(arena, {it->second, reference.generation}, error)) return false; }
      core::GraphEpoch graph_epoch; if (!graph_builder.seal(&graph_epoch, error)) return false;
    }
    if (capture.has_discovery) {
      std::vector<core::PointerRef> relocated_seeds;
      for (const auto& seed : capture.discovery_seeds) {
        auto it = result->relocation.find(seed.allocation);
        if (it == result->relocation.end()) throw std::runtime_error("capture discovery seed relocation missing allocation");
        relocated_seeds.push_back({it->second, seed.generation});
      }
      core::DiscoveryResult rediscovered;
      if (!core::discover_reachable(arena, relocated_seeds, &rediscovered, error)) return false;
      if (rediscovered.reachable.size() != capture.discovered_reachable.size()) {
        if (error) *error = "replay discovery reachable set does not match capture";
        return false;
      }
      for (const auto& recorded : capture.discovered_reachable) {
        auto it = result->relocation.find(recorded.allocation);
        if (it == result->relocation.end()) throw std::runtime_error("capture discovery reachable relocation missing allocation");
        const core::PointerRef want{it->second, recorded.generation};
        if (!pointer_ref_in(rediscovered.reachable, want)) {
          if (error) *error = "replay discovery reachable set does not match capture";
          return false;
        }
      }
    }
    auto module = capture.module; for (auto& instruction : module.instructions) { auto it = result->relocation.find(instruction.allocation); if (it == result->relocation.end()) throw std::runtime_error("capture relocation missing allocation"); instruction.allocation = it->second; }
    result->execution = reference::execute(module, arena, capture.certificate.ranges.empty() ? nullptr : &capture.certificate); return true;
  } catch (const std::exception& exception) { if (error) *error = exception.what(); return false; }
}

ReplayEnvironment reference_replay_environment() {
  return {"cpu-reference",
          {"LinearAddress", "TaskPublication", "Timeline", "EffectDag", "CaptureReplay", "IndirectTier1", "Raster",
           "RepresentationTransform", "CheckedFacetGeneration"}};
}

bool write_view(const Capture& capture, ViewReport* report, std::string* error) {
  try {
    if (report == nullptr) throw std::runtime_error("view report output is required");
    if (claims_metal_and_vulkan_executed(capture.view)) {
      if (error) *error = "cannot claim Metal and Vulkan both executed";
      return false;
    }

    const auto serialized = serialize(capture);
    auto document = json::parse(serialized);
    const std::string capture_hash = document.find("capture_hash") ? document.find("capture_hash")->string() : "";
    const std::string ir_hash = capture.module.hash;

    Value::Array allocations;
    std::ostringstream markdown;
    markdown << "# Capture view\n\n";
    markdown << "- schema: vg.capture/v1\n";
    markdown << "- schema_version: 2\n";
    markdown << "- capture_hash: " << capture_hash << "\n";
    markdown << "- ir_hash: " << ir_hash << "\n";
    markdown << "- source_backend: " << (capture.view.source_backend.empty() ? "unspecified" : capture.view.source_backend)
             << "\n";
    markdown << "- executed_backends:";
    if (capture.view.executed_backends.empty()) markdown << " none";
    else {
      for (const auto& backend : capture.view.executed_backends) markdown << " " << backend;
    }
    markdown << "\n";
    if (capture.view.semantic_correspondence_only || !capture.view.semantic_counterpart.empty()) {
      markdown << "- semantic_correspondence: " << capture.view.semantic_counterpart << " (not executed)\n";
      markdown << "- semantic_fields: stable allocation ids, ir hash, representation_epoch, fault taxonomy\n";
    }
    markdown << "\n## Allocations\n\n";
    markdown << "| id | generation | representation_epoch | size | stored_bytes | state |\n";
    markdown << "| --- | --- | --- | --- | --- | --- |\n";
    for (const auto& allocation : capture.allocations) {
      const auto stored = static_cast<int64_t>(allocation.bytes.size());
      markdown << "| " << allocation.id << " | " << allocation.generation << " | " << allocation.representation_epoch
               << " | " << allocation.size << " | " << stored << " | " << state_name(allocation.state) << " |\n";
      allocations.emplace_back(Value(Value::Object{
          {"generation", Value(static_cast<int64_t>(allocation.generation))},
          {"id", Value(static_cast<int64_t>(allocation.id))},
          {"representation_epoch", Value(static_cast<int64_t>(allocation.representation_epoch))},
          {"size", Value(static_cast<int64_t>(allocation.size))},
          {"state", Value(std::string(state_name(allocation.state)))},
          {"stored_bytes", Value(stored)},
      }));
    }

    markdown << "\n## Execution\n\n";
    Value::Object execution{{"fault_code", Value(std::string{})},
                            {"fault_message", Value(std::string{})},
                            {"ok", Value(int64_t{0})},
                            {"poison", Value(std::string("none"))}};
    if (capture.has_execution) {
      markdown << "- ok: " << (capture.execution.ok ? 1 : 0) << "\n";
      markdown << "- poison: " << poison_name(capture.execution.poison) << "\n";
      markdown << "- fault_code: " << capture.execution.fault.code << "\n";
      markdown << "- fault_message: " << capture.execution.fault.message << "\n";
      execution["fault_code"] = Value(capture.execution.fault.code);
      execution["fault_message"] = Value(capture.execution.fault.message);
      execution["ok"] = Value(int64_t{capture.execution.ok ? 1 : 0});
      execution["poison"] = Value(std::string(poison_name(capture.execution.poison)));
    } else {
      markdown << "- recorded: no\n";
    }

    markdown << "\n## Dynamic graph\n\n";
    Value::Object dynamic_graph;
    if (capture.has_discovery) {
      markdown << "- status: recorded\n";
      markdown << "- seed_count: " << capture.discovery_seeds.size() << "\n";
      markdown << "- reachable_count: " << capture.discovered_reachable.size() << "\n";
      markdown << "- frozen_topology_epoch: " << capture.frozen_topology_epoch << "\n";
      dynamic_graph = Value::Object{
          {"frozen_topology_epoch", Value(static_cast<int64_t>(capture.frozen_topology_epoch))},
          {"reachable_count", Value(static_cast<int64_t>(capture.discovered_reachable.size()))},
          {"seed_count", Value(static_cast<int64_t>(capture.discovery_seeds.size()))},
          {"status", Value(std::string("recorded"))},
      };
    } else {
      markdown << "- status: blocked\n";
      markdown << "- reason: no discovery API; graph snapshot not invented\n";
      dynamic_graph = Value::Object{{"reason", Value(std::string("no discovery API; graph snapshot not invented"))},
                                   {"status", Value(std::string("blocked"))}};
    }

    markdown << "\n## Cross-backend\n\n";
    markdown << "- mapping: semantic only\n";
    markdown << "- fields: stable allocation ids, ir hash, representation_epoch, fault taxonomy\n";
    markdown << "- executed_both_metal_and_vulkan: no\n";

    Value::Object root{
        {"allocations", Value(std::move(allocations))},
        {"capture_hash", Value(capture_hash)},
        {"cross_backend",
         Value(Value::Object{{"executed_backends", string_array(capture.view.executed_backends)},
                             {"mapping", Value(std::string("semantic-only"))},
                             {"metal_and_vulkan_both_executed", Value(int64_t{0})},
                             {"semantic_counterpart", Value(capture.view.semantic_counterpart)},
                             {"semantic_fields", string_array({"fault_taxonomy", "ir_hash", "representation_epoch",
                                                               "stable_allocation_ids"})}})},
        {"dynamic_graph", Value(std::move(dynamic_graph))},
        {"execution", Value(std::move(execution))},
        {"ir_hash", Value(ir_hash)},
        {"schema", Value(std::string("vg.capture/v1"))},
        {"schema_version", Value(int64_t{2})},
        {"source_backend", Value(capture.view.source_backend)},
    };
    report->markdown = markdown.str();
    report->json = json::canonical(Value(std::move(root)));
    if (contains_gpu_address_pattern(report->markdown) || contains_gpu_address_pattern(report->json)) {
      if (error) *error = "view report must not contain GPU addresses";
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}
}
