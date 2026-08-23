#ifndef VG_CAPTURE_CAPTURE_H_
#define VG_CAPTURE_CAPTURE_H_

#include "core/core.h"

#include <unordered_map>

namespace vg::capture {

struct AllocationSnapshot {
  uint64_t id{};
  uint32_t generation{};
  uint64_t size{};
  uint32_t representation_epoch{};
  core::ObjectState state{core::ObjectState::Active};
  std::vector<uint8_t> bytes;
};

struct Capture {
  ir::Module module;
  std::vector<AllocationSnapshot> allocations;
  uint64_t graph_epoch{};
  std::vector<core::PointerRef> graph_references;
  uint64_t timeline_value{};
  core::Certificate certificate;
  core::AccessWitness witness;
  core::ExecutionResult execution;
  bool has_execution{};
  std::string source_hash;
  std::string compiler_hash;
  std::string schema_hash;
};

struct ReplayResult {
  core::ExecutionResult execution;
  std::unordered_map<uint64_t, uint64_t> relocation;
};

Capture make_capture(const ir::Module& module, const core::Arena& arena);
std::string serialize(const Capture& capture);
bool deserialize(const std::string& text, Capture* capture, std::string* error = nullptr);
bool replay(const Capture& capture, ReplayResult* result, std::string* error = nullptr);

std::string serialize(const ir::Module& module, const core::Arena& arena);
bool deserialize(const std::string& text, ir::Module* module, std::string* error = nullptr);
}

#endif
