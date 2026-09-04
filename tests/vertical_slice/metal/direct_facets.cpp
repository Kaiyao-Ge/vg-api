#include "fixture.h"
#include "metal_adapter_harness.h"

namespace vg::tests::metal {

std::vector<vg::reference::SampleCoord> to_reference_coords(
    const std::vector<vg::metal::SampleCoord>& coords) {
  std::vector<vg::reference::SampleCoord> out;
  out.reserve(coords.size());
  for (const auto& coord : coords)
    out.push_back({coord.u, coord.v, coord.lod, coord.array_slice});
  return out;
}

// Layer-1: CanonicalView + FacetPool -> Sample/Storage/Attachment + representation transform.
bool run_representation_layer(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "representation-layer: no Metal device available on this host\n";
    return false;
  }

  constexpr uint32_t kW = 2;
  constexpr uint32_t kH = 2;
  constexpr uint32_t kBytes = kW * kH * 4;

  vg::core::Arena arena;
  auto& allocation = arena.allocate(kBytes);
  // Distinct texels for sample oracle: (0,0)=R, (1,0)=G, (0,1)=B, (1,1)=A-ish white.
  allocation.bytes = {
      255, 0, 0, 255,
      0, 255, 0, 255,
      0, 0, 255, 255,
      255, 255, 255, 255,
  };

  vg::core::CanonicalView view;
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  view.format = vg::core::PixelFormat::RGBA8Unorm;
  view.dimension = vg::core::ViewDimension::Texture2D;
  view.width = kW;
  view.height = kH;

  vg::core::FacetPool pool;
  std::string error;
  // One 8-bit quantization step, the tightest tolerance an RGBA8Unorm round
  // trip can honestly hold.
  constexpr float kTol = 1.0f / 255.0f + 1e-4f;
  const auto channels_match = [](const std::array<float, 4>& got, const std::array<float, 4>& want,
                                 const char* what) {
    for (int c = 0; c < 4; ++c) {
      if (std::fabs(got[c] - want[c]) <= kTol) continue;
      std::cerr << "representation-layer: " << what << " channel " << c << " got " << got[c] << " expected "
                << want[c] << "\n";
      return false;
    }
    return true;
  };

  // --- AddressFacet: the same CanonicalView's linear/BDA view (02 §3.3) ---
  vg::core::FacetRef address_ref;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Address, &address_ref, &error)) {
    std::cerr << "representation-layer: address acquire failed: " << error << "\n";
    return false;
  }
  vg::metal::AddressFacetResult address_result;
  if (!vg::metal::AdapterHarness(*metal_device).run_address_facet(arena, pool, address_ref, &address_result, &error)) {
    std::cerr << "representation-layer: address facet failed: " << error << "\n";
    return false;
  }
  if (address_result.byte_size < kBytes) {
    std::cerr << "representation-layer: address facet must cover the whole view extent\n";
    return false;
  }

  // --- SampleFacet via FacetRef ---
  vg::core::FacetRef sample_ref;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &sample_ref, &error)) {
    std::cerr << "representation-layer: sample acquire failed: " << error << "\n";
    return false;
  }
  const std::vector<std::array<float, 2>> uvs = {{0.25f, 0.25f}};
  vg::metal::SampleFacetResult sample_result;
  if (!vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, uvs, &sample_result, &error)) {
    std::cerr << "representation-layer: sample failed: " << error << "\n";
    return false;
  }
  // The oracle resolves the same capability token, so it enforces the same
  // kind/staleness rules Metal does before producing a comparison value.
  auto oracle = vg::reference::sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                            vg::core::WrapMode::Clamp, uvs);
  if (!oracle.ok) {
    std::cerr << "representation-layer: sample oracle failed: " << oracle.message << "\n";
    return false;
  }
  if (!channels_match(sample_result.sampled_rgba[0], oracle.sampled_rgba[0], "sample")) return false;
  vg::metal::SampleFacetResult sample_second;
  if (!vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, uvs, &sample_second, &error)) {
    std::cerr << "representation-layer: sample cache reuse failed: " << error << "\n";
    return false;
  }
  if (!sample_second.facet_cache_hit) {
    std::cerr << "representation-layer: expected SampleFacet cache hit on second use\n";
    return false;
  }

  // --- Swizzle is part of the view contract, so it is a different facet and
  // both backends must apply it identically (06 §6.1) ---
  vg::core::CanonicalView swizzled_view = view;
  swizzled_view.swizzle = {vg::core::Swizzle::Blue, vg::core::Swizzle::Green, vg::core::Swizzle::Red,
                           vg::core::Swizzle::One};
  vg::core::FacetRef swizzled_ref;
  if (!pool.acquire(arena, swizzled_view, vg::core::FacetKind::Sample, &swizzled_ref, &error)) {
    std::cerr << "representation-layer: swizzled sample acquire failed: " << error << "\n";
    return false;
  }
  vg::metal::SampleFacetResult swizzled_result;
  if (!vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, swizzled_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, uvs, &swizzled_result, &error)) {
    std::cerr << "representation-layer: swizzled sample failed: " << error << "\n";
    return false;
  }
  auto swizzled_oracle = vg::reference::sample_facet(arena, pool, swizzled_ref, vg::core::FilterMode::Nearest,
                                                     vg::core::WrapMode::Clamp, uvs);
  if (!swizzled_oracle.ok) {
    std::cerr << "representation-layer: swizzled oracle failed: " << swizzled_oracle.message << "\n";
    return false;
  }
  if (!channels_match(swizzled_result.sampled_rgba[0], swizzled_oracle.sampled_rgba[0], "swizzled sample"))
    return false;
  const std::array<float, 4> unswizzled = sample_result.sampled_rgba[0];
  const std::array<float, 4> expected_swizzle = {unswizzled[2], unswizzled[1], unswizzled[0], 1.0f};
  if (!channels_match(swizzled_result.sampled_rgba[0], expected_swizzle, "swizzle mapping")) return false;

  // --- StorageFacet, both representations 06 §6.2 allows ---
  vg::core::FacetRef storage_ref;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Storage, &storage_ref, &error)) {
    std::cerr << "representation-layer: storage acquire failed: " << error << "\n";
    return false;
  }
  const std::array<float, 4> write_rgba = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};
  vg::metal::StorageFacetResult storage_result;
  if (!vg::metal::AdapterHarness(*metal_device).run_storage_facet(arena, pool, storage_ref, vg::metal::StorageFacetTarget::Texture,
                                       write_rgba, &storage_result, &error)) {
    std::cerr << "representation-layer: storage texture write failed: " << error << "\n";
    return false;
  }
  if (!channels_match(storage_result.written_rgba, write_rgba, "storage texture writeback")) return false;

  vg::metal::StorageFacetResult storage_buffer_result;
  if (!vg::metal::AdapterHarness(*metal_device).run_storage_facet(arena, pool, storage_ref, vg::metal::StorageFacetTarget::LinearBuffer,
                                       write_rgba, &storage_buffer_result, &error)) {
    std::cerr << "representation-layer: storage buffer write failed: " << error << "\n";
    return false;
  }
  if (!channels_match(storage_buffer_result.written_rgba, write_rgba, "storage buffer writeback"))
    return false;
  if (storage_buffer_result.target != vg::metal::StorageFacetTarget::LinearBuffer) {
    std::cerr << "representation-layer: linear-buffer storage must report the target it actually used\n";
    return false;
  }
  // A non-identity swizzle has no meaning for an image write, and must be
  // refused rather than silently dropped.
  vg::core::FacetRef swizzled_storage_ref;
  if (!pool.acquire(arena, swizzled_view, vg::core::FacetKind::Storage, &swizzled_storage_ref, &error)) {
    std::cerr << "representation-layer: swizzled storage acquire failed: " << error << "\n";
    return false;
  }
  vg::metal::StorageFacetResult refused;
  if (vg::metal::AdapterHarness(*metal_device).run_storage_facet(arena, pool, swizzled_storage_ref,
                                      vg::metal::StorageFacetTarget::Texture, write_rgba, &refused, &error)) {
    std::cerr << "representation-layer: a swizzled StorageFacet must be reported Unsupported\n";
    return false;
  }

  // --- AttachmentFacet: clear+store, then load+store preserving contents ---
  vg::core::FacetRef attachment_ref;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Attachment, &attachment_ref, &error)) {
    std::cerr << "representation-layer: attachment acquire failed: " << error << "\n";
    return false;
  }
  const std::array<float, 4> clear_rgba = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f};
  vg::metal::AttachmentFacetDesc clear_desc;
  clear_desc.load = vg::metal::AttachmentLoadAction::Clear;
  clear_desc.store = vg::metal::AttachmentStoreAction::Store;
  clear_desc.clear_rgba = clear_rgba;
  vg::metal::AttachmentFacetResult attachment_result;
  if (!vg::metal::AdapterHarness(*metal_device).run_attachment_facet(arena, pool, attachment_ref, clear_desc, &attachment_result,
                                          &error)) {
    std::cerr << "representation-layer: attachment clear failed: " << error << "\n";
    return false;
  }
  if (!channels_match(attachment_result.resolved_rgba, clear_rgba, "attachment clear")) return false;
  if (attachment_result.store_traffic_avoided) {
    std::cerr << "representation-layer: a stored attachment must not claim avoided external traffic\n";
    return false;
  }

  vg::metal::AttachmentFacetDesc load_desc;
  load_desc.load = vg::metal::AttachmentLoadAction::Load;
  load_desc.store = vg::metal::AttachmentStoreAction::Store;
  vg::metal::AttachmentFacetResult loaded;
  if (!vg::metal::AdapterHarness(*metal_device).run_attachment_facet(arena, pool, attachment_ref, load_desc, &loaded, &error)) {
    std::cerr << "representation-layer: attachment load failed: " << error << "\n";
    return false;
  }
  if (!channels_match(loaded.resolved_rgba, clear_rgba, "attachment load preserves contents")) return false;

  // Resolve: render 4x multisampled and resolve into the facet's texture. With
  // no draw, every sample is the clear color, so the resolve must reproduce it.
  const std::array<float, 4> resolve_rgba = {32.0f / 255.0f, 96.0f / 255.0f, 160.0f / 255.0f, 1.0f};
  vg::metal::AttachmentFacetDesc resolve_desc;
  resolve_desc.load = vg::metal::AttachmentLoadAction::Clear;
  resolve_desc.store = vg::metal::AttachmentStoreAction::MultisampleResolve;
  resolve_desc.clear_rgba = resolve_rgba;
  resolve_desc.sample_count = 4;
  vg::metal::AttachmentFacetResult resolved;
  if (!vg::metal::AdapterHarness(*metal_device).run_attachment_facet(arena, pool, attachment_ref, resolve_desc, &resolved, &error)) {
    std::cerr << "representation-layer: attachment resolve failed: " << error << "\n";
    return false;
  }
  if (!channels_match(resolved.resolved_rgba, resolve_rgba, "attachment resolve")) return false;
  if (resolved.sample_count != 4) {
    std::cerr << "representation-layer: resolve must report the sample count it rendered at\n";
    return false;
  }
  // Mismatched load/store combinations are refused, not quietly reinterpreted.
  vg::metal::AttachmentFacetDesc contradictory = resolve_desc;
  contradictory.sample_count = 1;
  vg::metal::AttachmentFacetResult unused;
  if (vg::metal::AdapterHarness(*metal_device).run_attachment_facet(arena, pool, attachment_ref, contradictory, &unused, &error)) {
    std::cerr << "representation-layer: MultisampleResolve at sample_count 1 must be rejected\n";
    return false;
  }

  // --- Representation transform, once per target kind ---
  const vg::core::FacetKind target_kinds[] = {vg::core::FacetKind::Sample, vg::core::FacetKind::Storage,
                                              vg::core::FacetKind::Attachment};
  uint32_t last_epoch = 0;
  for (vg::core::FacetKind target_kind : target_kinds) {
    vg::metal::RepresentationTransformResult transform_result;
    if (!vg::metal::AdapterHarness(*metal_device).run_representation_transform(arena, pool, view, target_kind, &transform_result,
                                                    &error)) {
      std::cerr << "representation-layer: transform failed: " << error << "\n";
      return false;
    }
    if (transform_result.new_epoch <= last_epoch || !transform_result.used_private_optimal ||
        transform_result.encoder_count == 0) {
      std::cerr << "representation-layer: transform must advance the epoch and blit into Private storage\n";
      return false;
    }
    if (transform_result.retired_facet_count == 0) {
      std::cerr << "representation-layer: the new epoch must retire the facets it invalidated\n";
      return false;
    }
    last_epoch = transform_result.new_epoch;

    // Every facet minted against the previous epoch is now a stale token, and
    // the backend must refuse it rather than resolve its last-known texture.
    if (pool.lookup(arena, sample_ref) != nullptr || pool.lookup(arena, storage_ref) != nullptr ||
        pool.lookup(arena, attachment_ref) != nullptr || pool.lookup(arena, address_ref) != nullptr) {
      std::cerr << "representation-layer: pre-transform FacetRefs must be stale after transform\n";
      return false;
    }
    if (vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                       vg::core::WrapMode::Clamp, uvs, &sample_result, &error)) {
      std::cerr << "representation-layer: Metal must refuse a stale FacetRef\n";
      return false;
    }
    const vg::core::FacetSlot* out_slot = pool.lookup(arena, transform_result.out_facet);
    if (out_slot == nullptr || out_slot->kind != target_kind) {
      std::cerr << "representation-layer: transform must yield a live facet of the requested kind\n";
      return false;
    }

    // Use the transformed facet on its own kind's path: the Private optimal
    // texture has to be usable without falling back to a Shared re-upload.
    if (target_kind == vg::core::FacetKind::Sample) {
      vg::metal::SampleFacetResult post;
      if (!vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, transform_result.out_facet,
                                          vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp, uvs,
                                          &post, &error)) {
        std::cerr << "representation-layer: sample of transformed facet failed: " << error << "\n";
        return false;
      }
      if (!channels_match(post.sampled_rgba[0], oracle.sampled_rgba[0], "transformed sample")) return false;
    } else if (target_kind == vg::core::FacetKind::Storage) {
      vg::metal::StorageFacetResult post;
      if (!vg::metal::AdapterHarness(*metal_device).run_storage_facet(arena, pool, transform_result.out_facet,
                                           vg::metal::StorageFacetTarget::Texture, write_rgba, &post,
                                           &error)) {
        std::cerr << "representation-layer: storage write to transformed facet failed: " << error << "\n";
        return false;
      }
      if (!channels_match(post.written_rgba, write_rgba, "transformed storage writeback")) return false;
    } else {
      vg::metal::AttachmentFacetResult post;
      if (!vg::metal::AdapterHarness(*metal_device).run_attachment_facet(arena, pool, transform_result.out_facet, clear_desc, &post,
                                              &error)) {
        std::cerr << "representation-layer: attachment clear on transformed facet failed: " << error << "\n";
        return false;
      }
      if (!channels_match(post.resolved_rgba, clear_rgba, "transformed attachment clear")) return false;
    }

    // Re-mint the layer-1 refs against the epoch just published, so the next
    // iteration has live tokens to invalidate.
    if (!pool.acquire(arena, view, vg::core::FacetKind::Address, &address_ref, &error) ||
        !pool.acquire(arena, view, vg::core::FacetKind::Sample, &sample_ref, &error) ||
        !pool.acquire(arena, view, vg::core::FacetKind::Storage, &storage_ref, &error) ||
        !pool.acquire(arena, view, vg::core::FacetKind::Attachment, &attachment_ref, &error)) {
      std::cerr << "representation-layer: re-acquire after transform failed: " << error << "\n";
      return false;
    }
  }

  std::cout << "representation-layer: ok (address+sample+swizzle+storage+attachment+transform epoch="
            << last_epoch << ")\n";
  return true;
}

// E008: Texture2DArray + mip, sampled through metal::SampleCoord, compared
// against reference::sample_facet. A second call against the same FacetRef
// must report facet_cache_hit. Out-of-range slice/lod is a rejection, not a
// clamp. The three Phase C capability bits this adapter now advertises are
// asserted here so a device that dropped one cannot hide behind a green sample.
bool run_sample_facet(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "sample-facet: no Metal device available on this host\n";
    return false;
  }
  const auto& caps = metal_device->capabilities();
  if (!caps.supports(vg::hal::Capability::Raster) ||
      !caps.supports(vg::hal::Capability::RepresentationTransform) ||
      !caps.supports(vg::hal::Capability::CheckedFacetGeneration)) {
    std::cerr << "sample-facet: device must advertise Raster, RepresentationTransform and "
                 "CheckedFacetGeneration\n";
    return false;
  }

  vg::core::CanonicalView view;
  view.format = vg::core::PixelFormat::RGBA8Unorm;
  view.dimension = vg::core::ViewDimension::Texture2DArray;
  view.width = 2;
  view.height = 2;
  view.array_layers = 2;
  view.mip_levels = 2;
  std::string view_error;
  if (!view.valid(&view_error)) {
    std::cerr << "sample-facet: CanonicalView rejected: " << view_error << "\n";
    return false;
  }

  vg::core::Arena arena;
  auto& allocation = arena.allocate(view.byte_size());
  view.allocation = allocation.id;
  view.allocation_generation = allocation.generation;
  // Distinct solid colour per (layer, level), packed through the view's own
  // linear layout so Metal's upload and the CPU oracle read the same bytes.
  const std::array<std::array<uint8_t, 4>, 4> colours = {{
      {255, 0, 0, 255},
      {0, 255, 0, 255},
      {0, 0, 255, 255},
      {255, 255, 255, 255},
  }};
  fill_subresource(allocation, view, 0, 0, colours[0]);
  fill_subresource(allocation, view, 0, 1, colours[1]);
  fill_subresource(allocation, view, 1, 0, colours[2]);
  fill_subresource(allocation, view, 1, 1, colours[3]);

  vg::core::FacetPool pool;
  vg::core::FacetRef sample_ref;
  std::string error;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &sample_ref, &error)) {
    std::cerr << "sample-facet: acquire failed: " << error << "\n";
    return false;
  }

  const std::vector<vg::metal::SampleCoord> coords = {
      {0.5f, 0.5f, 0.0f, 0},
      {0.5f, 0.5f, 1.0f, 0},
      {0.5f, 0.5f, 0.0f, 1},
      {0.5f, 0.5f, 1.0f, 1},
  };
  vg::metal::SampleFacetResult result;
  if (!vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, coords,
                                      vg::core::ValidationProfile::FastNative, &result, &error)) {
    std::cerr << "sample-facet: Metal sample failed: " << error << "\n";
    return false;
  }
  auto oracle = vg::reference::sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                            vg::core::WrapMode::Clamp, to_reference_coords(coords));
  if (!oracle.ok) {
    std::cerr << "sample-facet: reference oracle failed: " << oracle.message << "\n";
    return false;
  }
  if (result.sampled_rgba.size() != oracle.sampled_rgba.size()) {
    std::cerr << "sample-facet: sampled_rgba size mismatch\n";
    return false;
  }
  for (size_t i = 0; i < coords.size(); ++i) {
    if (!channels_close(result.sampled_rgba[i], oracle.sampled_rgba[i], kNearestTol, "sample-facet",
                        "coord sample"))
      return false;
  }
  // lod 0 vs lod 1, and slice 0 vs slice 1, must actually select different
  // subresources -- a level-0-only or slice-0-only path would pass the oracle
  // comparison if both sides made the same mistake.
  const auto same_colour = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
    for (int c = 0; c < 4; ++c)
      if (std::fabs(a[c] - b[c]) > kNearestTol) return false;
    return true;
  };
  if (same_colour(result.sampled_rgba[0], result.sampled_rgba[1])) {
    std::cerr << "sample-facet: lod 0 and lod 1 produced the same colour\n";
    return false;
  }
  if (same_colour(result.sampled_rgba[0], result.sampled_rgba[2])) {
    std::cerr << "sample-facet: slice 0 and slice 1 produced the same colour\n";
    return false;
  }

  vg::metal::SampleFacetResult second;
  if (!vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, coords,
                                      vg::core::ValidationProfile::FastNative, &second, &error)) {
    std::cerr << "sample-facet: second sample failed: " << error << "\n";
    return false;
  }
  if (!second.facet_cache_hit) {
    std::cerr << "sample-facet: expected facet_cache_hit on second use\n";
    return false;
  }

  vg::metal::SampleFacetResult unused;
  const std::vector<vg::metal::SampleCoord> bad_slice{{0.5f, 0.5f, 0.0f, 2}};
  if (vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                     vg::core::WrapMode::Clamp, bad_slice,
                                     vg::core::ValidationProfile::FastNative, &unused, &error)) {
    std::cerr << "sample-facet: out-of-range slice must be rejected\n";
    return false;
  }
  const std::vector<vg::metal::SampleCoord> bad_lod{{0.5f, 0.5f, 2.0f, 0}};
  if (vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                     vg::core::WrapMode::Clamp, bad_lod,
                                     vg::core::ValidationProfile::FastNative, &unused, &error)) {
    std::cerr << "sample-facet: out-of-range lod must be rejected\n";
    return false;
  }

  std::cout << "sample-facet: ok\n";
  return true;
}

// 06 §6.4: live SampleFacet + CheckedNative writes no poison; a retired or
// forged generation is rejected in-shader (call succeeds, violations>0,
// channels == kFacetGenerationPoisonValue). FastNative still fails that token
// host-side. After Arena::transform the old FacetRef is EpochStale, which the
// generation table cannot encode, so CheckedNative is refused host-side.
bool run_checked_facet_generation(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "checked-facet-generation: no Metal device available on this host\n";
    return false;
  }

  constexpr uint32_t kW = 2;
  constexpr uint32_t kH = 2;
  vg::core::Arena arena;
  auto& allocation = arena.allocate(static_cast<uint64_t>(kW) * kH * 4);
  allocation.bytes = {
      255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
  };
  const vg::core::CanonicalView view = make_rgba8_view(allocation, {.width = kW, .height = kH});

  vg::core::FacetPool pool;
  vg::core::FacetRef live_ref;
  std::string error;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &live_ref, &error)) {
    std::cerr << "checked-facet-generation: acquire failed: " << error << "\n";
    return false;
  }

  const std::vector<vg::metal::SampleCoord> coords{{0.25f, 0.25f, 0.0f, 0}};
  vg::metal::SampleFacetResult live;
  if (!vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, live_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, coords,
                                      vg::core::ValidationProfile::CheckedNative, &live, &error)) {
    std::cerr << "checked-facet-generation: live CheckedNative sample failed: " << error << "\n";
    return false;
  }
  if (!live.checked_profile || live.generation_violations != 0) {
    std::cerr << "checked-facet-generation: live token must run checked with zero violations\n";
    return false;
  }

  const vg::core::FacetRef retired_ref = live_ref;
  if (!pool.retire(retired_ref, &error)) {
    std::cerr << "checked-facet-generation: retire failed: " << error << "\n";
    return false;
  }
  vg::metal::SampleFacetResult retired;
  if (!vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, retired_ref, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, coords,
                                      vg::core::ValidationProfile::CheckedNative, &retired, &error)) {
    std::cerr << "checked-facet-generation: retired CheckedNative must succeed (shader poison): "
              << error << "\n";
    return false;
  }
  if (!retired.checked_profile || retired.generation_violations == 0) {
    std::cerr << "checked-facet-generation: retired token must report generation_violations>0\n";
    return false;
  }
  for (int c = 0; c < 4; ++c) {
    if (retired.sampled_rgba[0][c] != vg::compiler::kFacetGenerationPoisonValue) {
      std::cerr << "checked-facet-generation: retired channel " << c
                << " is not kFacetGenerationPoisonValue\n";
      return false;
    }
  }

  vg::core::FacetRef forged = retired_ref;
  forged.generation = retired_ref.generation + 99;
  vg::metal::SampleFacetResult forged_result;
  if (!vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, forged, vg::core::FilterMode::Nearest,
                                      vg::core::WrapMode::Clamp, coords,
                                      vg::core::ValidationProfile::CheckedNative, &forged_result,
                                      &error)) {
    std::cerr << "checked-facet-generation: forged CheckedNative must succeed (shader poison): "
              << error << "\n";
    return false;
  }
  if (!forged_result.checked_profile || forged_result.generation_violations == 0) {
    std::cerr << "checked-facet-generation: forged token must report generation_violations>0\n";
    return false;
  }
  for (int c = 0; c < 4; ++c) {
    if (forged_result.sampled_rgba[0][c] != vg::compiler::kFacetGenerationPoisonValue) {
      std::cerr << "checked-facet-generation: forged channel " << c
                << " is not kFacetGenerationPoisonValue\n";
      return false;
    }
  }

  vg::metal::SampleFacetResult fast_dead;
  if (vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, retired_ref, vg::core::FilterMode::Nearest,
                                     vg::core::WrapMode::Clamp, coords,
                                     vg::core::ValidationProfile::FastNative, &fast_dead, &error)) {
    std::cerr << "checked-facet-generation: FastNative must refuse a dead token host-side\n";
    return false;
  }

  vg::core::FacetRef epoch_ref;
  if (!pool.acquire(arena, view, vg::core::FacetKind::Sample, &epoch_ref, &error)) {
    std::cerr << "checked-facet-generation: re-acquire failed: " << error << "\n";
    return false;
  }
  uint32_t new_epoch = 0;
  if (!arena.transform(allocation.id, allocation.generation, &new_epoch, &error)) {
    std::cerr << "checked-facet-generation: Arena::transform failed: " << error << "\n";
    return false;
  }
  vg::core::FacetStatus status = vg::core::FacetStatus::Ok;
  if (pool.lookup(arena, epoch_ref, &status) != nullptr || status != vg::core::FacetStatus::EpochStale) {
    std::cerr << "checked-facet-generation: old FacetRef must be EpochStale after Arena::transform\n";
    return false;
  }
  vg::metal::SampleFacetResult stale;
  if (vg::metal::AdapterHarness(*metal_device).run_sample_facet(arena, pool, epoch_ref, vg::core::FilterMode::Nearest,
                                     vg::core::WrapMode::Clamp, coords,
                                     vg::core::ValidationProfile::CheckedNative, &stale, &error)) {
    std::cerr << "checked-facet-generation: CheckedNative must refuse EpochStale host-side "
                 "(generation table cannot encode epoch staleness)\n";
    return false;
  }

  std::cout << "checked-facet-generation: ok\n";
  return true;
}

}  // namespace vg::tests::metal
