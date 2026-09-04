#include "backends/metal/metal_device_internal.h"
#include <algorithm>
#include <cstring>

namespace vg::metal {

MTLRenderPassDescriptor* DeviceHal::Impl::make_render_pass(id<MTLTexture> texture, const AttachmentFacetDesc& desc,
                                          const core::CanonicalView& view,
                                          bool* store_traffic_avoided, std::string* error) {
  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
  MTLRenderPassColorAttachmentDescriptor* color = rp.colorAttachments[0];
  color.clearColor =
      MTLClearColorMake(desc.clear_rgba[0], desc.clear_rgba[1], desc.clear_rgba[2], desc.clear_rgba[3]);
  switch (desc.load) {
    case AttachmentLoadAction::Clear: color.loadAction = MTLLoadActionClear; break;
    case AttachmentLoadAction::Load: color.loadAction = MTLLoadActionLoad; break;
    case AttachmentLoadAction::DontCare: color.loadAction = MTLLoadActionDontCare; break;
  }
  const uint32_t level = desc.subresource.level;
  if (desc.sample_count > 1) {
    MTLTextureDescriptor* ms = [MTLTextureDescriptor new];
    ms.textureType = MTLTextureType2DMultisample;
    ms.pixelFormat = texture.pixelFormat;
    // The transient target is sized for the subresource being rendered, not
    // for mip 0: rendering into level N of a mip chain is a smaller pass.
    ms.width = view.mip_width(level);
    ms.height = view.mip_height(level);
    ms.sampleCount = desc.sample_count;
    ms.usage = MTLTextureUsageRenderTarget;
    // Memoryless keeps the per-sample data on-tile, so the only external
    // write is the resolved single-sample result. Where that is unavailable
    // the samples really do go to device memory, and the result says so
    // rather than claiming an optimization the device did not perform.
    const bool memoryless = [device supportsFamily:MTLGPUFamilyApple1];
    ms.storageMode = memoryless ? MTLStorageModeMemoryless : MTLStorageModePrivate;
    id<MTLTexture> ms_texture = [device newTextureWithDescriptor:ms];
    if (ms_texture == nil) {
      if (error) *error = "Unsupported: device rejected a multisample render target for this format";
      return nil;
    }
    color.texture = ms_texture;
    color.resolveTexture = texture;
    color.resolveLevel = level;
    color.resolveSlice = desc.subresource.layer;
    color.storeAction = MTLStoreActionMultisampleResolve;
    if (store_traffic_avoided) *store_traffic_avoided = memoryless;
  } else {
    color.texture = texture;
    color.level = level;
    color.slice = desc.subresource.layer;
    color.storeAction =
        desc.store == AttachmentStoreAction::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
    if (store_traffic_avoided) *store_traffic_avoided = desc.store == AttachmentStoreAction::DontCare;
  }
  return rp;
}

bool DeviceHal::Impl::run_raster_pass(core::Arena& arena, core::FacetPool& pool, core::RasterFacetPair facets,
                     const RasterDesc& desc, id<MTLBuffer> vertex_buffer, id<MTLBuffer> scene_root_buffer,
                     id<MTLBuffer> tint_buffer,
                     uint32_t vertex_count, id<MTLBuffer> index_buffer, MTLIndexType index_type,
                     uint32_t index_count, RasterResult* result, std::string* error,
                     const ir::UserRasterShaderContract* user_shader,
                     bool* command_submitted) {
  if (command_submitted != nullptr) *command_submitted = false;
  if (result == nullptr) { if (error) *error = "raster result output is required"; return false; }
  const uint32_t primitive_count = index_buffer != nil ? index_count : vertex_count;
  if (vertex_count == 0 || primitive_count == 0 || primitive_count % 3 != 0) {
    if (error) *error = "raster vertex count must be a non-zero multiple of 3 (triangle list)";
    return false;
  }
  const bool multisampled = desc.attachment.sample_count > 1;
  if (multisampled != (desc.attachment.store == AttachmentStoreAction::MultisampleResolve)) {
    if (error) *error = "raster: MultisampleResolve and sample_count > 1 must be requested together";
    return false;
  }
  if (multisampled && desc.attachment.load == AttachmentLoadAction::Load) {
    if (error) *error = "Unsupported: a transient multisample attachment has no prior contents to load";
    return false;
  }

  // Both refs are capability tokens and both are bracketed: a pass reads one
  // facet and writes another, so neither slot may be recycled under work still
  // in flight (06 §6.4, §11).
  FacetUseGuard source_use(pool, facets.source);
  if (!source_use.begin(arena, error)) return false;
  FacetUseGuard target_use(pool, facets.target);
  if (!target_use.begin(arena, error)) return false;
  // A facet at slot zero is valid. Match core/reference's presence test so
  // a malformed `{nonzero index, zero generation}` never silently disables
  // depth on Metal while other backends reject it as a stale capability.
  const bool has_depth = desc.depth_attachment_ref.index != 0 || desc.depth_attachment_ref.generation != 0;
  std::unique_ptr<FacetUseGuard> depth_use;
  if (has_depth) {
    depth_use = std::make_unique<FacetUseGuard>(pool, desc.depth_attachment_ref);
    if (!depth_use->begin(arena, error)) return false;
  }

  const core::FacetSlot* source_slot =
      resolve_facet(arena, pool, facets.source, core::FacetKind::Sample, error);
  if (source_slot == nullptr) return false;
  const core::FacetSlot* target_slot =
      resolve_facet(arena, pool, facets.target, core::FacetKind::Attachment, error);
  if (target_slot == nullptr) return false;
  const core::FacetSlot* depth_slot = nullptr;
  if (has_depth) {
    depth_slot = resolve_facet(arena, pool, desc.depth_attachment_ref, core::FacetKind::Attachment, error);
    if (depth_slot == nullptr) return false;
  }
  const core::CanonicalView& source_view = source_slot->view;
  const core::CanonicalView& target_view = target_slot->view;
  const core::CanonicalView* depth_view = has_depth ? &depth_slot->view : nullptr;
  // F4's fixed fragment contract samples an RGBA8 source into one RGBA8
  // color attachment. Keep this aligned with the Reference oracle instead
  // of letting Metal's texture2d<float> accept R32Float as a divergent
  // one-channel interpretation.
  if (source_view.format != core::PixelFormat::RGBA8Unorm) {
    if (error) *error = "raster source must use PixelFormat::RGBA8Unorm";
    return false;
  }
  if (has_depth && depth_view->format != core::PixelFormat::Depth32Float) {
    if (error) *error = "raster depth attachment must use PixelFormat::Depth32Float";
    return false;
  }
  if (target_view.format != core::PixelFormat::RGBA8Unorm) {
    if (error) *error = "F4 raster color attachment must use PixelFormat::RGBA8Unorm";
    return false;
  }
  if (has_depth && (target_view.width != depth_view->width || target_view.height != depth_view->height ||
      target_view.array_layers != depth_view->array_layers || target_view.mip_levels != depth_view->mip_levels ||
      target_view.dimension != depth_view->dimension)) {
    if (error) *error = "raster color and depth attachment views must have identical dimensions, layers, and mips";
    return false;
  }
  if (has_depth && desc.attachment.sample_count != 1) {
    if (error) *error = "F4 depth raster supports only single-sample attachments";
    return false;
  }
  if (!subresource_in_range(target_view, desc.attachment.subresource, error) ||
      (has_depth && !subresource_in_range(*depth_view, desc.attachment.subresource, error)))
    return false;
  if (desc.source_array_slice >= source_view.array_layers) {
    if (error)
      *error = "raster source names array slice " + std::to_string(desc.source_array_slice) +
               " of a canonical view declaring " + std::to_string(source_view.array_layers) + " layer(s)";
    return false;
  }
  if (!(desc.source_lod >= 0.0f) || desc.source_lod > static_cast<float>(source_view.mip_levels - 1)) {
    if (error)
      *error = "raster source names lod " + std::to_string(desc.source_lod) +
               " of a canonical view declaring " + std::to_string(source_view.mip_levels) + " mip level(s)";
    return false;
  }
  // Reading the very subresource being written has no defined result, and a
  // pass that returned an order-dependent image for it would be worse than
  // useless as a differential against the oracle, which refuses it for exactly
  // this reason. Sharing an allocation is fine as long as the subresource
  // differs, so generating one mip level from another stays expressible.
  if (source_view.allocation == target_view.allocation &&
      desc.source_array_slice == desc.attachment.subresource.layer &&
      static_cast<uint32_t>(desc.source_lod) == desc.attachment.subresource.level &&
      desc.source_lod == static_cast<float>(static_cast<uint32_t>(desc.source_lod))) {
    if (error)
      *error = "raster source and target name the same subresource of the same allocation; a read of the "
               "surface being written has no defined result";
    return false;
  }

  bool source_cache_hit = false;
  std::string tex_error;
  id<MTLTexture> source_texture = ensure_facet_texture(arena, pool, facets.source,
                                                        core::FacetKind::Sample, &source_cache_hit,
                                                        nullptr, &tex_error);
  if (source_texture == nil) {
    if (error) *error = tex_error.empty() ? "Metal raster source texture creation failed" : tex_error;
    return false;
  }
  // The shared fragment stage declares `texture2d<float>` and takes no slice or
  // level argument, so an array source reaches it as a single-slice 2D view
  // over the whole mip chain -- a real reinterpretation of the same storage,
  // not a copy and not a silently ignored slice. The requested level is then
  // pinned through the sampler's lod clamps below, which is exact for a
  // fractional lod too because MipFilterLinear blends the two levels the clamp
  // lands between.
  if (source_view.dimension == core::ViewDimension::Texture2DArray) {
    source_texture = [source_texture newTextureViewWithPixelFormat:source_texture.pixelFormat
                                                       textureType:MTLTextureType2D
                                                            levels:NSMakeRange(0, source_view.mip_levels)
                                                            slices:NSMakeRange(desc.source_array_slice, 1)];
    if (source_texture == nil) {
      if (error) *error = "Metal raster source array-slice texture view creation failed";
      return false;
    }
  }
  id<MTLSamplerState> sampler =
      ensure_sampler_state(desc.filter, desc.wrap, {.min = desc.source_lod, .max = desc.source_lod});
  if (sampler == nil) { if (error) *error = "Metal raster sampler creation failed"; return false; }

  bool target_cache_hit = false;
  id<MTLTexture> target_texture = ensure_facet_texture(arena, pool, facets.target,
                                                        core::FacetKind::Attachment,
                                                        &target_cache_hit, nullptr, &tex_error);
  if (target_texture == nil) {
    if (error) *error = tex_error.empty() ? "Metal raster target texture creation failed" : tex_error;
    return false;
  }
  bool depth_cache_hit = false;
  id<MTLTexture> depth_texture = nil;
  if (has_depth)
    depth_texture = ensure_facet_texture(arena, pool, desc.depth_attachment_ref,
                                         core::FacetKind::Attachment,
                                         &depth_cache_hit, nullptr, &tex_error);
  if (has_depth && depth_texture == nil) {
    if (error) *error = tex_error.empty() ? "Metal raster depth texture creation failed" : tex_error;
    return false;
  }

  // Attachment format and sample count are compiled into the pipeline and are
  // therefore key state (06 §7); the viewport set below and the tint bound at
  // fragment buffer(0) are not, and must not enlarge the key.
  std::string pipeline_error;
  id<MTLRenderPipelineState> pipeline_state = nil;
  id<MTLDepthStencilState> depth_state = nil;
  bool depth_state_cache_hit = false;
  if (!ensure_raster_pipeline(target_view.format,
                              has_depth ? depth_view->format : core::PixelFormat::RGBA8Unorm,
                              has_depth,
                              desc.attachment.sample_count,
                              desc.depth_test_enable, desc.depth_write_enable, desc.depth_compare_op,
                              &pipeline_state, &depth_state, &depth_state_cache_hit,
                              &pipeline_error, user_shader)) {
    if (error) *error = "Metal raster pipeline compile failed: " + pipeline_error;
    return false;
  }

  const uint32_t level = desc.attachment.subresource.level;
  const uint32_t width = target_view.mip_width(level);
  const uint32_t height = target_view.mip_height(level);
  bool store_traffic_avoided = false;
  MTLRenderPassDescriptor* rp = make_render_pass(target_texture, desc.attachment, target_view,
                                                 &store_traffic_avoided, error);
  if (rp == nil) return false;
  // F4 has a deliberately fixed depth attachment policy: no load of old
  // depth, clear every task to the far plane, and preserve the resulting
  // depth texture for subsequent inspection/use.
  if (has_depth) {
    MTLRenderPassDepthAttachmentDescriptor* depth = rp.depthAttachment;
    depth.texture = depth_texture;
    depth.level = level;
    depth.slice = desc.attachment.subresource.layer;
    depth.loadAction = MTLLoadActionClear;
    depth.storeAction = MTLStoreActionStore;
    depth.clearDepth = 1.0;
  }

  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:rp];
  if (encoder == nil) { if (error) *error = "failed to create Metal render encoder"; return false; }
  [encoder setRenderPipelineState:pipeline_state];
  if (has_depth) [encoder setDepthStencilState:depth_state];
  // Dynamic state, deliberately: a viewport change must not compile a second
  // pipeline (06 §7's "小的动态状态不应无故扩大 key").
  [encoder setViewport:(MTLViewport){0.0, 0.0, static_cast<double>(width), static_cast<double>(height),
                                     0.0, 1.0}];
  [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:compiler::kRasterVertexBufferIndex];
  [encoder setVertexBuffer:scene_root_buffer offset:0 atIndex:compiler::kRasterSceneRootBufferIndex];
  [encoder setFragmentTexture:source_texture atIndex:compiler::kRasterTextureIndex];
  [encoder setFragmentSamplerState:sampler atIndex:compiler::kRasterSamplerIndex];
  [encoder setFragmentBuffer:tint_buffer offset:0 atIndex:compiler::kRasterTintBufferIndex];
  if (index_buffer != nil) {
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:index_count indexType:index_type
                       indexBuffer:index_buffer indexBufferOffset:0];
  } else {
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertex_count];
  }
  [encoder endEncoding];
  [command_buffer commit];
  if (command_submitted != nullptr) *command_submitted = true;
  [command_buffer waitUntilCompleted];
  result->encoder_count = 1;
  result->report = make_facet_report();
  result->report.encoder_count = 1;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal raster pass failed";
    return false;
  }

  const bool stored = desc.attachment.store == AttachmentStoreAction::Store ||
                      desc.attachment.store == AttachmentStoreAction::MultisampleResolve;
  // The whole target subresource, not texel (0,0): an image-correctness
  // differential against reference::raster_triangles needs every pixel, and a
  // single-texel readback would let a coverage or interpolation regression pass
  // unnoticed.
  if (!read_texture_region(target_texture, desc.attachment.subresource.layer, level, 0, 0, width,
                           height, &result->resolved_rgba, error))
    return false;
  if (has_depth) {
    std::vector<std::array<float, 4>> depth_rgba;
    if (!read_texture_region(depth_texture, desc.attachment.subresource.layer, level, 0, 0, width,
                             height, &depth_rgba, error)) return false;
    result->resolved_depth.reserve(depth_rgba.size());
    for (const auto& value : depth_rgba) result->resolved_depth.push_back(value[0]);
  }

  // F7 makes the canonical Arena bytes the public readback source. Commit
  // the completed GPU attachment(s) there before submit returns; cached
  // textures are then stamped with the same content epoch so a later use
  // does not upload stale pre-draw bytes over the result.
  auto commit_rgba = [&](core::FacetRef ref, const core::CanonicalView& view,
                         const std::vector<std::array<float, 4>>& pixels) {
    auto* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
    if (allocation == nullptr || view.format != core::PixelFormat::RGBA8Unorm) return;
    const uint64_t base = view.subresource_byte_offset({desc.attachment.subresource.layer, level});
    for (size_t i = 0; i < pixels.size(); ++i) {
      uint8_t* out = allocation->bytes.data() + base + i * 4;
      for (size_t c = 0; c < 4; ++c)
        out[c] = static_cast<uint8_t>(std::clamp(pixels[i][c], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    arena.mark_content_modified(*allocation);
    auto record = facet_map.find(facet_cache_key(ref));
    if (record != facet_map.end()) record->second.content_epoch = allocation->content_epoch;
  };
  commit_rgba(facets.target, target_view, result->resolved_rgba);
  if (has_depth) {
    auto* allocation = arena.lookup(core::PointerRef{depth_view->allocation, depth_view->allocation_generation});
    if (allocation != nullptr) {
      const uint64_t base = depth_view->subresource_byte_offset({desc.attachment.subresource.layer, level});
      for (size_t i = 0; i < result->resolved_depth.size(); ++i)
        std::memcpy(allocation->bytes.data() + base + i * sizeof(float), &result->resolved_depth[i], sizeof(float));
      arena.mark_content_modified(*allocation);
      auto record = facet_map.find(facet_cache_key(desc.depth_attachment_ref));
      if (record != facet_map.end()) record->second.content_epoch = allocation->content_epoch;
    }
  }

  result->width = width;
  result->height = height;
  result->sample_count = desc.attachment.sample_count;
  result->covered_fragment_count = 0;
  result->stored = stored;
  // A DontCare load leaves the previous bytes visible and a DontCare store
  // leaves memory untouched; in both cases the contract does not define what a
  // reader sees, so the values returned must not be used as an expectation.
  result->contents_defined = stored && desc.attachment.load != AttachmentLoadAction::DontCare;
  result->facet_cache_hit = source_cache_hit && target_cache_hit && (!has_depth || depth_cache_hit);
  result->report.add("raster_attachment_store", hal::LoweringClass::Direct, vertex_count / 3, 0,
                     std::string("real MTLRenderPipelineState triangle-list draw into a render "
                                 "attachment; ") +
                         kRasterClipSpaceNote);
  result->report.add("raster_source_sample",
                     source_cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::DevicePass, 1,
                     0,
                     "SampleFacet read through a texture2d view of the requested array slice, level pinned "
                     "by the sampler's lod clamps");
  result->report.add(multisampled ? "raster_resolve" : "raster_store", hal::LoweringClass::Direct, 1, 0,
                     store_traffic_avoided ? "attachment samples never reached device memory"
                                           : "attachment contents written to device memory");
  if (has_depth)
    result->report.add("raster_depth_state",
                       depth_state_cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::DevicePass,
                       1, 0, "real MTLDepthStencilState bound with F4 depth compare/write policy");
  return true;
}

}  // namespace vg::metal
