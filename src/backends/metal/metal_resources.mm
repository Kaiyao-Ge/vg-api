#include "backends/metal/metal_device_internal.h"
#include "vg_scene_root_layout.h"
#include <cstring>

namespace vg::metal {

bool same_swizzle(const core::SwizzleChannels& lhs, const core::SwizzleChannels& rhs) {
  return lhs.red == rhs.red && lhs.green == rhs.green && lhs.blue == rhs.blue &&
         lhs.alpha == rhs.alpha;
}

bool same_shape(const MetalFacetRecord& record, const core::CanonicalView& view) {
  return record.width == view.width && record.height == view.height &&
         record.dimension == view.dimension && record.array_layers == view.array_layers &&
         record.mip_levels == view.mip_levels && record.format == view.format &&
         same_swizzle(record.swizzle, view.swizzle);
}

MTLTextureSwizzle to_mtl_swizzle(core::Swizzle swizzle) {
  switch (swizzle) {
    case core::Swizzle::Red: return MTLTextureSwizzleRed;
    case core::Swizzle::Green: return MTLTextureSwizzleGreen;
    case core::Swizzle::Blue: return MTLTextureSwizzleBlue;
    case core::Swizzle::Alpha: return MTLTextureSwizzleAlpha;
    case core::Swizzle::Zero: return MTLTextureSwizzleZero;
    case core::Swizzle::One: return MTLTextureSwizzleOne;
  }
  return MTLTextureSwizzleRed;
}

MTLPixelFormat to_mtl_pixel_format(core::PixelFormat format) {
  switch (format) {
    case core::PixelFormat::RGBA8Unorm: return MTLPixelFormatRGBA8Unorm;
    case core::PixelFormat::R32Float: return MTLPixelFormatR32Float;
    case core::PixelFormat::Depth32Float: return MTLPixelFormatDepth32Float;
    // Index formats are byte-addressed MTLBuffers, never textures.
    case core::PixelFormat::R16Uint:
    case core::PixelFormat::R32Uint: return MTLPixelFormatInvalid;
  }
  return MTLPixelFormatInvalid;
}

MTLCompareFunction to_mtl_compare_function(core::DepthCompareOp op) {
  switch (op) {
    case core::DepthCompareOp::Never: return MTLCompareFunctionNever;
    case core::DepthCompareOp::Less: return MTLCompareFunctionLess;
    case core::DepthCompareOp::Equal: return MTLCompareFunctionEqual;
    case core::DepthCompareOp::LessEqual: return MTLCompareFunctionLessEqual;
    case core::DepthCompareOp::Greater: return MTLCompareFunctionGreater;
    case core::DepthCompareOp::NotEqual: return MTLCompareFunctionNotEqual;
    case core::DepthCompareOp::GreaterEqual: return MTLCompareFunctionGreaterEqual;
    case core::DepthCompareOp::Always: return MTLCompareFunctionAlways;
  }
  return MTLCompareFunctionAlways;
}

// A Texture2DArray view lowers to MTLTextureType2DArray even when it names a
// single layer: the shader-side type (texture2d vs texture2d_array) is part of
// the contract the view declares, and silently collapsing a one-layer array to
// a plain 2D texture would bind the wrong kernel.
MTLTextureType to_mtl_texture_type(core::ViewDimension dimension) {
  return dimension == core::ViewDimension::Texture2DArray ? MTLTextureType2DArray : MTLTextureType2D;
}

// Which MTLTextureUsage a facet kind needs. Sample additionally asks for
// PixelFormatView because a SampleFacet is exactly the kind that gets
// reinterpreted: a non-identity swizzle needs a swizzled view, and the raster
// path needs a single-slice 2D view over an array source to reach the
// texture2d<float> the shared raster fragment shader declares. Storage and
// Attachment get it only when the view really asks for a swizzle, so nothing
// pays for a reinterpretation it cannot use.
MTLTextureUsage facet_texture_usage(core::FacetKind kind, const core::CanonicalView& view) {
  MTLTextureUsage usage = 0;
  switch (kind) {
    case core::FacetKind::Sample:
      usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
      break;
    case core::FacetKind::Storage:
      usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
      break;
    case core::FacetKind::Attachment:
      usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      break;
    default:
      return 0;
  }
  if (!view.swizzle.identity()) usage |= MTLTextureUsagePixelFormatView;
  return usage;
}

// One descriptor shape for every texture this backend mints from a
// CanonicalView, so the Shared upload path and the Private transform path
// cannot drift on dimension, mip count or array length.
MTLTextureDescriptor* make_texture_descriptor(const core::CanonicalView& view, core::FacetKind kind,
                                              MTLStorageMode storage_mode) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor new];
  descriptor.textureType = to_mtl_texture_type(view.dimension);
  descriptor.pixelFormat = to_mtl_pixel_format(view.format);
  descriptor.width = view.width;
  descriptor.height = view.height;
  descriptor.depth = 1;
  descriptor.mipmapLevelCount = view.mip_levels;
  descriptor.arrayLength = view.array_layers;
  descriptor.sampleCount = 1;
  descriptor.storageMode = storage_mode;
  descriptor.usage = facet_texture_usage(kind, view);
  return descriptor;
}

// 05 §14: a rejection speaks VG concepts. Every caller that hands a view to
// Metal funnels its shape checks through here so the diagnostics are one text,
// not one per entry point.
bool view_expressible(const core::CanonicalView& view, core::FacetKind kind, uint64_t backing_bytes,
                      std::string* error) {
  if (!view.valid(error)) return false;
  if (kind != core::FacetKind::Sample && kind != core::FacetKind::Storage &&
      kind != core::FacetKind::Attachment) {
    if (error) *error = "Unsupported: Address/Transfer facets have no Metal texture representation";
    return false;
  }
  // Swizzle reinterprets a shader read. Metal applies no such remap to a render
  // target or to an image write, so rather than quietly dropping the channel
  // mapping the caller asked for, those kinds are refused.
  if (!view.swizzle.identity() && kind != core::FacetKind::Sample) {
    if (error) *error = "Unsupported: non-identity swizzle applies to SampleFacet only";
    return false;
  }
  if (backing_bytes < view.byte_size()) {
    if (error)
      *error = "canonical view declares " + std::to_string(view.byte_size()) +
               " bytes of subresources but its allocation holds only " + std::to_string(backing_bytes);
    return false;
  }
  return true;
}

bool subresource_in_range(const core::CanonicalView& view, const AttachmentSubresource& subresource,
                          std::string* error) {
  if (subresource.layer >= view.array_layers || subresource.level >= view.mip_levels) {
    if (error)
      *error = "render pass targets layer " + std::to_string(subresource.layer) + " level " +
               std::to_string(subresource.level) + " of a canonical view declaring " +
               std::to_string(view.array_layers) + " layer(s) and " + std::to_string(view.mip_levels) +
               " mip level(s)";
    return false;
  }
  return true;
}

std::array<float, 4> decode_texel(const void* bytes, MTLPixelFormat format) {
  if (format == MTLPixelFormatRGBA8Unorm) {
    const auto* rgba = static_cast<const uint8_t*>(bytes);
    return {rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f};
  }
  float value{};
  std::memcpy(&value, bytes, sizeof(value));
  return {value, 0.0f, 0.0f, 1.0f};
}

NSString* ns_utf8(std::string_view text) {
  return [[[NSString alloc] initWithBytes:text.data()
                                   length:text.size()
                                 encoding:NSUTF8StringEncoding] autorelease];
}

FacetUseGuard::FacetUseGuard(core::FacetPool& pool, core::FacetRef ref) : pool_(pool), ref_(ref) {}
FacetUseGuard::~FacetUseGuard() {
  if (held_) pool_.end_gpu_use(ref_);
}
bool FacetUseGuard::begin(const core::Arena& arena, std::string* error) {
  held_ = pool_.begin_gpu_use(arena, ref_, error);
  return held_;
}


DeviceHal::Impl::~Impl() {
  for (auto& entry : allocation_map) release_buffer(entry.second.buffer);
  release_buffer(identity_scene_root_buffer);
  for (auto& entry : facet_map) release_facet_textures(entry.second);
}

void DeviceHal::Impl::release_buffer(id<MTLBuffer>& buffer) {
  if (buffer != nil) {
    [buffer release];
    buffer = nil;
  }
}

void DeviceHal::Impl::release_facet_textures(MetalFacetRecord& record) {
  if (record.texture != nil && record.texture != record.storage_texture) [record.texture release];
  if (record.storage_texture != nil) [record.storage_texture release];
  record.texture = nil;
  record.storage_texture = nil;
}

uint32_t DeviceHal::Impl::retire_stale_facet_textures(const core::Arena& arena, const core::FacetPool& pool) {
  uint32_t retired = 0;
  for (auto it = facet_map.begin(); it != facet_map.end();) {
    const core::FacetRef ref{it->second.facet_index, it->second.facet_generation};
    if (pool.in_flight(ref) != 0) {
      ++it;
      continue;
    }
    core::FacetStatus status = core::FacetStatus::Ok;
    if (pool.lookup(arena, ref, &status) != nullptr) {
      ++it;
      continue;
    }
    release_facet_textures(it->second);
    it = facet_map.erase(it);
    ++retired;
  }
  return retired;
}

uint64_t DeviceHal::Impl::release_empty_linear_buffers(const core::Arena& arena) {
  uint64_t released = 0;
  for (auto it = allocation_map.begin(); it != allocation_map.end();) {
    const core::Allocation* allocation = arena.lookup(core::PointerRef{it->first, it->second.generation});
    if (allocation != nullptr && !allocation->bytes.empty()) {
      ++it;
      continue;
    }
    released += it->second.byte_size;
    release_buffer(it->second.buffer);
    it = allocation_map.erase(it);
  }
  return released;
}

void DeviceHal::Impl::reclaim_released_backing(const core::Arena& arena, const core::FacetPool& pool,
                              uint32_t* retired_textures, uint64_t* released_linear) {
  const uint32_t textures = retire_stale_facet_textures(arena, pool);
  const uint64_t linear = release_empty_linear_buffers(arena);
  if (retired_textures != nullptr) *retired_textures = textures;
  if (released_linear != nullptr) *released_linear = linear;
}

id<MTLBuffer> DeviceHal::Impl::ensure_buffer(const core::Allocation& allocation) {
  auto it = allocation_map.find(allocation.id);
  // ConsumeInput has already handed the linear representation back. A dummy
  // 1-byte buffer here would keep a device allocation the host just released
  // and let a later dispatch write into empty host bytes.
  if (allocation.bytes.empty()) {
    if (it != allocation_map.end()) {
      release_buffer(it->second.buffer);
      allocation_map.erase(it);
    }
    return nil;
  }
  const size_t needed = allocation.bytes.size();
  if (it != allocation_map.end() &&
      (it->second.generation != allocation.generation || it->second.byte_size < needed)) {
    release_buffer(it->second.buffer);
    allocation_map.erase(it);
    it = allocation_map.end();
  }
  if (it == allocation_map.end()) {
    id<MTLBuffer> buffer = [device newBufferWithLength:needed options:MTLResourceStorageModeShared];
    if (buffer == nil) return nil;
    // Start stale so the common copy below seeds a newly created buffer.
    MetalAllocationRecord record{buffer, allocation.id, allocation.generation, needed, 0};
    it = allocation_map.emplace(allocation.id, record).first;
  }
  if (it->second.content_epoch != allocation.content_epoch) {
    std::memcpy([it->second.buffer contents], allocation.bytes.data(), allocation.bytes.size());
    it->second.content_epoch = allocation.content_epoch;
  }
  return it->second.buffer;
}

id<MTLBuffer> DeviceHal::Impl::make_identity_scene_root_buffer(bool* created) {
  std::lock_guard<std::mutex> lock(identity_scene_root_mutex);
  if (identity_scene_root_buffer != nil) {
    if (created != nullptr) *created = false;
    return identity_scene_root_buffer;
  }
  identity_scene_root_buffer = [device newBufferWithLength:VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE
                                                   options:MTLResourceStorageModeShared];
  if (identity_scene_root_buffer == nil) return nil;
  std::memset([identity_scene_root_buffer contents], 0, VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE);
  auto* matrix = static_cast<float*>([identity_scene_root_buffer contents]);
  matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
  if (created != nullptr) *created = true;
  return identity_scene_root_buffer;
}

void DeviceHal::Impl::commit_buffer_write(core::Allocation& allocation, id<MTLBuffer> buffer) {
  if (buffer == nil || allocation.bytes.empty()) return;
  std::memcpy(allocation.bytes.data(), [buffer contents], allocation.bytes.size());
  ++allocation.content_epoch;
  auto it = allocation_map.find(allocation.id);
  if (it != allocation_map.end() && it->second.generation == allocation.generation)
    it->second.content_epoch = allocation.content_epoch;
}

uint64_t DeviceHal::Impl::facet_cache_key(core::FacetRef ref) {
  return (uint64_t(ref.index) << 32) | uint64_t(ref.generation);
}

const core::FacetSlot* DeviceHal::Impl::resolve_facet(const core::Arena& arena, const core::FacetPool& pool,
                                     core::FacetRef ref, core::FacetKind expected_kind,
                                     std::string* error) {
  core::FacetStatus status = core::FacetStatus::Ok;
  const core::FacetSlot* slot = pool.lookup(arena, ref, &status);
  if (slot == nullptr) {
    if (error) *error = core::to_string(status);
    return nullptr;
  }
  if (slot->kind != expected_kind) {
    if (error) *error = "facet kind mismatch";
    return nullptr;
  }
  return slot;
}

id<MTLBuffer> DeviceHal::Impl::ensure_facet_buffer(const core::Arena& arena, const core::FacetPool& pool,
                                  core::FacetRef ref, core::FacetKind expected_kind,
                                  std::string* error) {
  const core::FacetSlot* slot = resolve_facet(arena, pool, ref, expected_kind, error);
  if (slot == nullptr) return nil;
  const core::Allocation* allocation =
      arena.lookup(core::PointerRef{slot->view.allocation, slot->view.allocation_generation});
  if (allocation == nullptr) {
    if (error) *error = "facet backing allocation not found in arena";
    return nil;
  }
  id<MTLBuffer> buffer = ensure_buffer(*allocation);
  if (buffer == nil && error) *error = "Metal facet buffer allocation failed";
  return buffer;
}

bool DeviceHal::Impl::read_texture_region(id<MTLTexture> texture, uint32_t slice, uint32_t level, uint32_t origin_x,
                         uint32_t origin_y, uint32_t width, uint32_t height,
                         std::vector<std::array<float, 4>>* out, std::string* error) {
  if (width == 0 || height == 0) { if (error) *error = "facet readback window is empty"; return false; }
  const size_t row_bytes = static_cast<size_t>(width) * kBytesPerTexel;
  const size_t image_bytes = row_bytes * height;
  std::vector<uint8_t> bytes(image_bytes);
  if (texture.storageMode != MTLStorageModePrivate) {
    [texture getBytes:bytes.data()
          bytesPerRow:row_bytes
        bytesPerImage:image_bytes
           fromRegion:MTLRegionMake2D(origin_x, origin_y, width, height)
          mipmapLevel:level
                slice:slice];
  } else {
    id<MTLBuffer> readback = [device newBufferWithLength:image_bytes options:MTLResourceStorageModeShared];
    if (readback == nil) { if (error) *error = "facet readback buffer allocation failed"; return false; }
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    if (blit == nil) { if (error) *error = "failed to create Metal blit encoder"; return false; }
    [blit copyFromTexture:texture
                sourceSlice:slice
                sourceLevel:level
               sourceOrigin:MTLOriginMake(origin_x, origin_y, 0)
                 sourceSize:MTLSizeMake(width, height, 1)
                   toBuffer:readback
          destinationOffset:0
     destinationBytesPerRow:row_bytes
   destinationBytesPerImage:image_bytes];
    [blit endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
      if (error)
        *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                              : "facet readback blit failed";
      return false;
    }
    std::memcpy(bytes.data(), [readback contents], image_bytes);
  }
  out->resize(static_cast<size_t>(width) * static_cast<size_t>(height));
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      (*out)[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
          decode_texel(bytes.data() + static_cast<size_t>(y) * row_bytes +
                           static_cast<size_t>(x) * kBytesPerTexel,
                       texture.pixelFormat);
    }
  }
  return true;
}

bool DeviceHal::Impl::read_texel(id<MTLTexture> texture, uint32_t slice, uint32_t level, uint32_t x, uint32_t y,
                std::array<float, 4>* out, std::string* error) {
  std::vector<std::array<float, 4>> texels;
  if (!read_texture_region(texture, slice, level, x, y, 1, 1, &texels, error)) return false;
  *out = texels[0];
  return true;
}

id<MTLTexture> DeviceHal::Impl::install_facet_record(core::FacetRef ref, const core::CanonicalView& view,
                                    core::FacetKind kind, uint32_t representation_epoch,
                                    id<MTLTexture> storage_texture, std::string* error) {
  id<MTLTexture> shader_texture = storage_texture;
  if (!view.swizzle.identity()) {
    const MTLTextureSwizzleChannels channels =
        MTLTextureSwizzleChannelsMake(to_mtl_swizzle(view.swizzle.red), to_mtl_swizzle(view.swizzle.green),
                                      to_mtl_swizzle(view.swizzle.blue), to_mtl_swizzle(view.swizzle.alpha));
    // The swizzle applies to the whole view contract, so the view must cover
    // every level and slice the CanonicalView declares. A (0,1)/(0,1) range
    // would silently narrow a mip/array facet to its first subresource.
    shader_texture = [storage_texture newTextureViewWithPixelFormat:to_mtl_pixel_format(view.format)
                                                        textureType:to_mtl_texture_type(view.dimension)
                                                             levels:NSMakeRange(0, view.mip_levels)
                                                             slices:NSMakeRange(0, view.array_layers)
                                                            swizzle:channels];
    if (shader_texture == nil) {
      if (error) *error = "Metal facet swizzle texture view creation failed";
      return nil;
    }
  }
  MetalFacetRecord record;
  record.texture = shader_texture;
  record.storage_texture = storage_texture;
  record.facet_index = ref.index;
  record.facet_generation = ref.generation;
  record.representation_epoch = representation_epoch;
  record.kind = kind;
  record.width = view.width;
  record.height = view.height;
  record.dimension = view.dimension;
  record.array_layers = view.array_layers;
  record.mip_levels = view.mip_levels;
  record.format = view.format;
  record.swizzle = view.swizzle;
  const uint64_t key = facet_cache_key(ref);
  auto existing = facet_map.find(key);
  if (existing != facet_map.end()) {
    release_facet_textures(existing->second);
    facet_map.erase(existing);
  }
  facet_map[key] = record;
  return shader_texture;
}

void DeviceHal::Impl::upload_view_subresources(id<MTLTexture> texture, const core::CanonicalView& view,
                              const core::Allocation& allocation) {
  if (allocation.bytes.empty()) return;
  const uint8_t* base = allocation.bytes.data();
  for (uint32_t layer = 0; layer < view.array_layers; ++layer) {
    for (uint32_t level = 0; level < view.mip_levels; ++level) {
      const uint64_t offset = view.subresource_byte_offset({layer, level});
      const uint64_t row_bytes = view.bytes_per_row(level);
      const MTLRegion region = MTLRegionMake2D(0, 0, view.mip_width(level), view.mip_height(level));
      [texture replaceRegion:region
                 mipmapLevel:level
                       slice:layer
                   withBytes:base + offset
                 bytesPerRow:row_bytes
               bytesPerImage:0];
    }
  }
}

id<MTLTexture> DeviceHal::Impl::ensure_facet_texture(const core::Arena& arena, const core::FacetPool& pool,
                                    core::FacetRef ref, core::FacetKind expected_kind,
                                    bool* cache_hit, id<MTLTexture>* out_storage,
                                    std::string* error) {
  const core::FacetSlot* slot = resolve_facet(arena, pool, ref, expected_kind, error);
  if (slot == nullptr) return nil;
  const core::CanonicalView& view = slot->view;
  const core::Allocation* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
  if (allocation == nullptr) {
    if (error) *error = "facet backing allocation not found in arena";
    return nil;
  }
  // The cache is consulted before the backing is examined, and deliberately
  // so. After a Stage 5 ConsumeInput the linear bytes this facet was built
  // from are gone -- that is the whole point of reporting distinct_backing
  // (02 §4.2, 06 §11): the Private texture is independent storage, and the
  // facet the transform published "stays live across a ConsumeInput". Asking
  // the allocation how many bytes it still holds before answering would
  // retire exactly the facet a consume is supposed to leave usable.
  const uint64_t key = facet_cache_key(ref);
  auto it = facet_map.find(key);
  if (it != facet_map.end() &&
      it->second.representation_epoch == slot->representation_epoch &&
      it->second.kind == slot->kind && same_shape(it->second, view) &&
      it->second.facet_index == ref.index &&
      it->second.facet_generation == ref.generation) {
    // A host write changes bytes, not the facet contract. Refresh the
    // existing Shared texture rather than invalidating the capability.
    if (!allocation->bytes.empty() && it->second.content_epoch != allocation->content_epoch) {
      upload_view_subresources(it->second.storage_texture, view, *allocation);
      it->second.content_epoch = allocation->content_epoch;
    }
    if (cache_hit) *cache_hit = true;
    if (out_storage) *out_storage = it->second.storage_texture;
    return it->second.texture;
  }
  if (it != facet_map.end()) {
    release_facet_textures(it->second);
    facet_map.erase(it);
  }

  // Creating one, on the other hand, means seeding it from host bytes, so
  // here the backing really does have to cover every subresource the view
  // declares.
  if (!view_expressible(view, expected_kind, allocation->bytes.size(), error)) return nil;

  id<MTLTexture> storage_texture =
      [device newTextureWithDescriptor:make_texture_descriptor(view, expected_kind, MTLStorageModeShared)];
  if (storage_texture == nil) {
    if (error)
      *error = expected_kind == core::FacetKind::Storage
                   ? "Unsupported: pixel format does not support shader write on this device; "
                     "use StorageFacetTarget::LinearBuffer or transform the representation"
                   : "Metal facet texture creation failed";
    return nil;
  }

  // Every kind is seeded from the allocation, including Attachment: a
  // load-action pass must see the bytes the CanonicalView names, which is
  // also what the Private transform path produces for an Attachment target,
  // so the two ways of reaching an attachment texture agree on its initial
  // contents.
  upload_view_subresources(storage_texture, view, *allocation);

  id<MTLTexture> shader_texture = install_facet_record(ref, view, slot->kind, slot->representation_epoch,
                                                       storage_texture, error);
  if (shader_texture == nil) return nil;
  auto fresh = facet_map.find(key);
  if (fresh != facet_map.end()) fresh->second.content_epoch = allocation->content_epoch;
  if (cache_hit) *cache_hit = false;
  if (out_storage) *out_storage = storage_texture;
  return shader_texture;
}

bool DeviceHal::Impl::probe_gpu_addresses() {
  if (gpu_addresses_probed) return gpu_addresses_supported_value;
  gpu_addresses_probed = true;
  id<MTLBuffer> probe = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
  gpu_addresses_supported_value = probe != nil && [probe respondsToSelector:@selector(gpuAddress)];
  return gpu_addresses_supported_value;
}

id<MTLTexture> DeviceHal::Impl::ensure_guard_placeholder_texture(std::string* error) {
  if (guard_placeholder_texture != nil) return guard_placeholder_texture;
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:1
                                                        height:1
                                                     mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = MTLTextureUsageShaderRead;
  guard_placeholder_texture = [device newTextureWithDescriptor:descriptor];
  if (guard_placeholder_texture == nil && error)
    *error = "Metal guard placeholder texture creation failed";
  return guard_placeholder_texture;
}

}  // namespace vg::metal
