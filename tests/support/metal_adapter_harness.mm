#include "metal_adapter_harness.h"
#include "backends/metal/metal_device_internal.h"
#include "compiler/compute_package.h"
#include "compiler/compute_task_ring.h"
#include "compiler/shader_sources.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>

namespace vg::metal {
AdapterHarness::AdapterHarness(DeviceHal& device) : device_(device), impl_(device.impl_.get()) {}
void AdapterHarness::reclaim_released_backing(const core::Arena& arena) const {
  impl_->reclaim_released_backing(arena, device_.facet_pool(), nullptr, nullptr);
}

bool AdapterHarness::run_cull_compact(const std::vector<uint32_t>& instance_visible,
                                 const std::vector<uint32_t>& instance_ids, CullCompactResult* result,
                                 std::string* error) const {
  if (result == nullptr) { if (error) *error = "cull/compact result output is required"; return false; }
  if (instance_visible.size() != instance_ids.size()) {
    if (error) *error = "instance_visible and instance_ids must be the same size";
    return false;
  }
  const auto count = static_cast<uint32_t>(instance_visible.size());
  std::string pipeline_error;
  if (!impl_->ensure_cull_compact_pipeline(&pipeline_error)) {
    if (error) *error = "Metal cull/compact pipeline compile failed: " + pipeline_error;
    return false;
  }

  id<MTLBuffer> visible_buffer = [impl_->device newBufferWithLength:std::max<size_t>(count * sizeof(uint32_t), 1)
                                                             options:MTLResourceStorageModeShared];
  id<MTLBuffer> ids_buffer = [impl_->device newBufferWithLength:std::max<size_t>(count * sizeof(uint32_t), 1)
                                                        options:MTLResourceStorageModeShared];
  id<MTLBuffer> count_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t)
                                                           options:MTLResourceStorageModeShared];
  id<MTLBuffer> compact_buffer = [impl_->device newBufferWithLength:std::max<size_t>(count * sizeof(uint32_t), 1)
                                                             options:MTLResourceStorageModeShared];
  if (visible_buffer == nil || ids_buffer == nil || count_buffer == nil || compact_buffer == nil) {
    if (error) *error = "Metal cull/compact buffer allocation failed";
    return false;
  }
  if (!instance_visible.empty())
    std::memcpy([visible_buffer contents], instance_visible.data(), instance_visible.size() * sizeof(uint32_t));
  if (!instance_ids.empty())
    std::memcpy([ids_buffer contents], instance_ids.data(), instance_ids.size() * sizeof(uint32_t));
  std::memset([count_buffer contents], 0, sizeof(uint32_t));

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  [encoder setComputePipelineState:impl_->cull_compact_pipeline];
  [encoder setBuffer:visible_buffer offset:0 atIndex:0];
  [encoder setBuffer:ids_buffer offset:0 atIndex:1];
  [encoder setBuffer:count_buffer offset:0 atIndex:2];
  [encoder setBuffer:compact_buffer offset:0 atIndex:3];
  [encoder setBytes:&count length:sizeof(count) atIndex:4];
  NSUInteger max_tpg = [impl_->cull_compact_pipeline maxTotalThreadsPerThreadgroup];
  uint32_t tpg = 256;
  if (max_tpg > 0 && max_tpg < tpg) tpg = static_cast<uint32_t>(max_tpg);
  if (tpg == 0) tpg = 1;
  const uint32_t groups = count == 0 ? 1u : (count + tpg - 1u) / tpg;
  [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(tpg, 1, 1)];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal cull/compact dispatch failed";
    return false;
  }

  const uint32_t visible_count = *static_cast<const uint32_t*>([count_buffer contents]);
  const auto* compact = static_cast<const uint32_t*>([compact_buffer contents]);
  result->visible_count = visible_count;
  result->compact_ids.assign(compact, compact + std::min(visible_count, count));
  return true;
}


bool AdapterHarness::run_address_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                  AddressFacetResult* result, std::string* error) const {
  if (result == nullptr) { if (error) *error = "address facet result output is required"; return false; }
  FacetUseGuard use(pool, ref);
  if (!use.begin(arena, error)) return false;
  id<MTLBuffer> buffer = impl_->ensure_facet_buffer(arena, pool, ref, core::FacetKind::Address, error);
  if (buffer == nil) return false;

  result->report = make_facet_report();
  result->byte_size = [buffer length];
  result->gpu_address_available = [buffer respondsToSelector:@selector(gpuAddress)];
  result->gpu_address = result->gpu_address_available ? [buffer gpuAddress] : 0;
  result->report.add("address_facet", result->gpu_address_available ? hal::LoweringClass::Direct
                                                                    : hal::LoweringClass::CachedObject,
                     1, result->byte_size,
                     result->gpu_address_available
                         ? "linear device address; no texture object created"
                         : "device exposes no gpuAddress selector, buffer binding only");
  return true;
}

bool AdapterHarness::run_sample_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                 core::FilterMode filter, core::WrapMode wrap,
                                 const std::vector<std::array<float, 2>>& uv_coords,
                                 SampleFacetResult* result, std::string* error) const {
  std::vector<SampleCoord> coords;
  coords.reserve(uv_coords.size());
  for (const auto& uv : uv_coords) coords.push_back(SampleCoord{uv[0], uv[1], 0.0f, 0});
  // FastNative, not CheckedNative: this overload names no subresource and no
  // profile, so it means exactly what it meant before the guard existed --
  // level 0 of slice 0, sampled by a pipeline with the guard specialized away,
  // with FacetPool::lookup() as the authority on the token's liveness.
  return run_sample_facet(arena, pool, ref, filter, wrap, coords, core::ValidationProfile::FastNative,
                          result, error);
}

bool AdapterHarness::run_sample_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                 core::FilterMode filter, core::WrapMode wrap,
                                 const std::vector<SampleCoord>& coords, core::ValidationProfile profile,
                                 SampleFacetResult* result, std::string* error) const {
  if (result == nullptr) { if (error) *error = "sample facet result output is required"; return false; }
  // 03 §12's profiles change diagnosis and instrumentation, never meaning --
  // but only two of the four have a Metal meaning at all. ReferenceStrict asks
  // for the reference interpreter's own byte-exact judgement and Capture for a
  // canonical capture stream; this adapter is neither, and silently running one
  // of them as CheckedNative would let a caller believe a guarantee it never
  // got (START.md §4 invariant 10).
  if (profile != core::ValidationProfile::CheckedNative &&
      profile != core::ValidationProfile::FastNative) {
    if (error)
      *error = "Unsupported: this Metal adapter implements the CheckedNative and FastNative profiles; "
               "ReferenceStrict and Capture have no Metal lowering and must run on the reference backend";
    return false;
  }
  const bool checked = profile == core::ValidationProfile::CheckedNative;
  const auto count = static_cast<uint32_t>(coords.size());

  // The generation table is a snapshot of what the pool currently resolves
  // (core::FacetPool::snapshot_generations), which is exactly what the kernel's
  // guard compares against. Taken before anything else so the host-side verdict
  // below and the in-shader verdict are formed from the same state.
  std::vector<uint32_t> generations;
  pool.snapshot_generations(&generations);

  std::string lookup_error;
  const core::FacetSlot* slot = impl_->resolve_facet(arena, pool, ref, core::FacetKind::Sample, &lookup_error);

  // A token the host cannot resolve, whose failure the uploaded table *can*
  // encode (a retired or generation-mismatched slot), is precisely the case
  // 06 §6.4's in-shader check exists for: under the checked profile the shader
  // itself rejects it, writes poison and counts the violation, rather than the
  // host inferring the rejection on its behalf. Nothing of the dead facet is
  // resurrected to make that happen -- a 1x1 placeholder is bound purely to
  // satisfy MSL's texture argument, and the guard returns before any sample.
  //
  // A token whose failure the table cannot encode (an epoch that went stale
  // after the snapshot, or a lost allocation) is refused host-side with that
  // reason, because pretending the guard caught it would misreport which check
  // actually fired.
  const bool guard_rejects = checked && slot == nullptr && !pool.generation_valid(ref);
  if (slot == nullptr && !guard_rejects) {
    if (error)
      *error = checked ? lookup_error +
                             " (the checked-profile generation table encodes slot liveness only, so this "
                             "staleness is caught host-side rather than in the shader)"
                       : lookup_error;
    return false;
  }

  const bool array_dimension = slot != nullptr && slot->view.dimension == core::ViewDimension::Texture2DArray;
  if (slot != nullptr) {
    for (uint32_t i = 0; i < count; ++i) {
      // Neither an out-of-range slice nor an out-of-range level is clamped:
      // clamping would turn a caller's indexing bug into a plausible-looking
      // sampled value, and the reference oracle refuses the same coordinates
      // for the same reason.
      if (coords[i].array_slice >= slot->view.array_layers) {
        if (error)
          *error = "sample coordinate " + std::to_string(i) + " names array slice " +
                   std::to_string(coords[i].array_slice) + " of a canonical view declaring " +
                   std::to_string(slot->view.array_layers) + " layer(s)";
        return false;
      }
      // Mixing the two kernels is Unsupported rather than approximated: a
      // Texture2D view has no slice axis, so a non-zero slice on one is a
      // contract error, not something the texture2d_array kernel should be
      // substituted in to satisfy.
      if (!array_dimension && coords[i].array_slice != 0) {
        if (error)
          *error = "Unsupported: sample coordinate " + std::to_string(i) +
                   " names a non-zero array slice on a Texture2D canonical view; declare the view as "
                   "Texture2DArray instead of relying on the array sampling kernel";
        return false;
      }
      if (!(coords[i].lod >= 0.0f) || coords[i].lod > static_cast<float>(slot->view.mip_levels - 1)) {
        if (error)
          *error = "sample coordinate " + std::to_string(i) + " names lod " +
                   std::to_string(coords[i].lod) + " of a canonical view declaring " +
                   std::to_string(slot->view.mip_levels) + " mip level(s)";
        return false;
      }
    }
  }

  std::string pipeline_error;
  id<MTLComputePipelineState> pipeline_state = nil;
  if (!impl_->ensure_sample_facet_pipeline(array_dimension, checked, &pipeline_state, &pipeline_error)) {
    if (error) *error = "Metal sample facet pipeline compile failed: " + pipeline_error;
    return false;
  }

  bool cache_hit = false;
  id<MTLTexture> texture = nil;
  // The GPU-use bracket only exists for a token that resolves; there is nothing
  // to hold out of the free list for a slot the pool has already retired.
  FacetUseGuard use(pool, ref);
  if (slot != nullptr) {
    if (!use.begin(arena, error)) return false;
    std::string tex_error;
    texture = impl_->ensure_facet_texture(arena, pool, ref, core::FacetKind::Sample, &cache_hit, nullptr,
                                          &tex_error);
    if (texture == nil) {
      if (error) *error = tex_error.empty() ? "Metal sample facet texture creation failed" : tex_error;
      return false;
    }
  } else {
    texture = impl_->ensure_guard_placeholder_texture(error);
    if (texture == nil) return false;
  }
  id<MTLSamplerState> sampler = impl_->ensure_sampler_state(
      filter, wrap, {0.0f, std::numeric_limits<float>::max()});
  if (sampler == nil) { if (error) *error = "Metal sample facet sampler creation failed"; return false; }

  // The kernels take one `constant float& lod` per dispatch, not one per
  // thread, so a batch carrying several distinct levels is genuinely several
  // dispatches. They are grouped by exact lod value and scattered back into
  // the caller's coordinate order afterwards, which keeps every coordinate's
  // own level rather than picking one level for the batch (an approximation
  // START.md §4 invariant 10 forbids). All groups share one encoder and one
  // command buffer.
  std::vector<std::pair<float, std::vector<uint32_t>>> lod_groups;
  for (uint32_t i = 0; i < count; ++i) {
    const float lod = coords[i].lod;
    auto group = std::ranges::find_if(lod_groups,
                              [lod](const std::pair<float, std::vector<uint32_t>>& entry) {
                                return entry.first == lod;
                              });
    if (group == lod_groups.end()) {
      lod_groups.push_back({lod, {i}});
    } else {
      group->second.push_back(i);
    }
  }

  id<MTLBuffer> token_buffer = nil;
  id<MTLBuffer> table_buffer = nil;
  id<MTLBuffer> slot_count_buffer = nil;
  id<MTLBuffer> violation_buffer = nil;
  if (checked) {
    const uint32_t token[2] = {ref.index, ref.generation};
    const auto slot_count = static_cast<uint32_t>(generations.size());
    token_buffer = [impl_->device newBufferWithLength:sizeof(token) options:MTLResourceStorageModeShared];
    table_buffer =
        [impl_->device newBufferWithLength:std::max<size_t>(generations.size() * sizeof(uint32_t), 1)
                                   options:MTLResourceStorageModeShared];
    slot_count_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t)
                                                  options:MTLResourceStorageModeShared];
    violation_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t)
                                                 options:MTLResourceStorageModeShared];
    if (token_buffer == nil || table_buffer == nil || slot_count_buffer == nil || violation_buffer == nil) {
      if (error) *error = "Metal checked-profile facet guard buffer allocation failed";
      return false;
    }
    std::memcpy([token_buffer contents], token, sizeof(token));
    if (!generations.empty())
      std::memcpy([table_buffer contents], generations.data(), generations.size() * sizeof(uint32_t));
    std::memcpy([slot_count_buffer contents], &slot_count, sizeof(slot_count));
    std::memset([violation_buffer contents], 0, sizeof(uint32_t));
  }

  struct SampleGroupBuffers {
    id<MTLBuffer> uv = nil;
    id<MTLBuffer> slices = nil;
    id<MTLBuffer> lod = nil;
    id<MTLBuffer> output = nil;
    const std::vector<uint32_t>* indices = nullptr;
  };
  std::vector<SampleGroupBuffers> group_buffers;
  group_buffers.reserve(lod_groups.size());

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  [encoder setComputePipelineState:pipeline_state];
  [encoder setTexture:texture atIndex:compiler::kSampleFacetTextureIndex];
  [encoder setSamplerState:sampler atIndex:compiler::kSampleFacetSamplerIndex];
  uint32_t descriptor_writes = 2;  // setTexture + setSamplerState
  if (checked) {
    [encoder setBuffer:token_buffer offset:0 atIndex:compiler::kSampleFacetTokenBufferIndex];
    [encoder setBuffer:table_buffer offset:0 atIndex:compiler::kSampleFacetGenerationTableBufferIndex];
    [encoder setBuffer:slot_count_buffer offset:0 atIndex:compiler::kSampleFacetSlotCountBufferIndex];
    [encoder setBuffer:violation_buffer offset:0 atIndex:compiler::kSampleFacetViolationCounterBufferIndex];
    descriptor_writes += 4;
  }

  for (const auto& group : lod_groups) {
    const std::vector<uint32_t>& indices = group.second;
    SampleGroupBuffers buffers;
    buffers.indices = &indices;
    buffers.uv = [impl_->device newBufferWithLength:indices.size() * sizeof(float) * 2
                                            options:MTLResourceStorageModeShared];
    buffers.output = [impl_->device newBufferWithLength:indices.size() * sizeof(float) * 4
                                                options:MTLResourceStorageModeShared];
    buffers.lod = [impl_->device newBufferWithLength:sizeof(float) options:MTLResourceStorageModeShared];
    if (array_dimension)
      buffers.slices = [impl_->device newBufferWithLength:indices.size() * sizeof(uint32_t)
                                                 options:MTLResourceStorageModeShared];
    if (buffers.uv == nil || buffers.output == nil || buffers.lod == nil ||
        (array_dimension && buffers.slices == nil)) {
      if (error) *error = "Metal sample facet buffer allocation failed";
      return false;
    }
    auto* uv = static_cast<float*>([buffers.uv contents]);
    uint32_t* slices = array_dimension ? static_cast<uint32_t*>([buffers.slices contents]) : nullptr;
    for (size_t i = 0; i < indices.size(); ++i) {
      uv[i * 2 + 0] = coords[indices[i]].u;
      uv[i * 2 + 1] = coords[indices[i]].v;
      if (slices != nullptr) slices[i] = coords[indices[i]].array_slice;
    }
    const float lod = group.first;
    std::memcpy([buffers.lod contents], &lod, sizeof(lod));

    [encoder setBuffer:buffers.uv offset:0 atIndex:compiler::kSampleFacetUvBufferIndex];
    [encoder setBuffer:buffers.output offset:0 atIndex:compiler::kSampleFacetOutputBufferIndex];
    [encoder setBuffer:buffers.lod offset:0 atIndex:compiler::kSampleFacetLodBufferIndex];
    if (array_dimension)
      [encoder setBuffer:buffers.slices offset:0 atIndex:compiler::kSampleFacetArraySliceBufferIndex];
    descriptor_writes += array_dimension ? 4 : 3;
    [encoder dispatchThreadgroups:MTLSizeMake(indices.size(), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    group_buffers.push_back(buffers);
  }
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal sample facet dispatch failed";
    return false;
  }

  result->sampled_rgba.assign(count, {0.0f, 0.0f, 0.0f, 0.0f});
  for (const auto& buffers : group_buffers) {
    const auto* output = static_cast<const float*>([buffers.output contents]);
    for (size_t i = 0; i < buffers.indices->size(); ++i) {
      const uint32_t destination = (*buffers.indices)[i];
      result->sampled_rgba[destination] = {output[i * 4 + 0], output[i * 4 + 1], output[i * 4 + 2],
                                           output[i * 4 + 3]};
    }
  }
  result->generation_violations =
      checked ? *static_cast<const uint32_t*>([violation_buffer contents]) : 0;
  result->checked_profile = checked;
  result->facet_cache_hit = cache_hit;
  result->descriptor_write_count = descriptor_writes;
  result->report = make_facet_report();
  result->report.encoder_count = 1;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  if (guard_rejects) {
    result->report.add("facet_generation_guard", hal::LoweringClass::DevicePass, count, 0,
                       "the checked-profile shader rejected the facet token itself; every output slot is "
                       "poison and no sample was taken");
  } else {
    result->report.add("sample_facet",
                       cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::DevicePass, count,
                       0,
                       cache_hit ? "facet cache hit; no MTLTexture created for this use"
                                 : "facet cache miss; MTLTexture and sampler compiled for this view");
    result->report.add("facet_generation_guard", hal::LoweringClass::Direct, count, 0,
                       checked ? "in-shader generation check compiled in (06 §6.4)"
                               : "fast-native: the guard, its four bindings and its atomic are specialized "
                                 "away entirely");
  }
  if (lod_groups.size() > 1)
    result->report.add("sample_facet_lod_groups", hal::LoweringClass::Direct, lod_groups.size(), 0,
                       "one dispatch per distinct explicit level; the kernel's lod is per-dispatch state");
  return true;
}

bool AdapterHarness::run_storage_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                  StorageFacetTarget target, const std::array<float, 4>& write_rgba,
                                  StorageFacetResult* result, std::string* error) const {
  // Texel (0,0) of subresource (layer 0, level 0) is what every pre-mip caller
  // meant, so the defaulted StorageTexel is exactly the old behaviour.
  return run_storage_facet(arena, pool, ref, target, write_rgba, StorageTexel{}, result, error);
}

bool AdapterHarness::run_storage_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                  StorageFacetTarget target, const std::array<float, 4>& write_rgba,
                                  StorageTexel texel, StorageFacetResult* result,
                                  std::string* error) const {
  if (result == nullptr) { if (error) *error = "storage facet result output is required"; return false; }
  FacetUseGuard use(pool, ref);
  if (!use.begin(arena, error)) return false;
  const core::FacetSlot* slot = impl_->resolve_facet(arena, pool, ref, core::FacetKind::Storage, error);
  if (slot == nullptr) return false;
  const core::CanonicalView& view = slot->view;
  // Out-of-range coordinates are refused, never clamped: a clamped write lands
  // somewhere real and looks like it worked.
  if (texel.layer >= view.array_layers || texel.level >= view.mip_levels ||
      texel.x >= view.mip_width(texel.level) || texel.y >= view.mip_height(texel.level)) {
    if (error)
      *error = "storage facet target texel (" + std::to_string(texel.x) + ", " + std::to_string(texel.y) +
               ") of layer " + std::to_string(texel.layer) + " level " + std::to_string(texel.level) +
               " lies outside the subresources this canonical view declares";
    return false;
  }
  const uint64_t texel_byte_offset = view.subresource_byte_offset({texel.layer, texel.level}) +
                                     static_cast<uint64_t>(texel.y) * view.bytes_per_row(texel.level) +
                                     static_cast<uint64_t>(texel.x) * kBytesPerTexel;
  std::string pipeline_error;
  if (!impl_->ensure_storage_facet_pipelines(&pipeline_error)) {
    if (error) *error = "Metal storage facet pipeline compile failed: " + pipeline_error;
    return false;
  }

  id<MTLBuffer> rgba_buffer = [impl_->device newBufferWithLength:sizeof(float) * 4
                                                          options:MTLResourceStorageModeShared];
  if (rgba_buffer == nil) {
    if (error) *error = "Metal storage facet constant buffer allocation failed";
    return false;
  }
  std::memcpy([rgba_buffer contents], write_rgba.data(), sizeof(float) * 4);

  bool cache_hit = false;
  id<MTLTexture> texture = nil;
  id<MTLBuffer> linear = nil;
  id<MTLBuffer> format_buffer = nil;
  id<MTLBuffer> target_buffer = nil;
  id<MTLBuffer> texel_index_buffer = nil;
  const bool array_dimension = view.dimension == core::ViewDimension::Texture2DArray;
  if (target == StorageFacetTarget::Texture) {
    std::string tex_error;
    texture = impl_->ensure_facet_texture(arena, pool, ref, core::FacetKind::Storage, &cache_hit, nullptr,
                                          &tex_error);
    if (texture == nil) {
      if (error) *error = tex_error.empty() ? "Metal storage facet texture creation failed" : tex_error;
      return false;
    }
    target_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t) * 4
                                                options:MTLResourceStorageModeShared];
    if (target_buffer == nil) {
      if (error) *error = "Metal storage facet target buffer allocation failed";
      return false;
    }
    const uint32_t words[4] = {texel.x, texel.y, texel.layer, texel.level};
    std::memcpy([target_buffer contents], words, sizeof(words));
  } else {
    linear = impl_->ensure_facet_buffer(arena, pool, ref, core::FacetKind::Storage, error);
    if (linear == nil) return false;
    if ([linear length] < texel_byte_offset + kBytesPerTexel) {
      if (error) *error = "storage facet linear backing is smaller than the addressed subresource";
      return false;
    }
    // The write lands in the view's own format, so the buffer path never
    // trades away precision the caller asked for (06 §6.2).
    format_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t)
                                                options:MTLResourceStorageModeShared];
    texel_index_buffer = [impl_->device newBufferWithLength:sizeof(uint32_t)
                                                    options:MTLResourceStorageModeShared];
    if (format_buffer == nil || texel_index_buffer == nil) {
      if (error) *error = "Metal storage facet format buffer allocation failed";
      return false;
    }
    const auto format_code = static_cast<uint32_t>(view.format);
    std::memcpy([format_buffer contents], &format_code, sizeof(format_code));
    // The kernel indexes in texels, not bytes; core::bytes_per_texel is 4 for
    // both formats this milestone models, which is what makes that exact.
    const auto texel_index = static_cast<uint32_t>(texel_byte_offset / kBytesPerTexel);
    std::memcpy([texel_index_buffer contents], &texel_index, sizeof(texel_index));
  }

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  if (target == StorageFacetTarget::Texture) {
    [encoder setComputePipelineState:array_dimension ? impl_->storage_array_facet_pipeline
                                                     : impl_->storage_facet_pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:rgba_buffer offset:0 atIndex:0];
    [encoder setBuffer:target_buffer offset:0 atIndex:1];
    result->descriptor_write_count = 3;  // setTexture + rgba + target subresource
  } else {
    [encoder setComputePipelineState:impl_->storage_buffer_facet_pipeline];
    [encoder setBuffer:linear offset:0 atIndex:0];
    [encoder setBuffer:rgba_buffer offset:0 atIndex:1];
    [encoder setBuffer:format_buffer offset:0 atIndex:2];
    [encoder setBuffer:texel_index_buffer offset:0 atIndex:3];
    result->descriptor_write_count = 4;  // four buffer bindings, no texture
  }
  [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal storage facet dispatch failed";
    return false;
  }

  if (target == StorageFacetTarget::Texture) {
    if (!impl_->read_texel(texture, texel.layer, texel.level, texel.x, texel.y, &result->written_rgba,
                           error))
      return false;
  } else if (view.format == core::PixelFormat::RGBA8Unorm) {
    const uint8_t* bytes = static_cast<const uint8_t*>([linear contents]) + texel_byte_offset;
    result->written_rgba = {bytes[0] / 255.0f, bytes[1] / 255.0f, bytes[2] / 255.0f, bytes[3] / 255.0f};
  } else {
    float value{};
    std::memcpy(&value, static_cast<const uint8_t*>([linear contents]) + texel_byte_offset, sizeof(value));
    result->written_rgba = {value, 0.0f, 0.0f, 0.0f};
  }
  result->target = target;
  result->facet_cache_hit = cache_hit;
  result->encoder_count = 1;
  result->report = make_facet_report();
  result->report.encoder_count = 1;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  if (target == StorageFacetTarget::Texture) {
    result->report.add("storage_facet_texture",
                       cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::DevicePass, 1, 0,
                       "writable MTLTexture representation");
  } else {
    result->report.add("storage_facet_linear_buffer", hal::LoweringClass::Direct, 1, [linear length],
                       "linear MTLBuffer representation; view format preserved on write");
  }
  return true;
}

bool AdapterHarness::run_attachment_facet(const core::Arena& arena, core::FacetPool& pool, core::FacetRef ref,
                                     const AttachmentFacetDesc& desc, AttachmentFacetResult* result,
                                     std::string* error) const {
  if (result == nullptr) { if (error) *error = "attachment facet result output is required"; return false; }
  const bool multisampled = desc.sample_count > 1;
  if (multisampled != (desc.store == AttachmentStoreAction::MultisampleResolve)) {
    if (error)
      *error = "attachment facet: MultisampleResolve and sample_count > 1 must be requested together";
    return false;
  }
  if (multisampled && desc.load == AttachmentLoadAction::Load) {
    if (error) *error = "Unsupported: a transient multisample attachment has no prior contents to load";
    return false;
  }
  FacetUseGuard use(pool, ref);
  if (!use.begin(arena, error)) return false;
  const core::FacetSlot* slot = impl_->resolve_facet(arena, pool, ref, core::FacetKind::Attachment, error);
  if (slot == nullptr) return false;
  if (!subresource_in_range(slot->view, desc.subresource, error)) return false;
  bool cache_hit = false;
  std::string tex_error;
  id<MTLTexture> texture = impl_->ensure_facet_texture(arena, pool, ref, core::FacetKind::Attachment,
                                                       &cache_hit, nullptr, &tex_error);
  if (texture == nil) {
    if (error) *error = tex_error.empty() ? "Metal attachment facet texture creation failed" : tex_error;
    return false;
  }

  // No draw and no fragment shader: this exercises the load/store/resolve
  // lowering itself, which is where 06 §6.3's "not public object state"
  // constraint actually lives. run_raster_triangles() below is the same
  // lowering with a draw in it, built from the same helper.
  bool store_traffic_avoided = false;
  MTLRenderPassDescriptor* rp = impl_->make_render_pass(texture, desc, slot->view,
                                                        &store_traffic_avoided, error);
  if (rp == nil) return false;

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:rp];
  if (encoder == nil) { if (error) *error = "failed to create Metal render encoder"; return false; }
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal attachment facet render pass failed";
    return false;
  }

  if (!impl_->read_texel(texture, desc.subresource.layer, desc.subresource.level, 0, 0,
                         &result->resolved_rgba, error))
    return false;
  result->facet_cache_hit = cache_hit;
  result->encoder_count = 1;
  result->sample_count = desc.sample_count;
  result->store_traffic_avoided = store_traffic_avoided;
  result->report = make_facet_report();
  result->report.encoder_count = 1;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  result->report.add(multisampled ? "attachment_facet_resolve" : "attachment_facet_store",
                     cache_hit ? hal::LoweringClass::CachedObject : hal::LoweringClass::DevicePass, 1, 0,
                     store_traffic_avoided ? "attachment samples never reached device memory"
                                           : "attachment contents written to device memory");
  return true;
}

bool AdapterHarness::run_representation_transform(core::Arena& arena, core::FacetPool& pool,
                                             const core::CanonicalView& view, core::FacetKind target_kind,
                                             RepresentationTransformResult* result,
                                             std::string* error) const {
  if (result == nullptr) {
    if (error) *error = "representation transform result output is required";
    return false;
  }
  const core::Allocation* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
  if (allocation == nullptr) {
    if (error) *error = "representation transform: backing allocation not found in arena";
    return false;
  }
  if (!view_expressible(view, target_kind, allocation->bytes.size(), error)) return false;
  const uint64_t old_bytes = allocation->bytes.size();

  // Real transform pass (02 §8: a transform is not a barrier). Publishing the
  // new epoch first invalidates every facet minted against the old one;
  // retire_stale is what actually returns their slots, and only for slots with
  // no work still in flight.
  uint32_t new_epoch = 0;
  if (!arena.transform(view.allocation, view.allocation_generation, &new_epoch, error)) return false;
  const auto retired = static_cast<uint32_t>(pool.retire_stale(arena));

  core::FacetRef out_facet{};
  if (!pool.acquire(arena, view, target_kind, &out_facet, error)) return false;
  // Same helper submit()'s Stage 5 physical callback uses, so the standalone
  // entry point and the ExecutionPlan path cannot drift on which subresources
  // get copied or on what the transform costs.
  Impl::TransformCost cost;
  if (!impl_->transform_into_private_facet(arena, pool, view, target_kind, out_facet, &cost, error))
    return false;

  result->new_epoch = new_epoch;
  result->old_backing_bytes = old_bytes;
  result->new_backing_bytes = cost.new_backing_bytes;
  result->temporary_bytes = cost.temporary_bytes;
  result->encoder_count = cost.encoder_count;
  result->used_private_optimal = true;
  result->retired_facet_count = retired;
  result->out_facet = out_facet;
  result->report = make_facet_report();
  result->report.encoder_count = cost.encoder_count;
  result->report.command_buffer_count = 1;
  result->report.queue_wait_count = 1;
  result->report.add("representation_transform", hal::LoweringClass::DevicePass, view.subresource_count(),
                     cost.new_backing_bytes,
                     "blit every subresource from the TransferFacet's linear buffer into a Private "
                     "optimal texture");
  result->report.add("representation_transform_peak", hal::LoweringClass::Direct, 1,
                     old_bytes + cost.new_backing_bytes,
                     "old linear backing retained alongside the new texture; no staging copy. This entry "
                     "point never consumes it: 02 §4.2 makes ConsumeInput a proven exclusive consume, and "
                     "06 §11 forbids the adapter inferring a destructive transform on its own, so the "
                     "watermark is only reduced through ExecutionPlan::representation_requests");
  result->report.add("facet_retire_stale", hal::LoweringClass::Direct, retired, 0,
                     "facets invalidated by the new RepresentationEpoch");
  const uint32_t retired_textures = impl_->retire_stale_facet_textures(arena, pool);
  if (retired_textures != 0) {
    result->report.add("facet_texture_retire", hal::LoweringClass::Direct, retired_textures, 0,
                       "MTLTextures belonging to retired facet slots destroyed after the transform");
  }
  return true;
}

bool AdapterHarness::run_raster_triangles(core::Arena& arena, core::FacetPool& pool,
                                     core::RasterFacetPair facets,
                                     const RasterDesc& desc, const std::vector<RasterVertex>& vertices,
                                     RasterResult* result, std::string* error) const {
  for (const RasterVertex& vertex : vertices) {
    if (!std::isfinite(vertex.z) || vertex.z < 0.0f || vertex.z > 1.0f) {
      if (error) *error = "raster vertex z must be finite and normalized to [0,1]";
      return false;
    }
  }
  // Step 3 only ("moved, not rewritten" -- ADR-043 Decision #3, F2): building
  // the host-supplied vertex/tint buffers is the one piece of
  // run_raster_triangles() that a plan-driven raster TaskRecord cannot share,
  // since SubmitOps::raster resolves its vertex buffer straight off a facet
  // instead of a host std::vector<RasterVertex>. Everything else (facet
  // acquisition/validation, pipeline, draw, readback) now lives in
  // Impl::run_raster_pass(), unchanged.
  id<MTLBuffer> vertex_buffer =
      [impl_->device newBufferWithLength:vertices.size() * sizeof(RasterVertex)
                                 options:MTLResourceStorageModeShared];
  id<MTLBuffer> tint_buffer = [impl_->device newBufferWithLength:sizeof(float) * 4
                                                         options:MTLResourceStorageModeShared];
  if (vertex_buffer == nil || tint_buffer == nil) {
    if (error) *error = "Metal raster buffer allocation failed";
    return false;
  }
  // F4 RasterVertex is {x,y,z,u,v}; the MSL side declares
  // `packed_float3 position; packed_float2 uv`, the same 20-byte layout. Do not use
  // float3 here: its Metal alignment would silently change the public stride.
  std::memcpy([vertex_buffer contents], vertices.data(), vertices.size() * sizeof(RasterVertex));
  std::memcpy([tint_buffer contents], desc.tint.data(), sizeof(float) * 4);

  id<MTLBuffer> scene_root_buffer = impl_->make_identity_scene_root_buffer();
  if (scene_root_buffer == nil) {
    if (error) *error = "Metal SceneRoot buffer allocation failed";
    return false;
  }
  return impl_->run_raster_pass(arena, pool, facets, desc, vertex_buffer, scene_root_buffer, tint_buffer,
                                static_cast<uint32_t>(vertices.size()), nil, MTLIndexTypeUInt16, 0,
                                result, error);
}

bool AdapterHarness::run_pipeline_classification(PipelineClassificationRun* result, std::string* error) const {
  if (result == nullptr) {
    if (error) *error = "pipeline classification result output is required";
    return false;
  }
  *result = PipelineClassificationRun{};

  // E013's three axes, one per 07 §9 fate that is allowed to exist, plus the
  // two raster parameters E013 names by hand. Every axis carries two values so
  // the matrix is exactly the 2x2x2 the experiment asks for and the expected
  // counts are arithmetic rather than a judgement call.
  //
  //  - checked profile: a function constant. 06 §6.4 compiles the generation
  //    guard in or specializes it away, so it genuinely cannot be anything but
  //    PipelineKey state.
  //  - threadgroup width / viewport: set on the encoder. 06 §7's "小的动态状态
  //    不应无故扩大 key" is precisely about this one.
  //  - sample lod / tint: bytes the shader reads from a binding. Changing them
  //    changes the image, not the program.
  static constexpr std::array<uint64_t, 2> kCheckedValues{0, 1};
  static constexpr std::array<uint64_t, 2> kDynamicValues{32, 64};
  static constexpr std::array<uint64_t, 2> kShaderValues{0, 1};
  static constexpr std::array<core::PixelFormat, 2> kFormats{core::PixelFormat::RGBA8Unorm,
                                                             core::PixelFormat::R32Float};
  static constexpr std::array<uint32_t, 2> kSampleCounts{1, 4};

  const std::string compute_source = compiler::sample_facet_metal_source();
  const std::string raster_source = compiler::raster_facet_metal_source();
  const std::string compute_entry = "vg_sample_facet";
  const std::string raster_fragment_entry = "vg_raster_fragment";
  const std::string raster_vertex_entry = "vg_raster_vertex";

  auto compute_constants = [](uint64_t checked) {
    std::vector<std::pair<std::string, uint64_t>> constants;
    // An unset constant is fast-native (is_function_constant_defined() is
    // false in the kernel), so the guard-off variant names nothing rather than
    // naming a false.
    if (checked != 0) constants.emplace_back("vg_checked_profile", 1);
    return constants;
  };

  // ---- Naive variant -------------------------------------------------------
  //
  // What a backend does when it has no state taxonomy: every permutation of
  // every piece of pipeline-adjacent state is assumed to need its own compiled
  // object, so it compiles one. Nothing here consults a cache, and the count is
  // of MTLComputePipelineState / MTLRenderPipelineState objects this device
  // really created -- not of permutations enumerated.
  std::vector<id<MTLComputePipelineState>> naive_compute;
  std::vector<id<MTLRenderPipelineState>> naive_render;
  const auto release_naive = [&]() {
    for (id<MTLComputePipelineState> pipeline : naive_compute) [pipeline release];
    for (id<MTLRenderPipelineState> pipeline : naive_render) [pipeline release];
  };
  uint64_t naive_ns = 0;
  for (uint64_t checked : kCheckedValues) {
    for (uint64_t dynamic_value : kDynamicValues) {
      for (uint64_t shader_value : kShaderValues) {
        (void)dynamic_value;
        (void)shader_value;
        const auto start = std::chrono::steady_clock::now();
        compiler::PipelineKey key =
            impl_->make_pipeline_key({compute_source, compute_entry}, compute_constants(checked), {}, 1);
        id<MTLLibrary> library_object =
            impl_->ensure_library({compute_source, key.code_object_hash}, error);
        if (library_object == nil) { release_naive(); return false; }
        id<MTLFunction> function = impl_->ensure_function(library_object, key, error);
        if (function == nil) { release_naive(); return false; }
        NSError* pipeline_error = nil;
        id<MTLComputePipelineState> pipeline =
            [impl_->device newComputePipelineStateWithFunction:function error:&pipeline_error];
        naive_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                .count());
        if (pipeline == nil) {
          if (error)
            *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                           : "naive compute pipeline creation failed";
          release_naive();
          return false;
        }
        naive_compute.push_back(pipeline);
      }
    }
  }
  for (core::PixelFormat format : kFormats) {
    for (uint32_t sample_count : kSampleCounts) {
      for (uint64_t dynamic_value : kDynamicValues) {
        for (uint64_t shader_value : kShaderValues) {
          (void)dynamic_value;
          (void)shader_value;
          const auto start = std::chrono::steady_clock::now();
          const compiler::PipelineKey key =
              impl_->make_pipeline_key({raster_source, raster_fragment_entry}, {},
                                       {static_cast<uint32_t>(format)}, sample_count);
          id<MTLLibrary> library_object =
              impl_->ensure_library({raster_source, key.code_object_hash}, error);
          if (library_object == nil) { release_naive(); return false; }
          compiler::PipelineKey vertex_key = key;
          vertex_key.entry = raster_vertex_entry;
          id<MTLFunction> vertex_function = impl_->ensure_function(library_object, vertex_key, error);
          id<MTLFunction> fragment_function = impl_->ensure_function(library_object, key, error);
          if (vertex_function == nil || fragment_function == nil) { release_naive(); return false; }
          MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
          descriptor.vertexFunction = vertex_function;
          descriptor.fragmentFunction = fragment_function;
          descriptor.colorAttachments[0].pixelFormat = to_mtl_pixel_format(format);
          descriptor.rasterSampleCount = sample_count;
          NSError* pipeline_error = nil;
          id<MTLRenderPipelineState> pipeline =
              [impl_->device newRenderPipelineStateWithDescriptor:descriptor error:&pipeline_error];
          naive_ns += static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                  .count());
          if (pipeline == nil) {
            if (error)
              *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                             : "naive render pipeline creation failed";
            release_naive();
            return false;
          }
          naive_render.push_back(pipeline);
        }
      }
    }
  }
  result->naive_pipeline_count =
      static_cast<uint32_t>(naive_compute.size() + naive_render.size());
  result->naive_compile_ns = naive_ns;
  release_naive();

  // ---- VG classified variant ----------------------------------------------
  //
  // The cache and the object maps are local to this run so the numbers below
  // describe this experiment only. Reusing impl_->pipeline_cache would fold in
  // every pipeline the rest of the session happened to compile first, and a
  // hit/miss ratio measured against unrelated history is not the ratio E013
  // asks about.
  compiler::PipelineClassificationCache cache;
  std::unordered_map<uint64_t, id<MTLComputePipelineState>> compute_objects;
  std::unordered_map<uint64_t, id<MTLRenderPipelineState>> render_objects;
  const auto release_classified = [&]() {
    for (auto& entry : compute_objects) [entry.second release];
    for (auto& entry : render_objects) [entry.second release];
  };

  for (uint64_t checked : kCheckedValues) {
    for (uint64_t dynamic_value : kDynamicValues) {
      for (uint64_t shader_value : kShaderValues) {
        const compiler::PipelineKey base =
            impl_->make_pipeline_key({compute_source, compute_entry}, compute_constants(checked), {}, 1);
        const std::vector<compiler::StateBlock> blocks{
            {"facet_generation_guard", compiler::StateBlockKind::PipelineKey, checked},
            {"threadgroup_width", compiler::StateBlockKind::DynamicState, dynamic_value},
            {"sample_lod", compiler::StateBlockKind::ShaderVisibleData, shader_value},
        };
        const compiler::PipelineClassification classification = classify_pipeline_state(base, blocks);
        if (!classification.ok) {
          if (error) *error = "pipeline classification rejected a supported compute permutation: " +
                              classification.message;
          release_classified();
          return false;
        }
        if (!impl_->acquire_compute_pipeline(cache, compute_objects, compute_source, classification.key,
                                             "E013 classified SampleFacet permutation", nullptr, nullptr,
                                             error)) {
          release_classified();
          return false;
        }
      }
    }
  }
  for (core::PixelFormat format : kFormats) {
    for (uint32_t sample_count : kSampleCounts) {
      for (uint64_t dynamic_value : kDynamicValues) {
        for (uint64_t shader_value : kShaderValues) {
          const compiler::PipelineKey base =
              impl_->make_pipeline_key({raster_source, raster_fragment_entry}, {},
                                       {static_cast<uint32_t>(format)}, sample_count);
          const std::vector<compiler::StateBlock> blocks{
              {"viewport", compiler::StateBlockKind::DynamicState, dynamic_value},
              {"tint", compiler::StateBlockKind::ShaderVisibleData, shader_value},
          };
          const compiler::PipelineClassification classification = classify_pipeline_state(base, blocks);
          if (!classification.ok) {
            if (error) *error = "pipeline classification rejected a supported raster permutation: " +
                                classification.message;
            release_classified();
            return false;
          }
          if (!impl_->acquire_render_pipeline(cache, render_objects, raster_source, classification.key,
                                              raster_vertex_entry, to_mtl_pixel_format(format),
                                              MTLPixelFormatInvalid,
                                              "E013 classified raster permutation", nullptr, nullptr,
                                              error)) {
            release_classified();
            // The two raster axes are the only key state that lives late in
            // PipelineKey::canonical(), so a digest that cannot separate them
            // surfaces here first. Say so, rather than letting the caller read
            // a bare "collision" and conclude the classification is wrong: the
            // classification is right, and the key it produced is distinct
            // text. Reporting the failure is the only honest option, since
            // serving one pipeline for two attachment formats would be a
            // silently wrong PSO (START.md invariant 10).
            if (error)
              *error += ". The two keys differ in attachment format or sample count, which 06 §7 makes "
                        "key state; a digest that cannot separate them cannot answer E013";
            return false;
          }
        }
      }
    }
  }

  // A state block this device cannot express must stop the classification, not
  // become another key bit (START.md invariant 10, 06 §6.2). The probe is a
  // real call with a real rejection, so "we reject it" is measured here rather
  // than asserted in a comment.
  const compiler::PipelineKey unsupported_base =
      impl_->make_pipeline_key({raster_source, raster_fragment_entry}, {},
                               {static_cast<uint32_t>(core::PixelFormat::RGBA8Unorm)}, 1);
  const compiler::PipelineClassification unsupported = classify_pipeline_state(
      unsupported_base,
      {{"viewport", compiler::StateBlockKind::DynamicState, kDynamicValues[0]},
       {"attachment_format_needs_conversion", compiler::StateBlockKind::UnsupportedNeedsConversion, 0}});
  if (unsupported.ok) {
    if (error)
      *error = "an UnsupportedNeedsConversion state block was folded into a pipeline key instead of "
               "being rejected";
    release_classified();
    return false;
  }
  result->unsupported_rejected = true;

  result->classified_pipeline_count = cache.pipeline_count();
  result->cache_hits = cache.cache_hits();
  result->cache_misses = cache.cache_misses();
  result->reports = cache.reports();
  uint64_t classified_ns = 0;
  for (const compiler::SpecializationReport& report : result->reports) classified_ns += report.compile_ns;
  result->classified_compile_ns = classified_ns;
  release_classified();

  if (result->classified_pipeline_count >= result->naive_pipeline_count) {
    if (error)
      *error = "classification produced " + std::to_string(result->classified_pipeline_count) +
               " pipelines against a naive " + std::to_string(result->naive_pipeline_count) +
               "; E013 only has an answer if keeping dynamic and shader-visible state out of the key "
               "actually removes permutations";
    return false;
  }

  result->report = make_facet_report();
  result->report.add("pipeline_key_state", hal::LoweringClass::Direct,
                     result->classified_pipeline_count, 0,
                     "function constants, attachment format and sample count are compiled in and are the "
                     "only state that reached the key (06 §7)");
  result->report.add("pipeline_dynamic_state", hal::LoweringClass::Direct,
                     static_cast<uint32_t>(kDynamicValues.size()), 0,
                     "threadgroup width and viewport are set on the encoder and compiled no pipeline");
  result->report.add("pipeline_shader_visible_data", hal::LoweringClass::Direct,
                     static_cast<uint32_t>(kShaderValues.size()), 0,
                     "sample lod and tint are bytes the shader reads and compiled no pipeline");
  result->report.add("pipeline_permutations_avoided", hal::LoweringClass::Direct,
                     result->naive_pipeline_count - result->classified_pipeline_count, 0,
                     "permutations a taxonomy-free backend would have compiled");
  result->report.add("pipeline_unsupported_rejected", hal::LoweringClass::Unsupported, 1, 0,
                     "a state block requiring conversion was reported, never folded into a key");
  return true;
}

bool AdapterHarness::run_task_tier1_indirect_test_harness(
    const ir::Module& module, core::Arena& arena, const std::vector<core::TaskRecord>& tasks,
    hal::Submission* submission, std::string* error) const {
  if (submission == nullptr || tasks.empty()) {
    if (error) *error = "Tier1 physical harness requires tasks and a submission output";
    return false;
  }
  *submission = {};
  submission->abi_version = hal::kDeviceHalAbiVersion;
  const auto package = compiler::build_linear_compute_package(module);
  if (!package.ok) {
    if (error) *error = "Tier1 physical harness package compilation failed: " + package.message;
    return false;
  }
  if (!impl_->ensure_pipeline({package.package.canonical_ir_hash, package.package.metal_source}, error))
    return false;

  std::vector<id<MTLBuffer>> buffers;
  buffers.reserve(package.package.bindings.size());
  for (const auto& binding : package.package.bindings) {
    const auto instruction = std::ranges::find_if(module.instructions, [&](const auto& candidate) {
      return candidate.allocation == binding.allocation;
    });
    if (instruction == module.instructions.end()) {
      if (error) *error = "Tier1 physical harness binding has no immutable IR allocation";
      return false;
    }
    const auto* allocation = arena.lookup(core::RepresentationRef{
        binding.allocation, instruction->generation, instruction->representation_epoch});
    if (allocation == nullptr) {
      if (error) *error = "Tier1 physical harness encountered stale allocation generation or epoch";
      return false;
    }
    id<MTLBuffer> buffer = impl_->ensure_buffer(*allocation);
    if (buffer == nil) {
      if (error) *error = "Tier1 physical harness could not allocate a Metal buffer";
      return false;
    }
    buffers.push_back(buffer);
  }

  const uint32_t count = static_cast<uint32_t>(tasks.size());
  id<MTLBuffer> state = [impl_->device newBufferWithLength:count * sizeof(uint32_t)
                                                options:MTLResourceStorageModeShared];
  id<MTLBuffer> fields = [impl_->device newBufferWithLength:count * compiler::kTaskRingWordsPerRecord * sizeof(uint32_t)
                                                 options:MTLResourceStorageModeShared];
  id<MTLBuffer> inputs = [impl_->device newBufferWithLength:count * compiler::kTaskRingWordsPerRecord * sizeof(uint32_t)
                                                 options:MTLResourceStorageModeShared];
  const size_t indirect_stride = sizeof(MTLDispatchThreadgroupsIndirectArguments);
  id<MTLBuffer> indirect = [impl_->device newBufferWithLength:count * indirect_stride
                                                   options:MTLResourceStorageModeShared];
  if (state == nil || fields == nil || inputs == nil || indirect == nil) {
    if (error) *error = "Tier1 physical harness buffer allocation failed";
    return false;
  }
  std::memset([state contents], 0, count * sizeof(uint32_t));
  auto* packed = static_cast<uint32_t*>([inputs contents]);
  for (uint32_t index = 0; index < count; ++index) {
    compiler::ComputeTaskRingRecord record;
    if (!compiler::make_compute_task_ring_record(tasks[index], &record, error) ||
        !compiler::pack_compute_task_ring_record(
            record,
            std::span<uint32_t>(packed + index * compiler::kTaskRingWordsPerRecord,
                                compiler::kTaskRingWordsPerRecord),
            error))
      return false;
  }
  if (!impl_->ensure_task_ring_pipeline(error)) return false;
  DispatchStats stats;
  if (!impl_->dispatch_task_publish({.state = state, .fields = fields, .inputs = inputs},
                                    count, &stats, error))
    return false;
  std::vector<uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0);
  if (!impl_->dispatch_task_tier1_indirect(buffers, fields, order, indirect, &stats, error)) return false;

  const auto* states = static_cast<const uint32_t*>([state contents]);
  const auto* published = static_cast<const uint32_t*>([fields contents]);
  submission->published_tasks.reserve(count);
  impl_->last_tier1_indirect_dims.clear();
  impl_->last_tier1_indirect_dims.reserve(count);
  const auto* args = static_cast<const uint8_t*>([indirect contents]);
  for (uint32_t index = 0; index < count; ++index) {
    if (states[index] != static_cast<uint32_t>(core::PublicationState::Published)) {
      if (error) *error = "Tier1 physical harness task slot was not published";
      return false;
    }
    compiler::ComputeTaskRingRecord record;
    if (!compiler::unpack_compute_task_ring_record(
            std::span<const uint32_t>(published + index * compiler::kTaskRingWordsPerRecord,
                                      compiler::kTaskRingWordsPerRecord),
            &record, error))
      return false;
    submission->published_tasks.push_back(compiler::make_task_record(record));
    std::array<uint32_t, 3> dims{};
    std::memcpy(dims.data(), args + index * indirect_stride, 3 * sizeof(uint32_t));
    impl_->last_tier1_indirect_dims.push_back(dims);
  }
  submission->result.ok = true;
  submission->result.poison = core::PoisonState::Valid;
  submission->cpu_encode_ns = stats.cpu_encode_ns;
  submission->cpu_submit_ns = stats.cpu_submit_ns;
  submission->report.encoder_count = stats.encoder_count;
  submission->report.command_buffer_count = stats.command_buffer_count;
  submission->report.queue_wait_count = stats.queue_wait_count;
  submission->report.add("task_publication", hal::LoweringClass::Direct, count, 0,
                         "narrow physical harness publication");
  submission->report.add("tier1_indirect_dispatch", hal::LoweringClass::Direct, count, 0,
                         "GPU-authored dispatch dimensions consumed without ExecutionPlan feature flags");
  return true;
}

bool AdapterHarness::run_indexed_compute_test_harness(const ir::Module& module, core::Arena& arena,
                                                 IndexedComputeHarnessResult* result,
                                                 hal::Submission* submission,
                                                 std::string* error) const {
  if (result == nullptr || submission == nullptr) {
    if (error) *error = "indexed physical harness requires result and submission outputs";
    return false;
  }
  *result = {};
  *submission = {};
  submission->abi_version = hal::kDeviceHalAbiVersion;
  if (!impl_->probe_gpu_addresses()) {
    if (error) *error = "MTLBuffer.gpuAddress is unavailable";
    return false;
  }
  const auto package = compiler::build_indexed_compute_package(module);
  if (!package.ok) {
    if (error) *error = "indexed physical harness package compilation failed: " + package.message;
    return false;
  }
  if (!impl_->ensure_pipeline({package.package.canonical_ir_hash, package.package.metal_source},
                              error, "vg_indexed_compute"))
    return false;

  std::vector<id<MTLBuffer>> buffers;
  std::map<uint64_t, core::Allocation*> allocations;
  for (uint64_t allocation_id : package.package.referenced_allocations) {
    const auto instruction = std::ranges::find_if(module.instructions, [allocation_id](const auto& candidate) {
      return candidate.allocation == allocation_id;
    });
    if (instruction == module.instructions.end()) {
      if (error) *error = "indexed physical harness allocation has no immutable IR instruction";
      return false;
    }
    auto* allocation = arena.lookup(core::RepresentationRef{
        allocation_id, instruction->generation, instruction->representation_epoch});
    if (allocation == nullptr) {
      if (error) *error = "indexed physical harness encountered stale allocation generation or epoch";
      return false;
    }
    id<MTLBuffer> buffer = impl_->ensure_buffer(*allocation);
    if (buffer == nil || ![buffer respondsToSelector:@selector(gpuAddress)] || [buffer gpuAddress] == 0) {
      if (error) *error = "MTLBuffer.gpuAddress is unavailable for an indexed object";
      return false;
    }
    buffers.push_back(buffer);
    allocations.emplace(allocation_id, allocation);
  }
  DispatchStats stats;
  if (!impl_->dispatch_indexed_and_wait(buffers, {}, &stats, error)) return false;
  for (const auto& effect : module.declared_effects) {
    if (effect.access == ir::Access::Read) continue;
    const auto index = std::ranges::find(package.package.referenced_allocations, effect.allocation);
    if (index != package.package.referenced_allocations.end()) {
      const size_t position = static_cast<size_t>(index - package.package.referenced_allocations.begin());
      impl_->commit_buffer_write(*allocations.at(effect.allocation), buffers[position]);
    }
  }
  submission->result.ok = true;
  submission->result.poison = core::PoisonState::Valid;
  submission->result.trace = module.declared_effects;
  for (uint32_t index = 0; index < module.declared_effects.size(); ++index)
    submission->result.witness.record(module.declared_effects[index], index);
  submission->cpu_encode_ns = stats.cpu_encode_ns;
  submission->cpu_submit_ns = stats.cpu_submit_ns;
  submission->report.encoder_count = stats.encoder_count;
  submission->report.command_buffer_count = stats.command_buffer_count;
  submission->report.queue_wait_count = stats.queue_wait_count;
  submission->report.add("compute_package", hal::LoweringClass::Direct, 1, 1,
                         "narrow indexed-address table physical harness");
  result->referenced_allocation_count =
      static_cast<uint32_t>(package.package.referenced_allocations.size());
  result->report = submission->report;
  return true;
}

const std::vector<std::array<uint32_t, 3>>& AdapterHarness::last_tier1_indirect_dims() const {
  return impl_->last_tier1_indirect_dims;
}


}  // namespace vg::metal
