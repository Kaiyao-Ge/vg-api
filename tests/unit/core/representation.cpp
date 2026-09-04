#include "cases.h"
#include <cassert>
#include "capture/capture.h"

namespace vg::tests::core {

void test_consume_proof(const vg::core::ConsumeProof& discharged) {
  // --- ConsumeProof (02 §4.2): none of the four obligations is observable
  // from arena state, so they are attested rather than inferred, and the
  // rejection names *which* proof failed. 10 §3's "ConsumeInput proof"
  // conformance row and 10 §4's "illegal consume" negative case. ---
  {
    assert(vg::core::ConsumeProof{}.first_unmet() != nullptr);
    assert(std::string(vg::core::ConsumeProof{}.first_unmet()) == "old envelope has not completed");
    assert(std::string(vg::core::ConsumeProof{true, false, false, false}.first_unmet()) ==
           "an external reference to the old representation still exists");
    assert(std::string(vg::core::ConsumeProof{true, true, false, false}.first_unmet()) ==
           "the old representation may still be replayed");
    assert(std::string(vg::core::ConsumeProof{true, true, true, false}.first_unmet()) ==
           "destructive-failure semantics were not accepted");
    assert(discharged.complete());
    assert(discharged.first_unmet() == nullptr);

    vg::core::Arena consume_arena;
    auto& consume_backing = consume_arena.allocate(64);
    const uint64_t consume_id = consume_backing.id;
    const uint32_t consume_generation = consume_backing.generation;
    uint32_t consume_epoch = 0;
    assert(consume_arena.transform(consume_id, consume_generation, &consume_epoch) && consume_epoch == 1);
    assert(consume_arena.lookup(vg::core::PointerRef{consume_id, consume_generation})->live_representations == 2);

    // An incomplete proof is refused before any state is touched, by both
    // destructive operations.
    std::string consume_error;
    uint64_t released = 0;
    assert(!consume_arena.consume_representation(consume_id, consume_generation, consume_epoch,
                                                 vg::core::ConsumeProof{true, true, false, true}, &released,
                                                 &consume_error));
    assert(consume_error ==
           "ConsumeInput proof incomplete: the old representation may still be replayed");
    assert(released == 0);
    assert(!consume_arena.consume(consume_id, consume_generation, consume_epoch,
                                  vg::core::ConsumeProof{true, true, true, false}, &consume_error));
    assert(consume_error ==
           "ConsumeInput proof incomplete: destructive-failure semantics were not accepted");
    assert(consume_arena.lookup(vg::core::RepresentationRef{consume_id, consume_generation, consume_epoch}) != nullptr);

    // consume_representation() is the transform form (06 §11): the object
    // survives -- identity, generation and freshly published epoch all stay
    // live, so facets acquired against the new representation keep resolving --
    // but the superseded backing is handed back at once. That released byte
    // count is E005's watermark reduction.
    assert(consume_arena.consume_representation(consume_id, consume_generation, consume_epoch, discharged,
                                                &released, &consume_error));
    assert(released == 64);
    const auto* survivor = consume_arena.lookup(vg::core::RepresentationRef{consume_id, consume_generation, consume_epoch});
    assert(survivor != nullptr);
    assert(survivor->state == vg::core::ObjectState::Active);
    assert(survivor->generation == consume_generation);
    assert(survivor->representation_epoch == consume_epoch);
    // The old version is no longer retained as an extra live representation.
    assert(survivor->live_representations == 1);
    assert(survivor->bytes.empty());

    // Collapsing the two destructive operations would stale the very facet a
    // transform just published, so consume() -- the retiring form -- is
    // distinct: it retires the allocation and bumps its generation so no old
    // token can ever resolve again.
    vg::core::Arena retire_arena;
    auto& retire_backing = retire_arena.allocate(64);
    const uint64_t retire_id = retire_backing.id;
    const uint32_t retire_generation = retire_backing.generation;
    uint32_t retire_epoch = 0;
    assert(retire_arena.transform(retire_id, retire_generation, &retire_epoch));
    assert(retire_arena.consume(retire_id, retire_generation, retire_epoch, discharged, &consume_error));
    const auto& retired = retire_arena.allocations().at(retire_id);
    assert(retired.state == vg::core::ObjectState::Retired);
    assert(retired.generation == retire_generation + 1);
    assert(retired.live_representations == 0);
    assert(retired.bytes.empty());
    assert(retire_arena.lookup(vg::core::PointerRef{retire_id, retire_generation}) == nullptr);
    // 10 §5: a retired generation is never visible again, and the bumped one
    // was never handed out either.
    assert(retire_arena.lookup(vg::core::PointerRef{retire_id, retire_generation + 1}) == nullptr);
    assert(!retire_arena.consume(retire_id, retire_generation, retire_epoch, discharged, &consume_error));
    assert(consume_error == "stale allocation or representation epoch for consume");

    // 10 §4's "illegal consume": both forms require exclusive ownership, and a
    // superseded epoch token cannot consume the current representation.
    vg::core::Arena exclusive_arena;
    auto& exclusive_backing = exclusive_arena.allocate(32);
    const uint64_t exclusive_id = exclusive_backing.id;
    const uint32_t exclusive_generation = exclusive_backing.generation;
    uint32_t exclusive_epoch = 0;
    assert(exclusive_arena.transform(exclusive_id, exclusive_generation, &exclusive_epoch));
    assert(exclusive_arena.acquire(exclusive_id, exclusive_generation));
    assert(!exclusive_arena.consume_representation(exclusive_id, exclusive_generation, exclusive_epoch,
                                                   discharged, nullptr, &consume_error));
    assert(consume_error == "consume requires exclusive ownership");
    assert(!exclusive_arena.consume(exclusive_id, exclusive_generation, exclusive_epoch, discharged,
                                    &consume_error));
    assert(consume_error == "consume requires exclusive ownership");
    assert(exclusive_arena.release(exclusive_id, exclusive_generation));
    assert(!exclusive_arena.consume(exclusive_id, exclusive_generation, exclusive_epoch - 1, discharged,
                                    &consume_error));
    assert(consume_error == "stale allocation or representation epoch for consume");
    assert(exclusive_arena.consume(exclusive_id, exclusive_generation, exclusive_epoch, discharged,
                                   &consume_error));
  }
}

void test_capture_consumed_representation(const vg::core::ConsumeProof& discharged) {
  {
    vg::core::Arena cap_arena;
    auto& backing = cap_arena.allocate(16);
    backing.bytes.assign(16, 0);
    backing.bytes[0] = 255;
    vg::ir::Module module;
    module.version = 1;
    module.root_schema = "vg.test/v1";
    vg::ir::Instruction load;
    load.op = "load";
    load.allocation = backing.id;
    load.generation = backing.generation;
    load.representation_epoch = backing.representation_epoch;
    load.offset = 0;
    load.size = 4;
    module.instructions.push_back(load);
    module.declared_effects.push_back({backing.id, 0, 4, vg::ir::Access::Read, backing.representation_epoch});
    const auto pre = vg::capture::make_capture(module, cap_arena);
    uint32_t new_epoch = 0;
    assert(cap_arena.transform(backing.id, backing.generation, &new_epoch));
    uint64_t released = 0;
    std::string cap_error;
    assert(cap_arena.consume_representation(backing.id, backing.generation, new_epoch, discharged, &released,
                                           &cap_error));
    assert(released == 16);
    assert(backing.bytes.empty());
    vg::capture::ReplayResult pre_replay;
    assert(vg::capture::replay(pre, &pre_replay, &cap_error));
    assert(pre_replay.execution.ok);
    const auto post = vg::capture::make_capture(module, cap_arena);
    assert(post.allocations.size() == 1);
    assert(post.allocations[0].size == 16);
    assert(post.allocations[0].bytes.empty());
    // consume_representation clears bytes but leaves Allocation::size, so the
    // snapshot is not importable (bytes.size() != size). That is the lost
    // replay: the package cannot be reconstituted, not merely executed stale.
    vg::capture::ReplayResult post_replay;
    assert(!vg::capture::replay(post, &post_replay, &cap_error));
    assert(cap_error == "cannot restore a consumed representation");
  }
}

void test_representation_backpressure() {
  // --- E016 backpressure: "禁止无界创建版本" and "内存不足时可预测失败而非系统
  // 抖动". A non-zero budget makes transform() refuse with an explicit error
  // instead of letting versions accumulate; 0 (the default) is unbounded. ---
  {
    vg::core::Arena budget_arena;
    auto& budget_backing = budget_arena.allocate(32);
    const uint64_t budget_id = budget_backing.id;
    const uint32_t budget_generation = budget_backing.generation;
    assert(budget_arena.max_in_flight_representations() == 0);
    // An allocation starts at 1 live representation -- its own.
    assert(budget_backing.live_representations == 1);

    std::string budget_error;
    budget_arena.set_max_in_flight_representations(1);
    assert(budget_arena.max_in_flight_representations() == 1);
    // A budget of 1 is already exhausted by the initial representation, so the
    // first transform is refused predictably rather than blocking.
    assert(!budget_arena.transform(budget_id, budget_generation, nullptr, &budget_error));
    assert(budget_error == "in-flight representation budget exceeded");
    assert(budget_arena.lookup(vg::core::PointerRef{budget_id, budget_generation})->representation_epoch == 0);

    budget_arena.set_max_in_flight_representations(2);
    uint32_t budget_epoch = 0;
    assert(budget_arena.transform(budget_id, budget_generation, &budget_epoch) && budget_epoch == 1);
    assert(budget_arena.lookup(vg::core::PointerRef{budget_id, budget_generation})->live_representations == 2);
    // A second transform is refused until a version is released.
    assert(!budget_arena.transform(budget_id, budget_generation, nullptr, &budget_error));
    assert(budget_error == "in-flight representation budget exceeded");
    assert(budget_arena.release_representation(budget_id, budget_generation, &budget_error));
    assert(budget_arena.lookup(vg::core::PointerRef{budget_id, budget_generation})->live_representations == 1);
    assert(budget_arena.transform(budget_id, budget_generation, &budget_epoch) && budget_epoch == 2);

    // release_representation() never drops the last version: an Active
    // allocation always retains its current representation.
    assert(budget_arena.release_representation(budget_id, budget_generation, &budget_error));
    assert(budget_arena.lookup(vg::core::PointerRef{budget_id, budget_generation})->live_representations == 1);
    assert(!budget_arena.release_representation(budget_id, budget_generation, &budget_error));
    assert(budget_error == "an active allocation always retains its current representation");
    // A stale token cannot release a version either.
    assert(!budget_arena.release_representation(budget_id, budget_generation + 1, &budget_error));
    assert(budget_error == "stale allocation for representation release");

    // Back to unbounded: the budget is a policy input, not a property of the
    // allocation.
    budget_arena.set_max_in_flight_representations(0);
    assert(budget_arena.transform(budget_id, budget_generation, &budget_epoch) && budget_epoch == 3);
    assert(budget_arena.transform(budget_id, budget_generation, &budget_epoch) && budget_epoch == 4);
    assert(budget_arena.lookup(vg::core::PointerRef{budget_id, budget_generation})->live_representations == 3);
  }
}

}  // namespace vg::tests::core
