#include "compiler/compute_task_ring.h"

#include <array>
#include <cassert>
#include <string>

int main() {
  vg::core::TaskRecord task{};
  task.node_index = 1;
  task.node_generation = 2;
  task.root_allocation = (uint64_t{4} << 32) | 3;
  task.root_generation = 5;
  task.x = 6;
  task.y = 7;
  task.z = 8;
  task.flags = 9;
  task.contract_index = 10;
  task.payload_size = 11;
  task.payload_or_offset = (uint64_t{13} << 32) | 12;

  vg::compiler::ComputeTaskRingRecord record;
  std::string error;
  assert(vg::compiler::make_compute_task_ring_record(task, &record, &error));
  error.clear();
  assert(!vg::compiler::make_compute_task_ring_record(task, nullptr, &error));
  assert(error == "compute Task ring output record is required");
  vg::compiler::ComputeTaskRingWords words{};
  assert(vg::compiler::pack_compute_task_ring_record(record, words, &error));
  const vg::compiler::ComputeTaskRingWords expected{
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 12, 13};
  assert(words == expected);
  assert(words[vg::schema::compute_task_ring::kReservedWord] == 0);

  vg::compiler::ComputeTaskRingRecord decoded;
  assert(vg::compiler::unpack_compute_task_ring_record(words, &decoded, &error));
  error.clear();
  assert(!vg::compiler::unpack_compute_task_ring_record(words, nullptr, &error));
  assert(error == "compute Task ring output record is required");
  const auto round_trip = vg::compiler::make_task_record(decoded);
  assert(round_trip.kind == vg::core::TaskKind::Compute);
  assert(round_trip.node_index == task.node_index);
  assert(round_trip.node_generation == task.node_generation);
  assert(round_trip.root_allocation == task.root_allocation);
  assert(round_trip.root_generation == task.root_generation);
  assert(round_trip.x == task.x && round_trip.y == task.y && round_trip.z == task.z);
  assert(round_trip.flags == task.flags);
  assert(round_trip.contract_index == task.contract_index);
  assert(round_trip.payload_size == task.payload_size);
  assert(round_trip.payload_or_offset == task.payload_or_offset);
  assert(round_trip.raster_facets.source.index == 0);
  assert(round_trip.vertex_buffer_ref.index == 0);
  assert(round_trip.index_count == 0);

  std::string dump;
  assert(vg::compiler::dump_compute_task_ring_words(words, &dump, &error));
  assert(dump ==
         "node_index=1 node_generation=2 root_allocation_lo=3 root_allocation_hi=4 "
         "root_generation=5 x=6 y=7 z=8 flags=9 contract_index=10 payload_size=11 "
         "reserved=0 payload_or_offset_lo=12 payload_or_offset_hi=13");
  error.clear();
  assert(!vg::compiler::dump_compute_task_ring_words(words, nullptr, &error));
  assert(error == "compute Task ring dump output is required");
  static_assert(vg::schema::compute_task_ring::kYWord ==
                vg::schema::compute_task_ring::kXWord + 1);
  static_assert(vg::schema::compute_task_ring::kZWord ==
                vg::schema::compute_task_ring::kXWord + 2);

  task.kind = vg::core::TaskKind::Raster;
  error.clear();
  assert(!vg::compiler::make_compute_task_ring_record(task, &record, &error));
  assert(error == "compute Task ring codec requires a Compute task");
  task.kind = static_cast<vg::core::TaskKind>(99);
  error.clear();
  assert(!vg::compiler::make_compute_task_ring_record(task, &record, &error));
  assert(error == "compute Task ring codec requires a Compute task");

  std::array<uint32_t, vg::compiler::kTaskRingWordsPerRecord - 1> short_words{};
  error.clear();
  assert(!vg::compiler::pack_compute_task_ring_record(decoded, short_words, &error));
  assert(error == "compute Task ring wire record is shorter than the schema word count");
  error.clear();
  assert(!vg::compiler::unpack_compute_task_ring_record(short_words, &record, &error));
  assert(error == "compute Task ring wire record is shorter than the schema word count");
  error.clear();
  assert(!vg::compiler::dump_compute_task_ring_words(short_words, &dump, &error));
  assert(error == "compute Task ring wire record is shorter than the schema word count");

  words[vg::schema::compute_task_ring::kReservedWord] = 1;
  error.clear();
  assert(!vg::compiler::unpack_compute_task_ring_record(words, &record, &error));
  assert(error == "compute Task ring reserved word must be zero");
  return 0;
}
