#ifndef VG_COMPILER_COMPUTE_TASK_RING_H_
#define VG_COMPILER_COMPUTE_TASK_RING_H_

#include "core/core.h"
#include "vg_compute_task_ring_words.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace vg::compiler {

// Internal compute-only publication payload.  Raster parameters deliberately
// do not exist in this type: they are lowered through a render package/pass,
// never erased while entering the GPU publication ring.
struct ComputeTaskRingRecord {
  uint32_t node_index{};
  uint32_t node_generation{};
  uint64_t root_allocation{};
  uint32_t root_generation{};
  uint32_t x{};
  uint32_t y{};
  uint32_t z{};
  uint32_t flags{};
  uint32_t contract_index{};
  uint32_t payload_size{};
  uint64_t payload_or_offset{};
};

inline constexpr uint32_t kTaskRingWordsPerRecord = schema::compute_task_ring::kWordCount;
inline constexpr uint32_t kTaskRingNodeIndexWord = schema::compute_task_ring::kNodeIndexWord;
inline constexpr uint32_t kTaskRingDispatchXWord = schema::compute_task_ring::kXWord;
static_assert(schema::compute_task_ring::kYWord == kTaskRingDispatchXWord + 1 &&
              schema::compute_task_ring::kZWord == kTaskRingDispatchXWord + 2,
              "compute Task ring x/y/z must be a VkDispatchIndirectCommand-compatible window");
using ComputeTaskRingWords = std::array<uint32_t, kTaskRingWordsPerRecord>;

bool make_compute_task_ring_record(const core::TaskRecord& task, ComputeTaskRingRecord* out,
                                   std::string* error = nullptr);
core::TaskRecord make_task_record(const ComputeTaskRingRecord& record);

bool pack_compute_task_ring_record(const ComputeTaskRingRecord& record, std::span<uint32_t> words,
                                   std::string* error = nullptr);
bool unpack_compute_task_ring_record(std::span<const uint32_t> words, ComputeTaskRingRecord* out,
                                     std::string* error = nullptr);

// Diagnostic output is intentionally driven by the generated wire metadata,
// so field names and ordering cannot drift away from pack/unpack or shaders.
bool dump_compute_task_ring_words(std::span<const uint32_t> words, std::string* out,
                                  std::string* error = nullptr);

}  // namespace vg::compiler

#endif
