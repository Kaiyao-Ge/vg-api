#include "compiler/pipeline_classification.h"

#include "ir/sha256.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace vg::compiler {
namespace {
void emit_text(std::ostringstream& out, const char* field, const std::string& value) {
  out << field << '=' << value.size() << ':' << value << '\n';
}

void emit_pairs(std::ostringstream& out, const char* field,
                std::vector<std::pair<std::string, uint64_t>> pairs) {
  std::sort(pairs.begin(), pairs.end());
  out << field << '=' << pairs.size() << '\n';
  for (const auto& pair : pairs)
    out << "  " << pair.first.size() << ':' << pair.first << '=' << pair.second << '\n';
}

std::string hex_digest(uint64_t value) {
  std::ostringstream out;
  out << std::hex << value;
  return out.str();
}

const char* kind_name(StateBlockKind kind) {
  switch (kind) {
    case StateBlockKind::PipelineKey: return "pipeline-key";
    case StateBlockKind::DynamicState: return "dynamic";
    case StateBlockKind::ShaderVisibleData: return "shader-visible";
    case StateBlockKind::UnsupportedNeedsConversion: return "unsupported";
  }
  return "unknown";
}

std::string join_block_names(const std::vector<StateBlock>& blocks) {
  std::string joined;
  for (const auto& block : blocks) {
    if (!joined.empty()) joined += ", ";
    joined += "'" + block.name + "'";
  }
  return joined;
}
}  // namespace

std::string PipelineKey::canonical() const {
  std::ostringstream out;
  emit_text(out, "code_object_hash", code_object_hash);
  emit_text(out, "entry", entry);
  emit_pairs(out, "function_constants", function_constants);
  out << "attachment_formats=" << attachment_formats.size() << '\n';
  for (size_t index = 0; index < attachment_formats.size(); ++index)
    out << "  " << index << '=' << attachment_formats[index] << '\n';
  out << "sample_count=" << sample_count << '\n';
  emit_pairs(out, "raster_state", raster_state);
  emit_text(out, "target_identity", target_identity);
  return out.str();
}

uint64_t PipelineKey::hash() const {
  const std::string digest = ir::sha256_hex(canonical());
  uint64_t value = 0;
  for (size_t index = 0; index < 16 && index < digest.size(); ++index) {
    const char c = digest[index];
    const uint64_t nibble = (c >= '0' && c <= '9') ? static_cast<uint64_t>(c - '0')
                                                   : static_cast<uint64_t>((c | 0x20) - 'a' + 10);
    value = (value << 4) | nibble;
  }
  return value;
}

PipelineClassification classify_pipeline_state(const PipelineKey& base,
                                               const std::vector<StateBlock>& blocks) {
  PipelineClassification result;
  result.key = base;
  uint32_t compiled_in = 0;
  std::vector<StateBlock> conflicts;

  for (const auto& block : blocks) {
    switch (block.kind) {
      case StateBlockKind::PipelineKey: {
        const auto existing = std::find_if(result.key.raster_state.begin(), result.key.raster_state.end(),
                                           [&](const std::pair<std::string, uint64_t>& entry) {
                                             return entry.first == block.name;
                                           });
        if (existing == result.key.raster_state.end()) {
          result.key.raster_state.emplace_back(block.name, block.value);
          ++compiled_in;
        } else if (existing->second != block.value) {
          conflicts.push_back(block);
        }
        break;
      }
      case StateBlockKind::DynamicState:
        result.dynamic_state.push_back(block);
        break;
      case StateBlockKind::ShaderVisibleData:
        result.shader_visible_data.push_back(block);
        break;
      case StateBlockKind::UnsupportedNeedsConversion:
        result.unsupported.push_back(block);
        break;
    }
  }

  std::ostringstream message;
  if (!result.unsupported.empty()) {
    message << "Node entry '" << base.entry << "' declares StateBlock " << join_block_names(result.unsupported)
            << " that this backend cannot express; specialization rejected. A representation conversion or an"
               " Unsupported result is required -- the state was not folded into the pipeline key.";
  }
  if (!conflicts.empty()) {
    if (message.tellp() > 0) message << ' ';
    message << "Node entry '" << base.entry << "' declares conflicting pipeline-key StateBlock "
            << join_block_names(conflicts)
            << " whose value disagrees with an already folded block of the same name; the pipeline key would"
               " depend on classification order.";
  }
  if (!result.unsupported.empty() || !conflicts.empty()) {
    result.message = message.str();
    return result;
  }

  message << "Node entry '" << base.entry << "' pipeline key compiles in " << compiled_in
          << " StateBlock(s); " << result.dynamic_state.size() << " " << kind_name(StateBlockKind::DynamicState)
          << " and " << result.shader_visible_data.size() << " "
          << kind_name(StateBlockKind::ShaderVisibleData)
          << " StateBlock(s) stay out of the key.";
  result.ok = true;
  result.message = message.str();
  return result;
}

bool PipelineClassificationCache::acquire(const PipelineKey& key, const std::string& trigger_reason,
                                         const std::function<bool(uint64_t*, std::string*)>& create,
                                         SpecializationReport* out, std::string* error) {
  if (out == nullptr) {
    if (error) *error = "specialization report output is required";
    return false;
  }
  const std::string canonical = key.canonical();
  const uint64_t digest = key.hash();

  const auto existing = entries_.find(digest);
  if (existing != entries_.end()) {
    if (existing->second.canonical != canonical) {
      if (error)
        *error = "pipeline key digest collision at " + hex_digest(digest) + ": two distinct PipelineKeys for entry '" +
                 key.entry + "' share one 64-bit cache key; refusing to serve the cached pipeline";
      return false;
    }
    ++hits_;
    SpecializationReport report;
    report.cache_key = digest;
    report.cache_hit = true;
    report.binary_size = existing->second.binary_size;
    report.trigger_reason = trigger_reason;
    reports_.push_back(report);
    *out = report;
    return true;
  }

  if (!create) {
    if (error)
      *error = "pipeline cache miss for entry '" + key.entry + "' with no specialization callback to compile it";
    return false;
  }

  ++misses_;
  uint64_t binary_size = 0;
  std::string create_error;
  const auto start = std::chrono::steady_clock::now();
  const bool created = create(&binary_size, &create_error);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (!created) {
    if (error)
      *error = create_error.empty() ? "specialization of Node entry '" + key.entry + "' failed" : create_error;
    return false;
  }

  entries_.emplace(digest, Entry{canonical, binary_size});
  SpecializationReport report;
  report.cache_key = digest;
  report.cache_hit = false;
  report.compile_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  report.binary_size = binary_size;
  report.trigger_reason = trigger_reason;
  reports_.push_back(report);
  *out = report;
  return true;
}

uint32_t PipelineClassificationCache::pipeline_count() const { return static_cast<uint32_t>(entries_.size()); }
uint32_t PipelineClassificationCache::cache_hits() const { return hits_; }
uint32_t PipelineClassificationCache::cache_misses() const { return misses_; }
const std::vector<SpecializationReport>& PipelineClassificationCache::reports() const { return reports_; }

void PipelineClassificationCache::clear() {
  entries_.clear();
  reports_.clear();
  hits_ = 0;
  misses_ = 0;
}

}  // namespace vg::compiler
