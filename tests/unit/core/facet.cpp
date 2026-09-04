#include "cases.h"
#include <cassert>

namespace vg::tests::core {

void test_facet_pool() {
  // --- TASK-C1: FacetPool acquire/lookup/retire, and the "facet generation
  // vs epoch = stale token" check (02-principles-and-semantics.md Sec.10). ---
  {
    vg::core::Arena facet_arena;
    auto& backing = facet_arena.allocate(64);

    vg::core::CanonicalView view;
    view.allocation = backing.id;
    view.allocation_generation = backing.generation;
    view.format = vg::core::PixelFormat::RGBA8Unorm;
    view.dimension = vg::core::ViewDimension::Texture2D;
    view.width = 4; view.height = 4;

    vg::core::FacetPool pool;
    vg::core::FacetRef ref;
    std::string facet_error;
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Sample, &ref, &facet_error));
    const auto* slot = pool.lookup(facet_arena, ref);
    assert(slot != nullptr && slot->kind == vg::core::FacetKind::Sample);
    assert(slot->view.allocation == backing.id);

    // A CanonicalView over an allocation the arena doesn't hold is rejected.
    vg::core::CanonicalView bogus_view = view;
    bogus_view.allocation = 999;
    vg::core::FacetRef bogus_ref;
    assert(!pool.acquire(facet_arena, bogus_view, vg::core::FacetKind::Sample, &bogus_ref, &facet_error));

    // Advancing the backing allocation's representation_epoch stales the
    // facet ref -- lookup() must not return the slot's last-known contents.
    uint32_t new_epoch = 0;
    assert(facet_arena.transform(backing.id, backing.generation, &new_epoch));
    assert(pool.lookup(facet_arena, ref) == nullptr);

    // Re-acquiring against the now-current epoch succeeds and yields a
    // fresh, independently valid ref.
    vg::core::FacetRef reacquired;
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Sample, &reacquired, &facet_error));
    assert(pool.lookup(facet_arena, reacquired) != nullptr);

    // retire() frees the index for reuse with a bumped generation; the old
    // ref is rejected afterward, and a re-acquire recycles the same index.
    assert(pool.retire(reacquired, &facet_error));
    assert(pool.lookup(facet_arena, reacquired) == nullptr);
    assert(!pool.retire(reacquired, &facet_error));
    vg::core::FacetRef recycled;
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Storage, &recycled, &facet_error));
    assert(recycled.index == reacquired.index && recycled.generation != reacquired.generation);

    // Sample/Storage/Attachment are distinct kinds from the same CanonicalView
    // (02 §3.3: per-usage facets, not one maximal ViewRecord).
    vg::core::FacetRef sample_ref, storage_ref, attachment_ref;
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Sample, &sample_ref, &facet_error));
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Storage, &storage_ref, &facet_error));
    assert(pool.acquire(facet_arena, view, vg::core::FacetKind::Attachment, &attachment_ref, &facet_error));
    assert(pool.lookup(facet_arena, sample_ref)->kind == vg::core::FacetKind::Sample);
    assert(pool.lookup(facet_arena, storage_ref)->kind == vg::core::FacetKind::Storage);
    assert(pool.lookup(facet_arena, attachment_ref)->kind == vg::core::FacetKind::Attachment);
    uint32_t transform_epoch = 0;
    assert(facet_arena.transform(backing.id, backing.generation, &transform_epoch));
    assert(pool.lookup(facet_arena, sample_ref) == nullptr);
    assert(pool.lookup(facet_arena, storage_ref) == nullptr);
    assert(pool.lookup(facet_arena, attachment_ref) == nullptr);
    assert(pool.retire_stale(facet_arena) >= 3);
    assert(pool.lookup(facet_arena, sample_ref) == nullptr);

    // Failures say which rule rejected the token, so a caller can distinguish
    // a forged ref from one that merely outlived its epoch.
    vg::core::FacetStatus status = vg::core::FacetStatus::Ok;
    assert(pool.lookup(facet_arena, sample_ref, &status) == nullptr);
    assert(status == vg::core::FacetStatus::Retired);
    vg::core::FacetRef out_of_range{9999, 1};
    assert(pool.lookup(facet_arena, out_of_range, &status) == nullptr);
    assert(status == vg::core::FacetStatus::UnknownIndex);
  }

  // --- A slot referenced by in-flight GPU work is not reusable, even once the
  // token itself is dead (06-backend-macos-metal.md Sec.6.4, Sec.11). ---
  {
    vg::core::Arena arena;
    auto& backing = arena.allocate(64);
    vg::core::CanonicalView view;
    view.allocation = backing.id;
    view.allocation_generation = backing.generation;
    view.width = 4;
    view.height = 4;

    vg::core::FacetPool pool;
    vg::core::FacetRef ref;
    std::string error;
    assert(pool.acquire(arena, view, vg::core::FacetKind::Sample, &ref, &error));
    assert(pool.begin_gpu_use(arena, ref, &error));
    assert(pool.in_flight(ref) == 1);

    // Retiring while in flight kills the token immediately...
    assert(pool.retire(ref, &error));
    assert(pool.lookup(arena, ref) == nullptr);
    // ...but must not hand the index to an unrelated facet underneath the GPU.
    vg::core::FacetRef during_flight;
    assert(pool.acquire(arena, view, vg::core::FacetKind::Sample, &during_flight, &error));
    assert(during_flight.index != ref.index);

    // end_gpu_use matches the generation the use was begun under, not the
    // bumped one, and releasing the last use is what frees the index.
    assert(pool.end_gpu_use(ref, &error));
    assert(!pool.end_gpu_use(ref, &error));
    vg::core::FacetRef recycled;
    assert(pool.acquire(arena, view, vg::core::FacetKind::Sample, &recycled, &error));
    assert(recycled.index == ref.index);

    // An epoch bump takes the same path: retire_stale withholds an in-flight
    // slot's index until the work referencing it is done.
    assert(pool.begin_gpu_use(arena, recycled, &error));
    uint32_t epoch = 0;
    assert(arena.transform(backing.id, backing.generation, &epoch));
    assert(pool.retire_stale(arena) >= 1);
    vg::core::FacetRef after_stale;
    assert(pool.acquire(arena, view, vg::core::FacetKind::Sample, &after_stale, &error));
    assert(after_stale.index != recycled.index);
    assert(pool.end_gpu_use(recycled, &error));

    // A stale ref can never start new work in the first place.
    assert(!pool.begin_gpu_use(arena, recycled, &error));
  }
}

void test_view_and_representation_epoch() {
  // ==========================================================================
  // Phase C: CanonicalView shape contract, RepresentationEpoch, ConsumeInput
  // proofs, E016 backpressure, the checked-profile generation table, and the
  // reference backend's Stage 5. Each block states one documented requirement.
  // ==========================================================================

  // --- CanonicalView::valid()/layout (02 §3.3). Shape validity is a property
  // of the view contract, so every backend gets the same answer; the byte
  // layout is the single contract the Metal upload path and the reference
  // sampling oracle both encode against, so an image comparison between them
  // is only meaningful if it is pinned here. ---
  {
    vg::core::CanonicalView view;
    view.allocation = 1;
    view.allocation_generation = 1;
    view.format = vg::core::PixelFormat::RGBA8Unorm;
    view.dimension = vg::core::ViewDimension::Texture2D;
    view.width = 8;
    view.height = 4;
    std::string shape_error;
    assert(view.valid(&shape_error));

    // Zero extent cannot describe a real image.
    vg::core::CanonicalView zero_width = view;
    zero_width.width = 0;
    assert(!zero_width.valid(&shape_error));
    assert(shape_error == "canonical view extent must be non-zero");
    vg::core::CanonicalView zero_height = view;
    zero_height.height = 0;
    assert(!zero_height.valid(&shape_error));
    vg::core::CanonicalView zero_layers = view;
    zero_layers.array_layers = 0;
    assert(!zero_layers.valid(&shape_error));
    assert(shape_error == "canonical view must name at least one array layer");
    vg::core::CanonicalView zero_levels = view;
    zero_levels.mip_levels = 0;
    assert(!zero_levels.valid(&shape_error));
    assert(shape_error == "canonical view must name at least one mip level");

    // 8x4 supports exactly 4 levels (8x4, 4x2, 2x1, 1x1); a 5th would alias
    // the 1x1 rather than describe new texels, so it is malformed rather than
    // something to clamp.
    vg::core::CanonicalView full_chain = view;
    full_chain.mip_levels = 4;
    assert(full_chain.valid(&shape_error));
    vg::core::CanonicalView over_chain = view;
    over_chain.mip_levels = 5;
    assert(!over_chain.valid(&shape_error));
    assert(shape_error == "canonical view mip chain is longer than its extent supports");

    // array_layers > 1 needs the array dimension.
    vg::core::CanonicalView flat_array = view;
    flat_array.array_layers = 2;
    assert(!flat_array.valid(&shape_error));
    assert(shape_error == "Texture2D canonical view cannot name multiple array layers");
    vg::core::CanonicalView array_view = view;
    array_view.dimension = vg::core::ViewDimension::Texture2DArray;
    array_view.array_layers = 2;
    array_view.mip_levels = 4;
    assert(array_view.valid(&shape_error));
    // A Texture2DArray naming a single layer is legal; only the reverse is not.
    vg::core::CanonicalView single_layer_array = array_view;
    single_layer_array.array_layers = 1;
    assert(single_layer_array.valid(&shape_error));

    // Half-and-clamp, the sizing rule every graphics API's mip chain uses.
    assert(array_view.mip_width(0) == 8 && array_view.mip_height(0) == 4);
    assert(array_view.mip_width(1) == 4 && array_view.mip_height(1) == 2);
    assert(array_view.mip_width(2) == 2 && array_view.mip_height(2) == 1);
    assert(array_view.mip_width(3) == 1 && array_view.mip_height(3) == 1);
    // Clamped, not zero, past the end of the chain.
    assert(array_view.mip_width(9) == 1 && array_view.mip_height(9) == 1);

    assert(vg::core::bytes_per_texel(vg::core::PixelFormat::RGBA8Unorm) == 4);
    assert(vg::core::bytes_per_texel(vg::core::PixelFormat::R32Float) == 4);
    assert(array_view.subresource_count() == 8);
    assert(array_view.bytes_per_row(0) == 32 && array_view.bytes_per_row(2) == 8);
    assert(array_view.subresource_byte_size(0) == 128);
    assert(array_view.subresource_byte_size(3) == 4);

    // byte_size() is exactly the sum of every subresource's size.
    uint64_t summed = 0;
    for (uint32_t layer = 0; layer < array_view.array_layers; ++layer)
      for (uint32_t level = 0; level < array_view.mip_levels; ++level)
        summed += array_view.subresource_byte_size(level);
    assert(array_view.byte_size() == summed);
    assert(array_view.byte_size() == 344);

    // Offsets are slice-major, then ascending mip level, tightly packed.
    assert(array_view.subresource_byte_offset({0, 0}) == 0);
    assert(array_view.subresource_byte_offset({0, 1}) == 128);
    assert(array_view.subresource_byte_offset({0, 2}) == 160);
    assert(array_view.subresource_byte_offset({0, 3}) == 168);
    assert(array_view.subresource_byte_offset({1, 0}) == 172);
    assert(array_view.subresource_byte_offset({1, 3}) == 340);
  }

  // --- RepresentationEpoch/Builder (02 §4.1): a representation transform
  // produces a new frozen interpretation rather than editing this one, which
  // is 02 §8's "transform 不是纯 barrier" stated as a data structure. Mirrors
  // GraphEpochBuilder's shape, so seal() stamps the arena's clock the same way
  // GraphEpochBuilder::seal() stamps topology_epoch(). ---
  {
    vg::core::Arena epoch_arena;
    auto& epoch_backing = epoch_arena.allocate(256);
    const uint64_t backing_id = epoch_backing.id;
    const uint32_t backing_generation = epoch_backing.generation;

    uint32_t published = 0;
    assert(epoch_arena.transform(backing_id, backing_generation, &published) && published == 1);
    assert(epoch_arena.representation_clock() == 1);

    vg::core::CanonicalView view;
    view.allocation = backing_id;
    view.allocation_generation = backing_generation;
    view.width = 4;
    view.height = 4;

    vg::core::FacetPool epoch_pool;
    vg::core::FacetRef sample_ref;
    std::string epoch_error;
    assert(epoch_pool.acquire(epoch_arena, view, vg::core::FacetKind::Sample, &sample_ref, &epoch_error));

    vg::core::RepresentationEpochBuilder epoch_builder(&epoch_arena);
    // add_representation(arena, ...) snapshots the allocation's *current*
    // epoch, so a caller cannot freeze a version the arena is not at.
    assert(epoch_builder.add_representation(epoch_arena, backing_id, backing_generation, &epoch_error));
    assert(epoch_builder.add_facet(epoch_arena, epoch_pool, sample_ref, &epoch_error));
    // Re-adding the same reference is idempotent, not an error.
    assert(epoch_builder.add_representation(epoch_arena, backing_id, backing_generation, &epoch_error));
    // A reference the arena does not hold is refused rather than frozen.
    assert(!epoch_builder.add_representation(epoch_arena, 999, 1, &epoch_error));
    assert(epoch_error == "representation reference is not active in arena");
    assert(!epoch_builder.add_representation({backing_id, 0, published}, &epoch_error));
    assert(epoch_error == "representation reference generation must be non-zero");

    vg::core::RepresentationEpoch representation_epoch;
    assert(!epoch_builder.sealed());
    assert(epoch_builder.seal(&representation_epoch, &epoch_error));
    assert(epoch_builder.sealed());
    assert(representation_epoch.sealed());
    // Stamped from the arena's representation clock, the sibling of the
    // topology_epoch() stamp GraphEpochBuilder::seal() uses.
    assert(representation_epoch.value() == epoch_arena.representation_clock());
    assert(representation_epoch.representations().size() == 1);
    assert(representation_epoch.facets().size() == 1);
    assert(representation_epoch.contains(
        vg::core::RepresentationRef{backing_id, backing_generation, published}));
    assert(representation_epoch.contains(sample_ref));
    assert(!representation_epoch.contains(vg::core::FacetRef{sample_ref.index, sample_ref.generation + 1}));
    assert(!representation_epoch.stale(epoch_arena));
    // Seal once: a sealed builder is immutable (02 §4.1's build -> release ->
    // immutable -> retire).
    assert(!epoch_builder.seal(&representation_epoch, &epoch_error));
    assert(epoch_error == "representation epoch builder is already sealed");
    assert(!epoch_builder.add_representation({backing_id, backing_generation, published}, &epoch_error));
    assert(epoch_error == "representation epoch is sealed");

    // Another transform makes the frozen interpretation stale wholesale: every
    // facet it authorized has to be rebuilt rather than reused (02 §10 at epoch
    // granularity).
    uint32_t superseding = 0;
    assert(epoch_arena.transform(backing_id, backing_generation, &superseding) && superseding == 2);
    assert(representation_epoch.stale(epoch_arena));
    assert(epoch_pool.lookup(epoch_arena, sample_ref) == nullptr);

    // A facet whose slot is already stale cannot be frozen: doing so would
    // authorize a token that is dead on arrival.
    vg::core::RepresentationEpochBuilder stale_builder(&epoch_arena);
    assert(!stale_builder.add_facet(epoch_arena, epoch_pool, sample_ref, &epoch_error));
    assert(epoch_error == vg::core::to_string(vg::core::FacetStatus::EpochStale));

    // Without an arena the builder falls back to the next_epoch it was
    // constructed with, rather than inventing a clock value.
    vg::core::RepresentationEpochBuilder detached(static_cast<uint64_t>(7));
    assert(detached.add_representation({backing_id, backing_generation, superseding}));
    vg::core::RepresentationEpoch detached_epoch;
    assert(detached.seal(&detached_epoch));
    assert(detached_epoch.value() == 7);
  }
}

void test_facet_generation_table() {
  // --- FacetPool::snapshot_generations()/generation_valid() (06 §6.4): a
  // shader cannot call lookup(), so the checked profile uploads a table and
  // the kernel compares against it. generation_valid() is the host-side mirror
  // of that in-shader comparison and is deliberately *weaker* than lookup():
  // it sees only what the table encodes, so it cannot observe an epoch that
  // went stale, which is why the host-side lookup() stays authoritative. ---
  {
    vg::core::Arena table_arena;
    auto& table_backing = table_arena.allocate(256);
    const uint64_t table_id = table_backing.id;
    const uint32_t table_generation = table_backing.generation;

    vg::core::CanonicalView view;
    view.allocation = table_id;
    view.allocation_generation = table_generation;
    view.width = 4;
    view.height = 4;

    vg::core::FacetPool table_pool;
    vg::core::FacetRef live_ref;
    std::string table_error;
    assert(table_pool.acquire(table_arena, view, vg::core::FacetKind::Sample, &live_ref, &table_error));

    std::vector<uint32_t> table;
    table_pool.snapshot_generations(&table);
    assert(table.size() == table_pool.slot_count());
    assert(table.size() == 1);
    assert(table[live_ref.index] == live_ref.generation);
    assert(table_pool.generation_valid(live_ref));

    // A representation transform stales the token host-side...
    uint32_t table_epoch = 0;
    assert(table_arena.transform(table_id, table_generation, &table_epoch));
    vg::core::FacetStatus status = vg::core::FacetStatus::Ok;
    assert(table_pool.lookup(table_arena, live_ref, &status) == nullptr);
    assert(status == vg::core::FacetStatus::EpochStale);
    // ...but the uploaded table encodes only the slot's generation, so a
    // freshly pulled snapshot still shows the slot live and generation_valid()
    // -- the in-shader verdict -- still accepts the token. This is the exact
    // gap that makes lookup() authoritative rather than redundant.
    table_pool.snapshot_generations(&table);
    assert(table[live_ref.index] == live_ref.generation);
    assert(table_pool.generation_valid(live_ref));

    // Retirement is what the table *can* see: retire_stale() sweeps the
    // epoch-stale slot, and only then does the entry read 0 and the in-shader
    // check reject.
    assert(table_pool.retire_stale(table_arena) == 1);
    table_pool.snapshot_generations(&table);
    assert(table[live_ref.index] == 0);
    assert(!table_pool.generation_valid(live_ref));

    // A recycled index carries a bumped generation, so the old token is
    // rejected by the table while the new one is accepted -- the
    // index+generation discipline the shader relies on.
    vg::core::FacetRef recycled;
    assert(table_pool.acquire(table_arena, view, vg::core::FacetKind::Sample, &recycled, &table_error));
    assert(recycled.index == live_ref.index);
    assert(recycled.generation != live_ref.generation);
    table_pool.snapshot_generations(&table);
    assert(table[recycled.index] == recycled.generation);
    assert(table_pool.generation_valid(recycled));
    assert(!table_pool.generation_valid(live_ref));

    // A forged index is out of the table's range and rejected by both.
    const vg::core::FacetRef forged{9999, 1};
    assert(!table_pool.generation_valid(forged));
    assert(table_pool.lookup(table_arena, forged, &status) == nullptr);
    assert(status == vg::core::FacetStatus::UnknownIndex);
  }
}

}  // namespace vg::tests::core
