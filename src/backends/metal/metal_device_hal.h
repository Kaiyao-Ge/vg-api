#ifndef VG_BACKENDS_METAL_DEVICE_HAL_H_
#define VG_BACKENDS_METAL_DEVICE_HAL_H_

#include "backends/device_hal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vg::metal {

// Plain-data snapshot used by diagnostics and the backend loader. Objective-C
// objects remain private to the .mm implementation.
struct DeviceSnapshot {
  hal::CapabilitySnapshot hal;
  uint32_t gpu_family{};
  uint32_t argument_buffer_tier{};
  bool unified_memory{};
  bool supports_shared_events{};
  bool supports_indirect_command_buffers{};
  bool supports_gpu_addresses{};
  bool supports_counter_sampling{};
};

struct BufferSnapshot {
  size_t requested_length{};
  size_t allocated_length{};
  uint64_t gpu_address{};
  uint32_t storage_mode{};
  bool gpu_address_available{};
};

// TASK-B13 (E009): result of one run_cull_compact() call. `visible_count` is
// the GPU-authored atomic count of instances that passed the visibility
// check; only compact_ids[0..visible_count) are meaningful. Slot order
// reflects atomic_fetch_add arrival order across GPU threads, not the
// original instance index order -- compare as a set/sorted-multiset against
// a CPU oracle, never by position.
struct CullCompactResult {
  uint32_t visible_count{};
  std::vector<uint32_t> compact_ids;
};

// AddressFacet use of a FacetRef (02 §3.3): the linear/BDA view of the same
// CanonicalView, resolved to the allocation's device buffer. No texture object
// is involved, and none is exposed -- the caller gets an address and a length.
struct AddressFacetResult {
  uint64_t gpu_address{};
  uint64_t byte_size{};
  bool gpu_address_available{};
  hal::LoweringReport report;
};

// SampleFacet use of a FacetRef (06 §6.1). Callers acquire the ref from
// core::FacetPool; this method never accepts a raw CanonicalView as a
// capability token.
struct SampleFacetResult {
  std::vector<std::array<float, 4>> sampled_rgba;
  bool facet_cache_hit{};
  uint32_t descriptor_write_count{};
  hal::LoweringReport report;
};

// 06 §6.2: a StorageFacet maps to "可读写 texture 或线性 buffer". The caller
// chooses; the backend never silently substitutes one for the other, and never
// rewrites the view's format to make a write legal -- an unwritable format is
// reported Unsupported so the caller can pick LinearBuffer or transform the
// representation explicitly.
enum class StorageFacetTarget : uint32_t { Texture, LinearBuffer };

struct StorageFacetResult {
  std::array<float, 4> written_rgba{};
  bool facet_cache_hit{};
  uint32_t descriptor_write_count{};
  uint32_t encoder_count{};
  StorageFacetTarget target{StorageFacetTarget::Texture};
  hal::LoweringReport report;
};

// 06 §6.3: load/store/resolve are the lowering of effect and representation
// operations, so they are per-use parameters rather than state stored on the
// facet or on a public object.
enum class AttachmentLoadAction : uint32_t { Clear, Load, DontCare };
enum class AttachmentStoreAction : uint32_t { Store, DontCare, MultisampleResolve };

struct AttachmentFacetDesc {
  AttachmentLoadAction load{AttachmentLoadAction::Clear};
  AttachmentStoreAction store{AttachmentStoreAction::Store};
  std::array<float, 4> clear_rgba{};
  // >1 renders into a transient multisample texture that resolves into the
  // facet's texture. Only meaningful with MultisampleResolve.
  uint32_t sample_count{1};
};

// AttachmentFacet use: one render pass against the facet's texture, then host
// readback of texel (0,0). Not a full raster workload -- it proves the facet
// maps to a render-target-capable MTLTexture and that load/store/resolve lower
// without becoming public object state.
struct AttachmentFacetResult {
  std::array<float, 4> resolved_rgba{};
  bool facet_cache_hit{};
  uint32_t encoder_count{};
  uint32_t sample_count{1};
  // 06 §6.3 requires reporting whether external-memory traffic was avoided.
  // True only when the multisample samples really were never written to
  // device memory (memoryless transient attachment resolved on-tile).
  bool store_traffic_avoided{};
  hal::LoweringReport report;
};

// Linear device buffer -> Private sample/storage/attachment-optimal MTLTexture
// via an explicit blit (02 §8: transform ≠ barrier), then Arena::transform()
// to publish a new RepresentationEpoch. Old FacetRefs become stale; the
// transform retires them itself. The blit source is resolved through a
// TransferFacet over the same CanonicalView, so no capability escapes the
// pool.
struct RepresentationTransformResult {
  uint32_t new_epoch{};
  uint64_t old_backing_bytes{};
  uint64_t new_backing_bytes{};
  uint64_t temporary_bytes{};
  uint32_t encoder_count{};
  bool used_private_optimal{};
  uint32_t retired_facet_count{};
  core::FacetRef out_facet{};
  hal::LoweringReport report;
};

class DeviceHal final : public hal::DeviceHal {
 public:
  ~DeviceHal() override;
  const hal::CapabilitySnapshot& capabilities() const override;
  bool compile(const hal::ExecutionPlan& plan, hal::CompiledPlan* compiled,
               std::string* error = nullptr) override;
  bool submit(const hal::CompiledPlan& compiled, core::Arena& arena,
              hal::Submission* submission, std::string* error = nullptr) override;

  const DeviceSnapshot& snapshot() const;
  bool probe_buffer(size_t length, bool private_storage, BufferSnapshot* result,
                    std::string* error = nullptr) const;

  bool run_cull_compact(const std::vector<uint32_t>& instance_visible,
                       const std::vector<uint32_t>& instance_ids,
                       CullCompactResult* result, std::string* error = nullptr) const;

  // Every run_*_facet resolves `ref` through FacetPool::lookup before touching
  // a Metal object, and brackets the command buffer in
  // FacetPool::begin_gpu_use/end_gpu_use so the slot cannot be recycled under
  // work still in flight (06 §6.4, §11). `pool` is non-const for that reason.
  // The backend cache is keyed by FacetRef index+generation, never by a host
  // texture pointer exposed on the public API.
  bool run_address_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                        AddressFacetResult* result, std::string* error = nullptr) const;

  bool run_sample_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                       core::FilterMode filter, core::WrapMode wrap,
                       const std::vector<std::array<float, 2>>& uv_coords,
                       SampleFacetResult* result, std::string* error = nullptr) const;

  bool run_storage_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                        StorageFacetTarget target, const std::array<float, 4>& write_rgba,
                        StorageFacetResult* result, std::string* error = nullptr) const;

  bool run_attachment_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                           const AttachmentFacetDesc& desc, AttachmentFacetResult* result,
                           std::string* error = nullptr) const;

  // Representation transform: build target-kind MTLTexture from the linear
  // device buffer, Arena::transform to a new epoch, retire the facets the old
  // epoch invalidated, and acquire out_facet into `pool`. Does not
  // ConsumeInput (02 §4.2) -- old host bytes remain until a later consume.
  bool run_representation_transform(core::Arena& arena, core::FacetPool& pool,
                                   const core::CanonicalView& view, core::FacetKind target_kind,
                                   RepresentationTransformResult* result,
                                   std::string* error = nullptr) const;

  const std::vector<std::array<uint32_t, 3>>& last_tier1_indirect_dims() const;

 private:
  struct Impl;
  explicit DeviceHal(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend std::unique_ptr<DeviceHal> make_device_hal();
};

std::unique_ptr<DeviceHal> make_device_hal();

}  // namespace vg::metal

#endif
