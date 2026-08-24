#include "core/core.h"
#include <cassert>
#include <cstdint>
#include <random>

int main() {
  vg::core::PublicationRing ring(4);
  vg::core::TaskRecord task{}; task.node_generation = 1; task.root_generation = 1;
  const int32_t slot = ring.reserve();
  assert(slot >= 0);
  std::string error;
  assert(!ring.acquire(static_cast<uint32_t>(slot), &task, &error));
  assert(ring.write(static_cast<uint32_t>(slot), task));
  assert(ring.publish(static_cast<uint32_t>(slot)));
  vg::core::TaskRecord observed{};
  assert(ring.acquire(static_cast<uint32_t>(slot), &observed));
  assert(ring.consume(static_cast<uint32_t>(slot)));
  assert(!ring.acquire(static_cast<uint32_t>(slot), &observed));
  assert(!ring.write(static_cast<uint32_t>(slot), task));

  vg::core::PublicationRing full_ring(1);
  uint32_t published_slot = 99;
  assert(full_ring.publish_task(task, &published_slot));
  assert(published_slot == 0);
  assert(!full_ring.publish_task(task, nullptr, &error));
  assert(error == "publication ring quota overflow");
  vg::core::TaskGraphBuilder published_builder;
  uint32_t task_slot = 0;
  vg::core::PublicationRing task_ring(2);
  assert(task_ring.publish_task(task, &task_slot));
  assert(published_builder.append_published(task_ring, task_slot));
  vg::core::TaskGraph published_graph;
  assert(published_builder.seal(&published_graph));
  assert(!published_graph.validate_execution(&error));
  assert(published_graph.publish());
  assert(published_graph.validate_execution());

  std::mt19937_64 random(0x5647504841534531ull);
  vg::core::Arena arena;
  for (uint32_t iteration = 0; iteration < 100000; ++iteration) {
    auto& allocation = arena.allocate(8 + (random() % 64));
    const uint64_t id = allocation.id;
    const uint32_t generation = allocation.generation;
    assert(arena.lookup(id, generation) != nullptr);
    if ((random() & 1u) != 0) {
      assert(arena.retire(id, generation));
      assert(arena.lookup(id, generation) == nullptr);
      assert(!arena.acquire(id, generation));
      assert(!arena.release(id, generation));
    } else {
      assert(arena.acquire(id, generation));
      assert(!arena.transform(id, generation, nullptr, &error));
      assert(arena.release(id, generation));
      assert(arena.transform(id, generation, nullptr));
      const auto* current = arena.lookup(id, generation);
      assert(current != nullptr);
      assert(arena.consume(id, generation, current->representation_epoch,
                           vg::core::ConsumeProof{true, true, true, true}, &error));
      assert(arena.lookup(id, generation) == nullptr);
    }
  }
  return 0;
}
