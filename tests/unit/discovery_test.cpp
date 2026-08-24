// TASK-D2 / ADR-036: core seed-topology walk (02 §7.2). Distinct from
// core_test.cpp's B-era build_access_certificate coverage -- that function
// still scans every Active allocation and must keep doing so.
#include "core/core.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr size_t kPointerRefWireBytes = sizeof(uint64_t) + sizeof(uint32_t);

void write_ref(vg::core::Allocation& allocation, const vg::core::PointerRef& ref) {
  assert(allocation.bytes.size() >= kPointerRefWireBytes);
  std::memcpy(allocation.bytes.data(), &ref.allocation, sizeof(ref.allocation));
  std::memcpy(allocation.bytes.data() + sizeof(ref.allocation), &ref.generation, sizeof(ref.generation));
}

size_t active_count(const vg::core::Arena& arena) {
  size_t count = 0;
  for (const auto& [id, allocation] : arena.allocations()) {
    (void)id;
    if (allocation.state == vg::core::ObjectState::Active) ++count;
  }
  return count;
}

bool contains_ref(const std::vector<vg::core::PointerRef>& refs, const vg::core::PointerRef& wanted) {
  return std::ranges::any_of(refs, [&](const vg::core::PointerRef& ref) {
    return ref.allocation == wanted.allocation && ref.generation == wanted.generation;
  });
}

}  // namespace

int main() {
  // 4-node pointer chain n0->n1->n2->n3 with a broken hop at n1 (generation
  // 0 is not well-formed). Seed at n0 reaches only {n0, n1}. Universe is 4.
  {
    vg::core::Arena arena;
    auto& n0 = arena.allocate(12);
    auto& n1 = arena.allocate(12);
    auto& n2 = arena.allocate(12);
    auto& n3 = arena.allocate(12);
    write_ref(n0, {n1.id, n1.generation});
    write_ref(n1, {n2.id, 0});  // well-formed check rejects generation 0
    write_ref(n2, {n3.id, n3.generation});

    const vg::core::PointerRef seed{n0.id, n0.generation};
    vg::core::DiscoveryResult discovery;
    std::string error;
    assert(vg::core::discover_reachable(arena, {seed}, &discovery, &error));
    assert(active_count(arena) == 4);
    assert(discovery.reachable.size() == 2);
    assert(discovery.reachable.size() < active_count(arena));
    assert(contains_ref(discovery.reachable, seed));
    assert(contains_ref(discovery.reachable, {n1.id, n1.generation}));
    assert(!contains_ref(discovery.reachable, {n2.id, n2.generation}));
    assert(!contains_ref(discovery.reachable, {n3.id, n3.generation}));

    vg::core::AccessCertificate certificate;
    assert(vg::core::build_discovered_certificate(arena, discovery, &certificate, &error));
    assert(certificate.mode == vg::core::AccessCertificateMode::DiscoverThenLease);
    assert(certificate.epoch.references().size() == 2);
    assert(vg::core::certificate_covers_discovery_witness(certificate, discovery.reachable, &error));

    vg::core::WorkingSetLease lease;
    for (const auto& ref : discovery.reachable) {
      assert(lease.add(ref, discovery.reachable, &error));
    }
    lease.complete = true;
    assert(lease.valid(discovery.reachable, &error));

    // B-era DiscoverThenLease on the same Arena is still Universe-sized.
    vg::core::AccessCertificate era_b;
    assert(vg::core::build_access_certificate(arena, vg::core::AccessCertificateMode::DiscoverThenLease,
                                              {seed}, &era_b, &error));
    assert(era_b.epoch.references().size() == 4);

    std::cout << "discovery: 4-node chain seed0 reachable=" << discovery.reachable.size()
              << " universe=" << active_count(arena) << "\n";
  }

  // Second reachable ratio: seed at an isolated node reaches 1 of 4.
  {
    vg::core::Arena arena;
    auto& n0 = arena.allocate(12);
    auto& n1 = arena.allocate(12);
    auto& n2 = arena.allocate(12);
    auto& n3 = arena.allocate(12);
    (void)n0;
    write_ref(n1, {n2.id, n2.generation});
    write_ref(n2, {n3.id, n3.generation});

    vg::core::DiscoveryResult discovery;
    std::string error;
    const vg::core::PointerRef seed{n0.id, n0.generation};
    assert(vg::core::discover_reachable(arena, {seed}, &discovery, &error));
    assert(discovery.reachable.size() == 1);
    assert(discovery.reachable.size() < active_count(arena));
    std::cout << "discovery: isolated seed reachable=" << discovery.reachable.size()
              << " universe=" << active_count(arena) << "\n";
  }

  // Forged witness: an extra Active allocation not in the discovered set.
  {
    vg::core::Arena arena;
    auto& n0 = arena.allocate(12);
    auto& n1 = arena.allocate(12);
    auto& extra = arena.allocate(12);
    write_ref(n0, {n1.id, n1.generation});

    vg::core::DiscoveryResult discovery;
    std::string error;
    assert(vg::core::discover_reachable(arena, {{n0.id, n0.generation}}, &discovery, &error));
    vg::core::AccessCertificate certificate;
    assert(vg::core::build_discovered_certificate(arena, discovery, &certificate, &error));
    std::vector<vg::core::PointerRef> forged = discovery.reachable;
    forged.push_back({extra.id, extra.generation});
    assert(!vg::core::certificate_covers_discovery_witness(certificate, forged, &error));
    assert(error.find("witness") != std::string::npos);

    vg::core::WorkingSetLease lease;
    assert(!lease.add({extra.id, extra.generation}, discovery.reachable, &error));
    std::cout << "discovery: forged witness refused\n";
  }

  // Topology bump mid-walk refuses (02 §7.2 freeze).
  {
    vg::core::Arena arena;
    auto& n0 = arena.allocate(12);
    auto& n1 = arena.allocate(12);
    write_ref(n0, {n1.id, n1.generation});

    vg::core::DiscoveryResult discovery;
    std::string error;
    bool bumped = false;
    const auto bump = [&]() {
      if (bumped) return;
      bumped = true;
      arena.allocate(12);
    };
    assert(!vg::core::discover_reachable(arena, {{n0.id, n0.generation}}, &discovery, &error, bump));
    assert(error.find("topology epoch") != std::string::npos);
    std::cout << "discovery: mid-walk topology bump refused\n";
  }

  std::cout << "discovery: ok\n";
  return 0;
}
