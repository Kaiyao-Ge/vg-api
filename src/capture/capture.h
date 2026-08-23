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

// Optional portability / view metadata. Omitted from vg.capture/v1 when empty
// so schema_version stays 2 (ADR-040). Metal↔Vulkan correspondence lives here
// as a semantic map only — executed_backends must never name both.
struct ViewMetadata {
  std::string source_backend;
  std::vector<std::string> required_capabilities;
  std::vector<std::string> executed_backends;
  bool semantic_correspondence_only{};
  std::string semantic_counterpart;
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
  ViewMetadata view;
};

struct ReplayResult {
  core::ExecutionResult execution;
  std::unordered_map<uint64_t, uint64_t> relocation;
};

struct ReplayEnvironment {
  std::string backend{"cpu-reference"};
  std::vector<std::string> capabilities;
};

struct ViewReport {
  std::string markdown;
  std::string json;
};

Capture make_capture(const ir::Module& module, const core::Arena& arena);
std::string serialize(const Capture& capture);
bool deserialize(const std::string& text, Capture* capture, std::string* error = nullptr);
bool replay(const Capture& capture, ReplayResult* result, std::string* error = nullptr);
bool replay(const Capture& capture, const ReplayEnvironment& environment, ReplayResult* result,
            std::string* error = nullptr);
bool write_view(const Capture& capture, ViewReport* report, std::string* error = nullptr);
ReplayEnvironment reference_replay_environment();

std::string serialize(const ir::Module& module, const core::Arena& arena);
bool deserialize(const std::string& text, ir::Module* module, std::string* error = nullptr);
}

#endif
