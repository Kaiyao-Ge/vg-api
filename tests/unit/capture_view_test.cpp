#include "backends/reference/reference_executor.h"
#include "capture/capture.h"
#include "compiler/compiler.h"
#include "ir/json.h"
#include "ir/sha256.h"

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// Each CLI invocation owns its directory, even when test processes share TMPDIR.
// create_directory claims it atomically; cleanup never targets the shared root.
class CaptureTempDirectory {
 public:
  CaptureTempDirectory() {
    const auto root = std::filesystem::temp_directory_path();
    std::random_device random;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
      auto candidate = root / ("vg-e014-capture-view-" + std::to_string(random()) + "-" +
                               std::to_string(random()) + "-" + std::to_string(random()) + "-" +
                               std::to_string(random()));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = std::move(candidate);
        return;
      }
      if (error && error != std::errc::file_exists)
        throw std::filesystem::filesystem_error("cannot create capture test directory", candidate, error);
    }
    throw std::runtime_error("cannot claim a unique capture test directory");
  }
  ~CaptureTempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  CaptureTempDirectory(const CaptureTempDirectory&) = delete;
  CaptureTempDirectory& operator=(const CaptureTempDirectory&) = delete;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

vg::core::ConsumeProof discharged_proof() { return {true, true, true, true}; }

bool has_gpu_address_pattern(const std::string& text) {
  for (size_t i = 0; i + 2 < text.size(); ++i) {
    if (text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X') &&
        std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      return true;
    }
  }
  return false;
}

std::string field_string(const vg::json::Value& document, const char* key) {
  const auto* value = document.find(key);
  assert(value != nullptr && value->is_string());
  return value->string();
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path);
  assert(output);
  output << text;
  assert(output);
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path);
  assert(input);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

vg::capture::Capture exact_compute_capture() {
  auto compiled = vg::compiler::compile_c_like("@node @effects store(1,0,8,3) atomic_add(1,8,8,4) publish(1,0,1)");
  assert(compiled.ok);
  vg::core::Arena arena;
  auto& allocation = arena.allocate(32);
  std::memset(allocation.bytes.data(), 0, allocation.bytes.size());
  vg::core::Certificate certificate;
  certificate.ranges = compiled.module.declared_effects;
  auto result = vg::reference::execute(compiled.module, arena, &certificate);
  assert(result.ok && result.poison == vg::core::PoisonState::Valid);
  auto capture = vg::capture::make_capture(compiled.module, arena);
  capture.certificate = certificate;
  capture.execution = result;
  capture.witness = result.witness;
  capture.has_execution = true;
  capture.graph_epoch = arena.topology_epoch();
  capture.graph_references.push_back({allocation.id, allocation.generation});
  capture.source_hash = "e014-compute-exact";
  capture.view.source_backend = "cpu-reference";
  capture.view.executed_backends = {"cpu-reference"};
  capture.view.required_capabilities = {"CaptureReplay", "LinearAddress"};
  return capture;
}

constexpr size_t kPointerRefWireBytes = sizeof(uint64_t) + sizeof(uint32_t);

void write_ref(vg::core::Allocation& allocation, const vg::core::PointerRef& ref) {
  assert(allocation.bytes.size() >= kPointerRefWireBytes);
  std::memcpy(allocation.bytes.data(), &ref.allocation, sizeof(ref.allocation));
  std::memcpy(allocation.bytes.data() + sizeof(ref.allocation), &ref.generation, sizeof(ref.generation));
}

vg::capture::Capture dynamic_graph_capture() {
  auto compiled = vg::compiler::compile_c_like("@node @effects store(1,0,4,3)");
  assert(compiled.ok);
  vg::core::Arena arena;
  auto& n0 = arena.allocate(12);
  auto& n1 = arena.allocate(12);
  auto& n2 = arena.allocate(12);
  auto& n3 = arena.allocate(12);
  write_ref(n0, {n1.id, n1.generation});
  write_ref(n1, {n2.id, 0});
  write_ref(n2, {n3.id, n3.generation});
  auto capture = vg::capture::make_capture(compiled.module, arena);
  capture.certificate.ranges = compiled.module.declared_effects;
  capture.graph_epoch = arena.topology_epoch();
  capture.source_hash = "e014-dynamic-graph";
  capture.view.source_backend = "cpu-reference";
  capture.view.executed_backends = {"cpu-reference"};
  std::string error;
  assert(vg::capture::attach_discovery(&capture, arena, {{n0.id, n0.generation}}, &error));
  assert(capture.has_discovery);
  assert(capture.discovered_reachable.size() == 2);
  assert(capture.allocations.size() == 2);
  return capture;
}
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--write-fixture") {
    write_text(argv[2], vg::capture::serialize(exact_compute_capture()));
    return 0;
  }

  auto capture = exact_compute_capture();
  const auto first = vg::capture::serialize(capture);
  const auto second = vg::capture::serialize(capture);
  assert(first == second);
  const auto first_doc = vg::json::parse(first);
  const auto second_doc = vg::json::parse(second);
  assert(field_string(first_doc, "capture_hash") == field_string(second_doc, "capture_hash"));
  assert(field_string(first_doc, "ir_hash") == field_string(second_doc, "ir_hash"));
  assert(field_string(first_doc, "ir_hash") == capture.module.hash);

  vg::capture::Capture decoded;
  std::string error;
  assert(vg::capture::deserialize(first, &decoded, &error));
  assert(decoded.view.source_backend == "cpu-reference");
  assert(decoded.view.required_capabilities.size() == 2);
  const auto third = vg::capture::serialize(decoded);
  const auto third_doc = vg::json::parse(third);
  assert(field_string(third_doc, "capture_hash") == field_string(first_doc, "capture_hash"));
  assert(field_string(third_doc, "ir_hash") == field_string(first_doc, "ir_hash"));

  vg::capture::ReplayResult replay;
  assert(vg::capture::replay(decoded, &replay, &error));
  assert(replay.execution.ok);
  assert(replay.execution.poison == vg::core::PoisonState::Valid);

  {
    auto compiled = vg::compiler::compile_c_like("@node @effects store(1,16,4,9) atomic_add(1,20,4,1)");
    assert(compiled.ok);
    vg::core::Arena arena;
    auto& allocation = arena.allocate(32);
    std::memset(allocation.bytes.data(), 0, allocation.bytes.size());
    vg::core::Certificate certificate;
    certificate.ranges = compiled.module.declared_effects;
    auto partial = vg::reference::execute(compiled.module, arena, &certificate);
    assert(!partial.ok && partial.poison == vg::core::PoisonState::PartiallyProduced);
    assert(partial.fault.code == "ATOMIC_WIDTH");
    auto fault_capture = vg::capture::make_capture(compiled.module, arena);
    fault_capture.certificate = certificate;
    fault_capture.witness = partial.witness;
    fault_capture.execution = partial;
    fault_capture.has_execution = true;
    vg::capture::Capture decoded_fault;
    assert(vg::capture::deserialize(vg::capture::serialize(fault_capture), &decoded_fault, &error));
    vg::capture::ReplayResult replay_fault;
    assert(vg::capture::replay(decoded_fault, &replay_fault, &error));
    assert(!replay_fault.execution.ok);
    assert(replay_fault.execution.poison == vg::core::PoisonState::PartiallyProduced);
    vg::capture::ViewReport fault_view;
    assert(vg::capture::write_view(decoded_fault, &fault_view, &error));
    assert(fault_view.markdown.find("ATOMIC_WIDTH") != std::string::npos);
    assert(fault_view.markdown.find("PartiallyProduced") != std::string::npos);
  }

  {
    auto compiled = vg::compiler::compile_c_like("@node @effects store(1,0,8,3) atomic_add(1,8,8,4) publish(1,0,1)");
    assert(compiled.ok);
    vg::core::Arena arena;
    auto& allocation = arena.allocate(32);
    std::memset(allocation.bytes.data(), 0, allocation.bytes.size());
    vg::core::Certificate incomplete;
    incomplete.ranges.push_back(compiled.module.declared_effects.front());
    auto rejected = vg::reference::execute(compiled.module, arena, &incomplete);
    assert(!rejected.ok && rejected.poison == vg::core::PoisonState::Poisoned);
    assert(rejected.fault.code == "CERTIFICATE_MISS");
    auto poison_capture = vg::capture::make_capture(compiled.module, arena);
    poison_capture.certificate = incomplete;
    poison_capture.execution = rejected;
    poison_capture.has_execution = true;
    vg::capture::Capture decoded_poison;
    assert(vg::capture::deserialize(vg::capture::serialize(poison_capture), &decoded_poison, &error));
    vg::capture::ReplayResult replay_poison;
    assert(vg::capture::replay(decoded_poison, &replay_poison, &error));
    assert(!replay_poison.execution.ok);
    assert(replay_poison.execution.poison == vg::core::PoisonState::Poisoned);
    vg::capture::ViewReport poison_view;
    assert(vg::capture::write_view(decoded_poison, &poison_view, &error));
    assert(poison_view.markdown.find("CERTIFICATE_MISS") != std::string::npos);
    assert(poison_view.markdown.find("Poisoned") != std::string::npos);
  }

  {
    vg::core::Arena cap_arena;
    auto& backing = cap_arena.allocate(16);
    backing.bytes.assign(16, 0);
    backing.bytes[0] = 255;
    vg::ir::Module module;
    module.version = 1;
    module.root_schema = "vg.test/v1";
    vg::ir::Instruction load;
    load.op = "load";
    load.allocation = backing.id;
    load.generation = backing.generation;
    load.representation_epoch = backing.representation_epoch;
    load.offset = 0;
    load.size = 4;
    module.instructions.push_back(load);
    module.declared_effects.push_back({backing.id, 0, 4, vg::ir::Access::Read, backing.representation_epoch});
    module.canonical_json = vg::ir::serialize_module(module);
    module.hash = vg::ir::sha256_hex(module.canonical_json);
    uint32_t new_epoch = 0;
    assert(cap_arena.transform(backing.id, backing.generation, &new_epoch));
    uint64_t released = 0;
    assert(cap_arena.consume_representation(backing.id, backing.generation, new_epoch, discharged_proof(), &released,
                                           &error));
    assert(released == 16);
    assert(backing.bytes.empty());
    const auto post = vg::capture::make_capture(module, cap_arena);
    assert(post.allocations.size() == 1);
    assert(post.allocations[0].size == 16);
    assert(post.allocations[0].bytes.empty());
    vg::capture::Capture decoded_consumed;
    assert(vg::capture::deserialize(vg::capture::serialize(post), &decoded_consumed, &error));
    assert(decoded_consumed.allocations[0].size == 16);
    assert(decoded_consumed.allocations[0].bytes.empty());
    vg::capture::ReplayResult post_replay;
    assert(!vg::capture::replay(decoded_consumed, &post_replay, &error));
    assert(error == "cannot restore a consumed representation");
    vg::capture::ViewReport consumed_view;
    assert(vg::capture::write_view(post, &consumed_view, &error));
    assert(consumed_view.markdown.find("| 1 |") != std::string::npos);
    assert(consumed_view.markdown.find("representation_epoch") != std::string::npos);
  }

  vg::capture::ViewReport view;
  assert(vg::capture::write_view(decoded, &view, &error));
  assert(view.markdown.find("schema: vg.capture/v1") != std::string::npos);
  assert(view.markdown.find("schema_version: 2") != std::string::npos);
  assert(view.markdown.find(std::to_string(decoded.allocations.front().id)) != std::string::npos);
  assert(view.markdown.find("representation_epoch") != std::string::npos);
  assert(view.markdown.find("fault_code") != std::string::npos);
  assert(view.markdown.find("status: blocked") != std::string::npos);
  assert(view.json.find("stable_allocation_ids") != std::string::npos);
  assert(!has_gpu_address_pattern(view.markdown));
  assert(!has_gpu_address_pattern(view.json));

  {
    auto incompatible = decoded;
    incompatible.view.required_capabilities = {"SparseResidency"};
    vg::capture::ReplayResult refused;
    assert(!vg::capture::replay(incompatible, &refused, &error));
    assert(error == "incompatible capabilities refused: SparseResidency");
    vg::capture::ReplayEnvironment metal_only{"metal", {"LinearAddress", "CaptureReplay"}};
    auto raster = decoded;
    raster.view.required_capabilities = {"Raster"};
    assert(!vg::capture::replay(raster, metal_only, &refused, &error));
    assert(error == "incompatible capabilities refused: Raster");
    assert(vg::capture::replay(decoded, vg::capture::reference_replay_environment(), &refused, &error));
  }

  {
    auto mapped = decoded;
    mapped.view.source_backend = "metal";
    mapped.view.executed_backends = {"metal"};
    mapped.view.semantic_correspondence_only = true;
    mapped.view.semantic_counterpart = "vulkan";
    vg::capture::ViewReport mapped_view;
    assert(vg::capture::write_view(mapped, &mapped_view, &error));
    assert(mapped_view.markdown.find("vulkan (not executed)") != std::string::npos);
    assert(mapped_view.markdown.find("executed_both_metal_and_vulkan: no") != std::string::npos);
    assert(mapped_view.json.find("semantic-only") != std::string::npos);
    auto lying = mapped;
    lying.view.executed_backends = {"metal", "vulkan"};
    assert(!vg::capture::write_view(lying, &mapped_view, &error));
    assert(error == "cannot claim Metal and Vulkan both executed");
  }

  {
    auto dynamic = dynamic_graph_capture();
    assert(dynamic.allocations.size() == 2);
    const auto first_dynamic = vg::capture::serialize(dynamic);
    const auto second_dynamic = vg::capture::serialize(dynamic);
    assert(first_dynamic == second_dynamic);
    vg::capture::Capture decoded_dynamic;
    assert(vg::capture::deserialize(first_dynamic, &decoded_dynamic, &error));
    assert(decoded_dynamic.has_discovery);
    assert(decoded_dynamic.discovery_seeds.size() == 1);
    assert(decoded_dynamic.discovered_reachable.size() == 2);
    assert(decoded_dynamic.allocations.size() == 2);
    bool saw_n2 = false;
    bool saw_n3 = false;
    for (const auto& snapshot : decoded_dynamic.allocations) {
      if (snapshot.id == 3) saw_n2 = true;
      if (snapshot.id == 4) saw_n3 = true;
    }
    assert(!saw_n2);
    assert(!saw_n3);
    vg::capture::ReplayResult dynamic_replay;
    assert(vg::capture::replay(decoded_dynamic, &dynamic_replay, &error));
    vg::capture::ViewReport dynamic_view;
    assert(vg::capture::write_view(decoded_dynamic, &dynamic_view, &error));
    assert(dynamic_view.markdown.find("status: recorded") != std::string::npos);
    assert(dynamic_view.markdown.find("seed_count: 1") != std::string::npos);
    assert(dynamic_view.markdown.find("reachable_count: 2") != std::string::npos);
    assert(dynamic_view.json.find("\"status\":\"recorded\"") != std::string::npos ||
           dynamic_view.json.find("recorded") != std::string::npos);
    assert(!has_gpu_address_pattern(dynamic_view.markdown));
    assert(!has_gpu_address_pattern(dynamic_view.json));
    const auto round_trip = vg::capture::serialize(decoded_dynamic);
    auto first_hash = vg::json::parse(first_dynamic);
    auto round_hash = vg::json::parse(round_trip);
    assert(field_string(first_hash, "capture_hash") == field_string(round_hash, "capture_hash"));
    assert(field_string(first_hash, "ir_hash") == field_string(round_hash, "ir_hash"));
  }

#if defined(VG_CAPTURE_VIEW)
  {
    const CaptureTempDirectory temporary;
    const auto fixture = temporary.path() / "vg-e014-capture-view.json";
    const auto report_path = temporary.path() / "vg-e014-capture-view.md";
    write_text(fixture, first);
    const std::string command = std::string("\"") + VG_CAPTURE_VIEW + "\" --format markdown --output \"" +
                                report_path.string() + "\" \"" + fixture.string() + "\"";
    assert(std::system(command.c_str()) == 0);
    const auto cli = read_text(report_path);
    assert(cli.find("schema: vg.capture/v1") != std::string::npos);
    assert(cli.find(std::to_string(decoded.allocations.front().id)) != std::string::npos);
    assert(cli.find("generation") != std::string::npos);
    assert(cli.find("representation_epoch") != std::string::npos);
    assert(!has_gpu_address_pattern(cli));
  }
#endif
  return 0;
}
