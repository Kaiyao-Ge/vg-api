#include "core/node.h"

#include <atomic>
#include <utility>

namespace vg::core {

namespace {
std::atomic<uint64_t> g_next_node_token{(uint64_t{1} << 32) | 1};
uint64_t node_key(NodeTable::Ref ref) {
  return (static_cast<uint64_t>(ref.generation) << 32) | ref.index;
}
}

NodeTable::NodeTable() = default;

NodeTable::Ref NodeTable::create(std::shared_ptr<const CodeObject> code_object,
                                 const std::string& entry_name) {
  std::lock_guard lock(mutex_);
  NodeEntry entry;
  entry.code_object = std::move(code_object);
  entry.entry_name = entry_name;
  uint64_t token = g_next_node_token.load(std::memory_order_relaxed);
  for (;;) {
    if (token == 0 || token == UINT64_MAX) return {};
    if (g_next_node_token.compare_exchange_weak(token, token + 1,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed))
      break;
  }
  entry.generation = static_cast<uint32_t>(token >> 32);
  entry.live = true;
  Ref ref;
  ref.index = static_cast<uint32_t>(token);
  ref.generation = entry.generation;
  entries_.emplace(node_key(ref), std::move(entry));
  return ref;
}

bool NodeTable::destroy(Ref ref) {
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(node_key(ref));
  if (found == entries_.end()) return false;
  NodeEntry* entry = &found->second;
  if (!entry->live || entry->generation != ref.generation) return false;
  entries_.erase(found);
  return true;
}

bool NodeTable::snapshot(Ref ref, NodeEntry* out) const {
  if (out == nullptr) return false;
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(node_key(ref));
  if (found == entries_.end()) return false;
  const NodeEntry& entry = found->second;
  if (!entry.live || entry.generation != ref.generation) return false;
  *out = entry;
  return true;
}

bool NodeTable::contains(Ref ref) const {
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(node_key(ref));
  if (found == entries_.end()) return false;
  const NodeEntry& entry = found->second;
  return entry.live && entry.generation == ref.generation;
}

}  // namespace vg::core
