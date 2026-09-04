#include "raster_fixture.h"

namespace vg::tests::reference {

void test_facet_token_oracles() {
  // --- The FacetRef overloads: 03 §10 makes a facet an index+generation
  // capability token, never a raw backend resource, so the reference backend
  // enforces the pool's kind and staleness rules before it will produce a
  // value. A token a GPU backend must reject cannot quietly produce a
  // reference image to compare against. ---
  {
    vg::core::Arena arena;
    const uint64_t source_id = arena.allocate(64).id;
    const uint64_t target_id = arena.allocate(64).id;
    const vg::core::CanonicalView source = plain_view(source_id, {.width = 4, .height = 4});
    const vg::core::CanonicalView target = plain_view(target_id, {.width = 4, .height = 4});

    auto* source_allocation = arena.lookup(vg::core::PointerRef{source_id, 1});
    for (uint32_t y = 0; y < 4; ++y)
      for (uint32_t x = 0; x < 4; ++x) write_texel(*source_allocation, source, 0, 0, x, y, texel_pattern(x, y));

    vg::core::FacetPool pool;
    std::string acquire_error;
    vg::core::FacetRef sample_ref, storage_ref, attachment_ref, address_ref;
    assert(pool.acquire(arena, source, vg::core::FacetKind::Sample, &sample_ref, &acquire_error));
    assert(pool.acquire(arena, source, vg::core::FacetKind::Storage, &storage_ref, &acquire_error));
    assert(pool.acquire(arena, source, vg::core::FacetKind::Address, &address_ref, &acquire_error));
    assert(pool.acquire(arena, target, vg::core::FacetKind::Attachment, &attachment_ref, &acquire_error));

    const std::vector<vg::reference::SampleCoord> centre{{0.125f, 0.125f, 0.0f, 0}};

    // Reached through the right kind of token, the oracle agrees with the
    // direct-view call exactly.
    const auto by_view = vg::reference::sample_facet(arena, source, vg::core::FilterMode::Nearest,
                                                    vg::core::WrapMode::Clamp, centre);
    const auto by_token = vg::reference::sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                                     vg::core::WrapMode::Clamp, centre);
    assert(by_view.ok && by_token.ok);
    assert(exact_match(by_view.sampled_rgba[0], by_token.sampled_rgba[0]));

    // An AddressFacet names how an existing representation is reached; it is
    // not a sampleable facet, so it is refused rather than reinterpreted.
    const auto wrong_kind = vg::reference::sample_facet(arena, pool, address_ref, vg::core::FilterMode::Nearest,
                                                       vg::core::WrapMode::Clamp, centre);
    assert(!wrong_kind.ok);
    assert(wrong_kind.message == "facet kind mismatch");
    assert(wrong_kind.sampled_rgba.empty());

    const auto wrong_storage = vg::reference::storage_facet(arena, pool, sample_ref,
                                                            vg::reference::StorageTexel{0, 0, 0, 0},
                                                            Rgba{1.0f, 1.0f, 1.0f, 1.0f});
    assert(!wrong_storage.ok && wrong_storage.message == "facet kind mismatch");

    const auto wrong_attachment =
        vg::reference::attachment_facet(arena, pool, sample_ref, vg::reference::AttachmentFacetDesc{});
    assert(!wrong_attachment.ok && wrong_attachment.message == "facet kind mismatch");

    // A StorageFacet token does write, and the sampler sees it.
    const auto stored = vg::reference::storage_facet(arena, pool, storage_ref,
                                                     vg::reference::StorageTexel{0, 0, 0, 0},
                                                     Rgba{1.0f, 0.0f, 0.0f, 1.0f});
    assert(stored.ok);
    const auto stored_readback = vg::reference::sample_facet(
        arena, pool, sample_ref, vg::core::FilterMode::Nearest, vg::core::WrapMode::Clamp, centre);
    assert(stored_readback.ok);
    assert(exact_match(stored_readback.sampled_rgba[0], Rgba{1.0f, 0.0f, 0.0f, 1.0f}));

    // raster_triangles enforces both kinds independently, and says which one
    // was wrong (05 §14: the diagnostic names the VG concept that failed).
    vg::reference::RasterDesc desc;
    desc.filter = vg::core::FilterMode::Nearest;
    desc.attachment.load = vg::reference::AttachmentLoadAction::Clear;
    desc.attachment.store = vg::reference::AttachmentStoreAction::Store;
    const auto quad = full_target_quad();

    const auto bad_source = vg::reference::raster_triangles(arena, pool, {.source = attachment_ref, .target = attachment_ref}, desc, quad);
    assert(!bad_source.ok);
    assert(bad_source.message == "raster source: facet kind mismatch");
    const auto bad_target = vg::reference::raster_triangles(arena, pool, {.source = sample_ref, .target = sample_ref}, desc, quad);
    assert(!bad_target.ok);
    assert(bad_target.message == "raster target: facet kind mismatch");

    const auto via_tokens = vg::reference::raster_triangles(arena, pool, {.source = sample_ref, .target = attachment_ref}, desc, quad);
    assert(via_tokens.ok);
    assert(via_tokens.covered_fragment_count == 16);

    // A representation transform stales every token acquired against the old
    // epoch (02 §10), and none of the four entry points will honour one.
    uint32_t new_epoch = 0;
    assert(arena.transform(source_id, 1, &new_epoch) && new_epoch == 1);
    const auto stale_sample = vg::reference::sample_facet(arena, pool, sample_ref, vg::core::FilterMode::Nearest,
                                                         vg::core::WrapMode::Clamp, centre);
    assert(!stale_sample.ok);
    assert(stale_sample.message == vg::core::to_string(vg::core::FacetStatus::EpochStale));
    const auto stale_storage = vg::reference::storage_facet(arena, pool, storage_ref,
                                                            vg::reference::StorageTexel{0, 0, 0, 0},
                                                            Rgba{0.0f, 1.0f, 0.0f, 1.0f});
    assert(!stale_storage.ok);
    assert(stale_storage.message == vg::core::to_string(vg::core::FacetStatus::EpochStale));
    const auto stale_raster = vg::reference::raster_triangles(arena, pool, {.source = sample_ref, .target = attachment_ref}, desc, quad);
    assert(!stale_raster.ok);
    assert(stale_raster.message ==
           std::string("raster source: ") + vg::core::to_string(vg::core::FacetStatus::EpochStale));

    // The target token, whose own allocation did not transform, is still live:
    // staleness is per-allocation, not per-pool.
    assert(pool.lookup(arena, attachment_ref) != nullptr);
    const auto still_live =
        vg::reference::attachment_facet(arena, pool, attachment_ref, vg::reference::AttachmentFacetDesc{});
    assert(still_live.ok);

    // A retired token is refused with the retirement reason, not the epoch one.
    std::string retire_error;
    assert(pool.retire(attachment_ref, &retire_error));
    const auto retired =
        vg::reference::attachment_facet(arena, pool, attachment_ref, vg::reference::AttachmentFacetDesc{});
    assert(!retired.ok);
    assert(retired.message == vg::core::to_string(vg::core::FacetStatus::Retired));
  }
}

}  // namespace vg::tests::reference
