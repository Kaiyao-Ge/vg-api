// Deterministic concurrent coverage for the Device-owned NodeTable.
//
// Each NodeTable below stands in for a separate Device.  The public API does
// not permit destroyX(handle) to race another call using that same handle, so
// this test exercises the lower-level table contract instead: independent
// Nodes are created, queried, snapshotted, and retired concurrently.
#include "core/core.h"

#include <barrier>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

bool ok = true;

bool check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ok = false;
  }
  return condition;
}

uint64_t token(vg::core::NodeTable::Ref ref) {
  return (static_cast<uint64_t>(ref.generation) << 32) | ref.index;
}

std::shared_ptr<const vg::core::CodeObject> load_package() {
  auto code = std::make_shared<vg::core::CodeObject>();
  code->format_tag = "vg.ir/v1";
  return code;
}

void check_snapshot_ownership() {
  vg::core::NodeTable device;
  auto package = load_package();
  std::weak_ptr<const vg::core::CodeObject> weak_package = package;
  const auto ref = device.create(package, "snapshot-owner");
  package.reset();

  vg::core::NodeEntry snapshot;
  check(device.snapshot(ref, &snapshot), "snapshot resolves a live NodeRef");
  check(device.destroy(ref), "destroy retires the live NodeRef");
  check(!device.contains(ref), "destroyed NodeRef is stale");
  check(!device.snapshot(ref, &snapshot), "destroyed NodeRef cannot be snapshotted again");
  check(!weak_package.expired(), "existing snapshot retains CodeObject after Node destruction");
  snapshot.code_object.reset();
  check(weak_package.expired(), "CodeObject releases when final snapshot releases");
}

}  // namespace

int main() {
  check_snapshot_ownership();

  constexpr int kThreadCount = 8;
  constexpr int kRounds = 192;
  vg::core::NodeTable devices[2];
  std::barrier phase(kThreadCount);
  std::mutex tokens_mutex;
  std::unordered_set<uint64_t> issued_tokens;
  std::mutex result_mutex;
  bool workers_ok = true;

  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);
  for (int worker = 0; worker < kThreadCount; ++worker) {
    workers.emplace_back([&, worker] {
      for (int round = 0; round < kRounds; ++round) {
        const int device_index = (worker + round) % 2;
        const int other_device_index = 1 - device_index;
        auto package = load_package();
        const auto ref = devices[device_index].create(package, "concurrent-entry");
        package.reset();

        bool round_ok = ref.index != 0 || ref.generation != 0;
        {
          std::lock_guard lock(tokens_mutex);
          round_ok = issued_tokens.insert(token(ref)).second && round_ok;
        }

        // All workers have live Nodes before querying.  This drives concurrent
        // insertions and the early unordered_map rehashes on both Devices.
        phase.arrive_and_wait();
        vg::core::NodeEntry snapshot;
        round_ok = devices[device_index].contains(ref) && round_ok;
        round_ok = devices[device_index].snapshot(ref, &snapshot) && round_ok;
        round_ok = !devices[other_device_index].contains(ref) && round_ok;
        round_ok = !devices[other_device_index].snapshot(ref, &snapshot) && round_ok;

        // Keep every Node live until all snapshot/contains calls are in
        // flight, then concurrently retire them to cover destroy vs. rehash.
        phase.arrive_and_wait();
        round_ok = devices[device_index].destroy(ref) && round_ok;
        round_ok = !devices[device_index].contains(ref) && round_ok;
        round_ok = !devices[device_index].snapshot(ref, &snapshot) && round_ok;

        if (!round_ok) {
          std::lock_guard lock(result_mutex);
          workers_ok = false;
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();

  check(workers_ok, "concurrent create/contains/snapshot/destroy invariants");
  check(issued_tokens.size() == static_cast<size_t>(kThreadCount * kRounds),
        "all live 64-bit NodeRef tokens are process-wide unique");
  return ok ? 0 : 1;
}
