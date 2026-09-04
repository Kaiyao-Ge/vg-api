#include "backends/metal/metal_device_internal.h"

namespace vg::metal {

bool DeviceHal::Impl::transform_into_private_facet(core::Arena& arena, core::FacetPool& pool,
                                  const core::CanonicalView& view, core::FacetKind target_kind,
                                  core::FacetRef target_facet, TransformCost* cost, std::string* error) {
  const core::Allocation* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
  if (allocation == nullptr) {
    if (error) *error = "representation transform: backing allocation not found in arena";
    return false;
  }
  if (!view_expressible(view, target_kind, allocation->bytes.size(), error)) return false;

  id<MTLTexture> private_texture =
      [device newTextureWithDescriptor:make_texture_descriptor(view, target_kind, MTLStorageModePrivate)];
  if (private_texture == nil) {
    if (error) *error = "representation transform: Private MTLTexture creation failed";
    return false;
  }

  core::FacetRef transfer_ref{};
  if (!pool.acquire(arena, view, core::FacetKind::Transfer, &transfer_ref, error)) return false;
  {
    FacetUseGuard use(pool, transfer_ref);
    if (!use.begin(arena, error)) return false;
    id<MTLBuffer> source = ensure_facet_buffer(arena, pool, transfer_ref, core::FacetKind::Transfer, error);
    if (source == nil) return false;

    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    if (blit == nil) { if (error) *error = "failed to create Metal blit encoder"; return false; }
    for (uint32_t layer = 0; layer < view.array_layers; ++layer) {
      for (uint32_t level = 0; level < view.mip_levels; ++level) {
        const uint64_t offset = view.subresource_byte_offset({layer, level});
        const uint64_t row_bytes = view.bytes_per_row(level);
        [blit copyFromBuffer:source
                sourceOffset:offset
           sourceBytesPerRow:row_bytes
         sourceBytesPerImage:row_bytes * view.mip_height(level)
                  sourceSize:MTLSizeMake(view.mip_width(level), view.mip_height(level), 1)
                   toTexture:private_texture
            destinationSlice:layer
            destinationLevel:level
           destinationOrigin:MTLOriginMake(0, 0, 0)];
      }
    }
    [blit endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (cost != nullptr) {
      cost->encoder_count = 1;
      cost->command_buffer_count = 1;
      cost->queue_wait_count = 1;
    }
    if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
      if (error)
        *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                              : "representation transform blit failed";
      return false;
    }
  }
  // The TransferFacet existed only to give the blit a pool-resolved source;
  // its purpose is spent, so its slot goes back rather than being left to
  // linger until some later epoch happens to stale it.
  pool.retire(transfer_ref);

  if (install_facet_record(target_facet, view, target_kind, allocation->representation_epoch,
                           private_texture, error) == nil)
    return false;

  if (cost != nullptr) {
    // The device's own accounting for the texture it just created, not the
    // view's logical extent: alignment and tiling padding are real bytes the
    // peak-memory report of 06 §11 has to include.
    const uint64_t allocated = [private_texture allocatedSize];
    cost->new_backing_bytes = allocated != 0 ? allocated : view.byte_size();
    cost->temporary_bytes = 0;
    cost->encoder_count = 1;
    cost->command_buffer_count = 1;
    cost->queue_wait_count = 1;
  }
  return true;
}

}  // namespace vg::metal
