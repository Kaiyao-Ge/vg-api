#include "compiler/compute_task_ring.h"

#include <sstream>

namespace vg::compiler {
namespace ring = schema::compute_task_ring;

namespace {

void set_error(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
}

bool has_wire_capacity(size_t size, std::string* error) {
  if (size >= ring::kWordCount) return true;
  set_error(error, "compute Task ring wire record is shorter than the schema word count");
  return false;
}

}  // namespace

bool make_compute_task_ring_record(const core::TaskRecord& task, ComputeTaskRingRecord* out,
                                   std::string* error) {
  if (out == nullptr) {
    set_error(error, "compute Task ring output record is required");
    return false;
  }
  *out = {};
  if (task.kind != core::TaskKind::Compute) {
    set_error(error, "compute Task ring codec requires a Compute task");
    return false;
  }
  out->node_index = task.node_index;
  out->node_generation = task.node_generation;
  out->root_allocation = task.root_allocation;
  out->root_generation = task.root_generation;
  out->x = task.x;
  out->y = task.y;
  out->z = task.z;
  out->flags = task.flags;
  out->contract_index = task.contract_index;
  out->payload_size = task.payload_size;
  out->payload_or_offset = task.payload_or_offset;
  return true;
}

core::TaskRecord make_task_record(const ComputeTaskRingRecord& record) {
  core::TaskRecord task{};
  task.kind = core::TaskKind::Compute;
  task.node_index = record.node_index;
  task.node_generation = record.node_generation;
  task.root_allocation = record.root_allocation;
  task.root_generation = record.root_generation;
  task.x = record.x;
  task.y = record.y;
  task.z = record.z;
  task.flags = record.flags;
  task.contract_index = record.contract_index;
  task.payload_size = record.payload_size;
  task.payload_or_offset = record.payload_or_offset;
  return task;
}

bool pack_compute_task_ring_record(const ComputeTaskRingRecord& record, std::span<uint32_t> words,
                                   std::string* error) {
  if (!has_wire_capacity(words.size(), error)) return false;
  words[ring::kNodeIndexWord] = record.node_index;
  words[ring::kNodeGenerationWord] = record.node_generation;
  words[ring::kRootAllocationLoWord] = static_cast<uint32_t>(record.root_allocation);
  words[ring::kRootAllocationHiWord] = static_cast<uint32_t>(record.root_allocation >> 32);
  words[ring::kRootGenerationWord] = record.root_generation;
  words[ring::kXWord] = record.x;
  words[ring::kYWord] = record.y;
  words[ring::kZWord] = record.z;
  words[ring::kFlagsWord] = record.flags;
  words[ring::kContractIndexWord] = record.contract_index;
  words[ring::kPayloadSizeWord] = record.payload_size;
  words[ring::kReservedWord] = 0;
  words[ring::kPayloadOrOffsetLoWord] = static_cast<uint32_t>(record.payload_or_offset);
  words[ring::kPayloadOrOffsetHiWord] = static_cast<uint32_t>(record.payload_or_offset >> 32);
  return true;
}

bool unpack_compute_task_ring_record(std::span<const uint32_t> words, ComputeTaskRingRecord* out,
                                     std::string* error) {
  if (out == nullptr) {
    set_error(error, "compute Task ring output record is required");
    return false;
  }
  *out = {};
  if (!has_wire_capacity(words.size(), error)) return false;
  if (words[ring::kReservedWord] != 0) {
    set_error(error, "compute Task ring reserved word must be zero");
    return false;
  }
  out->node_index = words[ring::kNodeIndexWord];
  out->node_generation = words[ring::kNodeGenerationWord];
  out->root_allocation = static_cast<uint64_t>(words[ring::kRootAllocationLoWord]) |
                         (static_cast<uint64_t>(words[ring::kRootAllocationHiWord]) << 32);
  out->root_generation = words[ring::kRootGenerationWord];
  out->x = words[ring::kXWord];
  out->y = words[ring::kYWord];
  out->z = words[ring::kZWord];
  out->flags = words[ring::kFlagsWord];
  out->contract_index = words[ring::kContractIndexWord];
  out->payload_size = words[ring::kPayloadSizeWord];
  out->payload_or_offset = static_cast<uint64_t>(words[ring::kPayloadOrOffsetLoWord]) |
                           (static_cast<uint64_t>(words[ring::kPayloadOrOffsetHiWord]) << 32);
  return true;
}

bool dump_compute_task_ring_words(std::span<const uint32_t> words, std::string* out,
                                  std::string* error) {
  if (out == nullptr) {
    set_error(error, "compute Task ring dump output is required");
    return false;
  }
  out->clear();
  if (!has_wire_capacity(words.size(), error)) return false;
  std::ostringstream stream;
  for (size_t index = 0; index < ring::kWordFields.size(); ++index) {
    if (index != 0) stream << ' ';
    const auto& field = ring::kWordFields[index];
    stream << field.name << '=' << words[field.word_offset];
  }
  *out = stream.str();
  return true;
}

}  // namespace vg::compiler
