#ifndef VG_CORE_FACET_H_
#define VG_CORE_FACET_H_

#include "core/resource_types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace vg::core {

class Arena;

// A CanonicalView per-usage generates AddressFacet/SampleFacet/StorageFacet/
// AttachmentFacet/TransferFacet (02-principles-and-semantics.md §3.3). It
// names an allocation's shape/format contract; it does not own the
// allocation -- FacetPool slots reference it by value. Facets must not be
// folded into one maximal ViewRecord, and active facet refs must not be
// mutated in place.
enum class FacetKind : uint32_t { Address, Sample, Storage, Attachment, Transfer };

// Format/dimension inputs that Metal Sample/Storage/Attachment facets need
// (06-backend-macos-metal.md §6.1–6.3). Extend only when a real path requires it.
enum class PixelFormat : uint32_t { RGBA8Unorm, R32Float, Depth32Float, R16Uint, R32Uint };
enum class ViewDimension : uint32_t { Texture2D, Texture2DArray };

// Both formats this milestone models are 4 bytes wide, but the two reach that
// width differently (4x8-bit vs. 1x32-bit), so callers that pack rows must ask
// rather than assume.
uint32_t bytes_per_texel(PixelFormat format);

// Sampler policy for SampleFacet only (06 §6.1). Kept off CanonicalView so the
// same view can be sampled under different filter/wrap without a new view.
enum class FilterMode : uint32_t { Nearest, Bilinear };
enum class WrapMode : uint32_t { Clamp, Repeat };

// Per-channel source selection (06 §6.1 lists swizzle among the inputs a facet
// compiles from). Part of the view contract rather than sampler policy: it
// changes what the shader reads, so two views differing only in swizzle are
// different contracts and must not share a cached facet.
enum class Swizzle : uint32_t { Red, Green, Blue, Alpha, Zero, One };

struct SwizzleChannels {
  Swizzle red{Swizzle::Red};
  Swizzle green{Swizzle::Green};
  Swizzle blue{Swizzle::Blue};
  Swizzle alpha{Swizzle::Alpha};

  [[nodiscard]] bool identity() const {
    return red == Swizzle::Red && green == Swizzle::Green && blue == Swizzle::Blue &&
           alpha == Swizzle::Alpha;
  }
};

struct CanonicalView {
  uint64_t allocation{};
  uint32_t allocation_generation{};
  PixelFormat format{PixelFormat::RGBA8Unorm};
  ViewDimension dimension{ViewDimension::Texture2D};
  uint32_t width{};
  uint32_t height{};
  uint32_t array_layers{1};
  uint32_t mip_levels{1};
  SwizzleChannels swizzle;

  // Half-and-clamp, the sizing rule every graphics API's mip chain uses.
  [[nodiscard]] uint32_t mip_width(uint32_t level) const;
  [[nodiscard]] uint32_t mip_height(uint32_t level) const;
  [[nodiscard]] uint32_t subresource_count() const { return array_layers * mip_levels; }
  // Byte layout of the linear allocation this view names: slice-major, then
  // ascending mip level, each level tightly packed at
  // mip_width(level) * bytes_per_texel rows. This is the single contract the
  // Metal upload path and the reference sampling oracle both encode against --
  // if they disagreed on it, an image-correctness comparison between them
  // would be meaningless.
  struct SubresourceIndex {
    uint32_t array_layer{};
    uint32_t mip_level{};
  };
  [[nodiscard]] uint64_t bytes_per_row(uint32_t level) const;
  [[nodiscard]] uint64_t subresource_byte_size(uint32_t level) const;
  [[nodiscard]] uint64_t subresource_byte_offset(SubresourceIndex index) const;
  [[nodiscard]] uint64_t byte_size() const;
  // Rejects a view whose extents/counts cannot describe a real image (zero
  // extent, zero layers/levels, a mip chain longer than the extent supports,
  // or array_layers > 1 without the array dimension). Shape validity is a
  // property of the view contract, so every backend gets the same answer.
  bool valid(std::string* error = nullptr) const;
};

// Sample source + attachment target for one raster pass. Adjacent FacetRef
// parameters are otherwise interchangeable at every draw call site.
struct RasterFacetPair {
  FacetRef source{};
  FacetRef target{};
};

struct FacetSlot {
  uint32_t generation{1};
  bool active{};
  FacetKind kind{FacetKind::Address};
  CanonicalView view;
  uint32_t representation_epoch{};
  // Outstanding begin_gpu_use() calls not yet matched by end_gpu_use().
  // Non-zero means a command buffer may still dereference this slot, so its
  // index must not be handed to a new acquire() (06 §6.4, §11).
  uint32_t in_flight{};
  // Generation the outstanding uses were begun under. Retirement bumps
  // `generation` immediately, so end_gpu_use() matches against this instead.
  uint32_t in_flight_generation{};
};

// Why a lookup() failed, so callers can act on the reason instead of parsing
// a diagnostic string (04-public-c-abi.md §4: every failure programmatically
// determinable). Retired/GenerationMismatch/EpochStale are the three shapes
// of a stale capability token.
enum class FacetStatus : uint32_t {
  Ok,
  UnknownIndex,
  Retired,
  GenerationMismatch,
  EpochStale,
  AllocationLost,
};

const char* to_string(FacetStatus status);

// Side-effect-free validation shared by Stage-5 planning and FacetPool
// acquisition. It deliberately does not allocate a slot or alter an epoch.
bool validate_facet_target(const Arena& arena, const CanonicalView& view, FacetKind kind,
                           std::string* error = nullptr);

// Independent index+generation pool for facets. Sibling to Arena's
// allocation table, not a reuse of its id space
// (02-principles-and-semantics.md Sec.3.3: "不能把所有 facet 拼成一个最大
// ViewRecord，也不能在活动引用中原地修改") -- mirrors how GraphEpoch and
// PointerGraph already exist as separate parallel epoch concepts rather
// than being unified into one structure.
class FacetPool {
 public:
  // Snapshots view.allocation's current representation_epoch (read from
  // `arena`) into the new slot. Fails if that allocation is not active in
  // `arena`.
  bool acquire(const Arena& arena, const CanonicalView& view, FacetKind kind, FacetRef* out,
               std::string* error = nullptr);
  // A slot is stale once its backing allocation's representation_epoch in
  // `arena` no longer matches the epoch snapshotted at acquire() time --
  // this is the "facet generation vs epoch = stale token" check from
  // 02-principles-and-semantics.md Sec.10. Returns nullptr for a stale or
  // retired/generation-mismatched ref rather than the slot's last-known
  // contents.
  const FacetSlot* lookup(const Arena& arena, FacetRef ref, FacetStatus* status = nullptr) const;
  // Retires a slot so its index becomes eligible for reuse by a future
  // acquire(). Only a generation-matched, still-active ref may be retired.
  // Reuse is additionally gated on in_flight reaching zero.
  bool retire(FacetRef ref, std::string* error = nullptr);
  // Retires every active slot whose snapshotted representation_epoch no longer
  // matches the live allocation in `arena` (06 §6.4: slots become reusable only
  // after the referenced RepresentationEpoch is no longer current).
  size_t retire_stale(const Arena& arena);
  // Brackets a command buffer's use of a facet. Between begin and end the
  // slot's index stays out of the free list even after retirement, which is
  // what "默认保留旧 facet/backing 至相关 command buffer 完成" (06 §11) buys:
  // a retired-but-in-flight token stops resolving immediately, yet its backend
  // resource is not handed to an unrelated facet underneath the GPU.
  bool begin_gpu_use(const Arena& arena, FacetRef ref, std::string* error = nullptr);
  bool end_gpu_use(FacetRef ref, std::string* error = nullptr);
  [[nodiscard]] uint32_t in_flight(FacetRef ref) const;
  [[nodiscard]] uint32_t slot_count() const { return static_cast<uint32_t>(slots_.size()); }
  // 06 Sec.6.4 asks the checked profile to verify facet generation *in the
  // shader*. A shader cannot call lookup(), so the checked profile uploads this
  // table instead: entry[index] is the generation that index currently
  // resolves at, or 0 for a retired slot, so a kernel holding a FacetRef can
  // reject a stale token itself before dereferencing it. Snapshot semantics --
  // it is only as fresh as the submission that uploaded it, which is why the
  // host-side lookup() check stays authoritative.
  void snapshot_generations(std::vector<uint32_t>* out) const;
  // Host-side mirror of the comparison the checked-profile shader performs, so
  // a test can state the expected in-shader verdict without a GPU. Deliberately
  // weaker than lookup(): it sees only what the uploaded table encodes, and so
  // cannot observe an epoch that went stale after the snapshot.
  [[nodiscard]] bool generation_valid(FacetRef ref) const;
  // 02 §4.2 / 06 §11: a live token or an outstanding GPU use of this
  // allocation at `epoch` is an external reference ConsumeInput must see.
  // Retired slots still count while in_flight > 0, because the command buffer
  // that called begin_gpu_use() may still dereference the old backing.
  [[nodiscard]] bool references(const RepresentationRef& ref) const;

 private:
  void retire_slot(uint32_t index);

  std::vector<FacetSlot> slots_;
  std::vector<uint32_t> free_list_;
};

}  // namespace vg::core

#endif
