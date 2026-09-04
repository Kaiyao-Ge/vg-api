#ifndef VG_CORE_NODE_H_
#define VG_CORE_NODE_H_

#include "ir/ir.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vg::core {

// Owns the raw bytes handed to loadCodeObject plus a format tag. No caching
// or precompilation for v1.1 -- submit() still parses fresh via
// ir::parse_module every time, exactly matching pre-F1 behavior; this only
// gives the bytes a handle and a lifetime (ADR-044 disclosed narrowing).
struct CodeObject {
  std::vector<uint8_t> bytes;
  std::string format_tag;
  // Materialized at load time.  submit() only consumes this immutable
  // package; it never reinterprets application-owned source bytes.
  std::optional<ir::Module> module;
  std::optional<ir::UserRasterShaderContract> user_raster_shader;
};

// A CodeObject has exactly one runnable entry for v1.1, but the
// index+generation pair is what the public VgNodeRef/VgTaskRecord.node
// fields carry, mirroring VgFacetRef's staleness-checked-token model rather
// than a bare caller-picked integer.
struct NodeEntry {
  // A live Node retains the immutable package; destroying the public
  // CodeObject handle must not make existing NodeRefs dangle.
  std::shared_ptr<const CodeObject> code_object;
  std::string entry_name;
  uint32_t generation{1};
  bool live{true};
};

class NodeTable {
 public:
  struct Ref {
    uint32_t index{};
    uint32_t generation{};
  };

  NodeTable();
  Ref create(std::shared_ptr<const CodeObject> code_object, const std::string& entry_name);
  bool destroy(Ref ref);
  // Copies an entry while holding the table lock.  Submit uses this instead
  // of retaining a table-internal pointer across later Node creation.
  bool snapshot(Ref ref, NodeEntry* out) const;
  // Capability validation needs no table-internal pointer.  Returning one
  // after releasing mutex_ would let destroy() or a rehash invalidate it.
  [[nodiscard]] bool contains(Ref ref) const;

 private:
  mutable std::mutex mutex_;
  // Ref bits are allocated from a process-wide monotonic token.  The map is
  // still Device-owned: it only answers for Nodes created by that Device.
  std::unordered_map<uint64_t, NodeEntry> entries_;
};

}  // namespace vg::core

#endif
