#include "cases.h"
#include <cassert>
#include "backends/device_hal.h"

namespace vg::tests::core {

void test_lease_budget_overflow() {
  // --- TASK-D1 / ADR-035: lease, budget, overflow are independent types.
  // Budget 0 ≠ unset. A lease cannot cover an unproven allocation.
  // Rejected overflow cannot answer continued(). ---
  {
    const auto unlimited = vg::core::WorkingSetBudget::unlimited();
    assert(!unlimited.has_limit);
    assert(unlimited.allows(0));
    assert(unlimited.allows(1ull << 40));

    const auto zero = vg::core::WorkingSetBudget::limited(0);
    assert(zero.has_limit);
    assert(zero.byte_limit == 0);
    assert(zero.allows(0));
    std::string budget_error;
    assert(!zero.allows(1, &budget_error));
    assert(budget_error == "working-set budget exceeded");
    assert(zero.has_limit != unlimited.has_limit);

    const vg::core::PointerRef proven_a{1, 1};
    const vg::core::PointerRef proven_b{2, 1};
    const vg::core::PointerRef stranger{3, 1};
    const std::vector<vg::core::PointerRef> proven{proven_a, proven_b};

    vg::core::WorkingSetLease lease;
    std::string lease_error;
    assert(lease.add(proven_a, proven, &lease_error));
    assert(lease.covers(proven_a));
    assert(!lease.covers(stranger));
    assert(!lease.add(stranger, proven, &lease_error));
    assert(lease_error == "lease cannot cover an unproven allocation");
    assert(!lease.covers(stranger));
    lease.allocations.push_back(stranger);
    assert(!lease.valid(proven, &lease_error));
    assert(lease_error == "lease cannot cover an unproven allocation");
    lease.allocations.pop_back();
    assert(lease.valid(proven, &lease_error));

    vg::core::EnvelopeOverflow unused;
    assert(unused.valid());
    assert(!unused.continued());
    unused.overflow_task_count = 3;
    assert(!unused.valid(&lease_error));
    assert(lease_error == "an unused overflow record cannot carry leftover work or a continuation token");

    vg::core::EnvelopeOverflow rejected;
    rejected.disposition = vg::core::EnvelopeOverflowDisposition::Rejected;
    rejected.overflow_task_count = 4;
    assert(rejected.valid());
    assert(!rejected.continued());
    rejected.continuation_token = 9;
    assert(!rejected.valid(&lease_error));
    assert(lease_error == "a rejected overflow cannot be marked continued");
    assert(!rejected.continued());

    vg::core::EnvelopeOverflow deferred;
    deferred.disposition = vg::core::EnvelopeOverflowDisposition::Deferred;
    assert(!deferred.valid(&lease_error));
    assert(lease_error == "a deferred overflow requires leftover work and a continuation token");
    deferred.overflow_task_count = 2;
    deferred.continuation_token = 11;
    assert(deferred.valid());
    assert(deferred.continued());

    vg::core::ExecutionPlan plan;
    assert(!plan.working_set_budget.has_value());
    assert(!plan.working_set_lease.has_value());
    assert(!plan.pending_overflow.has_value());
    vg::hal::Submission submission;
    assert(!submission.envelope_overflow.has_value());

    const auto limited = vg::core::WorkingSetBudget::limited(16);
    std::string plan_error;
    assert(!limited.allows(32, &plan_error));
    assert(plan_error == "working-set budget exceeded");
    assert(limited.allows(16));
  }
}

void test_certificate_composition() {
  {
    vg::core::Arena arena;
    auto& left = arena.allocate(32);
    auto& right = arena.allocate(32);
    auto& unused = arena.allocate(32);
    (void)unused;
    vg::core::Certificate parent;
    parent.ranges.push_back({left.id, 0, 16, vg::ir::Access::Read, 0});
    vg::core::Certificate child;
    child.ranges.push_back({left.id, 16, 16, vg::ir::Access::Write, 0});
    vg::core::Certificate composed;
    std::string compose_error;
    assert(vg::core::compose_certificates({parent, child}, &composed, &compose_error));
    assert(composed.covers(parent.ranges.front()));
    assert(composed.covers(child.ranges.front()));
    vg::ir::Effect forged{right.id, 0, 8, vg::ir::Access::Read, 0};
    assert(!composed.covers(forged));
    vg::core::Certificate empty_out;
    assert(!vg::core::compose_certificates({}, &empty_out, &compose_error));
    assert(compose_error == "certificate composition requires at least one child certificate");

    vg::core::GraphEpochBuilder left_builder(&arena);
    assert(left_builder.add_reference(arena, {left.id, left.generation}));
    vg::core::GraphEpoch left_epoch;
    assert(left_builder.seal(&left_epoch));
    vg::core::AccessCertificate left_cert;
    left_cert.mode = vg::core::AccessCertificateMode::DiscoverThenLease;
    left_cert.epoch = left_epoch;

    vg::core::GraphEpochBuilder right_builder(&arena);
    assert(right_builder.add_reference(arena, {right.id, right.generation}));
    vg::core::GraphEpoch right_epoch;
    assert(right_builder.seal(&right_epoch));
    vg::core::AccessCertificate right_cert;
    right_cert.mode = vg::core::AccessCertificateMode::DiscoverThenLease;
    right_cert.epoch = right_epoch;

    vg::core::AccessCertificate union_cert;
    bool exploded = false;
    assert(vg::core::compose_access_certificates(arena, {left_cert, right_cert}, &union_cert, &exploded,
                                                 &compose_error));
    assert(union_cert.epoch.references().size() == 2);
    assert(!exploded);
    assert(vg::core::certificate_covers_discovery_witness(union_cert, left_cert.epoch.references()));
    assert(vg::core::certificate_covers_discovery_witness(union_cert, right_cert.epoch.references()));
    std::vector<vg::core::PointerRef> forged_witness{{unused.id, unused.generation}};
    assert(!vg::core::certificate_covers_discovery_witness(union_cert, forged_witness, &compose_error));
    assert(compose_error == "discovery witness is not covered by the certificate");

    vg::core::GraphEpochBuilder unused_builder(&arena);
    assert(unused_builder.add_reference(arena, {unused.id, unused.generation}));
    vg::core::GraphEpoch unused_epoch;
    assert(unused_builder.seal(&unused_epoch));
    vg::core::AccessCertificate unused_cert;
    unused_cert.mode = vg::core::AccessCertificateMode::DiscoverThenLease;
    unused_cert.epoch = unused_epoch;

    vg::core::AccessCertificate exploded_cert;
    bool did_explode = false;
    assert(vg::core::compose_access_certificates(arena, {left_cert, right_cert, unused_cert}, &exploded_cert,
                                                 &did_explode, &compose_error));
    assert(exploded_cert.epoch.references().size() == 3);
    assert(did_explode);

    auto& extra = arena.allocate(8);
    vg::core::GraphEpochBuilder later_builder(&arena);
    assert(later_builder.add_reference(arena, {extra.id, extra.generation}));
    vg::core::GraphEpoch later_epoch;
    assert(later_builder.seal(&later_epoch));
    vg::core::AccessCertificate later_cert;
    later_cert.mode = vg::core::AccessCertificateMode::DiscoverThenLease;
    later_cert.epoch = later_epoch;
    vg::core::AccessCertificate mixed;
    assert(!vg::core::compose_access_certificates(arena, {left_cert, later_cert}, &mixed, nullptr, &compose_error));
    assert(compose_error == "cannot compose access certificates from different graph epochs");
  }
}

}  // namespace vg::tests::core
